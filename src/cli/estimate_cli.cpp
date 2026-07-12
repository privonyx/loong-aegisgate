// aegisctl estimate — implementation (MVP-2 / TASK-20260526-01).
//
// SR1: 0 network calls; this TU does not include or instantiate any HTTP client.
// SR2: only the path passed via --models-config (default config/models.yaml) is read.
// SR3: rendered output contains user inputs + computed numbers only.
// SR4: 5 key texts are defined as named constants and emitted verbatim.
//
// Formula: spec §3.1; preset values: spec §2 D4.

#include "cli/estimate_cli.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace aegisgate::cli {

namespace {

// SR4 key texts (spec §4.2). Shared verbatim with smoke tests + docs.
constexpr const char* kTitle = "AegisGate Savings Estimate";
constexpr const char* kMonthlyLabel = "Estimated monthly savings:";
constexpr const char* kAnnualLabel = "Estimated annual savings:";
constexpr const char* kVerifyCta = "Want to verify?";
constexpr const char* kQuickstartLink = "docs/quickstart.md";

double formatCost(double v) {
    // Round half-away-from-zero to two decimals to keep table/json consistent.
    return std::round(v * 100.0) / 100.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pricing loader
// ---------------------------------------------------------------------------
std::unordered_map<std::string, ModelPricing> loadPricing(
    const std::string& yaml_path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(yaml_path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse models config: " +
                                 std::string(e.what()));
    }

    std::unordered_map<std::string, ModelPricing> out;
    if (!root || !root["providers"] || !root["providers"].IsSequence()) {
        return out;
    }

    for (const auto& prov : root["providers"]) {
        if (!prov || !prov.IsMap()) continue;
        std::string provider_name;
        if (prov["name"]) provider_name = prov["name"].as<std::string>();

        if (!prov["models"] || !prov["models"].IsSequence()) continue;
        for (const auto& m : prov["models"]) {
            if (!m || !m.IsMap() || !m["id"]) continue;
            ModelPricing p;
            p.provider = provider_name;
            if (m["cost_per_1k_input"])
                p.price_per_1k_input = m["cost_per_1k_input"].as<double>();
            if (m["cost_per_1k_output"])
                p.price_per_1k_output = m["cost_per_1k_output"].as<double>();
            out[m["id"].as<std::string>()] = p;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Preset table (spec §2 D4)
// ---------------------------------------------------------------------------
ScenarioPreset scenarioPreset(const std::string& name) {
    ScenarioPreset p;
    if (name == "conservative") {
        p.cache_hit_rate = 0.15;
        p.routing_savings_rate = 0.10;
        p.compression_rate = 0.05;
    } else if (name == "aggressive") {
        p.cache_hit_rate = 0.50;
        p.routing_savings_rate = 0.40;
        p.compression_rate = 0.15;
    } else {
        // balanced (default) / unknown name fallback
        p.cache_hit_rate = 0.30;
        p.routing_savings_rate = 0.20;
        p.compression_rate = 0.10;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Auto-pick target_model: cheapest sibling in same provider (spec §3.2)
// ---------------------------------------------------------------------------
std::optional<std::string> selectTargetModel(
    const std::string& source,
    const std::unordered_map<std::string, ModelPricing>& pricing) {
    auto src_it = pricing.find(source);
    if (src_it == pricing.end()) return std::nullopt;
    const std::string& provider = src_it->second.provider;
    if (provider.empty()) return std::nullopt;

    auto weighted = [](const ModelPricing& p) {
        return (p.price_per_1k_input + 3.0 * p.price_per_1k_output) / 4.0;
    };
    const double src_w = weighted(src_it->second);

    std::optional<std::string> best;
    double best_w = src_w;  // strictly cheaper than source
    for (const auto& [id, p] : pricing) {
        if (id == source) continue;
        if (p.provider != provider) continue;
        const double w = weighted(p);
        if (w < best_w) {
            best_w = w;
            best = id;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Compute (pure)
// ---------------------------------------------------------------------------
EstimateResult computeEstimate(
    const EstimateInput& input,
    const std::unordered_map<std::string, ModelPricing>& pricing) {
    EstimateResult r;
    r.resolved_input = input;

    auto src_it = pricing.find(input.model);
    if (src_it == pricing.end()) {
        throw std::invalid_argument("Model '" + input.model +
                                    "' not found in pricing config");
    }
    r.source_pricing = src_it->second;

    // Resolve target_model (manual or auto)
    if (!input.target_model.empty()) {
        auto tit = pricing.find(input.target_model);
        if (tit == pricing.end()) {
            throw std::invalid_argument("Target model '" + input.target_model +
                                        "' not found in pricing config");
        }
        r.target_pricing = tit->second;
        r.target_model_used = input.target_model;
    } else {
        auto picked = selectTargetModel(input.model, pricing);
        if (picked) {
            r.target_pricing = pricing.at(*picked);
            r.target_model_used = *picked;
        } else {
            r.notes.push_back(
                "no cheaper alternative in same provider; "
                "routing_saved is $0.00");
        }
    }

    if (input.monthly_calls <= 0) {
        r.notes.push_back("monthly_calls=0 -> no estimate possible");
        return r;  // all zero
    }

    const double in_tok = static_cast<double>(input.avg_input_tokens);
    const double out_tok = static_cast<double>(input.avg_output_tokens);
    const double n = static_cast<double>(input.monthly_calls);

    r.baseline_input_cost = n * in_tok * r.source_pricing.price_per_1k_input / 1000.0;
    r.baseline_output_cost = n * out_tok * r.source_pricing.price_per_1k_output / 1000.0;
    r.baseline_monthly_total = r.baseline_input_cost + r.baseline_output_cost;

    const double cache_rate = std::clamp(input.cache_hit_rate, 0.0, 1.0);
    r.cache_saved = r.baseline_monthly_total * cache_rate;

    const double non_cached_share = 1.0 - cache_rate;

    // Routing: cost diff per call * non-cached calls * routing_savings_rate
    if (r.target_pricing.has_value()) {
        const double per_call_src =
            (in_tok * r.source_pricing.price_per_1k_input +
             out_tok * r.source_pricing.price_per_1k_output) / 1000.0;
        const double per_call_tgt =
            (in_tok * r.target_pricing->price_per_1k_input +
             out_tok * r.target_pricing->price_per_1k_output) / 1000.0;
        const double per_call_diff = std::max(0.0, per_call_src - per_call_tgt);
        r.routing_saved = n * non_cached_share *
                          std::clamp(input.routing_savings_rate, 0.0, 1.0) *
                          per_call_diff;
        if (per_call_src <= per_call_tgt) {
            r.notes.push_back("target_model is not cheaper; routing_saved is $0.00");
        }
    }

    // Compression: input-side only, only on non-cached calls.
    const double non_cached_input =
        n * non_cached_share * in_tok * r.source_pricing.price_per_1k_input / 1000.0;
    r.compression_saved = non_cached_input *
                          std::clamp(input.compression_rate, 0.0, 1.0);

    r.total_saved = r.cache_saved + r.routing_saved + r.compression_saved;
    r.saved_percentage = (r.baseline_monthly_total > 0)
                             ? (r.total_saved / r.baseline_monthly_total)
                             : 0.0;
    r.with_aegisgate_monthly = r.baseline_monthly_total - r.total_saved;
    r.annual_saved = r.total_saved * 12.0;

    return r;
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------
std::string renderTable(const EstimateResult& r) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    const auto& in = r.resolved_input;

    os << kTitle << "\n\n";
    os << "Input:\n";
    os << "  Model:              " << in.model << "\n";
    os << "  Monthly volume:     " << in.monthly_calls << " calls x ("
       << in.avg_input_tokens << " in + " << in.avg_output_tokens
       << " out tokens)\n";
    os << "  Scenario:           " << in.scenario << "\n\n";

    os << "Baseline cost (without AegisGate):\n";
    os << "  Input cost:                                            $"
       << std::setw(10) << formatCost(r.baseline_input_cost) << "\n";
    os << "  Output cost:                                           $"
       << std::setw(10) << formatCost(r.baseline_output_cost) << "\n";
    os << "  Total monthly:                                         $"
       << std::setw(10) << formatCost(r.baseline_monthly_total) << "\n\n";

    os << "With AegisGate:\n";
    os << "  Cache hits ("
       << std::setprecision(0) << (in.cache_hit_rate * 100.0) << "%):"
       << std::setprecision(2) << "                                  -$"
       << std::setw(10) << formatCost(r.cache_saved) << "\n";
    if (r.target_pricing.has_value()) {
        os << "  Routing ("
           << std::setprecision(0) << (in.routing_savings_rate * 100.0) << "% to "
           << r.target_model_used << "):"
           << std::setprecision(2) << "      -$"
           << std::setw(10) << formatCost(r.routing_saved) << "\n";
    } else {
        os << "  Routing (no cheaper sibling in same provider):       -$"
           << std::setw(10) << formatCost(r.routing_saved) << "\n";
    }
    os << "  Compression ("
       << std::setprecision(0) << (in.compression_rate * 100.0)
       << "% on input):"
       << std::setprecision(2) << "                       -$"
       << std::setw(10) << formatCost(r.compression_saved) << "\n\n";

    os << "  " << kMonthlyLabel
       << "                            -$"
       << std::setw(10) << formatCost(r.total_saved) << " ("
       << std::setprecision(1) << (r.saved_percentage * 100.0) << "%)\n";
    os << std::setprecision(2)
       << "  Estimated monthly cost with AegisGate:                  $"
       << std::setw(10) << formatCost(r.with_aegisgate_monthly) << "\n\n";
    os << "  " << kAnnualLabel
       << "                              $"
       << std::setw(10) << formatCost(r.annual_saved) << "\n\n";

    os << kVerifyCta << " Run quickstart and check after 24h:\n";
    os << "  -> " << kQuickstartLink << "\n";
    os << "  -> http://localhost:8080/admin/api/savings/summary\n";

    if (in.explain) {
        os << "\nAssumptions for \"" << in.scenario << "\" scenario:\n";
        os << "  - cache hit rate: " << std::setprecision(0)
           << (in.cache_hit_rate * 100.0) << "%\n";
        os << "    Source: AegisGate community baseline. Industry studies "
              "show 25-45% in production.\n";
        os << "  - routing savings: "
           << (in.routing_savings_rate * 100.0)
           << "% of non-cached calls routed to cheaper model\n";
        os << "  - compression: "
           << (in.compression_rate * 100.0) << "% input token reduction\n";
        os << std::setprecision(2);
    }

    if (!r.notes.empty()) {
        os << "\nNotes:\n";
        for (const auto& n : r.notes) os << "  - " << n << "\n";
    }
    return os.str();
}

std::string renderJson(const EstimateResult& r) {
    nlohmann::json j;
    const auto& in = r.resolved_input;
    j["version"] = "0.1.0";
    j["input"] = {
        {"model", in.model},
        {"monthly_calls", in.monthly_calls},
        {"avg_input_tokens", in.avg_input_tokens},
        {"avg_output_tokens", in.avg_output_tokens},
        {"scenario", in.scenario},
        {"cache_hit_rate", in.cache_hit_rate},
        {"routing_savings_rate", in.routing_savings_rate},
        {"compression_rate", in.compression_rate},
        {"target_model", r.target_model_used},
    };
    j["pricing"] = {
        {"source",
         {{"input_per_1k", r.source_pricing.price_per_1k_input},
          {"output_per_1k", r.source_pricing.price_per_1k_output}}},
    };
    if (r.target_pricing) {
        j["pricing"]["target"] = {
            {"input_per_1k", r.target_pricing->price_per_1k_input},
            {"output_per_1k", r.target_pricing->price_per_1k_output},
        };
    }
    j["baseline"] = {
        {"input_cost", formatCost(r.baseline_input_cost)},
        {"output_cost", formatCost(r.baseline_output_cost)},
        {"monthly_total", formatCost(r.baseline_monthly_total)},
    };
    j["savings"] = {
        {"cache",
         {{"rate", in.cache_hit_rate}, {"amount", formatCost(r.cache_saved)}}},
        {"routing",
         {{"rate", in.routing_savings_rate},
          {"amount", formatCost(r.routing_saved)},
          {"target_model", r.target_model_used}}},
        {"compression",
         {{"rate", in.compression_rate},
          {"amount", formatCost(r.compression_saved)}}},
        {"monthly_total", formatCost(r.total_saved)},
        {"saved_percentage", r.saved_percentage},
    };
    j["with_aegisgate"] = {
        {"monthly_cost", formatCost(r.with_aegisgate_monthly)},
        {"annual_savings", formatCost(r.annual_saved)},
    };
    if (!r.notes.empty()) j["notes"] = r.notes;
    return j.dump(2);
}

// ---------------------------------------------------------------------------
// CLI entry — parses flags, loads pricing, computes, renders.
// ---------------------------------------------------------------------------
namespace {

void printEstimateUsage(std::ostream& os) {
    os << "aegisctl estimate -- pre-flight savings estimator\n\n"
       << "Usage:\n"
       << "  aegisctl estimate --model <id> --monthly-calls <N> "
       << "--avg-input-tokens <N> --avg-output-tokens <N>\n"
       << "                    [--scenario conservative|balanced|aggressive]\n"
       << "                    [--cache-hit-rate <0-1>]"
       << " [--routing-savings-rate <0-1>] [--compression-rate <0-1>]\n"
       << "                    [--target-model <id>] "
       << "[--models-config <path>]\n"
       << "                    [--output table|json] [--explain]\n";
}

bool parseDouble(const std::string& s, double& out) {
    try {
        size_t pos = 0;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool parseLong(const std::string& s, long long& out) {
    try {
        size_t pos = 0;
        out = std::stoll(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

}  // namespace

int runEstimate(const std::vector<std::string>& args) {
    EstimateInput in;
    bool model_set = false, calls_set = false, in_tok_set = false,
         out_tok_set = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--help" || a == "-h") {
            printEstimateUsage(std::cout);
            return 0;
        }
        auto need = [&](const char* flag) -> const std::string* {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return nullptr;
            }
            return &args[++i];
        };
        if (a == "--model") {
            const auto* v = need("--model");
            if (!v) return 1;
            in.model = *v;
            model_set = true;
        } else if (a == "--monthly-calls") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (!parseLong(*v, in.monthly_calls)) {
                std::cerr << "Error: --monthly-calls expects an integer\n";
                return 1;
            }
            calls_set = true;
        } else if (a == "--avg-input-tokens") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (!parseLong(*v, in.avg_input_tokens)) {
                std::cerr << "Error: --avg-input-tokens expects an integer\n";
                return 1;
            }
            in_tok_set = true;
        } else if (a == "--avg-output-tokens") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (!parseLong(*v, in.avg_output_tokens)) {
                std::cerr << "Error: --avg-output-tokens expects an integer\n";
                return 1;
            }
            out_tok_set = true;
        } else if (a == "--scenario") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (*v != "conservative" && *v != "balanced" && *v != "aggressive") {
                std::cerr << "Error: --scenario must be one of "
                             "conservative|balanced|aggressive\n";
                return 1;
            }
            in.scenario = *v;
            in.scenario_explicit = true;
        } else if (a == "--cache-hit-rate") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (!parseDouble(*v, in.cache_hit_rate) || in.cache_hit_rate < 0 ||
                in.cache_hit_rate > 1) {
                std::cerr << "Error: cache_hit_rate must be in [0, 1]\n";
                return 1;
            }
            in.cache_explicit = true;
        } else if (a == "--routing-savings-rate") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (!parseDouble(*v, in.routing_savings_rate) ||
                in.routing_savings_rate < 0 || in.routing_savings_rate > 1) {
                std::cerr << "Error: routing_savings_rate must be in [0, 1]\n";
                return 1;
            }
            in.routing_explicit = true;
        } else if (a == "--compression-rate") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (!parseDouble(*v, in.compression_rate) ||
                in.compression_rate < 0 || in.compression_rate > 1) {
                std::cerr << "Error: compression_rate must be in [0, 1]\n";
                return 1;
            }
            in.compression_explicit = true;
        } else if (a == "--target-model") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            in.target_model = *v;
        } else if (a == "--models-config") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            in.models_config = *v;
        } else if (a == "--output") {
            const auto* v = need(a.c_str());
            if (!v) return 1;
            if (*v != "table" && *v != "json") {
                std::cerr << "Error: --output must be 'table' or 'json'\n";
                return 1;
            }
            in.output = *v;
        } else if (a == "--explain") {
            in.explain = true;
        } else {
            std::cerr << "Error: unknown flag '" << a << "'\n";
            printEstimateUsage(std::cerr);
            return 1;
        }
    }

    if (!model_set || !calls_set || !in_tok_set || !out_tok_set) {
        std::cerr << "Error: --model, --monthly-calls, --avg-input-tokens, "
                     "--avg-output-tokens are all required\n";
        printEstimateUsage(std::cerr);
        return 1;
    }

    // Apply preset unless the user explicitly overrode each rate.
    const ScenarioPreset preset = scenarioPreset(in.scenario);
    if (!in.cache_explicit) in.cache_hit_rate = preset.cache_hit_rate;
    if (!in.routing_explicit) in.routing_savings_rate = preset.routing_savings_rate;
    if (!in.compression_explicit) in.compression_rate = preset.compression_rate;

    std::unordered_map<std::string, ModelPricing> pricing;
    try {
        pricing = loadPricing(in.models_config);
    } catch (const std::runtime_error& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }
    if (pricing.empty()) {
        std::cerr << "Error: no providers loaded from " << in.models_config
                  << "\n";
        return 2;
    }
    if (pricing.find(in.model) == pricing.end()) {
        std::cerr << "Error: Model '" << in.model
                  << "' not found in " << in.models_config
                  << ". Run 'aegisctl models' to list available models.\n";
        return 1;
    }

    EstimateResult r;
    try {
        r = computeEstimate(in, pricing);
    } catch (const std::invalid_argument& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    if (in.output == "json") {
        std::cout << renderJson(r) << "\n";
    } else {
        std::cout << renderTable(r);
    }
    return 0;
}

}  // namespace aegisgate::cli
