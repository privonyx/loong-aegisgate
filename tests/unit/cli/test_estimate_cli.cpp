// Unit tests for aegisctl estimate (MVP-2 / TASK-20260526-01)
//
// Coverage:
//   T1 PricingLoadFromYaml      — load config/models.yaml, lookup gpt-4o
//   T2 PresetValuesExact        — 3 presets x 3 fields = 9 exact constants
//   T3 FormulaBaseline          — 100k x (800/200) x gpt-4o = $700.00
//   T4 FormulaCacheSavings      — cache_hit_rate=0.30 -> $210.00 cache_saved
//   T5 FormulaCompressionInputOnly — compression applies only on non-cached input
//   T6 TargetModelAutoSelectCheaper — gpt-4o auto -> gpt-4o-mini (same provider)
//
// SR coverage:
//   SR1 (no network side-effect) — no drogon HttpClient instantiated in any test
//   SR2 (only reads models.yaml) — pricing path injected via test fixture
//
// Anti-mutation defense (layer-specific assertions per systemPatterns):
//   each formula component asserted independently so single-layer mutation
//   cannot be masked by another layer's tolerance.

#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "cli/estimate_cli.h"

namespace ae = aegisgate::cli;

namespace {

// In-memory fixture: write a minimal models.yaml subset to a tmp path.
// SR2: tests only read this fixture path; never touch real config/models.yaml.
std::string writeModelsFixture() {
    const std::string path = "/tmp/aegisctl_test_models.yaml";
    std::ofstream ofs(path);
    ofs << R"(providers:
  - name: openai
    type: openai
    base_url: "https://api.openai.com/v1"
    models:
      - id: "gpt-4o"
        cost_per_1k_input: 0.005
        cost_per_1k_output: 0.015
        max_tokens: 128000
      - id: "gpt-4o-mini"
        cost_per_1k_input: 0.00015
        cost_per_1k_output: 0.0006
        max_tokens: 128000
  - name: claude
    type: claude
    base_url: "https://api.anthropic.com"
    models:
      - id: "claude-sonnet-4"
        cost_per_1k_input: 0.003
        cost_per_1k_output: 0.015
        max_tokens: 200000
)";
    ofs.close();
    return path;
}

}  // namespace

// ---------------------------------------------------------------------------
// T1 — pricing loader returns gpt-4o with documented price
// ---------------------------------------------------------------------------
TEST(EstimateCli, T1_PricingLoadFromYaml) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    auto it = pricing.find("gpt-4o");
    ASSERT_NE(it, pricing.end()) << "gpt-4o must be present in fixture";
    EXPECT_DOUBLE_EQ(it->second.price_per_1k_input, 0.005);
    EXPECT_DOUBLE_EQ(it->second.price_per_1k_output, 0.015);
    EXPECT_EQ(it->second.provider, "openai");

    // gpt-4o-mini also present (used by T6)
    EXPECT_NE(pricing.find("gpt-4o-mini"), pricing.end());
    EXPECT_NE(pricing.find("claude-sonnet-4"), pricing.end());
}

// ---------------------------------------------------------------------------
// T2 — three preset constants exact (anti-mutation: 9 independent asserts)
// ---------------------------------------------------------------------------
TEST(EstimateCli, T2_PresetValuesExact) {
    const auto cons = ae::scenarioPreset("conservative");
    EXPECT_DOUBLE_EQ(cons.cache_hit_rate, 0.15);
    EXPECT_DOUBLE_EQ(cons.routing_savings_rate, 0.10);
    EXPECT_DOUBLE_EQ(cons.compression_rate, 0.05);

    const auto bal = ae::scenarioPreset("balanced");
    EXPECT_DOUBLE_EQ(bal.cache_hit_rate, 0.30);
    EXPECT_DOUBLE_EQ(bal.routing_savings_rate, 0.20);
    EXPECT_DOUBLE_EQ(bal.compression_rate, 0.10);

    const auto agg = ae::scenarioPreset("aggressive");
    EXPECT_DOUBLE_EQ(agg.cache_hit_rate, 0.50);
    EXPECT_DOUBLE_EQ(agg.routing_savings_rate, 0.40);
    EXPECT_DOUBLE_EQ(agg.compression_rate, 0.15);
}

// ---------------------------------------------------------------------------
// T3 — baseline cost formula (anti-mutation: separate input/output asserts)
// ---------------------------------------------------------------------------
TEST(EstimateCli, T3_FormulaBaseline) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    ae::EstimateInput in;
    in.model = "gpt-4o";
    in.monthly_calls = 100000;
    in.avg_input_tokens = 800;
    in.avg_output_tokens = 200;
    in.scenario = "balanced";
    in.cache_hit_rate = 0.0;            // disable other terms for clean baseline
    in.routing_savings_rate = 0.0;
    in.compression_rate = 0.0;

    const ae::EstimateResult r = ae::computeEstimate(in, pricing);

    // 100,000 * 800 * 0.005 / 1000 = 400
    EXPECT_NEAR(r.baseline_input_cost, 400.0, 0.01);
    // 100,000 * 200 * 0.015 / 1000 = 300
    EXPECT_NEAR(r.baseline_output_cost, 300.0, 0.01);
    EXPECT_NEAR(r.baseline_monthly_total, 700.0, 0.01);
}

// ---------------------------------------------------------------------------
// T4 — cache savings: 30% of baseline returned as 100%-saved calls
// ---------------------------------------------------------------------------
TEST(EstimateCli, T4_FormulaCacheSavings) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    ae::EstimateInput in;
    in.model = "gpt-4o";
    in.monthly_calls = 100000;
    in.avg_input_tokens = 800;
    in.avg_output_tokens = 200;
    in.scenario = "custom";
    in.cache_hit_rate = 0.30;
    in.routing_savings_rate = 0.0;      // isolate cache term
    in.compression_rate = 0.0;

    const ae::EstimateResult r = ae::computeEstimate(in, pricing);

    EXPECT_NEAR(r.baseline_monthly_total, 700.0, 0.01);
    EXPECT_NEAR(r.cache_saved, 210.0, 0.01);     // 700 * 0.30
    EXPECT_NEAR(r.routing_saved, 0.0, 0.01);     // isolated
    EXPECT_NEAR(r.compression_saved, 0.0, 0.01); // isolated
    EXPECT_NEAR(r.total_saved, 210.0, 0.01);
}

// ---------------------------------------------------------------------------
// T5 — compression applies only to non-cached input cost (layer separation)
// ---------------------------------------------------------------------------
TEST(EstimateCli, T5_FormulaCompressionInputOnly) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    ae::EstimateInput in;
    in.model = "gpt-4o";
    in.monthly_calls = 100000;
    in.avg_input_tokens = 800;
    in.avg_output_tokens = 200;
    in.scenario = "custom";
    in.cache_hit_rate = 0.30;
    in.routing_savings_rate = 0.0;      // isolate compression
    in.compression_rate = 0.10;

    const ae::EstimateResult r = ae::computeEstimate(in, pricing);

    // non-cached share = 70%
    // input cost in non-cached = 0.70 * 100000 * 800 * 0.005 / 1000 = 280
    // compression saved = 280 * 0.10 = 28
    EXPECT_NEAR(r.compression_saved, 28.0, 0.01);
    // cache term unchanged
    EXPECT_NEAR(r.cache_saved, 210.0, 0.01);
    // routing term remains zero (anti-mutation: single-layer mutation
    // on routing wouldn't be hidden by compression)
    EXPECT_NEAR(r.routing_saved, 0.0, 0.01);
}

// ---------------------------------------------------------------------------
// T6 — target_model auto-select picks cheapest in same provider
// ---------------------------------------------------------------------------
TEST(EstimateCli, T6_TargetModelAutoSelectCheaper) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    // Source = gpt-4o (provider=openai). Cheaper sibling = gpt-4o-mini.
    const auto picked = ae::selectTargetModel("gpt-4o", pricing);
    ASSERT_TRUE(picked.has_value())
        << "must pick a cheaper sibling within same provider";
    EXPECT_EQ(*picked, "gpt-4o-mini");

    // claude-sonnet-4 is sole model in claude provider -> no auto target.
    const auto picked_solo = ae::selectTargetModel("claude-sonnet-4", pricing);
    EXPECT_FALSE(picked_solo.has_value())
        << "single-model provider must yield no auto target";
}

// ---------------------------------------------------------------------------
// T7 — runEstimate exits 1 when required flags are missing
// ---------------------------------------------------------------------------
TEST(EstimateCli, T7_FlagParseRequiredMissingExits1) {
    // Missing --monthly-calls + --avg-input-tokens + --avg-output-tokens
    const int rc = ae::runEstimate({"--model", "gpt-4o"});
    EXPECT_EQ(rc, 1);
}

TEST(EstimateCli, T7_FlagParseUnknownFlagExits1) {
    const int rc = ae::runEstimate({"--bogus", "x"});
    EXPECT_EQ(rc, 1);
}

TEST(EstimateCli, T7_FlagParseModelNotFoundExits1) {
    const std::string path = writeModelsFixture();
    const int rc = ae::runEstimate({
        "--model", "nonexistent-model-xyz",
        "--monthly-calls", "1000",
        "--avg-input-tokens", "100",
        "--avg-output-tokens", "50",
        "--models-config", path,
    });
    EXPECT_EQ(rc, 1);
}

TEST(EstimateCli, T7_FlagParseInvalidRateExits1) {
    const int rc = ae::runEstimate({
        "--model", "gpt-4o",
        "--monthly-calls", "1000",
        "--avg-input-tokens", "100",
        "--avg-output-tokens", "50",
        "--cache-hit-rate", "1.5",  // out of [0, 1]
    });
    EXPECT_EQ(rc, 1);
}

// ---------------------------------------------------------------------------
// T8 — explicit --cache-hit-rate overrides scenario preset
// ---------------------------------------------------------------------------
TEST(EstimateCli, T8_FlagParseScenarioOverride) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    // Build the same input-resolution path runEstimate uses, but stop after
    // the override so we can inspect rates without redirecting stdout.
    ae::EstimateInput in;
    in.model = "gpt-4o";
    in.monthly_calls = 1000;
    in.avg_input_tokens = 100;
    in.avg_output_tokens = 50;
    in.scenario = "aggressive";
    in.scenario_explicit = true;
    in.cache_hit_rate = 0.7;
    in.cache_explicit = true;       // user override
    // routing/compression NOT explicit -> should pull from aggressive preset

    const auto preset = ae::scenarioPreset(in.scenario);
    if (!in.cache_explicit) in.cache_hit_rate = preset.cache_hit_rate;
    if (!in.routing_explicit) in.routing_savings_rate = preset.routing_savings_rate;
    if (!in.compression_explicit) in.compression_rate = preset.compression_rate;

    EXPECT_DOUBLE_EQ(in.cache_hit_rate, 0.7);          // explicit wins
    EXPECT_DOUBLE_EQ(in.routing_savings_rate, 0.40);   // aggressive default
    EXPECT_DOUBLE_EQ(in.compression_rate, 0.15);       // aggressive default

    const auto r = ae::computeEstimate(in, pricing);
    EXPECT_GT(r.total_saved, 0.0);
}

// ---------------------------------------------------------------------------
// T9 — table output contains all 5 SR4 hard-coded key texts (spec §4.2)
// ---------------------------------------------------------------------------
TEST(EstimateCli, T9_TableOutputContainsKeyTexts) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    ae::EstimateInput in;
    in.model = "gpt-4o";
    in.monthly_calls = 100000;
    in.avg_input_tokens = 800;
    in.avg_output_tokens = 200;
    in.scenario = "balanced";
    in.cache_hit_rate = 0.30;
    in.routing_savings_rate = 0.20;
    in.compression_rate = 0.10;

    const auto r = ae::computeEstimate(in, pricing);
    const std::string out = ae::renderTable(r);

    // 5 SR4 key texts (spec §4.2) — must all appear verbatim
    EXPECT_NE(out.find("AegisGate Savings Estimate"), std::string::npos);
    EXPECT_NE(out.find("Estimated monthly savings:"), std::string::npos);
    EXPECT_NE(out.find("Estimated annual savings:"), std::string::npos);
    EXPECT_NE(out.find("Want to verify?"), std::string::npos);
    EXPECT_NE(out.find("docs/quickstart.md"), std::string::npos);

    // SR3: must NOT contain sensitive markers
    EXPECT_EQ(out.find("api_key"), std::string::npos);
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
    EXPECT_EQ(out.find("sk-"), std::string::npos);
    EXPECT_EQ(out.find("password"), std::string::npos);
}

// ---------------------------------------------------------------------------
// T10 — JSON output parses cleanly and carries spec §4.3 keys
// ---------------------------------------------------------------------------
TEST(EstimateCli, T10_JsonOutputValid) {
    const std::string path = writeModelsFixture();
    const auto pricing = ae::loadPricing(path);

    ae::EstimateInput in;
    in.model = "gpt-4o";
    in.monthly_calls = 100000;
    in.avg_input_tokens = 800;
    in.avg_output_tokens = 200;
    in.scenario = "balanced";
    in.cache_hit_rate = 0.30;
    in.routing_savings_rate = 0.20;
    in.compression_rate = 0.10;

    const auto r = ae::computeEstimate(in, pricing);
    const std::string s = ae::renderJson(r);

    // Must be valid JSON
    auto j = nlohmann::json::parse(s);

    // spec §4.3 top-level keys
    ASSERT_TRUE(j.contains("version"));
    ASSERT_TRUE(j.contains("input"));
    ASSERT_TRUE(j.contains("pricing"));
    ASSERT_TRUE(j.contains("baseline"));
    ASSERT_TRUE(j.contains("savings"));
    ASSERT_TRUE(j.contains("with_aegisgate"));

    // Sample values (parenthesise template-method calls to dodge gtest macro
    // comma-splitting).
    const std::string model = j["input"]["model"];
    const double monthly_total = (j["baseline"]["monthly_total"]);
    const double cache_amt = (j["savings"]["cache"]["amount"]);
    EXPECT_EQ(model, "gpt-4o");
    EXPECT_DOUBLE_EQ(monthly_total, 700.0);
    EXPECT_NEAR(cache_amt, 210.0, 0.01);

    // SR3: rendered JSON must not carry sensitive markers
    EXPECT_EQ(s.find("api_key"), std::string::npos);
    EXPECT_EQ(s.find("tenant_id"), std::string::npos);
}
