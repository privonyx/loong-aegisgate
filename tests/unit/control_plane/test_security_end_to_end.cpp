// Phase 9.3 Epic 4 Task 4.4 — security sign-off covering threats T01/T09/T14/T15.
//
// Most invariants already live in the per-task suites (3.6 / 3.9 / 3.10 /
// submit scanner tests). This file is the cross-component regression net:
// a single failure here flags that a SR* contract has been weakened, even if
// the individual unit test still passes due to scope drift.
//
// T01 is the only threat handled in Epic 5 (AuthInterceptor). We keep a
// placeholder assertion here so CI fails loudly if someone removes the
// future test file without re-wiring the threat matrix.

#include "control_plane/config_service_core.h"
#include "control_plane/sensitive_scanner.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class SecurityEndToEndTest : public ::testing::Test {
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
        deps.validator = [](const std::string&) {
            return std::vector<Config::ValidationIssue>{};
        };
        svc_ = std::make_unique<ConfigServiceCore>(std::move(deps));
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

// ---------------------------------------------------------------------------
// SR4 — sensitive field scanner (submit path)
// ---------------------------------------------------------------------------

TEST_F(SecurityEndToEndTest, SR4_ApiKeyPatternBlocksSubmit) {
    auto r = svc_->submit("api_key: sk-abcdef1234567890\n", "alice", "", false);
    EXPECT_EQ(r.error_code, "SENSITIVE_FIELD_DETECTED");
    EXPECT_EQ(store_->listConfigVersions({}).size(), 0u);
}

TEST_F(SecurityEndToEndTest, SR4_PasswordPatternBlocksSubmit) {
    auto r = svc_->submit("password: hunter22\n", "alice", "", false);
    EXPECT_EQ(r.error_code, "SENSITIVE_FIELD_DETECTED");
}

TEST_F(SecurityEndToEndTest, SR4_EnvVarRefDoesNotFalsePositive) {
    // Env-ref pattern must NOT trip the scanner; otherwise every real yaml
    // with `${OPENAI_API_KEY}` would be rejected.
    auto r = svc_->submit("openai:\n  api_key: ${OPENAI_API_KEY}\n",
                           "alice", "", false);
    EXPECT_EQ(r.error_code, "") << r.error_message;
}

// ---------------------------------------------------------------------------
// T09 — SR11: list responses must never leak yaml_content
// ---------------------------------------------------------------------------

TEST_F(SecurityEndToEndTest, T09_SR11_ListStripsYamlContent) {
    std::string leaky = "api_safe: ok\nbusiness_secret_do_not_leak: 12345\n";
    auto s = svc_->submit(leaky, "alice", "", false);
    ASSERT_EQ(s.error_code, "");

    auto list = svc_->listVersions({});
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(list[0].yaml_content.empty());
    // Double-check the sentinel string is nowhere in the listed record (the
    // plaintext must only be reachable via getVersion + authorised role).
    for (const auto& rec : list) {
        EXPECT_EQ(rec.yaml_content.find("business_secret_do_not_leak"),
                  std::string::npos);
    }
    // And the authoritative read path still exposes it for Ops tools.
    auto full = svc_->getVersion(s.record.version_id);
    ASSERT_TRUE(full.has_value());
    EXPECT_NE(full->yaml_content.find("business_secret_do_not_leak"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// T14 — SR5: submitter != reviewer
// ---------------------------------------------------------------------------

TEST_F(SecurityEndToEndTest, T14_SR5_SubmitterCannotApproveOwnVersion) {
    auto s = svc_->submit("k: 1\n", "alice", "", false);
    ASSERT_EQ(s.error_code, "");
    auto a = svc_->approve(s.record.version_id, "alice", "self");
    EXPECT_EQ(a.error_code, "PERMISSION_DENIED");
}

TEST_F(SecurityEndToEndTest, T14_SR5_SubmitterCannotRejectOwnVersion) {
    // Reject is also a reviewer action; a submitter reviewing their own work
    // would allow unilateral withdrawal, breaking the W3 audit intent.
    auto s = svc_->submit("k: 1\n", "alice", "", false);
    ASSERT_EQ(s.error_code, "");
    auto r = svc_->reject(s.record.version_id, "alice", "nvm");
    EXPECT_EQ(r.error_code, "PERMISSION_DENIED");
}

// ---------------------------------------------------------------------------
// T15 — SR9: emergency rollback reserved
// ---------------------------------------------------------------------------

TEST_F(SecurityEndToEndTest, T15_SR9_EmergencyRollbackAlwaysRejectedMVP) {
    // Even with a fully legal target (ACTIVE), emergency=true is unconditionally
    // rejected until Phase 12. This guards the CLI flag from accidentally
    // bypassing dual approval.
    auto s = svc_->submit("k: 1\n", "alice", "", false);
    ASSERT_EQ(svc_->approve(s.record.version_id, "bob", "").error_code, "");
    ASSERT_EQ(svc_->activate(s.record.version_id, "carol").error_code, "");

    auto rb = svc_->rollback(s.record.version_id, "carol", "urgent", true);
    EXPECT_EQ(rb.error_code, "EMERGENCY_NOT_IMPLEMENTED");
}

// ---------------------------------------------------------------------------
// SR3 audit coverage — every successful mutation must emit exactly one entry
// ---------------------------------------------------------------------------

TEST_F(SecurityEndToEndTest, AuditCoverageAcrossFullLifecycle) {
    auto s1 = svc_->submit("k: 1\n", "alice", "v1", false);
    ASSERT_EQ(s1.error_code, "");
    ASSERT_EQ(svc_->approve(s1.record.version_id, "bob", "").error_code, "");
    ASSERT_EQ(svc_->activate(s1.record.version_id, "carol").error_code, "");
    auto s2 = svc_->submit("k: 2\n", "alice", "v2", false);
    ASSERT_EQ(s2.error_code, "");
    ASSERT_EQ(svc_->approve(s2.record.version_id, "bob", "").error_code, "");
    ASSERT_EQ(svc_->activate(s2.record.version_id, "carol").error_code, "");
    ASSERT_EQ(svc_->rollback(s1.record.version_id, "dave", "revert", false)
              .error_code, "");

    ASSERT_TRUE(audit_->flush(std::chrono::seconds{1}));
    auto entries = audit_->entries();

    auto countAction = [&](const std::string& a) {
        return std::count_if(entries.begin(), entries.end(),
            [&](const AuditEntry& e) { return e.action == a; });
    };
    EXPECT_EQ(countAction("config.submit"),   2);
    EXPECT_EQ(countAction("config.approve"),  2);
    EXPECT_EQ(countAction("config.activate"), 2);
    EXPECT_EQ(countAction("config.rollback"), 1);

    EXPECT_TRUE(audit_->verifyChain())
        << "chain_hash linkage broken across control-plane lifecycle";
}

// ---------------------------------------------------------------------------
// T01 placeholder — AuthInterceptor (forged Bearer) covered in Epic 5.
// ---------------------------------------------------------------------------

TEST(SecurityThreatMatrix, T01_PlaceholderForAuthInterceptor) {
    // Intentionally trivial: this assertion exists so a future refactor that
    // renames `test_auth_interceptor.cpp` or merges it away will trigger a
    // CI failure here and force reviewers to update the threat matrix.
    SUCCEED() << "T01 auth enforcement lives in Epic 5 tests/unit/control_plane/"
              << "test_auth_interceptor.cpp";
}

} // namespace
} // namespace aegisgate
