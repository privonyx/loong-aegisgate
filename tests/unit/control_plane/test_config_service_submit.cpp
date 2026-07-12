// Phase 9.3 Epic 3 Task 3.5 — ConfigServiceCore::submit.
//
// Covers the Submit path end-to-end with an in-memory PersistentStore + a real
// AuditLogger. Validation hooks inject deterministic results so SR3 failure,
// sensitive-field detection (SR4) and size cap (SR2) can each be exercised in
// isolation without depending on the actual aegisgate.yaml shape.

#include "control_plane/config_service_core.h"
#include "control_plane/sensitive_scanner.h"
#include "control_plane/ulid.h"
#include "control_plane/state_machine.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <memory>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class ConfigServiceSubmitTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());

        ConfigServiceCore::Deps deps;
        deps.store = store_.get();
        deps.audit = audit_.get();
        deps.clock = []() { return kT0; };
        // Default validator accepts anything; individual tests override.
        deps.validator = [](const std::string&) {
            return std::vector<Config::ValidationIssue>{};
        };
        svc_ = std::make_unique<ConfigServiceCore>(std::move(deps));
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

TEST_F(ConfigServiceSubmitTest, HappyPathPersistsPendingRecord) {
    auto res = svc_->submit("server:\n  port: 8080\n", "alice", "initial", false);
    EXPECT_EQ(res.error_code, "");
    EXPECT_EQ(res.record.status, ConfigStatus::PENDING);
    EXPECT_EQ(res.record.submitter, "alice");
    EXPECT_EQ(res.record.submitter_comment, "initial");
    EXPECT_EQ(res.record.size_bytes,
              static_cast<std::int64_t>(std::string("server:\n  port: 8080\n").size()));
    EXPECT_EQ(res.record.submitted_at, kT0);
    EXPECT_EQ(res.record.version_id.size(), 26u);
    EXPECT_FALSE(res.record.content_sha256.empty());

    auto loaded = store_->getConfigVersion(res.record.version_id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, ConfigStatus::PENDING);
    EXPECT_EQ(loaded->yaml_content, "server:\n  port: 8080\n");
}

TEST_F(ConfigServiceSubmitTest, PayloadOverOneMiBRejectedWithoutWriting) {
    std::string huge(1024 * 1024 + 1, 'a');
    auto res = svc_->submit(huge, "alice", "", false);
    EXPECT_EQ(res.error_code, "PAYLOAD_TOO_LARGE");
    EXPECT_EQ(store_->listConfigVersions({}).size(), 0u);
}

TEST_F(ConfigServiceSubmitTest, SensitiveFieldBlocksSubmit) {
    auto res = svc_->submit("api_key: sk-abcdef1234567890\n", "alice", "", false);
    EXPECT_EQ(res.error_code, "SENSITIVE_FIELD_DETECTED");
    // The error message must identify the field without leaking the secret.
    EXPECT_NE(res.error_message.find("api_key"), std::string::npos);
    EXPECT_EQ(res.error_message.find("sk-abcdef"), std::string::npos);
    EXPECT_EQ(store_->listConfigVersions({}).size(), 0u);
}

TEST_F(ConfigServiceSubmitTest, InvalidYamlRejected) {
    auto res = svc_->submit("foo: [unterminated\n", "alice", "", false);
    EXPECT_EQ(res.error_code, "INVALID_YAML");
    EXPECT_EQ(store_->listConfigVersions({}).size(), 0u);
}

TEST_F(ConfigServiceSubmitTest, ValidatorErrorBlocksSubmit) {
    ConfigServiceCore::Deps deps;
    deps.store = store_.get();
    deps.audit = audit_.get();
    deps.clock = []() { return kT0; };
    deps.validator = [](const std::string&) {
        return std::vector<Config::ValidationIssue>{
            {Config::ValidationIssue::Error, "server.port", "must be > 0"}};
    };
    ConfigServiceCore svc(std::move(deps));

    auto res = svc.submit("server:\n  port: 0\n", "alice", "", false);
    EXPECT_EQ(res.error_code, "CONFIG_VALIDATION_FAILED");
    EXPECT_NE(res.error_message.find("server.port"), std::string::npos);
    EXPECT_EQ(store_->listConfigVersions({}).size(), 0u);
}

TEST_F(ConfigServiceSubmitTest, WarningsDoNotBlockSubmit) {
    ConfigServiceCore::Deps deps;
    deps.store = store_.get();
    deps.audit = audit_.get();
    deps.clock = []() { return kT0; };
    deps.validator = [](const std::string&) {
        return std::vector<Config::ValidationIssue>{
            {Config::ValidationIssue::Warning, "server.threads",
             "recommended >= 4"}};
    };
    ConfigServiceCore svc(std::move(deps));

    auto res = svc.submit("server:\n  port: 8080\n", "alice", "", false);
    EXPECT_EQ(res.error_code, "");
    EXPECT_EQ(res.record.status, ConfigStatus::PENDING);
}

TEST_F(ConfigServiceSubmitTest, DuplicateSha256ReturnsAlreadyExists) {
    std::string yaml = "server:\n  port: 8080\n";
    auto first = svc_->submit(yaml, "alice", "v1", false);
    ASSERT_EQ(first.error_code, "");

    auto second = svc_->submit(yaml, "bob", "retry", false);
    EXPECT_EQ(second.error_code, "ALREADY_EXISTS");
    // Only one record persisted.
    EXPECT_EQ(store_->listConfigVersions({}).size(), 1u);
}

TEST_F(ConfigServiceSubmitTest, DuplicateWhenActiveStillBlocks) {
    auto first = svc_->submit("server:\n  port: 8080\n", "alice", "", false);
    ASSERT_EQ(first.error_code, "");
    // Transition through APPROVE -> ACTIVE so we know active records also
    // participate in the dedupe check per design §3 ALREADY_EXISTS rules.
    ASSERT_TRUE(store_->updateConfigStatus(
        first.record.version_id, ConfigStatus::APPROVED, "bob", "ok", kT0 + 1));
    ASSERT_TRUE(store_->activateConfig(first.record.version_id, "bob", kT0 + 2));

    auto again = svc_->submit("server:\n  port: 8080\n", "carol", "", false);
    EXPECT_EQ(again.error_code, "ALREADY_EXISTS");
}

TEST_F(ConfigServiceSubmitTest, DedupeIgnoresRejectedVersions) {
    auto first = svc_->submit("server:\n  port: 8080\n", "alice", "", false);
    ASSERT_EQ(first.error_code, "");
    ASSERT_TRUE(store_->updateConfigStatus(
        first.record.version_id, ConfigStatus::REJECTED, "bob", "nope", kT0 + 1));

    auto again = svc_->submit("server:\n  port: 8080\n", "carol", "", false);
    EXPECT_EQ(again.error_code, "") << "rejected versions must not shadow";
    EXPECT_NE(again.record.version_id, first.record.version_id);
}

TEST_F(ConfigServiceSubmitTest, ValidateOnlySkipsPersistence) {
    auto res = svc_->submit("server:\n  port: 8080\n", "alice", "", true);
    EXPECT_EQ(res.error_code, "");
    // The record is populated with derived fields but not persisted, so the
    // version_id is empty and the store stays empty.
    EXPECT_TRUE(res.record.version_id.empty());
    EXPECT_FALSE(res.record.content_sha256.empty());
    EXPECT_EQ(store_->listConfigVersions({}).size(), 0u);
}

TEST_F(ConfigServiceSubmitTest, AuditEntryEmittedOnSuccess) {
    auto res = svc_->submit("server:\n  port: 8080\n", "alice", "note", false);
    ASSERT_EQ(res.error_code, "");
    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));

    bool seen = false;
    for (const auto& e : audit_->entries()) {
        if (e.action == "config.submit" &&
            e.detail.find(res.record.version_id) != std::string::npos) {
            seen = true;
            break;
        }
    }
    EXPECT_TRUE(seen) << "expected config.submit audit event";
}

TEST_F(ConfigServiceSubmitTest, ContentSha256IsHex64) {
    auto res = svc_->submit("server:\n  port: 8080\n", "alice", "", false);
    ASSERT_EQ(res.error_code, "");
    EXPECT_EQ(res.record.content_sha256.size(), 64u);
    for (char c : res.record.content_sha256) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-hex char '" << c << "' in sha256";
    }
}

} // namespace
} // namespace aegisgate
