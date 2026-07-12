#include <gtest/gtest.h>
#include "plugin/plugin_loader.h"
#include "core/context.h"
#include <filesystem>
#include <fstream>

using namespace aegisgate;

TEST(PluginLoaderTest, ListEmptyWhenNoneLoaded) {
    PluginLoader loader;
    EXPECT_TRUE(loader.listLoaded().empty());
}

TEST(PluginLoaderTest, LoadNonexistent) {
    PluginLoader loader;
    EXPECT_FALSE(loader.load("/nonexistent/path.so"));
}

TEST(PluginLoaderTest, LoadInvalidFile) {
    auto tmp = std::filesystem::temp_directory_path() / "not_a_plugin.so";
    std::ofstream(tmp) << "not a shared library";
    PluginLoader loader;
    EXPECT_FALSE(loader.load(tmp.string()));
    std::filesystem::remove(tmp);
}

TEST(PluginLoaderTest, LoadSamplePlugin) {
    auto plugin_path = std::filesystem::path(SAMPLE_PLUGIN_PATH);
    if (!std::filesystem::exists(plugin_path)) {
        GTEST_SKIP() << "sample_plugin.so not found at " << plugin_path;
    }
    PluginLoader loader;
    EXPECT_TRUE(loader.load(plugin_path.string()));
    EXPECT_TRUE(loader.isLoaded("sample"));

    auto list = loader.listLoaded();
    EXPECT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "sample");
    EXPECT_EQ(list[0].version, "1.0.0");
}

TEST(PluginLoaderTest, CreateStageFromPlugin) {
    auto plugin_path = std::filesystem::path(SAMPLE_PLUGIN_PATH);
    if (!std::filesystem::exists(plugin_path)) {
        GTEST_SKIP() << "sample_plugin.so not found";
    }
    PluginLoader loader;
    loader.load(plugin_path.string());

    auto stage = loader.createStage("sample");
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(stage->name(), "SamplePlugin");

    RequestContext ctx;
    EXPECT_EQ(stage->process(ctx), StageResult::Continue);
}

TEST(PluginLoaderTest, UnloadPlugin) {
    auto plugin_path = std::filesystem::path(SAMPLE_PLUGIN_PATH);
    if (!std::filesystem::exists(plugin_path)) {
        GTEST_SKIP() << "sample_plugin.so not found";
    }
    PluginLoader loader;
    loader.load(plugin_path.string());
    EXPECT_TRUE(loader.isLoaded("sample"));

    loader.unload("sample");
    EXPECT_FALSE(loader.isLoaded("sample"));
    EXPECT_TRUE(loader.listLoaded().empty());
}

TEST(PluginLoaderTest, CreateStageUnknownPlugin) {
    PluginLoader loader;
    auto stage = loader.createStage("nonexistent");
    EXPECT_EQ(stage, nullptr);
}

TEST(PluginLoaderTest, DuplicateLoadRejected) {
    auto plugin_path = std::filesystem::path(SAMPLE_PLUGIN_PATH);
    if (!std::filesystem::exists(plugin_path)) {
        GTEST_SKIP() << "sample_plugin.so not found";
    }
    PluginLoader loader;
    EXPECT_TRUE(loader.load(plugin_path.string()));
    EXPECT_FALSE(loader.load(plugin_path.string()));
    EXPECT_EQ(loader.listLoaded().size(), 1u);
}
