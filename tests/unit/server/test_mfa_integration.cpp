#include "server/admin_controller.h"
#include "auth/auth_service.h"
#include "auth/totp_service.h"
#include "auth/encryption.h"
#include "guardrail/audit.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using json = nlohmann::json;

class MfaIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        ASSERT_TRUE(store_->initialize());

        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        auth_svc_ = std::make_unique<AuthService>(
            store_.get(), nullptr, gate_.get());
        audit_ = std::make_unique<AuditLogger>();
        ctrl_ = std::make_unique<AdminController>(
            store_.get(), auth_svc_.get(), audit_.get());

        Tenant t;
        t.id = "t1"; t.name = "Test Tenant"; t.status = "active";
        t.created_at = "2026-01-01T00:00:00Z"; t.updated_at = t.created_at;
        store_->insertTenant(t);

        User u;
        u.id = "u1"; u.tenant_id = "t1"; u.username = "testuser";
        u.display_name = "Test User"; u.role = Role::TenantAdmin;
        u.status = "active"; u.created_at = "2026-01-01T00:00:00Z";
        u.updated_at = u.created_at;
        store_->insertUser(u);

        ctx_.tenant_id = "t1";
        ctx_.user_id = "u1";
        ctx_.role = Role::TenantAdmin;
        ctx_.is_rbac_enabled = true;
        ctx_.session_id = "test-session-001";
        ctx_.auth_method = "sso";
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    void enableMfaForUser(const std::string& user_id) {
        auto mfa = store_->getMfaSecret(user_id);
        ASSERT_TRUE(mfa.has_value());
        mfa->enabled = true;
        store_->upsertMfaSecret(*mfa);
    }

    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<FeatureGate> gate_;
    std::unique_ptr<AuthService> auth_svc_;
    std::unique_ptr<AuditLogger> audit_;
    std::unique_ptr<AdminController> ctrl_;
    AuthContext ctx_;
};

// TASK-20260702-02 P2-2（SR-2）：恢复码熵 ≥ 80bit（≥10 字节 → ≥20 hex 字符）。
// 此前 RECOVERY_CODE_BYTES=4（仅 32bit）→ 在线爆破可行。
TEST(TotpRecoveryCodeTest, RecoveryCodesHaveSufficientEntropy) {
    auto codes = TotpService::generateRecoveryCodes(4);
    ASSERT_EQ(codes.size(), 4u);
    for (const auto& c : codes) {
        EXPECT_GE(c.size(), 20u)
            << "recovery code must carry >=80 bits of entropy (>=10 bytes hex)";
    }
}

TEST_F(MfaIntegrationTest, SetupMfa) {
    auto r = ctrl_->setupMfa(ctx_);
    EXPECT_EQ(r.status, 200);
    EXPECT_FALSE(r.is_error);
    EXPECT_TRUE(r.body.contains("secret"));
    EXPECT_TRUE(r.body.contains("qr_uri"));
    EXPECT_TRUE(r.body.contains("recovery_codes"));

    auto secret = r.body["secret"].get<std::string>();
    EXPECT_FALSE(secret.empty());

    auto qr_uri = r.body["qr_uri"].get<std::string>();
    EXPECT_EQ(qr_uri.substr(0, 15), "otpauth://totp/");

    auto codes = r.body["recovery_codes"];
    EXPECT_TRUE(codes.is_array());
    EXPECT_EQ(codes.size(), 8u);
    for (const auto& c : codes) {
        EXPECT_FALSE(c.get<std::string>().empty());
    }
}

TEST_F(MfaIntegrationTest, SetupMfaAlreadyEnabled) {
    auto r1 = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(r1.is_error);

    enableMfaForUser("u1");

    auto r2 = ctrl_->setupMfa(ctx_);
    EXPECT_TRUE(r2.is_error);
    EXPECT_EQ(r2.error_code, ErrorCode::InvalidRequest);
}

TEST_F(MfaIntegrationTest, SetupMfaCanResetupBeforeVerify) {
    auto r1 = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(r1.is_error);

    auto r2 = ctrl_->setupMfa(ctx_);
    EXPECT_FALSE(r2.is_error);
    EXPECT_EQ(r2.status, 200);
}

TEST_F(MfaIntegrationTest, VerifyMfaInvalidCode) {
    ctrl_->setupMfa(ctx_);

    auto r = ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(MfaIntegrationTest, VerifyMfaNoSetup) {
    auto r = ctrl_->verifyMfa(ctx_, {{"code", "123456"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaNotSetup);
}

TEST_F(MfaIntegrationTest, VerifyMfaMissingCode) {
    ctrl_->setupMfa(ctx_);

    auto r = ctrl_->verifyMfa(ctx_, json::object());
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MissingRequiredField);
}

TEST_F(MfaIntegrationTest, DisableMfaRequiresAdmin) {
    User viewer;
    viewer.id = "u2"; viewer.tenant_id = "t1"; viewer.username = "viewer1";
    viewer.display_name = "Viewer"; viewer.role = Role::Viewer;
    viewer.status = "active"; viewer.created_at = "2026-01-01T00:00:00Z";
    viewer.updated_at = viewer.created_at;
    store_->insertUser(viewer);

    AuthContext viewer_ctx;
    viewer_ctx.tenant_id = "t1";
    viewer_ctx.user_id = "u2";
    viewer_ctx.role = Role::Viewer;
    viewer_ctx.is_rbac_enabled = true;

    auto r = ctrl_->disableMfa(viewer_ctx, {{"code", "123456"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::InsufficientPermissions);
}

TEST_F(MfaIntegrationTest, DisableMfaNotSetup) {
    auto r = ctrl_->disableMfa(ctx_, {{"code", "123456"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaNotSetup);
}

TEST_F(MfaIntegrationTest, DisableMfaInvalidCode) {
    ctrl_->setupMfa(ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->disableMfa(ctx_, {{"code", "000000"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(MfaIntegrationTest, DisableMfaMissingCode) {
    ctrl_->setupMfa(ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->disableMfa(ctx_, json::object());
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MissingRequiredField);
}

// TASK-20260702-02 P2-2（SR-2）：失败锁定 —— 阻断在线爆破，且持久化在 store。
TEST_F(MfaIntegrationTest, VerifyMfaLocksOutAfterRepeatedFailures) {
    ctrl_->setupMfa(ctx_);
    enableMfaForUser("u1");
    ctrl_->setMfaPolicy(8, 3, 900);  // 连续 3 次失败即锁定

    for (int i = 0; i < 3; ++i) {
        auto r = ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
        EXPECT_TRUE(r.is_error);
        EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
    }
    // 已锁定：后续验证直接 429，不再进入 TOTP 校验。
    auto locked = ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    EXPECT_TRUE(locked.is_error);
    EXPECT_EQ(locked.error_code, ErrorCode::RateLimitExceeded);
}

TEST_F(MfaIntegrationTest, MfaLockoutPersistsInStore) {
    ctrl_->setupMfa(ctx_);
    enableMfaForUser("u1");
    ctrl_->setMfaPolicy(8, 2, 900);
    ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    // 锁定态写入了持久化 store（跨会话可见）。
    int64_t now = static_cast<int64_t>(::time(nullptr));
    EXPECT_GT(store_->getMfaLockedUntil("u1"), now);
}

TEST_F(MfaIntegrationTest, RecoveryMfaSuccessClearsFailureCounter) {
    auto setup = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(setup.is_error);
    auto first_code = setup.body["recovery_codes"][0].get<std::string>();
    enableMfaForUser("u1");
    ctrl_->setMfaPolicy(8, 3, 900);

    ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    auto ok = ctrl_->recoveryMfa(ctx_, {{"code", first_code}});
    ASSERT_FALSE(ok.is_error);
    // 成功清零：再失败 2 次仍不应锁定（否则计数未清）。
    ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    auto r = ctrl_->verifyMfa(ctx_, {{"code", "000000"}});
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(MfaIntegrationTest, RecoveryMfaValid) {
    auto setup = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(setup.is_error);
    auto recovery_codes = setup.body["recovery_codes"];
    auto first_code = recovery_codes[0].get<std::string>();

    enableMfaForUser("u1");

    auto r = ctrl_->recoveryMfa(ctx_, {{"code", first_code}});
    EXPECT_FALSE(r.is_error);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["verified"].get<bool>());
    EXPECT_EQ(r.body["remaining_codes"].get<int>(), 7);
}

TEST_F(MfaIntegrationTest, RecoveryMfaUsedCode) {
    auto setup = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(setup.is_error);
    auto first_code = setup.body["recovery_codes"][0].get<std::string>();

    enableMfaForUser("u1");

    auto r1 = ctrl_->recoveryMfa(ctx_, {{"code", first_code}});
    ASSERT_FALSE(r1.is_error);

    auto r2 = ctrl_->recoveryMfa(ctx_, {{"code", first_code}});
    EXPECT_TRUE(r2.is_error);
    EXPECT_EQ(r2.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(MfaIntegrationTest, RecoveryMfaInvalidCode) {
    ctrl_->setupMfa(ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->recoveryMfa(ctx_, {{"code", "INVALID-CODE-123"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(MfaIntegrationTest, RecoveryMfaNotSetup) {
    auto r = ctrl_->recoveryMfa(ctx_, {{"code", "ABCD1234"}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaNotSetup);
}

TEST_F(MfaIntegrationTest, RecoveryMfaMissingCode) {
    ctrl_->setupMfa(ctx_);
    enableMfaForUser("u1");

    auto r = ctrl_->recoveryMfa(ctx_, json::object());
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MissingRequiredField);
}

TEST_F(MfaIntegrationTest, MfaSetupStoresSecret) {
    auto setup = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(setup.is_error);

    auto mfa = store_->getMfaSecret("u1");
    ASSERT_TRUE(mfa.has_value());
    EXPECT_EQ(mfa->user_id, "u1");
    EXPECT_FALSE(mfa->secret_enc.empty());
    EXPECT_FALSE(mfa->enabled);
    EXPECT_FALSE(mfa->recovery_codes_hash.empty());
    EXPECT_EQ(mfa->recovery_codes_hash.size(), 8u);
    EXPECT_FALSE(mfa->created_at.empty());
}

TEST_F(MfaIntegrationTest, RecoveryMfaAllCodesUsed) {
    auto setup = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(setup.is_error);
    auto codes = setup.body["recovery_codes"];

    enableMfaForUser("u1");

    for (size_t i = 0; i < codes.size(); ++i) {
        auto r = ctrl_->recoveryMfa(ctx_, {{"code", codes[i].get<std::string>()}});
        ASSERT_FALSE(r.is_error) << "Failed at code index " << i;
        EXPECT_EQ(r.body["remaining_codes"].get<int>(),
                  static_cast<int>(codes.size() - i - 1));
    }

    auto r = ctrl_->recoveryMfa(ctx_, {{"code", codes[0].get<std::string>()}});
    EXPECT_TRUE(r.is_error);
    EXPECT_EQ(r.error_code, ErrorCode::MfaInvalidCode);
}

TEST_F(MfaIntegrationTest, SetupMfaQrUriContainsUsername) {
    auto r = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(r.is_error);

    auto qr_uri = r.body["qr_uri"].get<std::string>();
    EXPECT_NE(qr_uri.find("testuser"), std::string::npos);
    EXPECT_NE(qr_uri.find("AegisGate"), std::string::npos);
}

TEST_F(MfaIntegrationTest, SetupMfaAuditEntry) {
    auto before = audit_->entries().size();
    ctrl_->setupMfa(ctx_);

    auto entries = audit_->entries();
    ASSERT_GT(entries.size(), before);
    bool found = false;
    for (const auto& e : entries) {
        if (e.action == "admin.mfa_setup") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(MfaIntegrationTest, RecoveryMfaAuditEntry) {
    auto setup = ctrl_->setupMfa(ctx_);
    ASSERT_FALSE(setup.is_error);
    enableMfaForUser("u1");

    auto before = audit_->entries().size();
    auto code = setup.body["recovery_codes"][0].get<std::string>();
    ctrl_->recoveryMfa(ctx_, {{"code", code}});

    auto entries = audit_->entries();
    ASSERT_GT(entries.size(), before);
    bool found = false;
    for (const auto& e : entries) {
        if (e.action == "admin.mfa_recovery") { found = true; break; }
    }
    EXPECT_TRUE(found);
}
