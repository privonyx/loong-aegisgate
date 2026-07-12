// Phase 9.3 Epic 4 Task 4.2 — AuditBridge.
//
// AuditBridge is the single seam between ConfigServiceCore and the global
// AuditLogger chain (C3 decision: option A — a single chain so verifyChain()
// is unified and compliance bundles do not fracture).
//
// Invariants exercised here:
//   * Each record* method produces exactly one entry with the correct action.
//   * All entries live on "control_plane" stage with tenant "system".
//   * AuditLogger::verifyChain() passes across a 5-event timeline.
//   * JSON detail contains the parameters we rely on in §5.3 (version_id,
//     sha256, previous_active, etc.) and never the yaml_content (SR11).

#include "control_plane/audit_bridge.h"
#include "control_plane/config_version_record.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace aegisgate {
namespace {

class AuditBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());
        bridge_ = std::make_unique<AuditBridge>(audit_.get());
    }

    void flush() {
        ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    }

    ConfigVersionRecord makeRecord(const std::string& id,
                                    const std::string& submitter) const {
        ConfigVersionRecord r;
        r.version_id = id;
        r.content_sha256 = std::string(64, 'a');
        r.size_bytes = 128;
        r.yaml_content = "top-secret-yaml: value\n";  // MUST NOT reach audit
        r.submitter = submitter;
        r.submitter_comment = "initial";
        r.submitted_at = 1'700'000'000'000LL;
        return r;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<AuditBridge>           bridge_;
};

TEST_F(AuditBridgeTest, RecordSubmitEmitsOneEntry) {
    bridge_->recordSubmit(makeRecord("01VER0000000000000000000001", "alice"));
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].action, "config.submit");
    EXPECT_EQ(entries[0].stage_name, "control_plane");
    EXPECT_EQ(entries[0].tenant_id, "system");
    EXPECT_NE(entries[0].detail.find("01VER0000000000000000000001"),
              std::string::npos);
    EXPECT_NE(entries[0].detail.find("sha256"), std::string::npos);
}

TEST_F(AuditBridgeTest, SubmitAuditDoesNotLeakYamlContent) {
    // SR11 applies to audit detail too — the rendered JSON must never embed
    // the raw bundle text, or operators reading ciphered audit logs could
    // exfil config contents bypassing the yaml_content strip.
    bridge_->recordSubmit(makeRecord("01VER0000000000000000000001", "alice"));
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].detail.find("top-secret-yaml"), std::string::npos)
        << "yaml_content leaked into audit: " << entries[0].detail;
}

TEST_F(AuditBridgeTest, RecordApproveAndRejectActions) {
    bridge_->recordApprove("01VERAPPROVED", "bob", "LGTM");
    bridge_->recordReject("01VERREJECTED", "carol", "bad config");
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].action, "config.approve");
    EXPECT_EQ(entries[1].action, "config.reject");
    EXPECT_NE(entries[0].detail.find("01VERAPPROVED"), std::string::npos);
    EXPECT_NE(entries[0].detail.find("LGTM"), std::string::npos);
    EXPECT_NE(entries[1].detail.find("bad config"), std::string::npos);
}

TEST_F(AuditBridgeTest, RecordActivateCarriesPreviousActive) {
    bridge_->recordActivate("01VERNEW", "dave", "01VEROLD");
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].action, "config.activate");
    EXPECT_NE(entries[0].detail.find("01VERNEW"), std::string::npos);
    EXPECT_NE(entries[0].detail.find("01VEROLD"), std::string::npos);
    EXPECT_NE(entries[0].detail.find("previous_active"), std::string::npos);
}

TEST_F(AuditBridgeTest, RecordActivateHandlesNoPreviousActive) {
    // First activation: previous_active is empty. The field must still be
    // present so log parsers can rely on the schema.
    bridge_->recordActivate("01VERFIRST", "dave", "");
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].detail.find("previous_active"), std::string::npos);
}

TEST_F(AuditBridgeTest, RecordRollbackCarriesTargetAndPrevious) {
    bridge_->recordRollback("01VERTARGET", "dave", "01VERNOW", "revert");
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].action, "config.rollback");
    EXPECT_NE(entries[0].detail.find("01VERTARGET"), std::string::npos);
    EXPECT_NE(entries[0].detail.find("01VERNOW"), std::string::npos);
    EXPECT_NE(entries[0].detail.find("revert"), std::string::npos);
}

TEST_F(AuditBridgeTest, ChainVerifiesAcrossFiveEvents) {
    // C3: events must feed the *global* AuditLogger chain so verifyChain()
    // remains authoritative across gateway + control-plane actions.
    bridge_->recordSubmit(makeRecord("01VER001", "alice"));
    bridge_->recordApprove("01VER001", "bob", "");
    bridge_->recordActivate("01VER001", "carol", "");
    bridge_->recordSubmit(makeRecord("01VER002", "alice"));
    bridge_->recordRollback("01VER001", "carol", "01VER002", "revert");
    flush();

    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 5u);
    EXPECT_TRUE(audit_->verifyChain());

    // Each entry must carry a non-empty chain_hash and differ from its
    // neighbour so Merkle-ish linking actually happened.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_FALSE(entries[i].chain_hash.empty()) << "entry " << i;
        if (i > 0) {
            EXPECT_NE(entries[i].chain_hash, entries[i - 1].chain_hash);
        }
    }
}

TEST_F(AuditBridgeTest, RequestIdIsPerEventUlid) {
    bridge_->recordSubmit(makeRecord("01VER001", "alice"));
    bridge_->recordSubmit(makeRecord("01VER002", "alice"));
    flush();
    auto entries = audit_->entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_FALSE(entries[0].request_id.empty());
    EXPECT_FALSE(entries[1].request_id.empty());
    EXPECT_NE(entries[0].request_id, entries[1].request_id);
    EXPECT_EQ(entries[0].request_id.size(), 26u);
}

TEST_F(AuditBridgeTest, NullAuditIsNoOp) {
    // Production wiring may omit AuditLogger when the control plane runs in a
    // synthetic harness (e.g. bench). Bridge must tolerate that without UB.
    AuditBridge null_bridge(nullptr);
    EXPECT_NO_THROW(null_bridge.recordSubmit(
        makeRecord("01VER", "alice")));
    EXPECT_NO_THROW(null_bridge.recordApprove("01VER", "bob", ""));
}

} // namespace
} // namespace aegisgate
