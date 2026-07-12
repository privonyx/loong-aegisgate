#include <gtest/gtest.h>
#include "guardrail/inbound/prompt_template_stage.h"
#include "auth/prompt_template_service.h"
#include "storage/memory_persistent_store.h"

using namespace aegisgate;

class PromptTemplateStageTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.initialize();
        service_ = std::make_unique<PromptTemplateService>(&store_);
        stage_.setService(service_.get());
    }

    std::string createTpl(const std::string& tenant, const std::string& name,
                          const std::string& content, int weight = 100,
                          bool active = true) {
        PromptTemplate t;
        t.tenant_id = tenant;
        t.name = name;
        t.content = content;
        t.weight = weight;
        t.is_active = active;
        return service_->create(t);
    }

    MemoryPersistentStore store_;
    std::unique_ptr<PromptTemplateService> service_;
    PromptTemplateStage stage_;
};

TEST_F(PromptTemplateStageTest, InjectsWhenNoSystem) {
    createTpl("t1", "greet", "You are helpful.");
    RequestContext ctx;
    ctx.tenant_id = "t1";
    ctx.chat_request.messages = {{"user", "Hi"}};

    EXPECT_EQ(stage_.process(ctx), StageResult::Continue);
    ASSERT_EQ(ctx.chat_request.messages.size(), 2u);
    EXPECT_EQ(ctx.chat_request.messages[0].role, "system");
    EXPECT_EQ(ctx.chat_request.messages[0].content, "You are helpful.");
    EXPECT_EQ(ctx.chat_request.messages[1].role, "user");
    EXPECT_EQ(ctx.response_headers[PromptTemplateStage::kAppliedHeader], "greet");
}

TEST_F(PromptTemplateStageTest, SkipsWhenSystemExists) {
    createTpl("t1", "greet", "Injected");
    RequestContext ctx;
    ctx.tenant_id = "t1";
    ctx.chat_request.messages = {
        {"system", "Client system"},
        {"user", "Hi"},
    };

    EXPECT_EQ(stage_.process(ctx), StageResult::Continue);
    ASSERT_EQ(ctx.chat_request.messages.size(), 2u);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "Client system");
    EXPECT_TRUE(ctx.response_headers.find(PromptTemplateStage::kAppliedHeader) ==
                ctx.response_headers.end());
}

TEST_F(PromptTemplateStageTest, TenantIsolation) {
    createTpl("t2", "other", "Other tenant template");
    RequestContext ctx;
    ctx.tenant_id = "t1";
    ctx.chat_request.messages = {{"user", "Hi"}};

    EXPECT_EQ(stage_.process(ctx), StageResult::Continue);
    ASSERT_EQ(ctx.chat_request.messages.size(), 1u);
    EXPECT_EQ(ctx.chat_request.messages[0].role, "user");
}

TEST_F(PromptTemplateStageTest, NoopWhenNoActive) {
    createTpl("t1", "off", "Inactive", 100, false);
    RequestContext ctx;
    ctx.tenant_id = "t1";
    ctx.chat_request.messages = {{"user", "Hi"}};

    EXPECT_EQ(stage_.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages.size(), 1u);
}

TEST_F(PromptTemplateStageTest, HeaderOverridesDefault) {
    createTpl("t1", "defaultish", "Default content", 100);
    createTpl("t1", "special", "Special content", 1);

    RequestContext ctx;
    ctx.tenant_id = "t1";
    ctx.request_headers[PromptTemplateStage::kTemplateHeader] = "special";
    ctx.chat_request.messages = {{"user", "Hi"}};

    EXPECT_EQ(stage_.process(ctx), StageResult::Continue);
    ASSERT_EQ(ctx.chat_request.messages.size(), 2u);
    EXPECT_EQ(ctx.chat_request.messages[0].content, "Special content");
    EXPECT_EQ(ctx.response_headers[PromptTemplateStage::kAppliedHeader], "special");
}

TEST_F(PromptTemplateStageTest, NullServiceNoop) {
    PromptTemplateStage bare;
    RequestContext ctx;
    ctx.tenant_id = "t1";
    ctx.chat_request.messages = {{"user", "Hi"}};
    EXPECT_EQ(bare.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages.size(), 1u);
}

TEST_F(PromptTemplateStageTest, EmptyTenantNoop) {
    createTpl("t1", "greet", "You are helpful.");
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Hi"}};
    EXPECT_EQ(stage_.process(ctx), StageResult::Continue);
    EXPECT_EQ(ctx.chat_request.messages.size(), 1u);
}
