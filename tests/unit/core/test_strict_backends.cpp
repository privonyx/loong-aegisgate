// TASK-20260622-01 E1 — G1 误配告警 + strict 后端校验。
//
// 信任陷阱：YAML 配 redis/postgres 但二进制未编入对应后端（或启动时后端不可达），
// 当前会静默回退 memory（仅一行 "Cache store: memory"），造成「以为上了生产」假阳性。
// E1 引入「请求 vs 实际」一致性校验：strict=true（默认）误配则 fail-closed 拒绝启动。
//
// 这里直接单测信任关键纯逻辑 PipelineAssembler::enforceBackendActive（避免跑重量级
// 全 assemble）+ Config::strictBackends() 访问器（默认/YAML/env 覆盖）。

#include "core/pipeline_assembler.h"
#include "core/config.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace aegisgate;

namespace {

// --- enforceBackendActive：信任关键纯逻辑 ---

TEST(StrictBackendsTest, RequestRedisNotCompiledStrictThrows) {
    // 请求 redis 但实际落到 memory（未编入 ENABLE_REDIS）+ strict=true → 拒绝启动
    EXPECT_THROW(
        PipelineAssembler::enforceBackendActive(
            "cache", "redis", "memory", "ENABLE_REDIS",
            /*compiled_in=*/false, /*strict=*/true),
        std::runtime_error);
}

TEST(StrictBackendsTest, RequestRedisNotCompiledNonStrictNoThrow) {
    // 同上但 strict=false → 仅告警不阻断（向后兼容逃生阀）
    EXPECT_NO_THROW(
        PipelineAssembler::enforceBackendActive(
            "cache", "redis", "memory", "ENABLE_REDIS",
            /*compiled_in=*/false, /*strict=*/false));
}

TEST(StrictBackendsTest, RequestPostgresInitFailedStrictThrows) {
    // 已编入但运行时连不上 → 实际 memory + strict=true → 拒绝启动
    EXPECT_THROW(
        PipelineAssembler::enforceBackendActive(
            "persistent", "postgres", "memory", "ENABLE_PG",
            /*compiled_in=*/true, /*strict=*/true),
        std::runtime_error);
}

TEST(StrictBackendsTest, BackendActiveNeverThrows) {
    // 请求 redis 且实际就是 redis → 一致 → 永不抛（strict 与否都不抛）
    EXPECT_NO_THROW(
        PipelineAssembler::enforceBackendActive(
            "cache", "redis", "redis", "ENABLE_REDIS", true, true));
    EXPECT_NO_THROW(
        PipelineAssembler::enforceBackendActive(
            "persistent", "postgres", "postgres", "ENABLE_PG", true, true));
}

TEST(StrictBackendsTest, MemoryRequestedNeverThrows) {
    // 显式请求 memory（社区默认）→ 不是误配 → 永不抛
    EXPECT_NO_THROW(
        PipelineAssembler::enforceBackendActive(
            "cache", "memory", "memory", "ENABLE_REDIS", false, true));
}

// SR2：告警/异常文案不得回显敏感配置值（仅 backend 名 + 编译开关名）
TEST(StrictBackendsTest, ThrowMessageHasNoSecrets) {
    try {
        PipelineAssembler::enforceBackendActive(
            "persistent", "postgres", "memory", "ENABLE_PG", false, true);
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        // 只允许出现 backend 名与开关名，绝不含 url/host/password 等
        EXPECT_EQ(msg.find("password"), std::string::npos);
        EXPECT_EQ(msg.find("postgres://"), std::string::npos);
        EXPECT_EQ(msg.find("@"), std::string::npos);
        EXPECT_NE(msg.find("postgres"), std::string::npos);   // backend 名可出现
        EXPECT_NE(msg.find("ENABLE_PG"), std::string::npos);  // 开关名可出现
    }
}

// --- Config::strictBackends()：默认 / YAML / env 覆盖 ---

TEST(StrictBackendsTest, DefaultIsTrueWhenSectionMissing) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString("server:\n  port: 8080\n"));
    ::unsetenv("AEGISGATE_STRICT_BACKENDS");
    EXPECT_TRUE(cfg.strictBackends());
}

TEST(StrictBackendsTest, YamlFalseDisables) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString("storage:\n  strict_backends: false\n"));
    ::unsetenv("AEGISGATE_STRICT_BACKENDS");
    EXPECT_FALSE(cfg.strictBackends());
}

TEST(StrictBackendsTest, EnvOverridesYaml) {
    Config cfg;
    ASSERT_TRUE(cfg.loadFromString("storage:\n  strict_backends: true\n"));
    ::setenv("AEGISGATE_STRICT_BACKENDS", "0", 1);
    EXPECT_FALSE(cfg.strictBackends());
    ::setenv("AEGISGATE_STRICT_BACKENDS", "1", 1);
    EXPECT_TRUE(cfg.strictBackends());
    ::unsetenv("AEGISGATE_STRICT_BACKENDS");
}

}  // namespace
