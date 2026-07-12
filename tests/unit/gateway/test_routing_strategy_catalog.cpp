// Phase 11.2 TASK-20260521-03 — RoutingStrategyCatalog unit tests.
//
// 5 hardcoded default templates + YAML override path (D5=C decision,
// mirrors PIIFilter::addDefaultPatterns + loadPatterns pattern).

#include <gtest/gtest.h>
#include "gateway/routing_strategy_catalog.h"

#include <filesystem>
#include <fstream>

using namespace aegisgate;

namespace {

std::string writeTempYaml(const std::string& body, const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path);
    f << body;
    return path.string();
}

}  // namespace

// T1: default ctor loads 5 hardcoded templates.
TEST(RoutingStrategyCatalogTest, DefaultsLoadsFiveTemplates) {
    RoutingStrategyCatalog catalog;
    auto list = catalog.list();
    EXPECT_EQ(list.size(), 5u);
}

// T2: cost-first has cost-dominant weights.
TEST(RoutingStrategyCatalogTest, CostFirstWeightsCorrect) {
    RoutingStrategyCatalog catalog;
    auto s = catalog.get("cost-first");
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->weights.cost, 0.7);
    EXPECT_DOUBLE_EQ(s->weights.quality, 0.2);
    EXPECT_DOUBLE_EQ(s->weights.latency, 0.1);
}

// T3: quality-first has quality-dominant weights.
TEST(RoutingStrategyCatalogTest, QualityFirstWeightsCorrect) {
    RoutingStrategyCatalog catalog;
    auto s = catalog.get("quality-first");
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->weights.cost, 0.2);
    EXPECT_DOUBLE_EQ(s->weights.quality, 0.6);
    EXPECT_DOUBLE_EQ(s->weights.latency, 0.2);
}

// T4: all 5 expected templates present.
TEST(RoutingStrategyCatalogTest, AllFiveTemplatesPresent) {
    RoutingStrategyCatalog catalog;
    EXPECT_TRUE(catalog.get("cost-first").has_value());
    EXPECT_TRUE(catalog.get("quality-first").has_value());
    EXPECT_TRUE(catalog.get("hybrid").has_value());
    EXPECT_TRUE(catalog.get("canary").has_value());
    EXPECT_TRUE(catalog.get("shadow").has_value());
}

// T5: get("unknown") returns nullopt.
TEST(RoutingStrategyCatalogTest, UnknownStrategyReturnsNullopt) {
    RoutingStrategyCatalog catalog;
    EXPECT_FALSE(catalog.get("nonexistent").has_value());
}

// T6: shadow template defaults bandit_mode = "shadow" (SR5 anchor).
TEST(RoutingStrategyCatalogTest, ShadowTemplateDefaultsToShadowMode) {
    RoutingStrategyCatalog catalog;
    auto s = catalog.get("shadow");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->bandit_mode, "shadow");
}

// T7: canary template has canary_pct = 0.05.
TEST(RoutingStrategyCatalogTest, CanaryTemplateHasFivePercent) {
    RoutingStrategyCatalog catalog;
    auto s = catalog.get("canary");
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->canary_pct, 0.05);
}

// T8: YAML override path (A19 mode — YAML::LoadFile baseline + node update).
TEST(RoutingStrategyCatalogTest, YamlOverrideUpdatesExistingTemplate) {
    auto path = writeTempYaml(
        "strategies:\n"
        "  - name: cost-first\n"
        "    weights:\n"
        "      cost: 0.9\n"
        "      quality: 0.05\n"
        "      latency: 0.05\n"
        "    bandit_algorithm: epsilon_greedy\n"
        "    bandit_mode: live\n"
        "    canary_pct: 0.10\n"
        "    description: aggressive cost reduction\n",
        "test_strategies_override.yaml");

    RoutingStrategyCatalog catalog;
    catalog.loadOverrides(path);

    auto s = catalog.get("cost-first");
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->weights.cost, 0.9);
    EXPECT_DOUBLE_EQ(s->weights.quality, 0.05);
    EXPECT_EQ(s->bandit_algorithm, "epsilon_greedy");
    EXPECT_DOUBLE_EQ(s->canary_pct, 0.10);

    // Other templates remain untouched.
    auto q = catalog.get("quality-first");
    ASSERT_TRUE(q.has_value());
    EXPECT_DOUBLE_EQ(q->weights.quality, 0.6);

    std::filesystem::remove(path);
}

// T9: YAML override can introduce a brand-new template.
TEST(RoutingStrategyCatalogTest, YamlOverrideCanAddNewTemplate) {
    auto path = writeTempYaml(
        "strategies:\n"
        "  - name: experimental-low-latency\n"
        "    weights:\n"
        "      cost: 0.1\n"
        "      quality: 0.2\n"
        "      latency: 0.7\n"
        "    bandit_algorithm: thompson\n"
        "    bandit_mode: shadow\n"
        "    canary_pct: 0.05\n"
        "    description: minimize p99 latency\n",
        "test_strategies_add.yaml");

    RoutingStrategyCatalog catalog;
    catalog.loadOverrides(path);

    EXPECT_EQ(catalog.list().size(), 6u);
    auto s = catalog.get("experimental-low-latency");
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->weights.latency, 0.7);

    std::filesystem::remove(path);
}

// T10: YAML override on nonexistent file leaves defaults intact.
TEST(RoutingStrategyCatalogTest, YamlOverrideMissingFileNoOp) {
    RoutingStrategyCatalog catalog;
    catalog.loadOverrides("/nonexistent/path/strategies.yaml");
    EXPECT_EQ(catalog.list().size(), 5u);
    EXPECT_TRUE(catalog.get("cost-first").has_value());
}
