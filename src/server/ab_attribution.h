#pragma once
#include "core/context.h"
#include "observe/metrics.h"

namespace aegisgate {

// P1-9: attribute an A/B experiment assignment. ABTestRouter writes
// ctx.ab_experiment / ctx.ab_variant when a variant is selected, but nothing
// ever consumed them — so experiments produced no metric, no header and no
// way to analyse variant performance.
//
// This increments a labeled counter (experiment + variant) and exposes the
// assigned variant to the client via the shared response-header channel
// (X-AegisGate-AB-Variant). It is a no-op when no experiment was assigned, so
// it is safe to call unconditionally on the routing hot path.
//
// Header-only so the attribution logic is unit-testable without driving the
// full GatewayRuntime.
inline void recordAbAttribution(RequestContext& ctx, MetricsRegistry& metrics) {
    if (ctx.ab_experiment.empty() || ctx.ab_variant.empty()) {
        return;
    }
    LabelSet labels;
    labels.labels.emplace_back("experiment", ctx.ab_experiment);
    labels.labels.emplace_back("variant", ctx.ab_variant);
    metrics.abExperimentAssignedTotal().inc(labels);
    ctx.response_headers["X-AegisGate-AB-Variant"] = ctx.ab_variant;
}

} // namespace aegisgate
