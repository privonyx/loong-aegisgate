#include <gtest/gtest.h>
#include "gateway/connector/factory.h"
#include "gateway/connector/openai.h"

using namespace aegisgate;

namespace {
ProviderConfig makeConfig(const std::string& name, const std::string& type) {
    ProviderConfig pc;
    pc.name = name;
    pc.type = type;
    pc.base_url = "https://example.com";
    pc.api_keys = {{"test-key", 1}};
    pc.models = {{"test-model", name, 0.001, 0.002, 4096, {}, {}}};
    return pc;
}
} // namespace

TEST(ConnectorFactoryTest, CreateRegisteredType) {
    ConnectorFactory factory;
    factory.registerType("mock", [](const ProviderConfig& pc) {
        return std::make_unique<OpenAIConnector>(pc);
    });

    auto connector = factory.create(makeConfig("test", "mock"));
    ASSERT_NE(connector, nullptr);
    EXPECT_EQ(connector->provider(), "test");
}

TEST(ConnectorFactoryTest, CreateUnregisteredType_ReturnsNull) {
    ConnectorFactory factory;
    auto connector = factory.create(makeConfig("test", "unknown"));
    EXPECT_EQ(connector, nullptr);
}

TEST(ConnectorFactoryTest, HasType) {
    ConnectorFactory factory;
    factory.registerType("openai", [](const ProviderConfig& pc) {
        return std::make_unique<OpenAIConnector>(pc);
    });

    EXPECT_TRUE(factory.hasType("openai"));
    EXPECT_FALSE(factory.hasType("nonexistent"));
}

TEST(ConnectorFactoryTest, RegisteredTypes) {
    ConnectorFactory factory;
    factory.registerType("a", [](const ProviderConfig& pc) {
        return std::make_unique<OpenAIConnector>(pc);
    });
    factory.registerType("b", [](const ProviderConfig& pc) {
        return std::make_unique<OpenAIConnector>(pc);
    });

    auto types = factory.registeredTypes();
    EXPECT_EQ(types.size(), 2u);
}

TEST(ConnectorFactoryTest, DefaultsIncludeAllBuiltinTypes) {
    auto& factory = ConnectorFactory::defaults();
    EXPECT_TRUE(factory.hasType("openai"));
    EXPECT_TRUE(factory.hasType("claude"));
    EXPECT_TRUE(factory.hasType("deepseek"));
    EXPECT_TRUE(factory.hasType("doubao"));
    EXPECT_TRUE(factory.hasType("qwen"));
    EXPECT_TRUE(factory.hasType("gemini"));
    EXPECT_TRUE(factory.hasType("mistral"));
    EXPECT_TRUE(factory.hasType("openai_compatible"));
    EXPECT_FALSE(factory.hasType("nonexistent"));
}

TEST(ConnectorFactoryTest, DefaultsCreateOpenAICompatible) {
    auto& factory = ConnectorFactory::defaults();

    for (const auto& type : {"openai", "deepseek", "doubao", "qwen", "openai_compatible"}) {
        auto pc = makeConfig(std::string(type) + "-provider", type);
        auto connector = factory.create(pc);
        ASSERT_NE(connector, nullptr) << "Failed to create type: " << type;
        EXPECT_EQ(connector->provider(), std::string(type) + "-provider");
    }
}

TEST(ConnectorFactoryTest, DefaultsCreateClaude) {
    auto& factory = ConnectorFactory::defaults();
    auto pc = makeConfig("claude", "claude");
    auto connector = factory.create(pc);
    ASSERT_NE(connector, nullptr);
    EXPECT_EQ(connector->provider(), "claude");
}

TEST(ConnectorFactoryTest, DefaultsCreateGemini) {
    auto& factory = ConnectorFactory::defaults();
    auto config = makeConfig("gemini", "gemini");
    config.base_url = "https://generativelanguage.googleapis.com/v1beta/openai";
    auto connector = factory.create(config);
    ASSERT_NE(connector, nullptr);
    EXPECT_EQ(connector->provider(), "gemini");
}

TEST(ConnectorFactoryTest, DefaultsCreateMistral) {
    auto& factory = ConnectorFactory::defaults();
    auto config = makeConfig("mistral", "mistral");
    config.base_url = "https://api.mistral.ai/v1";
    auto connector = factory.create(config);
    ASSERT_NE(connector, nullptr);
    EXPECT_EQ(connector->provider(), "mistral");
}
