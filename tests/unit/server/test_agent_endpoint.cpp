#include <gtest/gtest.h>
#include "server/gateway_runtime.h"
#include "core/config.h"
#include <nlohmann/json.hpp>
#include <cstdlib>

using namespace aegisgate;

// TASK-20260703-04 Epic 1（D1 / SR-1）— /v1/agent 装配 + 认证契约。
// 通过 GatewayRuntime::processAgentRequest（controller 底层）验证认证优先与
// 端点 opt-in 门禁。设置 AEGISGATE_API_KEY 使一个已知 key 有效。
class AgentEndpointTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        setenv("AEGISGATE_API_KEY", "sk-agent-test", 1);
        auto& rt = GatewayRuntime::instance();
        rt.resetShutdownForTesting();
        if (!rt.isInitialized()) {
            static Config config;
            config.loadFromFile("config/aegisgate.yaml");
            rt.initialize(config);
        }
    }

    void TearDown() override {
        GatewayRuntime::instance().resetShutdownForTesting();
    }
};

// SR-1：无效 API key 必须被拒（认证优先于任何编排）。
TEST_F(AgentEndpointTest, RejectsInvalidApiKey) {
    auto& rt = GatewayRuntime::instance();
    auto r = rt.processAgentRequest("gpt-4o", "hello", 0, "totally-wrong-key");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error.http_status, 401);
}

// opt-in 门禁：合法 key 但端点默认未启用 → 501（装配 != 暴露执行面）。
TEST_F(AgentEndpointTest, ValidKeyButEndpointDisabledReturns501) {
    auto& rt = GatewayRuntime::instance();
    auto r = rt.processAgentRequest("gpt-4o", "hello", 0, "sk-agent-test");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error.http_status, 501);
}

// --- Epic 2 (D2 / SR-4)：/v1/workflow 认证 + opt-in 门禁 ---

TEST_F(AgentEndpointTest, WorkflowRejectsInvalidApiKey) {
    auto& rt = GatewayRuntime::instance();
    nlohmann::json wf = {{"id", "wf"}, {"version", "v1"}, {"nodes", nlohmann::json::array()}};
    auto r = rt.processWorkflowRequest(wf, nlohmann::json::object(), "totally-wrong-key");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error.http_status, 401);
}

TEST_F(AgentEndpointTest, WorkflowValidKeyButEndpointDisabledReturns501) {
    auto& rt = GatewayRuntime::instance();
    nlohmann::json wf = {{"id", "wf"}, {"version", "v1"}, {"nodes", nlohmann::json::array()}};
    auto r = rt.processWorkflowRequest(wf, nlohmann::json::object(), "sk-agent-test");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error.http_status, 501);
}
