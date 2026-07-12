#include <gtest/gtest.h>
#include "core/pipeline.h"
#include "core/context.h"

using namespace aegisgate;

class TrackingChunkStage : public PipelineStage {
public:
    StageResult process(RequestContext&) override {
        return StageResult::Continue;
    }
    StageResult processChunk(RequestContext& ctx,
                             std::string_view chunk) override {
        (void)ctx;
        last_chunk_ = std::string(chunk);
        call_count_++;
        return StageResult::Continue;
    }
    std::string name() const override { return "TrackingChunk"; }
    std::string last_chunk_;
    int call_count_ = 0;
};

class ModifyChunkStage : public PipelineStage {
public:
    StageResult process(RequestContext&) override {
        return StageResult::Continue;
    }
    StageResult processChunk(RequestContext& ctx,
                             std::string_view chunk) override {
        ctx.chunk_output = std::string(chunk) + "_modified";
        return StageResult::Continue;
    }
    std::string name() const override { return "ModifyChunk"; }
};

class RejectChunkStage : public PipelineStage {
public:
    StageResult process(RequestContext&) override {
        return StageResult::Continue;
    }
    StageResult processChunk(RequestContext&, std::string_view) override {
        return StageResult::Reject;
    }
    std::string name() const override { return "RejectChunk"; }
};

TEST(PipelineStreamTest, ExecuteChunkCallsAllStages) {
    Pipeline pipeline;
    auto s1 = std::make_unique<TrackingChunkStage>();
    auto s2 = std::make_unique<TrackingChunkStage>();
    auto* s1_ptr = s1.get();
    auto* s2_ptr = s2.get();
    pipeline.addStage(std::move(s1));
    pipeline.addStage(std::move(s2));

    RequestContext ctx;
    auto [result, output] = pipeline.executeChunk(ctx, "hello");
    EXPECT_EQ(result, PipelineResult::Success);
    EXPECT_EQ(s1_ptr->call_count_, 1);
    EXPECT_EQ(s2_ptr->call_count_, 1);
    EXPECT_EQ(output, "hello");
}

TEST(PipelineStreamTest, ExecuteChunkPropagatesModifiedContent) {
    Pipeline pipeline;
    pipeline.addStage(std::make_unique<ModifyChunkStage>());
    auto tracker = std::make_unique<TrackingChunkStage>();
    auto* tracker_ptr = tracker.get();
    pipeline.addStage(std::move(tracker));

    RequestContext ctx;
    auto [result, output] = pipeline.executeChunk(ctx, "test");
    EXPECT_EQ(result, PipelineResult::Success);
    EXPECT_EQ(tracker_ptr->last_chunk_, "test_modified");
    EXPECT_EQ(output, "test_modified");
}

TEST(PipelineStreamTest, ExecuteChunkStopsOnReject) {
    Pipeline pipeline;
    pipeline.addStage(std::make_unique<RejectChunkStage>());
    auto tracker = std::make_unique<TrackingChunkStage>();
    auto* tracker_ptr = tracker.get();
    pipeline.addStage(std::move(tracker));

    RequestContext ctx;
    auto [result, output] = pipeline.executeChunk(ctx, "blocked");
    EXPECT_EQ(result, PipelineResult::Rejected);
    EXPECT_EQ(tracker_ptr->call_count_, 0);
    EXPECT_TRUE(output.empty());
}

TEST(PipelineStreamTest, EmptyPipelineExecuteChunkReturnsOriginal) {
    Pipeline pipeline;
    RequestContext ctx;
    auto [result, output] = pipeline.executeChunk(ctx, "passthrough");
    EXPECT_EQ(result, PipelineResult::Success);
    EXPECT_EQ(output, "passthrough");
}

TEST(PipelineStreamTest, RequestContextStreamingFields) {
    RequestContext ctx;
    EXPECT_FALSE(ctx.is_streaming);
    EXPECT_TRUE(ctx.accumulated_response.empty());
    EXPECT_TRUE(ctx.stream_model.empty());
    EXPECT_TRUE(ctx.chunk_output.empty());
    EXPECT_DOUBLE_EQ(ctx.hallucination_score, 1.0);
    EXPECT_FALSE(ctx.hallucination_flagged);
}
