#include <gtest/gtest.h>
#include "agent/tool_sandbox.h"
#include <stdexcept>

using namespace aegisgate;

static ToolDefinition makeTool(const std::string& id, bool enabled = true) {
    ToolDefinition t(id, id, "Test tool " + id);
    t.enabled = enabled;
    t.parameters_schema = {
        {"type", "object"},
        {"properties", {{"input", {{"type", "string"}}}}},
        {"required", {"input"}}
    };
    return t;
}

class ToolSandboxTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_.registerTool(makeTool("echo"));
        registry_.registerTool(makeTool("disabled_tool", false));
        sandbox_ = std::make_unique<ToolSandbox>(&registry_);
    }

    ToolRegistry registry_;
    std::unique_ptr<ToolSandbox> sandbox_;
};

TEST_F(ToolSandboxTest, ExecuteSuccess) {
    sandbox_->setExecutor([](const std::string& /*tool_id*/,
                             const nlohmann::json& args) -> std::string {
        return "echo: " + args["input"].get<std::string>();
    });

    nlohmann::json args = {{"input", "hello"}};
    auto result = sandbox_->execute("echo", args);

    EXPECT_EQ(result.status, ToolExecutionStatus::Success);
    EXPECT_EQ(result.tool_id, "echo");
    EXPECT_EQ(result.output, "echo: hello");
    EXPECT_GT(result.duration_ms, 0.0);
    EXPECT_TRUE(result.error_message.empty());
}

TEST_F(ToolSandboxTest, ExecuteNoExecutor) {
    nlohmann::json args = {{"input", "hello"}};
    auto result = sandbox_->execute("echo", args);

    EXPECT_EQ(result.status, ToolExecutionStatus::Error);
    EXPECT_NE(result.error_message.find("No tool executor configured"),
              std::string::npos);
}

TEST_F(ToolSandboxTest, ExecuteUnknownTool) {
    sandbox_->setExecutor([](const std::string&,
                             const nlohmann::json&) -> std::string {
        return "ok";
    });

    auto result = sandbox_->execute("nonexistent", {});

    EXPECT_EQ(result.status, ToolExecutionStatus::Error);
    EXPECT_NE(result.error_message.find("Unknown tool"), std::string::npos);
}

TEST_F(ToolSandboxTest, ExecuteDisabledTool) {
    sandbox_->setExecutor([](const std::string&,
                             const nlohmann::json&) -> std::string {
        return "ok";
    });

    nlohmann::json args = {{"input", "hello"}};
    auto result = sandbox_->execute("disabled_tool", args);

    EXPECT_EQ(result.status, ToolExecutionStatus::Rejected);
    EXPECT_NE(result.error_message.find("disabled"), std::string::npos);
}

TEST_F(ToolSandboxTest, ExecuteOutputTruncation) {
    sandbox_->setExecutor([](const std::string&,
                             const nlohmann::json&) -> std::string {
        return std::string(ToolSandbox::kMaxToolOutputBytes + 1000, 'x');
    });

    nlohmann::json args = {{"input", "hello"}};
    auto result = sandbox_->execute("echo", args);

    EXPECT_EQ(result.status, ToolExecutionStatus::Success);
    EXPECT_EQ(result.output.size(), ToolSandbox::kMaxToolOutputBytes);
}

TEST_F(ToolSandboxTest, ExecuteExceptionHandling) {
    sandbox_->setExecutor([](const std::string&,
                             const nlohmann::json&) -> std::string {
        throw std::runtime_error("executor crashed");
    });

    nlohmann::json args = {{"input", "hello"}};
    auto result = sandbox_->execute("echo", args);

    EXPECT_EQ(result.status, ToolExecutionStatus::Error);
    EXPECT_NE(result.error_message.find("executor crashed"), std::string::npos);
}

TEST_F(ToolSandboxTest, ExecutionCounters) {
    EXPECT_EQ(sandbox_->totalExecutions(), 0u);
    EXPECT_EQ(sandbox_->failedExecutions(), 0u);

    sandbox_->execute("echo", {{"input", "test"}});
    EXPECT_EQ(sandbox_->totalExecutions(), 1u);
    EXPECT_EQ(sandbox_->failedExecutions(), 1u);

    sandbox_->setExecutor([](const std::string&,
                             const nlohmann::json&) -> std::string {
        return "ok";
    });

    sandbox_->execute("echo", {{"input", "test"}});
    EXPECT_EQ(sandbox_->totalExecutions(), 2u);
    EXPECT_EQ(sandbox_->failedExecutions(), 1u);

    sandbox_->execute("nonexistent", {});
    EXPECT_EQ(sandbox_->totalExecutions(), 3u);
    EXPECT_EQ(sandbox_->failedExecutions(), 2u);
}
