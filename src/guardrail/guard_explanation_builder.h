#pragma once

// Phase 11.1 TASK-20260523-01 — Epic R2.4 GuardExplanationBuilder.
//
// Translates the layer-specific result structs (InjectionResult / RuleResult /
// GuardResult / ExternalSafetyStage outputs) into the unified
// `GuardExplanation` payload defined in `include/aegisgate/guard_explanation.h`.
//
// The HTTP layer (v2 wiring) reads this on every blocked request and:
//   * stores it in GuardAdminController::recordExplanation for `GET
//     /admin/api/guard/explanation/{id}` lookups,
//   * emits it on the wire as the structured `why was this blocked` JSON.
//
// Keeping the builder in one place avoids each stage re-inventing
// trigger_layer string conventions and risk of drift.

#include "aegisgate/guard_explanation.h"
#include "guardrail/inbound/guard_classifier.h"
#include "guardrail/inbound/injection.h"
#include "guardrail/rule_engine.h"

#include <string>

namespace aegisgate::guard {

class GuardExplanationBuilder {
public:
    // L1: regex / keyword / heuristic detector. Maps InjectionResult.layer
    // ("L1-keyword", "L1-regex", "L2-heuristic") to the canonical
    // trigger_layer prefix expected by the admin UI ("L1" / "L2").
    static GuardExplanation fromInjection(const InjectionResult& r);

    // L2: declarative rule engine. trigger_layer is always "L2".
    static GuardExplanation fromRule(const RuleResult& r);

    // L3: ML classifier. Requires the model_version from the registry; pass
    // an empty string for the legacy classifier without ModelRegistry.
    static GuardExplanation fromMlClassifier(const GuardResult& r,
                                              const std::string& model_version);

    // L4: external safety provider. Carries the provider name + raw
    // verdict string for forward compatibility.
    static GuardExplanation fromExternalProvider(const std::string& provider,
                                                  const std::string& verdict,
                                                  float confidence);
};

}  // namespace aegisgate::guard
