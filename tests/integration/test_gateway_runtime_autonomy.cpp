// Phase 11.5 (TASK-20260518-02 E4.3) — GatewayRuntime autonomy wiring.
//
// 3 integration tests covering the new yaml gates added in E4.1 + the wiring
// added in E4.2.  We avoid touching the data plane (no requests dispatched)
// and only assert that GatewayRuntime exposes the right accessors after
// initialize() under three yaml shapes:
//
//   A. all-disabled         → no workflow, no applier, no budget stage
//   B. budget_guard-only    → no workflow, no applier, BUT budget stage on
//   C. autonomy + cost_opt  → workflow + applier (router_type=ml +
//      AdvancedRouting license), no stage
//
// We deliberately split (B) and (C) so the BudgetGuardStage installation
// path is exercised independently of the workflow path (the two are wired
// from disjoint config flags per design §6.1). CostAutonomyApplier needs a
// live MLRouter; post-REV20260707-I13 that requires Feature::AdvancedRouting.

#include <gtest/gtest.h>

#include "core/config.h"
#include "core/feature_gate.h"
#include "server/gateway_runtime.h"
#include "observe/autonomy/approval_workflow.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace aegisgate;

namespace {

// Mirrors tests/unit/core/test_feature_gate.cpp — CostAutonomyApplier
// needs a live MLRouter, and MLRouter is gated on Feature::AdvancedRouting
// (REV20260707-I13). Community edition falls back to CostAware, so Test C
// must unlock Enterprise with a signed license that includes
// advanced_routing.
std::string sha256Hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(hash[i]);
    return oss.str();
}

std::string writeAdvancedRoutingLicense() {
    const std::vector<std::string> features = {"advanced_routing"};
    const std::string payload =
        FeatureGate::buildLicensePayload("enterprise", "autonomy-test",
                                         "2099-12-31", features);
    constexpr const char* kSalt = "aegisgate-v1-f7e2a9c4d1b8";
    const std::string full_hash = sha256Hex(payload + kSalt);
    const std::string key =
        "AEGIS-ENT-" + full_hash.substr(0, 16) + full_hash.substr(0, 16);

    auto path = std::filesystem::temp_directory_path() /
        ("aegis_runtime_autonomy_license_" + std::to_string(getpid()) +
         ".json");
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "autonomy-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key},
    };
    std::ofstream ofs(path);
    ofs << j.dump();
    return path.string();
}

// We start from the repo-shipped config/aegisgate.yaml so PipelineAssembler
// has every section it expects (storage / cache / audit / models / etc.),
// then surgically flip the autonomy + budget_guard + routing flags via the
// yaml-cpp DOM. Re-emitting through the same library guarantees a single
// well-formed document with no duplicate-key ambiguity.
std::string buildYaml(bool autonomy_on,
                      bool cost_opt_on,
                      bool budget_guard_on,
                      const std::string& router_type = "ml",
                      bool unlock_advanced_routing = false) {
    YAML::Node root = YAML::LoadFile("config/aegisgate.yaml");
    root["routing"]["type"] = router_type;
    root["autonomy"]["enabled"] = autonomy_on;
    root["autonomy"]["auto_apply_mode"] = "manual_only";
    root["autonomy"]["proposal_retention_days"] = 7;
    root["autonomy"]["cost_optimizer"]["enabled"] = cost_opt_on;
    root["budget_guard"]["enabled"] = budget_guard_on;
    root["budget_guard"]["per_tenant_24h_usd"] = 50.0;
    root["budget_guard"]["per_request_max_usd"] = 0.5;
    root["budget_guard"]["fail_open_on_error"] = true;
    root["budget_guard"]["downgrade_tier"] = "economy";
    root["auth"]["enabled"] = false;
    root["auth"]["api_keys"] = YAML::Node(YAML::NodeType::Sequence);
    if (unlock_advanced_routing) {
        root["edition"] = "enterprise";
        root["license_file"] = writeAdvancedRoutingLicense();
    }
    std::stringstream ss;
    ss << root;
    return ss.str();
}

void initRuntimeWithYaml(const std::string& yaml) {
    auto& rt = GatewayRuntime::instance();
    rt.resetShutdownForTesting();
    if (rt.isInitialized()) {
        rt.beginShutdown();
        rt.shutdown();
        rt.reinitializeForTesting();
        // reinitializeForTesting re-initializes from the previous config_;
        // we don't want that for these tests, so tear down again and rebuild
        // from the new yaml below.
        rt.beginShutdown();
        rt.shutdown();
    }
    rt.resetShutdownForTesting();

    // Storage isolation: each test gets a unique SQLite file so the
    // persistent_store created by PipelineAssembler doesn't collide with
    // sibling integration suites running in the same gtest binary.
    static int counter = 0;
    auto tmp = std::filesystem::temp_directory_path() /
        ("aegis_runtime_autonomy_" + std::to_string(getpid()) +
         "_" + std::to_string(++counter) + ".db");
    setenv("AEGISGATE_SQLITE_PATH", tmp.string().c_str(), 1);

    // Config has to outlive the runtime initialize() call; we hand-off via
    // a static so each test's local config doesn't drop out from under the
    // singleton's stored pointer.
    static Config cfg;
    ASSERT_TRUE(cfg.loadFromString(yaml));
    rt.initialize(cfg);
}

} // namespace

// --- Test A: all-disabled yaml leaves every autonomy hook null ----------

TEST(GatewayRuntimeAutonomyTest, AllDisabledLeavesAccessorsNull) {
    initRuntimeWithYaml(buildYaml(/*autonomy*/ false,
                                  /*cost_opt*/ false,
                                  /*budget_guard*/ false));
    auto& rt = GatewayRuntime::instance();
    ASSERT_TRUE(rt.isInitialized());

    EXPECT_EQ(rt.approvalWorkflow(), nullptr);
    EXPECT_EQ(rt.approvalQueue(), nullptr);
    EXPECT_EQ(rt.costAutonomyApplier(), nullptr);
    EXPECT_EQ(rt.budgetGuardStage(), nullptr);
}

// --- Test B: budget_guard-only installs the inbound stage but no workflow --

TEST(GatewayRuntimeAutonomyTest, BudgetGuardOnlyInstallsStageWithoutWorkflow) {
    initRuntimeWithYaml(buildYaml(/*autonomy*/ false,
                                  /*cost_opt*/ false,
                                  /*budget_guard*/ true));
    auto& rt = GatewayRuntime::instance();
    ASSERT_TRUE(rt.isInitialized());

    // BudgetGuardStage is wired from its own gate, independent of the
    // workflow.
    EXPECT_NE(rt.budgetGuardStage(), nullptr);
    if (auto* stage = rt.budgetGuardStage()) {
        auto cfg = stage->config();
        EXPECT_TRUE(cfg.enabled);
        EXPECT_DOUBLE_EQ(cfg.per_tenant_24h_usd, 50.0);
        EXPECT_DOUBLE_EQ(cfg.per_request_max_usd, 0.5);
        EXPECT_EQ(cfg.downgrade_tier, "economy");
    }

    // Workflow stays off because both `autonomy.enabled` and
    // `cost_optimizer.enabled` are false.
    EXPECT_EQ(rt.approvalWorkflow(), nullptr);
    EXPECT_EQ(rt.approvalQueue(), nullptr);
    EXPECT_EQ(rt.costAutonomyApplier(), nullptr);
}

// --- Test C: autonomy + cost_optimizer wires workflow + applier (no stage) --
// AdvancedRouting license is required so MLRouter (and thus
// CostAutonomyApplier) actually assemble under community-default CI.

TEST(GatewayRuntimeAutonomyTest, AutonomyEnabledWiresWorkflowAndApplier) {
    initRuntimeWithYaml(buildYaml(/*autonomy*/ true,
                                  /*cost_opt*/ true,
                                  /*budget_guard*/ false,
                                  /*router_type*/ "ml",
                                  /*unlock_advanced_routing*/ true));
    auto& rt = GatewayRuntime::instance();
    ASSERT_TRUE(rt.isInitialized());

    ASSERT_NE(rt.approvalWorkflow(), nullptr);
    ASSERT_NE(rt.approvalQueue(), nullptr);
    ASSERT_NE(rt.costAutonomyApplier(), nullptr);

    // BudgetGuardStage is OFF in this scenario.
    EXPECT_EQ(rt.budgetGuardStage(), nullptr);

    // The workflow respects the per-instance enabled override we set in
    // initialize() — yaml `autonomy.enabled=true` translates to the env-
    // kill-switch-aware isAutonomyEnabled() returning true here.
    EXPECT_TRUE(autonomy::AutonomyApprovalWorkflow::isAutonomyEnabled());
}
