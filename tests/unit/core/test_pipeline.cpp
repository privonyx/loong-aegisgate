#include <gtest/gtest.h>
#include "core/pipeline.h"
#include "core/context.h"

using namespace aegisgate;

class PassStage : public PipelineStage {
public:
    StageResult process(RequestContext& /*ctx*/) override {
        return StageResult::Continue;
    }
    std::string name() const override { return "pass"; }
};

class RejectStage : public PipelineStage {
public:
    StageResult process(RequestContext& /*ctx*/) override {
        return StageResult::Reject;
    }
    std::string name() const override { return "reject"; }
};

class ShortCircuitStage : public PipelineStage {
public:
    StageResult process(RequestContext& /*ctx*/) override {
        return StageResult::ShortCircuit;
    }
    std::string name() const override { return "shortcircuit"; }
};

class ErrorStage : public PipelineStage {
public:
    StageResult process(RequestContext& /*ctx*/) override {
        return StageResult::Error;
    }
    std::string name() const override { return "error"; }
};

class CountingStage : public PipelineStage {
public:
    int count = 0;
    StageResult process(RequestContext& /*ctx*/) override {
        ++count;
        return StageResult::Continue;
    }
    std::string name() const override { return "counting"; }
};

TEST(PipelineTest, EmptyPipelineReturnsSuccess) {
    Pipeline pipeline;
    RequestContext ctx;
    auto result = pipeline.execute(ctx);
    EXPECT_EQ(result, PipelineResult::Success);
}

TEST(PipelineTest, AllStagesContinue) {
    Pipeline pipeline;
    pipeline.addStage(std::make_unique<PassStage>());
    pipeline.addStage(std::make_unique<PassStage>());
    pipeline.addStage(std::make_unique<PassStage>());
    RequestContext ctx;
    auto result = pipeline.execute(ctx);
    EXPECT_EQ(result, PipelineResult::Success);
}

TEST(PipelineTest, RejectStopsExecution) {
    Pipeline pipeline;
    auto* counter = new CountingStage();
    pipeline.addStage(std::make_unique<PassStage>());
    pipeline.addStage(std::make_unique<RejectStage>());
    pipeline.addStage(std::unique_ptr<CountingStage>(counter));
    RequestContext ctx;
    auto result = pipeline.execute(ctx);
    EXPECT_EQ(result, PipelineResult::Rejected);
    EXPECT_EQ(counter->count, 0);
}

TEST(PipelineTest, ShortCircuitStopsExecution) {
    Pipeline pipeline;
    auto* counter = new CountingStage();
    pipeline.addStage(std::make_unique<ShortCircuitStage>());
    pipeline.addStage(std::unique_ptr<CountingStage>(counter));
    RequestContext ctx;
    auto result = pipeline.execute(ctx);
    EXPECT_EQ(result, PipelineResult::ShortCircuited);
    EXPECT_EQ(counter->count, 0);
}

TEST(PipelineTest, ErrorStopsExecution) {
    Pipeline pipeline;
    pipeline.addStage(std::make_unique<ErrorStage>());
    pipeline.addStage(std::make_unique<PassStage>());
    RequestContext ctx;
    auto result = pipeline.execute(ctx);
    EXPECT_EQ(result, PipelineResult::Error);
}
