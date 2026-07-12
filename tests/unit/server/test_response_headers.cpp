// TASK-20260609-02 P1-8 / P1-9 — surface pipeline-accumulated response headers
// (budget downgrade, A/B variant) to the HTTP transport.

#include "server/response_headers.h"
#include "server/gateway_runtime.h"

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace aegisgate;

namespace {

// Minimal stand-in for drogon::HttpResponse exposing only addHeader, so the
// header-application logic is testable without a live HTTP server.
struct FakeResponse {
    std::vector<std::pair<std::string, std::string>> headers;
    void addHeader(const std::string& k, const std::string& v) {
        headers.emplace_back(k, v);
    }
};

} // namespace

TEST(ResponseHeadersP1_8, AppliesAllHeaders) {
    FakeResponse resp;
    std::unordered_map<std::string, std::string> h{
        {"X-AegisGate-Budget-Guard", "triggered"},
    };
    applyResponseHeaders(resp, h);

    ASSERT_EQ(resp.headers.size(), 1u);
    EXPECT_EQ(resp.headers[0].first, "X-AegisGate-Budget-Guard");
    EXPECT_EQ(resp.headers[0].second, "triggered");
}

TEST(ResponseHeadersP1_8, SkipsEmptyKey) {
    FakeResponse resp;
    std::unordered_map<std::string, std::string> h{{"", "orphan"}, {"K", "v"}};
    applyResponseHeaders(resp, h);

    ASSERT_EQ(resp.headers.size(), 1u);
    EXPECT_EQ(resp.headers[0].first, "K");
}

// The result structs must carry response_headers so processRequest /
// processProxyRequest can hand the budget/AB headers back to the controller.
// The chat result struct must carry response_headers so processRequest can
// hand budget/AB headers back to the controller. (The proxy path bypasses the
// inbound pipeline and has no header-writing stages, so ProxyResult is left
// unchanged to avoid a write-never field — the very anti-pattern this fixes.)
TEST(ResponseHeadersP1_8, ProcessResultCarriesHeaders) {
    ProcessResult r;
    r.response_headers["X-AegisGate-Budget-Guard"] = "triggered";
    EXPECT_EQ(r.response_headers.at("X-AegisGate-Budget-Guard"), "triggered");
}
