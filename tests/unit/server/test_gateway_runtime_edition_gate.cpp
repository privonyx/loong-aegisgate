// TASK-20260711-01 / REV20260707-I13 Epic 1 — Edition gate predicate.
//
// aegisgate::isAdvancedRoutingEnabled(const FeatureGate*) decides whether
// the assembler / admin controller may enable AdvancedRouting-tier
// features (MLRouter / ABTestRouter / GeoRouter wrapping / GET
// /admin/api/savings/summary). Pure function; nullable-safe.
//
// Truth table (mirrors pipeline_assembler.cpp:525 `if (feature_gate &&
// feature_gate->isEnabled(X))` idiom):
//
//   gate ptr   edition                gate.isEnabled(AR)  expected
//   ---------  ---------------------  ------------------  --------
//   nullptr    (n/a)                  (n/a)               false
//   Community  license-inert          false               false
//   Enterprise createUnlocked         true                true
//   Enterprise (license absent)       false               false  <- Enterprise
//                                                                   locked
//   Community  createUnlocked(Ent)    true                true   <- test-only
//                                                                   helper does
//                                                                   NOT downgrade
//                                                                   edition flag,
//                                                                   just enables
//                                                                   all features

#include <gtest/gtest.h>
#include <memory>

#include "core/config.h"
#include "core/feature_gate.h"
#include "server/gateway_runtime.h"

using namespace aegisgate;

// -----------------------------------------------------------------------
// SR-6 · Nullable-safe: nullptr gate falls closed (Community-equivalent).
// -----------------------------------------------------------------------
TEST(AdvancedRoutingGatePredicate, NullGateFallsClosed) {
    EXPECT_FALSE(isAdvancedRoutingEnabled(nullptr));
}

// -----------------------------------------------------------------------
// SR-5 (Enterprise non-regression side · createUnlocked path):
// FeatureGate::createUnlocked(Edition::Enterprise) enables ALL features
// including AdvancedRouting. This is the standard Enterprise-unlocked
// test fixture pattern per TASK-20260708-03 P1 ("Enterprise gated
// component unit tests must use FeatureGate::createUnlocked").
// -----------------------------------------------------------------------
TEST(AdvancedRoutingGatePredicate, EnterpriseUnlockedGateReturnsTrue) {
    auto gate = FeatureGate::createUnlocked(Edition::Enterprise);
    EXPECT_TRUE(isAdvancedRoutingEnabled(&gate));
}

// -----------------------------------------------------------------------
// SR-1/2/3 (Community closes gate):
// FeatureGate(Edition::Community) explicitly closes all enterprise
// features. AdvancedRouting is enterprise-tier → predicate returns false
// → router assembly branches must fall back to CostAware / skip wrap.
// -----------------------------------------------------------------------
TEST(AdvancedRoutingGatePredicate, CommunityEditionGateReturnsFalse) {
    FeatureGate gate(Edition::Community);
    EXPECT_FALSE(isAdvancedRoutingEnabled(&gate));
}

// -----------------------------------------------------------------------
// SR-1/2/3 (Enterprise without license also closes gate):
// FeatureGate(Edition::Enterprise) constructor alone does NOT enable
// features — a valid license (or createUnlocked test helper) must
// activate them. So Enterprise edition + no license = feature disabled.
// This matches the production behaviour and keeps the predicate honest
// about the license contract.
// -----------------------------------------------------------------------
TEST(AdvancedRoutingGatePredicate, EnterpriseGateWithoutLicenseReturnsFalse) {
    FeatureGate gate(Edition::Enterprise);
    // No license loaded, no createUnlocked → all Feature::* remain false.
    EXPECT_FALSE(isAdvancedRoutingEnabled(&gate));
}

// -----------------------------------------------------------------------
// SR-1..6 · Determinism / idempotency: calling the predicate twice on
// the same gate returns the same value.
// -----------------------------------------------------------------------
TEST(AdvancedRoutingGatePredicate, ResultIsDeterministicAcrossCalls) {
    auto enterprise = FeatureGate::createUnlocked(Edition::Enterprise);
    EXPECT_TRUE(isAdvancedRoutingEnabled(&enterprise));
    EXPECT_TRUE(isAdvancedRoutingEnabled(&enterprise));

    FeatureGate community(Edition::Community);
    EXPECT_FALSE(isAdvancedRoutingEnabled(&community));
    EXPECT_FALSE(isAdvancedRoutingEnabled(&community));
}

// -----------------------------------------------------------------------
// Meta · The predicate is a strict `gate && gate->isEnabled(...)`
// composition: it does not smuggle any other license state (customer /
// expires / signature method) into the decision. Documenting via a
// black-box test.
// -----------------------------------------------------------------------
TEST(AdvancedRoutingGatePredicate, StrictAndComposition) {
    // Community edition may not be silently upgraded even if a rogue
    // 'other' feature is enabled — predicate only inspects
    // Feature::AdvancedRouting.
    FeatureGate community(Edition::Community);
    // (Cannot force-enable a single feature without going through
    // createUnlocked, but the negative side of the AND is what matters:
    // gate == !null, feature isEnabled == false → false.)
    EXPECT_FALSE(isAdvancedRoutingEnabled(&community));
}
