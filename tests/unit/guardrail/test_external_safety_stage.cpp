#include <gtest/gtest.h>
#include "guardrail/inbound/external_safety_stage.h"
#include "guardrail/inbound/external_safety_api.h"
#include "guardrail/audit.h"
#include <vector>

using namespace aegisgate;

namespace {

class MockSafetyApi : public ExternalSafetyApi {
public:
    MockSafetyApi(const std::string& name, bool flagged, bool success = true)
        : name_(name), flagged_(flagged), success_(success) {}

    SafetyResult check(const std::string& /*text*/) override {
        check_count_++;
        SafetyResult r;
        r.provider = name_;
        r.flagged = flagged_;
        r.success = success_;
        if (!success_) r.error = "mock error";
        if (flagged_) {
            SafetyCategory cat;
            cat.name = "test_category";
            cat.flagged = true;
            cat.score = 0.95;
            r.categories.push_back(cat);
        }
        return r;
    }

    std::string providerName() const override { return name_; }
    bool isConfigured() const override { return true; }
    int checkCount() const { return check_count_; }

private:
    std::string name_;
    bool flagged_;
    bool success_;
    int check_count_ = 0;
};

RequestContext makeTestCtx(const std::string& text) {
    RequestContext ctx;
    ctx.request_id = "test-req";
    ctx.chat_request.messages = {{"user", text}};
    return ctx;
}

// Records the text handed to the provider so tests can assert which view
// (raw vs normalized) the stage scanned.
class CapturingSafetyApi : public ExternalSafetyApi {
public:
    SafetyResult check(const std::string& text) override {
        last_text = text;
        SafetyResult r;
        r.provider = "cap";
        r.flagged = false;
        r.success = true;
        return r;
    }
    std::string providerName() const override { return "cap"; }
    bool isConfigured() const override { return true; }
    std::string last_text;
};

} // namespace

class ExternalSafetyStageTest : public ::testing::Test {};

TEST_F(ExternalSafetyStageTest, NoProvidersPassThrough) {
    ExternalSafetyStage stage;
    auto ctx = makeTestCtx("hello");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyStageTest, EmptyTextPassThrough) {
    ExternalSafetyStageConfig cfg;
    ExternalSafetyStage stage(cfg);
    auto mock = std::make_unique<MockSafetyApi>("test", true);
    stage.addProvider(std::move(mock));

    RequestContext ctx;
    ctx.request_id = "test";
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyStageTest, SingleProviderClean) {
    ExternalSafetyStage stage;
    stage.addProvider(std::make_unique<MockSafetyApi>("clean", false));
    auto ctx = makeTestCtx("safe content");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

// C3 (REV20260702-C3): extractTextFromRequest must feed the normalized view to
// providers (via ctx.scanText) so obfuscated raw text can't reach the model
// while safety providers scanned only the raw bytes.
TEST_F(ExternalSafetyStageTest, ScansNormalizedText) {
    ExternalSafetyStage stage;
    auto cap = std::make_unique<CapturingSafetyApi>();
    auto* cap_ptr = cap.get();
    stage.addProvider(std::move(cap));

    RequestContext ctx;
    ctx.request_id = "test-norm";
    ctx.chat_request.messages = {{"user", "ｈｅｌｌｏ"}};
    ctx.normalized_messages = {"hello"};
    ctx.input_preprocessed = true;

    stage.process(ctx);
    EXPECT_EQ(cap_ptr->last_text, "hello");
}

// P1-C: a synchronous L4 block is a security-relevant decision and MUST be
// audited (previously only shadow-mode scans wrote audit entries).
TEST_F(ExternalSafetyStageTest, SyncBlockWritesAudit) {
    AuditLogger audit;
    std::vector<AuditEntry> captured;
    audit.setSink([&](const AuditEntry& e) { captured.push_back(e); });

    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Any;
    ExternalSafetyStage stage(cfg);
    stage.setAuditLogger(&audit);
    stage.addProvider(std::make_unique<MockSafetyApi>("flagger", true));

    auto ctx = makeTestCtx("blocked content");
    ctx.tenant_id = "tenant-x";
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(captured.back().stage_name, "ExternalSafetyStage");
    EXPECT_EQ(captured.back().action, "blocked");
    EXPECT_EQ(captured.back().request_id, "test-req");
}

TEST_F(ExternalSafetyStageTest, SingleProviderFlagged) {
    ExternalSafetyStage stage;
    stage.addProvider(std::make_unique<MockSafetyApi>("flagged", true));
    auto ctx = makeTestCtx("bad content");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyStageTest, ModeAnyOneFlagged) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Any;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("clean", false));
    stage.addProvider(std::make_unique<MockSafetyApi>("flagged", true));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyStageTest, ModeAllNotAllFlagged) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::All;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("clean", false));
    stage.addProvider(std::make_unique<MockSafetyApi>("flagged", true));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyStageTest, ModeAllBothFlagged) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::All;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", true));
    stage.addProvider(std::make_unique<MockSafetyApi>("p2", true));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyStageTest, ModeMajorityBelow) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Majority;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", true));
    stage.addProvider(std::make_unique<MockSafetyApi>("p2", false));
    stage.addProvider(std::make_unique<MockSafetyApi>("p3", false));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyStageTest, ModeMajorityAbove) {
    ExternalSafetyStageConfig cfg;
    cfg.mode = ExternalSafetyMode::Majority;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", true));
    stage.addProvider(std::make_unique<MockSafetyApi>("p2", true));
    stage.addProvider(std::make_unique<MockSafetyApi>("p3", false));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyStageTest, FailOpenOnError) {
    ExternalSafetyStageConfig cfg;
    cfg.fail_policy = ExternalSafetyFailPolicy::Open;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("err", false, false));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyStageTest, FailClosedOnError) {
    ExternalSafetyStageConfig cfg;
    cfg.fail_policy = ExternalSafetyFailPolicy::Closed;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("err", false, false));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyStageTest, FailClosedWithMixedErrors) {
    ExternalSafetyStageConfig cfg;
    cfg.fail_policy = ExternalSafetyFailPolicy::Closed;
    cfg.mode = ExternalSafetyMode::Any;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("err", false, false));
    stage.addProvider(std::make_unique<MockSafetyApi>("clean", false, true));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Reject);
}

TEST_F(ExternalSafetyStageTest, ParallelExecution) {
    ExternalSafetyStageConfig cfg;
    cfg.async_parallel = true;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", false));
    stage.addProvider(std::make_unique<MockSafetyApi>("p2", false));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    EXPECT_EQ(stage.lastResults().size(), 2);
}

TEST_F(ExternalSafetyStageTest, SequentialExecution) {
    ExternalSafetyStageConfig cfg;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", false));
    stage.addProvider(std::make_unique<MockSafetyApi>("p2", false));
    auto ctx = makeTestCtx("test");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    EXPECT_EQ(stage.lastResults().size(), 2);
}

TEST_F(ExternalSafetyStageTest, StageName) {
    ExternalSafetyStage stage;
    EXPECT_EQ(stage.name(), "ExternalSafetyStage");
}

TEST_F(ExternalSafetyStageTest, ProviderCount) {
    ExternalSafetyStage stage;
    EXPECT_EQ(stage.providerCount(), 0);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", false));
    EXPECT_EQ(stage.providerCount(), 1);
    stage.addProvider(std::make_unique<MockSafetyApi>("p2", false));
    EXPECT_EQ(stage.providerCount(), 2);
}

TEST_F(ExternalSafetyStageTest, MultipleMessagesExtracted) {
    ExternalSafetyStageConfig cfg;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    std::string captured;
    auto mock = std::make_unique<MockSafetyApi>("test", false);
    stage.addProvider(std::move(mock));

    RequestContext ctx;
    ctx.request_id = "test";
    ctx.chat_request.messages = {
        {"system", "You are a helpful assistant"},
        {"user", "Hello world"}
    };
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}

TEST_F(ExternalSafetyStageTest, ToolMessagesSkipped) {
    ExternalSafetyStageConfig cfg;
    cfg.async_parallel = false;
    ExternalSafetyStage stage(cfg);

    stage.addProvider(std::make_unique<MockSafetyApi>("test", false));

    RequestContext ctx;
    ctx.request_id = "test";
    Message tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "tool response";
    ctx.chat_request.messages = {
        {"user", "Hello"},
        tool_msg
    };
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
}
