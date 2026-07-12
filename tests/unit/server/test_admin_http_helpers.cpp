// TASK-20260702-02 P2-1（SR-1）：CORS 决策纯函数单元测试。
//
// 安全铁律：通配 `*` 不得携带 credentials。此前 applyCorsHeaders 命中 `*` 时
// 回显请求 origin + Access-Control-Allow-Credentials:true → 任意站点可带 cookie
// 跨域访问 /admin/*。decideCors 是可单测的纯决策内核（不依赖 GatewayRuntime）。
//
// 设计：docs/specs/2026-07-02-admin-p2-hardening-design.md §2 P2-1。

#include "server/admin_session.h"

#include <gtest/gtest.h>

using namespace aegisgate;
using aegisgate::admin::decideCors;

// 通配 `*`：允许放行，但发 `Allow-Origin: *` 且**不**发 credentials。
TEST(CorsDecisionTest, WildcardAllowsWithoutCredentials) {
    auto d = decideCors("http://evil.example.com", {"*"});
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.allow_origin, "*");
    EXPECT_FALSE(d.allow_credentials);
    EXPECT_FALSE(d.vary_origin);
}

// 具体 origin 精确匹配：回显 origin + credentials + Vary: Origin。
TEST(CorsDecisionTest, ExactOriginGetsCredentialsAndVary) {
    auto d = decideCors("https://app.example.com",
                        {"https://app.example.com"});
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.allow_origin, "https://app.example.com");
    EXPECT_TRUE(d.allow_credentials);
    EXPECT_TRUE(d.vary_origin);
}

// 同时配 `*` 与具体 origin：精确匹配优先，仍给 credentials（不因 `*` 降级）。
TEST(CorsDecisionTest, ExactMatchTakesPrecedenceOverWildcard) {
    auto d = decideCors("https://app.example.com",
                        {"*", "https://app.example.com"});
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.allow_origin, "https://app.example.com");
    EXPECT_TRUE(d.allow_credentials);
}

// 不在名单：不放行。
TEST(CorsDecisionTest, UnlistedOriginDenied) {
    auto d = decideCors("https://evil.example.com",
                        {"https://app.example.com"});
    EXPECT_FALSE(d.allowed);
    EXPECT_FALSE(d.allow_credentials);
}

// 空 Origin：不放行（无 Origin 头的同源/非浏览器请求不加 CORS 头）。
TEST(CorsDecisionTest, EmptyOriginDenied) {
    auto d = decideCors("", {"*"});
    EXPECT_FALSE(d.allowed);
}
