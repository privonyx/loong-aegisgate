#include <gtest/gtest.h>
#include "auth/prompt_template_service.h"
#include "storage/memory_persistent_store.h"
#include <set>

using namespace aegisgate;

class PromptTemplateServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        service_ = std::make_unique<PromptTemplateService>(&store_);
    }

    MemoryPersistentStore store_;
    std::unique_ptr<PromptTemplateService> service_;
};

TEST_F(PromptTemplateServiceTest, CreateAndGet) {
    PromptTemplate tpl;
    tpl.tenant_id = "t1";
    tpl.name = "greeting";
    tpl.content = "Hello, {{name}}!";
    tpl.weight = 100;

    auto id = service_->create(tpl);
    EXPECT_FALSE(id.empty());

    auto retrieved = service_->get(id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->name, "greeting");
    EXPECT_EQ(retrieved->content, "Hello, {{name}}!");
    EXPECT_EQ(retrieved->tenant_id, "t1");
    EXPECT_EQ(retrieved->weight, 100);
    EXPECT_TRUE(retrieved->is_active);
}

TEST_F(PromptTemplateServiceTest, ListByTenantAndName) {
    PromptTemplate t1;
    t1.tenant_id = "t1";
    t1.name = "greeting";
    t1.content = "Hi {{name}}";
    t1.weight = 70;
    service_->create(t1);

    PromptTemplate t2;
    t2.tenant_id = "t1";
    t2.name = "greeting";
    t2.content = "Hey {{name}}!";
    t2.weight = 30;
    service_->create(t2);

    PromptTemplate t3;
    t3.tenant_id = "t1";
    t3.name = "farewell";
    t3.content = "Bye {{name}}";
    t3.weight = 100;
    service_->create(t3);

    auto greetings = service_->listByName("t1", "greeting");
    EXPECT_EQ(greetings.size(), 2u);

    auto farewells = service_->listByName("t1", "farewell");
    EXPECT_EQ(farewells.size(), 1u);

    auto all = service_->listByTenant("t1");
    EXPECT_EQ(all.size(), 3u);
}

TEST_F(PromptTemplateServiceTest, Update) {
    PromptTemplate tpl;
    tpl.tenant_id = "t1";
    tpl.name = "greeting";
    tpl.content = "Hello";
    auto id = service_->create(tpl);

    PromptTemplate updated;
    updated.id = id;
    updated.tenant_id = "t1";
    updated.name = "greeting";
    updated.content = "Howdy, {{name}}!";
    updated.weight = 50;
    EXPECT_TRUE(service_->update(updated));

    auto got = service_->get(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->content, "Howdy, {{name}}!");
    EXPECT_EQ(got->weight, 50);
}

TEST_F(PromptTemplateServiceTest, Delete) {
    PromptTemplate tpl;
    tpl.tenant_id = "t1";
    tpl.name = "temporary";
    tpl.content = "temp content";
    auto id = service_->create(tpl);

    EXPECT_TRUE(service_->remove(id));
    EXPECT_FALSE(service_->get(id).has_value());
    EXPECT_FALSE(service_->remove(id));
}

TEST_F(PromptTemplateServiceTest, RenderTemplate) {
    auto result = service_->render(
        "Hello {{name}}, welcome to {{place}}!",
        {{"name", "Alice"}, {"place", "Wonderland"}});
    EXPECT_EQ(result, "Hello Alice, welcome to Wonderland!");
}

TEST_F(PromptTemplateServiceTest, RenderMissingVariable) {
    auto result = service_->render(
        "Hello {{name}}, your role is {{role}}",
        {{"name", "Bob"}});
    EXPECT_EQ(result, "Hello Bob, your role is {{role}}");
}

TEST_F(PromptTemplateServiceTest, RenderNoVariables) {
    auto result = service_->render("No variables here", {});
    EXPECT_EQ(result, "No variables here");
}

TEST_F(PromptTemplateServiceTest, SelectTemplateWeighted) {
    PromptTemplate t1;
    t1.tenant_id = "t1";
    t1.name = "msg";
    t1.content = "Version A";
    t1.weight = 100;
    service_->create(t1);

    PromptTemplate t2;
    t2.tenant_id = "t1";
    t2.name = "msg";
    t2.content = "Version B";
    t2.weight = 0;
    service_->create(t2);

    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        auto selected = service_->selectTemplate("t1", "msg");
        ASSERT_TRUE(selected.has_value());
        seen.insert(selected->content);
    }
    EXPECT_TRUE(seen.count("Version A") > 0);
}

TEST_F(PromptTemplateServiceTest, SelectTemplateEmpty) {
    auto selected = service_->selectTemplate("t1", "nonexistent");
    EXPECT_FALSE(selected.has_value());
}

TEST_F(PromptTemplateServiceTest, SelectTemplateOnlyActive) {
    PromptTemplate t1;
    t1.tenant_id = "t1";
    t1.name = "msg";
    t1.content = "Active";
    t1.weight = 100;
    t1.is_active = true;
    auto id1 = service_->create(t1);

    PromptTemplate t2;
    t2.tenant_id = "t1";
    t2.name = "msg";
    t2.content = "Inactive";
    t2.weight = 100;
    t2.is_active = false;
    service_->create(t2);

    for (int i = 0; i < 20; ++i) {
        auto selected = service_->selectTemplate("t1", "msg");
        ASSERT_TRUE(selected.has_value());
        EXPECT_EQ(selected->id, id1);
    }
}

TEST_F(PromptTemplateServiceTest, SelectDefaultActiveEmpty) {
    EXPECT_FALSE(service_->selectDefaultActive("t1").has_value());
}

TEST_F(PromptTemplateServiceTest, SelectDefaultActiveOnlyActive) {
    PromptTemplate active;
    active.tenant_id = "t1";
    active.name = "a";
    active.content = "Active default";
    active.weight = 100;
    active.is_active = true;
    auto id_active = service_->create(active);

    PromptTemplate inactive;
    inactive.tenant_id = "t1";
    inactive.name = "b";
    inactive.content = "Inactive";
    inactive.weight = 1000;
    inactive.is_active = false;
    service_->create(inactive);

    PromptTemplate other_tenant;
    other_tenant.tenant_id = "t2";
    other_tenant.name = "c";
    other_tenant.content = "Other tenant";
    other_tenant.weight = 1000;
    other_tenant.is_active = true;
    service_->create(other_tenant);

    for (int i = 0; i < 20; ++i) {
        auto selected = service_->selectDefaultActive("t1");
        ASSERT_TRUE(selected.has_value());
        EXPECT_EQ(selected->id, id_active);
        EXPECT_EQ(selected->content, "Active default");
    }
}

TEST_F(PromptTemplateServiceTest, SelectDefaultActiveReturnsOneOfActives) {
    PromptTemplate t1;
    t1.tenant_id = "t1";
    t1.name = "alpha";
    t1.content = "A";
    t1.weight = 50;
    auto id1 = service_->create(t1);

    PromptTemplate t2;
    t2.tenant_id = "t1";
    t2.name = "beta";
    t2.content = "B";
    t2.weight = 50;
    auto id2 = service_->create(t2);

    std::set<std::string> seen;
    for (int i = 0; i < 40; ++i) {
        auto selected = service_->selectDefaultActive("t1");
        ASSERT_TRUE(selected.has_value());
        seen.insert(selected->id);
    }
    EXPECT_TRUE(seen.count(id1) > 0 || seen.count(id2) > 0);
    for (const auto& id : seen) {
        EXPECT_TRUE(id == id1 || id == id2);
    }
}
