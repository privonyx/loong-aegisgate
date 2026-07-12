// REV20260707-I14 (D2 Option A) Layer 2 integration tests.
// Verifies pre-stream multi-key retry on OpenAI and Claude connectors via a
// FakeStreamingUpstream that synchronously replays a scripted sequence of
// modes (transport error / non-200 status / mid-stream / success).

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <aegisgate/error_codes.h>
#include "gateway/connector/claude.h"
#include "gateway/connector/openai.h"
#include "gateway/connector/upstream_client.h"

using namespace aegisgate;

namespace {

class FakeStreamingUpstream : public UpstreamClient {
public:
    enum class Mode {
        PRE_STREAM_ERROR,   // onError immediately, zero chunks
        PRE_STREAM_STATUS,  // onDone(status, body) immediately, zero chunks
        MID_STREAM_ERROR,   // onChunk once, then onError
        SUCCESS,            // onChunk × 2, then onDone(200, body with usage)
    };

    struct Step {
        Mode mode;
        int status = 0;
    };

    void setScript(std::vector<Step> script) {
        script_ = std::move(script);
        attempt_ = 0;
    }

    int attempts() const { return attempt_; }

    void send(UpstreamRequest, ResponseCallback, ErrorCallback) override {
        ADD_FAILURE() << "FakeStreamingUpstream::send should not be called";
    }

    void sendStreaming(UpstreamRequest req, ChunkCallback onChunk,
                       ResponseCallback onDone, ErrorCallback onError) override {
        ASSERT_LT(static_cast<size_t>(attempt_), script_.size())
            << "FakeStreamingUpstream ran out of scripted steps";
        auto step = script_[attempt_++];
        last_req_ = std::move(req);
        switch (step.mode) {
            case Mode::PRE_STREAM_ERROR:
                onError("mock transport error");
                break;
            case Mode::PRE_STREAM_STATUS:
                onDone(step.status, "mock error body");
                break;
            case Mode::MID_STREAM_ERROR:
                onChunk("data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n");
                onError("mock mid-stream error");
                break;
            case Mode::SUCCESS:
                onChunk("data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n");
                onChunk("data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n");
                onDone(200,
                    "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n"
                    "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n"
                    "data: {\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}\n"
                    "data: [DONE]\n");
                break;
        }
    }

private:
    std::vector<Step> script_;
    int attempt_ = 0;
    UpstreamRequest last_req_;
};

ProviderConfig makeOpenAIConfig(int max_retries = 2) {
    ProviderConfig cfg;
    cfg.name = "openai";
    cfg.type = "openai";
    cfg.base_url = "https://api.openai.com/v1";
    cfg.api_keys = {{"sk-key-a", 1}, {"sk-key-b", 1}, {"sk-key-c", 1}};
    cfg.models = {
        {"gpt-4o", "openai", 0.005, 0.015, 128000, {"stream"},
         {{Capability::Streaming}}}
    };
    cfg.timeout_ms = 30000;
    cfg.max_retries = max_retries;
    return cfg;
}

ProviderConfig makeClaudeConfig(int max_retries = 2) {
    ProviderConfig cfg;
    cfg.name = "claude";
    cfg.type = "claude";
    cfg.base_url = "https://api.anthropic.com/v1";
    cfg.api_keys = {{"sk-ant-a", 1}, {"sk-ant-b", 1}, {"sk-ant-c", 1}};
    cfg.models = {
        {"claude-sonnet-4-20250514", "claude", 0.003, 0.015, 200000,
         {"stream"}, {{Capability::Streaming}}}
    };
    cfg.timeout_ms = 30000;
    cfg.max_retries = max_retries;
    return cfg;
}

struct StreamCollector {
    std::atomic<int> deltas{0};
    std::atomic<int> done_calls{0};
    std::atomic<int> error_calls{0};
    int last_error_status = 0;
    std::string last_error_message;

    std::function<void(const StreamDelta&)> deltaCb() {
        return [this](const StreamDelta&) { deltas.fetch_add(1); };
    }
    std::function<void(const TokenUsage&)> doneCb() {
        return [this](const TokenUsage&) { done_calls.fetch_add(1); };
    }
    std::function<void(const GatewayError&)> errorCb() {
        return [this](const GatewayError& err) {
            error_calls.fetch_add(1);
            last_error_status = err.http_status;
            last_error_message = err.message;
        };
    }
};

} // namespace

// ---------------------------------------------------------------------------
// OpenAI Layer 2 integration tests (SR-3 / SR-4 / SR-6)
// ---------------------------------------------------------------------------

TEST(ConnectorStreamRetryTest, OpenAIPreStreamError_RetriesNextKey_SR3a) {
    auto fake = std::make_unique<FakeStreamingUpstream>();
    fake->setScript({
        {FakeStreamingUpstream::Mode::PRE_STREAM_ERROR, 0},
        {FakeStreamingUpstream::Mode::PRE_STREAM_ERROR, 0},
        {FakeStreamingUpstream::Mode::PRE_STREAM_ERROR, 0},
    });
    auto* fake_raw = fake.get();
    OpenAIConnector conn(makeOpenAIConfig(/*max_retries=*/2), std::move(fake));

    StreamCollector c;
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};
    conn.streamComplete(req, c.deltaCb(), c.doneCb(), c.errorCb());

    // 3 attempts (initial + max_retries=2 retries) then NoHealthyKeys/status.
    EXPECT_EQ(fake_raw->attempts(), 3);
    EXPECT_EQ(c.done_calls.load(), 0);
    EXPECT_EQ(c.error_calls.load(), 1);
    EXPECT_EQ(c.deltas.load(), 0);
    // Transport-only failures never set last_status > 0, so we surface
    // the NoHealthyKeys code (mirrors complete()).
    EXPECT_EQ(c.last_error_status, toHttpStatus(ErrorCode::NoHealthyKeys));
}

TEST(ConnectorStreamRetryTest, OpenAIPreStreamStatus429_RetriesNextKey_SR3b) {
    auto fake = std::make_unique<FakeStreamingUpstream>();
    fake->setScript({
        {FakeStreamingUpstream::Mode::PRE_STREAM_STATUS, 429},
        {FakeStreamingUpstream::Mode::SUCCESS, 200},
    });
    auto* fake_raw = fake.get();
    OpenAIConnector conn(makeOpenAIConfig(/*max_retries=*/2), std::move(fake));

    StreamCollector c;
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};
    conn.streamComplete(req, c.deltaCb(), c.doneCb(), c.errorCb());

    EXPECT_EQ(fake_raw->attempts(), 2);
    EXPECT_EQ(c.done_calls.load(), 1);
    EXPECT_EQ(c.error_calls.load(), 0);
    EXPECT_GT(c.deltas.load(), 0);  // SUCCESS emits 2 chunks
}

TEST(ConnectorStreamRetryTest, OpenAIMidStreamError_DoesNotRetry_SR4) {
    auto fake = std::make_unique<FakeStreamingUpstream>();
    fake->setScript({
        {FakeStreamingUpstream::Mode::MID_STREAM_ERROR, 0},
        // If retry incorrectly triggered, this step would fire — the
        // assertion below on attempts() == 1 catches the drift.
        {FakeStreamingUpstream::Mode::SUCCESS, 200},
    });
    auto* fake_raw = fake.get();
    OpenAIConnector conn(makeOpenAIConfig(/*max_retries=*/2), std::move(fake));

    StreamCollector c;
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};
    conn.streamComplete(req, c.deltaCb(), c.doneCb(), c.errorCb());

    // Exactly one attempt: mid-stream failure is not retryable.
    EXPECT_EQ(fake_raw->attempts(), 1);
    EXPECT_EQ(c.error_calls.load(), 1);
    EXPECT_EQ(c.done_calls.load(), 0);
    // The single delta that MID_STREAM_ERROR emitted must have been
    // delivered to the client (stream committed).
    EXPECT_EQ(c.deltas.load(), 1);
}

TEST(ConnectorStreamRetryTest, OpenAIMaxAttemptsBoundary_ThrowsAfterExhaustion_SR6) {
    auto fake = std::make_unique<FakeStreamingUpstream>();
    fake->setScript({
        {FakeStreamingUpstream::Mode::PRE_STREAM_STATUS, 429},
        {FakeStreamingUpstream::Mode::PRE_STREAM_STATUS, 429},
    });
    auto* fake_raw = fake.get();
    // max_retries=1 -> max_attempts=2
    OpenAIConnector conn(makeOpenAIConfig(/*max_retries=*/1), std::move(fake));

    StreamCollector c;
    ChatRequest req;
    req.model = "gpt-4o";
    req.messages = {{"user", "hi"}};
    conn.streamComplete(req, c.deltaCb(), c.doneCb(), c.errorCb());

    EXPECT_EQ(fake_raw->attempts(), 2);
    EXPECT_EQ(c.done_calls.load(), 0);
    EXPECT_EQ(c.error_calls.load(), 1);
    EXPECT_EQ(c.last_error_status, 429);
}

// ---------------------------------------------------------------------------
// Claude Layer 2 integration tests (SR-5) — will be added in Epic 4
// ---------------------------------------------------------------------------

TEST(ConnectorStreamRetryTest, ClaudePreStreamError_RetriesNextKey_SR5a) {
    auto fake = std::make_unique<FakeStreamingUpstream>();
    fake->setScript({
        {FakeStreamingUpstream::Mode::PRE_STREAM_ERROR, 0},
        {FakeStreamingUpstream::Mode::PRE_STREAM_STATUS, 500},
        {FakeStreamingUpstream::Mode::SUCCESS, 200},
    });
    auto* fake_raw = fake.get();
    ClaudeConnector conn(makeClaudeConfig(/*max_retries=*/2), std::move(fake));

    StreamCollector c;
    ChatRequest req;
    req.model = "claude-sonnet-4-20250514";
    req.messages = {{"user", "hi"}};
    conn.streamComplete(req, c.deltaCb(), c.doneCb(), c.errorCb());

    EXPECT_EQ(fake_raw->attempts(), 3);
    EXPECT_EQ(c.done_calls.load(), 1);
    EXPECT_EQ(c.error_calls.load(), 0);
}

TEST(ConnectorStreamRetryTest, ClaudeMidStreamError_DoesNotRetry_SR5b) {
    auto fake = std::make_unique<FakeStreamingUpstream>();
    fake->setScript({
        {FakeStreamingUpstream::Mode::MID_STREAM_ERROR, 0},
        {FakeStreamingUpstream::Mode::SUCCESS, 200},
    });
    auto* fake_raw = fake.get();
    ClaudeConnector conn(makeClaudeConfig(/*max_retries=*/2), std::move(fake));

    StreamCollector c;
    ChatRequest req;
    req.model = "claude-sonnet-4-20250514";
    req.messages = {{"user", "hi"}};
    conn.streamComplete(req, c.deltaCb(), c.doneCb(), c.errorCb());

    EXPECT_EQ(fake_raw->attempts(), 1);
    EXPECT_EQ(c.error_calls.load(), 1);
    EXPECT_EQ(c.done_calls.load(), 0);
}
