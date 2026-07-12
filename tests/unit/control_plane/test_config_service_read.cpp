// Phase 9.3 Epic 3 Task 3.10 — read-side orchestration.
//
// listVersions(query) delegates to PersistentStore and MUST strip
// yaml_content before returning (SR11 — avoid secret exfil via plain list).
// getVersion(id) and getActive() retain yaml_content for authorised callers.
// diffVersions(from, to) composes getVersion + DiffEngine.

#include "control_plane/config_service_core.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <memory>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class ConfigServiceReadTest : public ::testing::Test {
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

    std::string submitAndActivate(const std::string& yaml) {
        auto sub = svc_->submit(yaml, "alice", "", false);
        EXPECT_EQ(sub.error_code, "");
        EXPECT_EQ(svc_->approve(sub.record.version_id, "bob", "").error_code, "");
        EXPECT_EQ(svc_->activate(sub.record.version_id, "carol").error_code, "");
        return sub.record.version_id;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

TEST_F(ConfigServiceReadTest, ListOmitsYamlContent) {
    submitAndActivate("v: 1\n");
    submitAndActivate("v: 2\n");

    auto list = svc_->listVersions({});
    ASSERT_GE(list.size(), 2u);
    for (const auto& r : list) {
        EXPECT_TRUE(r.yaml_content.empty())
            << "SR11 violation: list returned yaml_content for "
            << r.version_id;
        // Size/sha256 are still expected so clients can render list UIs.
        EXPECT_GT(r.size_bytes, 0);
        EXPECT_FALSE(r.content_sha256.empty());
    }
}

TEST_F(ConfigServiceReadTest, GetVersionRetainsYamlContent) {
    auto id = submitAndActivate("v: 1\n");
    auto rec = svc_->getVersion(id);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->yaml_content, "v: 1\n");
}

TEST_F(ConfigServiceReadTest, GetVersionNotFoundReturnsNullopt) {
    auto rec = svc_->getVersion("01HXXXDOESNOTEXIST00000000");
    EXPECT_FALSE(rec.has_value());
}

TEST_F(ConfigServiceReadTest, GetActiveReturnsCurrentActive) {
    auto id = submitAndActivate("v: 1\n");
    auto active = svc_->getActive();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, id);
    EXPECT_EQ(active->yaml_content, "v: 1\n");
}

TEST_F(ConfigServiceReadTest, GetActiveWhenNoneReturnsNullopt) {
    auto active = svc_->getActive();
    EXPECT_FALSE(active.has_value());
}

TEST_F(ConfigServiceReadTest, DiffVersionsShowsChangedLines) {
    auto v1 = submitAndActivate("foo: 1\nbar: 2\n");
    auto v2 = submitAndActivate("foo: 1\nbar: 3\n");

    auto diff = svc_->diffVersions(v1, v2);
    EXPECT_TRUE(diff.error_code.empty());
    EXPECT_NE(diff.unified_diff.find("-bar: 2"), std::string::npos);
    EXPECT_NE(diff.unified_diff.find("+bar: 3"), std::string::npos);
}

TEST_F(ConfigServiceReadTest, DiffVersionsIdenticalIsEmpty) {
    auto v1 = submitAndActivate("foo: 1\n");
    auto diff = svc_->diffVersions(v1, v1);
    EXPECT_TRUE(diff.error_code.empty());
    EXPECT_TRUE(diff.unified_diff.empty());
}

TEST_F(ConfigServiceReadTest, DiffVersionsMissingReturnsNotFound) {
    auto v1 = submitAndActivate("foo: 1\n");
    auto diff = svc_->diffVersions(v1, "01HXXXDOESNOTEXIST00000000");
    EXPECT_EQ(diff.error_code, "NOT_FOUND");
}

TEST_F(ConfigServiceReadTest, ListHonoursStatusFilter) {
    auto v1 = submitAndActivate("v: 1\n");
    auto v2 = submitAndActivate("v: 2\n");
    (void)v2;

    ConfigVersionQuery q;
    q.statuses = {ConfigStatus::SUPERSEDED};
    auto list = svc_->listVersions(q);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].version_id, v1);
    EXPECT_TRUE(list[0].yaml_content.empty());
}

} // namespace
} // namespace aegisgate
