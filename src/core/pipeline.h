#pragma once
#include "context.h"
#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include <functional>

namespace aegisgate {

enum class StageResult { Continue, ShortCircuit, Reject, Error };
enum class PipelineResult { Success, Rejected, Error, ShortCircuited };

class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    virtual StageResult process(RequestContext& ctx) = 0;
    virtual StageResult processChunk(RequestContext& /*ctx*/,
                                      std::string_view /*chunk*/) {
        return StageResult::Continue;
    }
    virtual std::string name() const = 0;
};

class Pipeline {
public:
    void addStage(std::unique_ptr<PipelineStage> stage);
    PipelineResult execute(RequestContext& ctx);
    std::pair<PipelineResult, std::string> executeChunk(
        RequestContext& ctx, std::string_view chunk);

    // P2-#3: the single source of truth for which stages are actually wired,
    // in execution order. Replaces the hand-maintained static descriptor that
    // drifted from the conditional assembly (missing RetrievalStage, listing
    // unconditionally-present stages that are in fact gated by config).
    std::vector<std::string> stageNames() const;
    size_t stageCount() const { return stages_.size(); }

private:
    std::vector<std::unique_ptr<PipelineStage>> stages_;
};

} // namespace aegisgate
