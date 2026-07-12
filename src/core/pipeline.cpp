#include "pipeline.h"
#include "observe/tracing.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

void Pipeline::addStage(std::unique_ptr<PipelineStage> stage) {
    stages_.push_back(std::move(stage));
}

std::vector<std::string> Pipeline::stageNames() const {
    std::vector<std::string> names;
    names.reserve(stages_.size());
    for (const auto& stage : stages_) {
        names.push_back(stage->name());
    }
    return names;
}

std::pair<PipelineResult, std::string> Pipeline::executeChunk(
    RequestContext& ctx, std::string_view chunk) {
    std::string current(chunk);
    for (auto& stage : stages_) {
        ctx.chunk_output.clear();
        auto result = stage->processChunk(ctx, current);
        switch (result) {
            case StageResult::Continue:
                if (!ctx.chunk_output.empty()) {
                    current = ctx.chunk_output;
                }
                break;
            case StageResult::Reject:
#ifdef AEGISGATE_ENABLE_OTEL
                if (ctx.root_span) {
                    ctx.root_span->AddEvent("chunk_rejected", {
                        {"stage", stage->name()}});
                }
#endif
                spdlog::warn("Chunk rejected at stage: {}", stage->name());
                return {PipelineResult::Rejected, ""};
            case StageResult::ShortCircuit:
                spdlog::info("Chunk short-circuited at stage: {}", stage->name());
                return {PipelineResult::ShortCircuited, current};
            case StageResult::Error:
#ifdef AEGISGATE_ENABLE_OTEL
                if (ctx.root_span) {
                    ctx.root_span->AddEvent("chunk_error", {
                        {"stage", stage->name()}});
                }
#endif
                spdlog::error("Chunk error at stage: {}", stage->name());
                return {PipelineResult::Error, ""};
        }
    }
    return {PipelineResult::Success, current};
}

PipelineResult Pipeline::execute(RequestContext& ctx) {
    for (auto& stage : stages_) {
        ScopedSpan stage_span;
#ifdef AEGISGATE_ENABLE_OTEL
        if (Tracing::instance().isEnabled() && ctx.root_span)
            stage_span = ScopedSpan(
                "aegisgate.stage." + stage->name(), ctx.trace_ctx);
#endif
        auto result = stage->process(ctx);
        stage_span.setAttribute("aegisgate.stage.result",
            std::string(
                result == StageResult::Continue ? "continue" :
                result == StageResult::ShortCircuit ? "short_circuit" :
                result == StageResult::Reject ? "reject" : "error"));
        if (result == StageResult::Reject || result == StageResult::Error)
            stage_span.setError("stage failed");
        switch (result) {
            case StageResult::Continue:
                break;
            case StageResult::ShortCircuit:
                spdlog::info("Pipeline short-circuited at stage: {}", stage->name());
                return PipelineResult::ShortCircuited;
            case StageResult::Reject:
                spdlog::warn("Pipeline rejected at stage: {}", stage->name());
                ctx.reject_stage = stage->name();
                return PipelineResult::Rejected;
            case StageResult::Error:
                spdlog::error("Pipeline error at stage: {}", stage->name());
                return PipelineResult::Error;
        }
    }
    return PipelineResult::Success;
}

} // namespace aegisgate
