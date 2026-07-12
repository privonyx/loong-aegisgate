// aegisctl estimate — pre-flight savings estimator (MVP-2 / TASK-20260526-01)
//
// Public entry: aegisgate::cli::runEstimate(args) -> int
//
// Pricing source: config/models.yaml (cost_per_1k_input + cost_per_1k_output).
// Scenarios: conservative / balanced (default) / aggressive — see spec §2 D4.
// Output: human-readable table (default) or --output json.
//
// Security posture (spec §5):
//   SR1 zero network side-effect (no HTTP client touched in this module)
//   SR2 reads only the pricing yaml path passed in (default config/models.yaml)
//   SR3 output never carries tenant_id / api_key / prompt / secret material
//   SR4 5 key texts hard-coded and shared with spec/plan/test/docs
//
// Formula derivation: spec §3.1.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate::cli {

struct ModelPricing {
    double price_per_1k_input = 0.0;
    double price_per_1k_output = 0.0;
    std::string provider;
};

struct ScenarioPreset {
    double cache_hit_rate = 0.0;
    double routing_savings_rate = 0.0;
    double compression_rate = 0.0;
};

struct EstimateInput {
    std::string model;
    long long monthly_calls = 0;
    long long avg_input_tokens = 0;
    long long avg_output_tokens = 0;
    std::string scenario = "balanced";   // conservative / balanced / aggressive / custom
    double cache_hit_rate = 0.30;
    double routing_savings_rate = 0.20;
    double compression_rate = 0.10;
    std::string target_model;             // empty -> auto-pick cheapest sibling
    std::string models_config = "config/models.yaml";
    std::string output = "table";         // table / json
    bool explain = false;
    bool scenario_explicit = false;       // --scenario provided
    bool cache_explicit = false;          // --cache-hit-rate provided
    bool routing_explicit = false;
    bool compression_explicit = false;
};

struct EstimateResult {
    // Echo of resolved input (after preset + flag overrides)
    EstimateInput resolved_input;
    ModelPricing source_pricing;
    std::optional<ModelPricing> target_pricing;
    std::string target_model_used;        // either user-supplied or auto

    // Baseline (no AegisGate)
    double baseline_input_cost = 0.0;
    double baseline_output_cost = 0.0;
    double baseline_monthly_total = 0.0;

    // Savings breakdown (aligned with SavingsAggregator three-class semantics)
    double cache_saved = 0.0;
    double routing_saved = 0.0;
    double compression_saved = 0.0;

    double total_saved = 0.0;
    double saved_percentage = 0.0;
    double with_aegisgate_monthly = 0.0;
    double annual_saved = 0.0;

    // Notes / warnings collected during compute
    std::vector<std::string> notes;
};

// Loads pricing from a YAML file matching config/models.yaml schema.
// Throws std::runtime_error on parse failure (caller maps to exit 2).
std::unordered_map<std::string, ModelPricing> loadPricing(const std::string& yaml_path);

// Returns one of the three named presets. Unknown name -> "balanced".
ScenarioPreset scenarioPreset(const std::string& name);

// Auto-picks cheapest model within the same provider (excluding source).
// Cost metric: (P_in + 3*P_out)/4 weighted average (spec §3.2).
// Returns std::nullopt if no cheaper sibling exists in the same provider.
std::optional<std::string> selectTargetModel(
    const std::string& source,
    const std::unordered_map<std::string, ModelPricing>& pricing);

// Pure compute. Does not perform IO. Caller supplies pricing map.
EstimateResult computeEstimate(
    const EstimateInput& input,
    const std::unordered_map<std::string, ModelPricing>& pricing);

// CLI entry point dispatched from src/cli/aegisctl.cpp.
// Parses args, loads pricing, runs computeEstimate, renders output.
// Exit codes:
//   0 success / 1 user input error / 2 config file error
int runEstimate(const std::vector<std::string>& args);

// Renderers exposed for unit tests.
std::string renderTable(const EstimateResult& r);
std::string renderJson(const EstimateResult& r);

}  // namespace aegisgate::cli
