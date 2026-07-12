#include "auth/scim_service.h"
#include "core/crypto.h"
#include "storage/sqlite_persistent_store.h"
#include "guardrail/audit.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using json = nlohmann::json;

class ScimServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        store_->initialize();
        svc_ = std::make_unique<ScimService>(store_.get());

        Tenant t;
        t.id = "tenant-1";
        t.name = "Test Tenant";
        t.created_at = "2026-01-01T00:00:00Z";
        t.updated_at = t.created_at;
        store_->insertTenant(t);

        token_raw_ = "scim-bearer-token-secret";
        ScimToken st;
        st.id = "scim-tok-1";
        st.tenant_id = "tenant-1";
        st.token_hash = crypto::sha256(token_raw_);
        st.description = "test token";
        st.created_at = "2026-01-01T00:00:00Z";
        st.expires_at = "2099-12-31T23:59:59Z";
        store_->insertScimToken(st);
    }

    void TearDown() override {
        svc_.reset();
        store_.reset();
    }

    json makeUserResource(const std::string& userName,
                          const std::string& displayName = "",
                          const std::string& email = "") {
        json r = {{"userName", userName}};
        if (!displayName.empty()) r["displayName"] = displayName;
        if (!email.empty()) {
            r["emails"] = {{{"value", email}, {"primary", true}}};
        }
        return r;
    }

    std::string createAndGetId(const std::string& userName) {
        auto res = svc_->createUser("tenant-1", makeUserResource(userName, userName + " Name"));
        return res.value("id", "");
    }

    std::string token_raw_;
    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<ScimService> svc_;
};

// --- Token Authentication ---

TEST_F(ScimServiceTest, AuthenticateValidToken) {
    auto tid = svc_->authenticateToken(token_raw_);
    ASSERT_TRUE(tid.has_value());
    EXPECT_EQ(*tid, "tenant-1");
}

TEST_F(ScimServiceTest, AuthenticateInvalidToken) {
    auto tid = svc_->authenticateToken("wrong-token");
    EXPECT_FALSE(tid.has_value());
}

TEST_F(ScimServiceTest, AuthenticateExpiredToken) {
    ScimToken expired;
    expired.id = "scim-tok-expired";
    expired.tenant_id = "tenant-1";
    expired.token_hash = crypto::sha256("expired-token");
    expired.description = "expired";
    expired.created_at = "2020-01-01T00:00:00Z";
    expired.expires_at = "2020-01-02T00:00:00Z";
    store_->insertScimToken(expired);

    auto tid = svc_->authenticateToken("expired-token");
    EXPECT_FALSE(tid.has_value());
}

// --- User CRUD ---

TEST_F(ScimServiceTest, CreateUser) {
    auto res = svc_->createUser("tenant-1",
        makeUserResource("alice", "Alice Smith", "alice@example.com"));

    EXPECT_TRUE(res.contains("schemas"));
    EXPECT_EQ(res["schemas"][0], "urn:ietf:params:scim:schemas:core:2.0:User");
    EXPECT_FALSE(res["id"].get<std::string>().empty());
    EXPECT_EQ(res["userName"], "alice");
    EXPECT_EQ(res["displayName"], "Alice Smith");
    EXPECT_TRUE(res["active"].get<bool>());
    EXPECT_TRUE(res.contains("meta"));
    EXPECT_EQ(res["meta"]["resourceType"], "User");
    EXPECT_FALSE(res["meta"]["created"].get<std::string>().empty());
}

TEST_F(ScimServiceTest, CreateUserDuplicate) {
    svc_->createUser("tenant-1", makeUserResource("dup-user"));
    auto res = svc_->createUser("tenant-1", makeUserResource("dup-user"));

    EXPECT_EQ(res["status"], "409");
    EXPECT_TRUE(res.contains("detail"));
}

TEST_F(ScimServiceTest, GetUser) {
    auto created = svc_->createUser("tenant-1",
        makeUserResource("bob", "Bob Jones"));
    std::string id = created["id"];

    auto res = svc_->getUser("tenant-1", id);
    EXPECT_EQ(res["id"], id);
    EXPECT_EQ(res["userName"], "bob");
    EXPECT_EQ(res["displayName"], "Bob Jones");
}

TEST_F(ScimServiceTest, GetUserNotFound) {
    auto res = svc_->getUser("tenant-1", "nonexistent-id");
    EXPECT_EQ(res["status"], "404");
}

TEST_F(ScimServiceTest, UpdateUser) {
    auto created = svc_->createUser("tenant-1",
        makeUserResource("carol", "Carol Old"));
    std::string id = created["id"];

    auto res = svc_->updateUser("tenant-1", id, {{"displayName", "Carol New"}});
    EXPECT_EQ(res["displayName"], "Carol New");
    EXPECT_EQ(res["userName"], "carol");
}

TEST_F(ScimServiceTest, UpdateUserActive) {
    auto created = svc_->createUser("tenant-1", makeUserResource("deactivate-me"));
    std::string id = created["id"];

    auto res = svc_->updateUser("tenant-1", id, {{"active", false}});
    EXPECT_FALSE(res["active"].get<bool>());

    auto fetched = svc_->getUser("tenant-1", id);
    EXPECT_FALSE(fetched["active"].get<bool>());
}

TEST_F(ScimServiceTest, DeleteUser) {
    auto created = svc_->createUser("tenant-1", makeUserResource("delete-me"));
    std::string id = created["id"];

    auto res = svc_->deleteUser("tenant-1", id);
    EXPECT_TRUE(res.is_object());

    auto fetched = svc_->getUser("tenant-1", id);
    EXPECT_FALSE(fetched["active"].get<bool>());
}

TEST_F(ScimServiceTest, ListUsers) {
    createAndGetId("list-user-1");
    createAndGetId("list-user-2");
    createAndGetId("list-user-3");

    auto res = svc_->listUsers("tenant-1");
    EXPECT_EQ(res["schemas"][0], "urn:ietf:params:scim:api:messages:2.0:ListResponse");
    EXPECT_GE(res["totalResults"].get<int>(), 3);
    EXPECT_TRUE(res["Resources"].is_array());
    EXPECT_GE(res["Resources"].size(), 3u);
}

TEST_F(ScimServiceTest, ListUsersWithFilter) {
    createAndGetId("filter-user-1");
    createAndGetId("filter-user-2");
    createAndGetId("filter-user-3");

    auto res = svc_->listUsers("tenant-1", "userName eq \"filter-user-2\"");
    EXPECT_EQ(res["totalResults"].get<int>(), 1);
    EXPECT_EQ(res["Resources"][0]["userName"], "filter-user-2");
}

TEST_F(ScimServiceTest, ListUsersPagination) {
    for (int i = 1; i <= 5; ++i) {
        createAndGetId("page-user-" + std::to_string(i));
    }

    auto res = svc_->listUsers("tenant-1", "userName eq \"\"", 1, 100);
    int total_unfiltered = 0;
    {
        auto all = svc_->listUsers("tenant-1");
        total_unfiltered = all["totalResults"].get<int>();
    }

    auto paged = svc_->listUsers("tenant-1", "", 2, 2);
    EXPECT_EQ(paged["startIndex"], 2);
    EXPECT_EQ(paged["itemsPerPage"], 2);
    EXPECT_EQ(paged["Resources"].size(), 2u);
    EXPECT_EQ(paged["totalResults"].get<int>(), total_unfiltered);
}

// --- Group CRUD ---

TEST_F(ScimServiceTest, CreateGroup) {
    json resource = {{"displayName", "Developers"}};
    auto res = svc_->createGroup("tenant-1", resource);

    EXPECT_EQ(res["schemas"][0], "urn:ietf:params:scim:schemas:core:2.0:Group");
    EXPECT_FALSE(res["id"].get<std::string>().empty());
    EXPECT_EQ(res["displayName"], "Developers");
    EXPECT_TRUE(res.contains("meta"));
    EXPECT_EQ(res["meta"]["resourceType"], "Group");
}

TEST_F(ScimServiceTest, GetGroup) {
    auto created = svc_->createGroup("tenant-1", {{"displayName", "Ops"}});
    std::string id = created["id"];

    auto res = svc_->getGroup("tenant-1", id);
    EXPECT_EQ(res["id"], id);
    EXPECT_EQ(res["displayName"], "Ops");
}

TEST_F(ScimServiceTest, GetGroupNotFound) {
    auto res = svc_->getGroup("tenant-1", "no-such-group");
    EXPECT_EQ(res["status"], "404");
}

TEST_F(ScimServiceTest, UpdateGroup) {
    auto created = svc_->createGroup("tenant-1", {{"displayName", "OldName"}});
    std::string id = created["id"];

    auto res = svc_->updateGroup("tenant-1", id, {{"displayName", "NewName"}});
    EXPECT_EQ(res["displayName"], "NewName");

    auto fetched = svc_->getGroup("tenant-1", id);
    EXPECT_EQ(fetched["displayName"], "NewName");
}

TEST_F(ScimServiceTest, DeleteGroup) {
    auto created = svc_->createGroup("tenant-1", {{"displayName", "ToDelete"}});
    std::string id = created["id"];

    auto res = svc_->deleteGroup("tenant-1", id);
    EXPECT_TRUE(res.is_object());

    auto fetched = svc_->getGroup("tenant-1", id);
    EXPECT_EQ(fetched["status"], "404");
}

TEST_F(ScimServiceTest, ListGroups) {
    svc_->createGroup("tenant-1", {{"displayName", "G1"}});
    svc_->createGroup("tenant-1", {{"displayName", "G2"}});

    auto res = svc_->listGroups("tenant-1");
    EXPECT_EQ(res["schemas"][0], "urn:ietf:params:scim:api:messages:2.0:ListResponse");
    EXPECT_EQ(res["totalResults"].get<int>(), 2);
    EXPECT_EQ(res["Resources"].size(), 2u);
}

// --- SCIM Response Format ---

TEST_F(ScimServiceTest, ScimErrorFormat) {
    auto err = ScimService::scimError(404, "Not found");
    EXPECT_EQ(err["schemas"][0], "urn:ietf:params:scim:api:messages:2.0:Error");
    EXPECT_EQ(err["status"], "404");
    EXPECT_EQ(err["detail"], "Not found");
}

TEST_F(ScimServiceTest, UserResponseHasRequiredFields) {
    auto created = svc_->createUser("tenant-1",
        makeUserResource("reqfields", "Required Fields User", "req@example.com"));

    EXPECT_TRUE(created.contains("schemas"));
    EXPECT_TRUE(created.contains("id"));
    EXPECT_TRUE(created.contains("userName"));
    EXPECT_TRUE(created.contains("displayName"));
    EXPECT_TRUE(created.contains("active"));
    EXPECT_TRUE(created.contains("meta"));
    EXPECT_EQ(created["meta"]["resourceType"], "User");
    EXPECT_TRUE(created["meta"].contains("created"));
    EXPECT_TRUE(created["meta"].contains("lastModified"));
}

// --- SCIM 审计（TASK-20260604-01 P0-D / SR-1）---
//
// 注入 AuditLogger，验证 6 个写操作（user/group CRUD）每个都落审计；
// 读操作（get/list）不落审计。

class ScimServiceAuditTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        store_->initialize();
        audit_ = std::make_unique<AuditLogger>();
        svc_ = std::make_unique<ScimService>(store_.get(), audit_.get());

        Tenant t; t.id = "tenant-1"; t.name = "T";
        t.created_at = "2026-01-01T00:00:00Z"; t.updated_at = t.created_at;
        store_->insertTenant(t);
    }
    void TearDown() override {
        if (audit_) audit_->shutdown();
    }

    int countAction(const std::string& action) {
        int n = 0;
        for (const auto& e : audit_->entries())
            if (e.action == action) ++n;
        return n;
    }

    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<AuditLogger> audit_;
    std::unique_ptr<ScimService> svc_;
};

TEST_F(ScimServiceAuditTest, UserWritesAreAudited) {
    auto created = svc_->createUser("tenant-1", {{"userName", "alice"}});
    auto id = created.value("id", "");
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(countAction("scim.create_user"), 1);

    svc_->updateUser("tenant-1", id, {{"displayName", "Alice Updated"}});
    EXPECT_EQ(countAction("scim.update_user"), 1);

    svc_->deleteUser("tenant-1", id);
    EXPECT_EQ(countAction("scim.delete_user"), 1);
}

TEST_F(ScimServiceAuditTest, GroupWritesAreAudited) {
    auto created = svc_->createGroup("tenant-1", {{"displayName", "Devs"}});
    auto id = created.value("id", "");
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(countAction("scim.create_group"), 1);

    svc_->updateGroup("tenant-1", id, {{"displayName", "Devs2"}});
    EXPECT_EQ(countAction("scim.update_group"), 1);

    svc_->deleteGroup("tenant-1", id);
    EXPECT_EQ(countAction("scim.delete_group"), 1);
}

TEST_F(ScimServiceAuditTest, ReadsAreNotAudited) {
    auto created = svc_->createUser("tenant-1", {{"userName", "bob"}});
    auto id = created.value("id", "");
    auto before = audit_->entries().size();
    svc_->getUser("tenant-1", id);
    svc_->listUsers("tenant-1");
    EXPECT_EQ(audit_->entries().size(), before);
}

TEST_F(ScimServiceAuditTest, FailedWritesAreNotAudited) {
    // userName 缺失 → 400，不应落审计。
    svc_->createUser("tenant-1", json::object());
    EXPECT_EQ(countAction("scim.create_user"), 0);
    // 不存在的用户更新 → 404，不应落审计。
    svc_->updateUser("tenant-1", "nonexistent", {{"displayName", "x"}});
    EXPECT_EQ(countAction("scim.update_user"), 0);
}

TEST_F(ScimServiceAuditTest, NullAuditLoggerDoesNotCrash) {
    // 向后兼容：单参 ctor（audit=nullptr）写操作不崩溃。
    ScimService no_audit(store_.get());
    auto r = no_audit.createUser("tenant-1", {{"userName", "carol"}});
    EXPECT_FALSE(r.value("id", "").empty());
}
