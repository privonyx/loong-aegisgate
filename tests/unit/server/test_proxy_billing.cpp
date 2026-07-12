#include <gtest/gtest.h>
#include "server/proxy_billing.h"
#include "observe/cost_tracker.h"

using namespace aegisgate;

// --- P1-4: proxy / multimodal billing helpers ---

TEST(ProxyBillingTest, ParsesEmbeddingsUsage) {
    // OpenAI embeddings response shape: usage carries prompt + total only.
    std::string body = R"({"object":"list","data":[],"usage":{"prompt_tokens":42,"total_tokens":42}})";
    auto [in_tok, out_tok] = parseProxyUsageTokens(body);
    EXPECT_EQ(in_tok, 42);
    EXPECT_EQ(out_tok, 0);
}

TEST(ProxyBillingTest, ParsesChatStyleUsage) {
    std::string body = R"({"usage":{"prompt_tokens":10,"completion_tokens":7,"total_tokens":17}})";
    auto [in_tok, out_tok] = parseProxyUsageTokens(body);
    EXPECT_EQ(in_tok, 10);
    EXPECT_EQ(out_tok, 7);
}

TEST(ProxyBillingTest, NoUsageYieldsZero) {
    // Image generation responses typically carry no usage object.
    std::string body = R"({"created":1,"data":[{"url":"http://x/y.png"}]})";
    auto [in_tok, out_tok] = parseProxyUsageTokens(body);
    EXPECT_EQ(in_tok, 0);
    EXPECT_EQ(out_tok, 0);
}

TEST(ProxyBillingTest, MalformedBodyDoesNotThrow) {
    auto [in_tok, out_tok] = parseProxyUsageTokens("not-json{{{");
    EXPECT_EQ(in_tok, 0);
    EXPECT_EQ(out_tok, 0);
}

TEST(ProxyBillingTest, BuildsEmbeddingCostRecordWithModality) {
    CostTracker tracker;
    tracker.setPricing("text-embedding-3-small", 0.00002, 0.0);

    ProxyResponse resp;
    resp.http_status = 200;
    resp.body = R"({"usage":{"prompt_tokens":1000,"total_tokens":1000}})";

    auto rec = buildProxyCostRecord(tracker, "/v1/embeddings",
                                    "text-embedding-3-small", "tenant-x",
                                    "proxy-123", "2026-06-09T00:00:00Z", resp);
    EXPECT_EQ(rec.modality, "embedding");
    EXPECT_EQ(rec.input_tokens, 1000);
    EXPECT_EQ(rec.tenant_id, "tenant-x");
    EXPECT_EQ(rec.request_id, "proxy-123");
    EXPECT_EQ(rec.routing_decision_reason, "proxy_passthrough");
    // 1000/1000 * 0.00002 = 0.00002
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.00002);
}

TEST(ProxyBillingTest, BuildsImageGenRecordModalityWithoutUsage) {
    CostTracker tracker;
    ProxyResponse resp;
    resp.http_status = 200;
    resp.body = R"({"created":1,"data":[{"url":"http://x/y.png"}]})";

    auto rec = buildProxyCostRecord(tracker, "/v1/images/generations",
                                    "dall-e-3", "tenant-y", "proxy-456",
                                    "2026-06-09T00:00:00Z", resp);
    EXPECT_EQ(rec.modality, "image_gen");
    EXPECT_EQ(rec.input_tokens, 0);
    EXPECT_EQ(rec.output_tokens, 0);
    // No pricing entry -> best-effort 0 cost, but the record still exists with
    // the right modality (summaryByModality stops being dead code).
    EXPECT_DOUBLE_EQ(rec.total_cost, 0.0);
}

// Recording a proxy cost record routes through CostTracker::summaryByModality,
// proving the previously-dead per-modality attribution path works (P1-4).
TEST(ProxyBillingTest, RecordedProxyCostShowsInModalitySummary) {
    CostTracker tracker;
    tracker.setPricing("text-embedding-3-small", 0.00002, 0.0);

    ProxyResponse resp;
    resp.body = R"({"usage":{"prompt_tokens":500,"total_tokens":500}})";
    auto rec = buildProxyCostRecord(tracker, "/v1/embeddings",
                                    "text-embedding-3-small", "tenant-z",
                                    "proxy-789", "2026-06-09T00:00:00Z", resp);
    tracker.record(rec);

    auto summary = tracker.summaryByModality("embedding");
    EXPECT_EQ(summary.request_count, 1);
    EXPECT_EQ(summary.total_input_tokens, 500);
}
