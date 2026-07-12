#include <gtest/gtest.h>
#include "agent/tool_registry.h"
#include <thread>
#include <vector>

using namespace aegisgate;

static ToolDefinition makeTool(const std::string& id,
                               const std::string& name = "test_tool") {
    ToolDefinition t(id, name, "A test tool");
    t.version = "1.0";
    t.tags = {"test"};
    t.parameters_schema = {
        {"type", "object"},
        {"properties", {{"input", {{"type", "string"}}}}},
        {"required", {"input"}}
    };
    return t;
}

TEST(ToolRegistryTest, RegisterAndRetrieve) {
    ToolRegistry reg;
    auto tool = makeTool("tool-1", "my_tool");

    ASSERT_TRUE(reg.registerTool(tool));
    ASSERT_EQ(reg.size(), 1u);

    auto retrieved = reg.getTool("tool-1");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->id, "tool-1");
    EXPECT_EQ(retrieved->name, "my_tool");
    EXPECT_EQ(retrieved->description, "A test tool");
}

TEST(ToolRegistryTest, RegisterDuplicate) {
    ToolRegistry reg;
    ASSERT_TRUE(reg.registerTool(makeTool("tool-1")));
    ASSERT_FALSE(reg.registerTool(makeTool("tool-1")));
    EXPECT_EQ(reg.size(), 1u);
}

TEST(ToolRegistryTest, UnregisterTool) {
    ToolRegistry reg;
    reg.registerTool(makeTool("tool-1"));

    ASSERT_TRUE(reg.unregisterTool("tool-1"));
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_FALSE(reg.getTool("tool-1").has_value());

    EXPECT_FALSE(reg.unregisterTool("nonexistent"));
}

TEST(ToolRegistryTest, ListToolsNoFilter) {
    ToolRegistry reg;
    reg.registerTool(makeTool("t1"));
    reg.registerTool(makeTool("t2"));

    auto disabled = makeTool("t3");
    disabled.enabled = false;
    reg.registerTool(disabled);

    auto tools = reg.listTools();
    EXPECT_EQ(tools.size(), 2u);
}

TEST(ToolRegistryTest, ListToolsWithTags) {
    ToolRegistry reg;

    auto t1 = makeTool("t1");
    t1.tags = {"search", "web"};
    reg.registerTool(t1);

    auto t2 = makeTool("t2");
    t2.tags = {"code"};
    reg.registerTool(t2);

    auto t3 = makeTool("t3");
    t3.tags = {"search"};
    reg.registerTool(t3);

    auto results = reg.listTools({"search"});
    EXPECT_EQ(results.size(), 2u);

    auto code_results = reg.listTools({"code"});
    EXPECT_EQ(code_results.size(), 1u);
    EXPECT_EQ(code_results[0].id, "t2");
}

TEST(ToolRegistryTest, ValidateCall_ValidTool) {
    ToolRegistry reg;
    reg.registerTool(makeTool("t1"));

    nlohmann::json args = {{"input", "hello"}};
    auto result = reg.validateCall("t1", args);
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error.empty());
}

TEST(ToolRegistryTest, ValidateCall_MissingRequired) {
    ToolRegistry reg;
    reg.registerTool(makeTool("t1"));

    nlohmann::json args = {{"other", "value"}};
    auto result = reg.validateCall("t1", args);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("Missing required field"), std::string::npos);
}

TEST(ToolRegistryTest, ValidateCall_UnknownTool) {
    ToolRegistry reg;
    auto result = reg.validateCall("unknown", {});
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("Unknown tool"), std::string::npos);
}

TEST(ToolRegistryTest, ValidateCall_DisabledTool) {
    ToolRegistry reg;
    auto t = makeTool("t1");
    t.enabled = false;
    reg.registerTool(t);

    auto result = reg.validateCall("t1", {});
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("disabled"), std::string::npos);
}

TEST(ToolRegistryTest, ToOpenAITools) {
    ToolRegistry reg;
    auto t = makeTool("t1", "search_web");
    reg.registerTool(t);

    auto tools_json = reg.toOpenAITools();
    ASSERT_TRUE(tools_json.is_array());
    ASSERT_EQ(tools_json.size(), 1u);

    auto& entry = tools_json[0];
    EXPECT_EQ(entry["type"], "function");
    EXPECT_EQ(entry["function"]["name"], "search_web");
    EXPECT_EQ(entry["function"]["description"], "A test tool");
    EXPECT_TRUE(entry["function"]["parameters"].contains("type"));
}

TEST(ToolRegistryTest, ClearRemovesAll) {
    ToolRegistry reg;
    reg.registerTool(makeTool("t1"));
    reg.registerTool(makeTool("t2"));
    ASSERT_EQ(reg.size(), 2u);

    reg.clear();
    EXPECT_EQ(reg.size(), 0u);
}

TEST(ToolRegistryTest, ThreadSafety) {
    ToolRegistry reg;
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&reg, i]() {
            for (int j = 0; j < kOpsPerThread; ++j) {
                std::string id =
                    "tool-" + std::to_string(i) + "-" + std::to_string(j);
                reg.registerTool(makeTool(id));
                reg.listTools();
                reg.getTool(id);
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(reg.size(), static_cast<size_t>(kThreads * kOpsPerThread));
}
