// TASK-20260609-02 P1-9 — attribute A/B experiment assignment via a labeled
// metric and a client-facing response header.

#include "server/ab_attribution.h"

#include "core/context.h"
#include "observe/metrics.h"

#include <gtest/gtest.h>
#include <string>

using namespace aegisgate;

TEST(AbAttributionP1_9, IncrementsLabeledMetricAndSetsHeader) {
    auto& m = MetricsRegistry::instance();
    m.abExperimentAssignedTotal().reset();

    RequestContext ctx;
    ctx.ab_experiment = "exp-routing";
    ctx.ab_variant = "gpt-4o-mini";

    recordAbAttribution(ctx, m);

    const auto exposed = m.abExperimentAssignedTotal().expose();
    EXPECT_NE(exposed.find("experiment=\"exp-routing\""), std::string::npos)
        << exposed;
    EXPECT_NE(exposed.find("variant=\"gpt-4o-mini\""), std::string::npos)
        << exposed;
    ASSERT_TRUE(ctx.response_headers.count("X-AegisGate-AB-Variant"));
    EXPECT_EQ(ctx.response_headers.at("X-AegisGate-AB-Variant"), "gpt-4o-mini");
}

TEST(AbAttributionP1_9, NoopWhenNoExperimentAssigned) {
    auto& m = MetricsRegistry::instance();

    RequestContext ctx;  // no ab_experiment / ab_variant
    recordAbAttribution(ctx, m);

    EXPECT_EQ(ctx.response_headers.find("X-AegisGate-AB-Variant"),
              ctx.response_headers.end());
}
