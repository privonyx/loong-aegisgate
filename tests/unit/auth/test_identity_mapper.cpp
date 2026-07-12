#include "auth/identity_mapper.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using json = nlohmann::json;

static SsoProvider makeTestProvider(bool jit = true) {
    SsoProvider p;
    p.id = "sso-1";
    p.tenant_id = "tenant-1";
    p.name = "test-idp";
    p.issuer_url = "https://idp.example.com";
    p.claim_mapping_json = R"({"username":"preferred_username","email":"email","display_name":"name","groups":"groups"})";
    p.group_role_mapping_json = R"({"Admins":"super_admin","Developers":"developer"})";
    p.jit_provisioning = jit;
    p.default_role = "viewer";
    return p;
}

static json makeTestClaims() {
    return {
        {"sub", "ext-user-123"},
        {"preferred_username", "john.doe"},
        {"email", "john@example.com"},
        {"name", "John Doe"},
        {"groups", {"Developers", "Engineering"}}
    };
}

class IdentityMapperTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        store_->initialize();
        mapper_ = std::make_unique<IdentityMapper>(store_.get());
    }

    void TearDown() override {
        mapper_.reset();
        store_.reset();
    }

    User insertTestUser(const std::string& id, const std::string& status = "active") {
        User u;
        u.id = id;
        u.tenant_id = "tenant-1";
        u.username = "john@example.com";
        u.display_name = "John Doe";
        u.role = Role::Developer;
        u.status = status;
        u.created_at = "2025-01-01T00:00:00Z";
        u.updated_at = u.created_at;
        store_->insertUser(u);
        return u;
    }

    IdentityMapping insertTestMapping(const std::string& id,
                                       const std::string& user_id,
                                       const std::string& sub = "ext-user-123") {
        IdentityMapping m;
        m.id = id;
        m.tenant_id = "tenant-1";
        m.external_subject = sub;
        m.external_issuer = "https://idp.example.com";
        m.user_id = user_id;
        m.email = "john@example.com";
        m.last_login_at = "2025-01-01T00:00:00Z";
        m.created_at = m.last_login_at;
        store_->insertIdentityMapping(m);
        return m;
    }

    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<IdentityMapper> mapper_;
};

TEST_F(IdentityMapperTest, MapIdentity_ExistingMapping) {
    auto user = insertTestUser("user-1");
    insertTestMapping("map-1", "user-1");

    auto result = mapper_->mapIdentity(makeTestClaims(), makeTestProvider());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->newly_created);
    EXPECT_EQ(result->user.id, "user-1");
    EXPECT_EQ(result->mapping.external_subject, "ext-user-123");
}

TEST_F(IdentityMapperTest, MapIdentity_JitCreateNewUser) {
    auto provider = makeTestProvider(true);
    auto result = mapper_->mapIdentity(makeTestClaims(), provider);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->newly_created);
    EXPECT_EQ(result->user.tenant_id, "tenant-1");
    EXPECT_EQ(result->user.username, "john@example.com");
    EXPECT_EQ(result->user.display_name, "John Doe");
    EXPECT_EQ(result->user.role, Role::Developer);
    EXPECT_EQ(result->user.status, "active");
    EXPECT_EQ(result->mapping.external_subject, "ext-user-123");
    EXPECT_EQ(result->mapping.external_issuer, "https://idp.example.com");
    EXPECT_FALSE(result->user.id.empty());
    EXPECT_FALSE(result->mapping.id.empty());
}

TEST_F(IdentityMapperTest, MapIdentity_JitDisabled) {
    auto provider = makeTestProvider(false);
    auto result = mapper_->mapIdentity(makeTestClaims(), provider);
    EXPECT_FALSE(result.has_value());
}

TEST_F(IdentityMapperTest, MapIdentity_ExistingButInactiveUser) {
    insertTestUser("user-inactive", "inactive");
    insertTestMapping("map-2", "user-inactive");

    auto result = mapper_->mapIdentity(makeTestClaims(), makeTestProvider());
    EXPECT_FALSE(result.has_value());
}

TEST_F(IdentityMapperTest, MapIdentity_UpdatesLastLogin) {
    insertTestUser("user-login");
    auto original = insertTestMapping("map-login", "user-login");

    auto result = mapper_->mapIdentity(makeTestClaims(), makeTestProvider());
    ASSERT_TRUE(result.has_value());

    auto updated = store_->getIdentityMapping("ext-user-123", "https://idp.example.com");
    ASSERT_TRUE(updated.has_value());
    EXPECT_GT(updated->last_login_at, original.last_login_at);
}

TEST_F(IdentityMapperTest, ApplyGroupRoleMapping_MatchesSuperAdmin) {
    json grm = {{"Admins", "super_admin"}, {"Developers", "developer"}};
    std::vector<std::string> groups = {"Admins"};

    Role role = mapper_->applyGroupRoleMapping(groups, grm, "viewer");
    EXPECT_EQ(role, Role::SuperAdmin);
}

TEST_F(IdentityMapperTest, ApplyGroupRoleMapping_HighestRoleWins) {
    json grm = {{"Admins", "super_admin"}, {"Developers", "developer"}, {"Ops", "tenant_admin"}};
    std::vector<std::string> groups = {"Developers", "Admins", "Ops"};

    Role role = mapper_->applyGroupRoleMapping(groups, grm, "viewer");
    EXPECT_EQ(role, Role::SuperAdmin);
}

TEST_F(IdentityMapperTest, ApplyGroupRoleMapping_NoMatch) {
    json grm = {{"Admins", "super_admin"}};
    std::vector<std::string> groups = {"Engineering", "Sales"};

    Role role = mapper_->applyGroupRoleMapping(groups, grm, "developer");
    EXPECT_EQ(role, Role::Developer);
}

TEST_F(IdentityMapperTest, MapIdentity_WithGroupRoleMapping) {
    auto provider = makeTestProvider(true);
    auto claims = makeTestClaims();
    claims["groups"] = {"Admins", "Engineering"};

    auto result = mapper_->mapIdentity(claims, provider);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->newly_created);
    EXPECT_EQ(result->user.role, Role::SuperAdmin);
}

TEST_F(IdentityMapperTest, ExtractClaim_CustomMapping) {
    auto provider = makeTestProvider(true);
    provider.claim_mapping_json = R"({"username":"upn","email":"mail","display_name":"displayName","groups":"memberOf"})";

    json claims = {
        {"sub", "custom-sub-456"},
        {"upn", "jane.doe"},
        {"mail", "jane@corp.com"},
        {"displayName", "Jane Doe"},
        {"memberOf", {"Developers"}}
    };

    auto result = mapper_->mapIdentity(claims, provider);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->newly_created);
    EXPECT_EQ(result->user.username, "jane@corp.com");
    EXPECT_EQ(result->user.display_name, "Jane Doe");
    EXPECT_EQ(result->user.role, Role::Developer);
}
