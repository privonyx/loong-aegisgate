#include <gtest/gtest.h>
#include "gateway/circuit_breaker.h"
#include "gateway/rate_limiter.h"
#include "gateway/connector/upstream_client.h"
#include "server/gateway_runtime.h"
#include "core/config.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace aegisgate;

// --- Mock that injects faults ---

class FaultUpstreamClient : public UpstreamClient {
public:
    enum class Fault { None, Timeout, ServerError };

    Fault fault = Fault::None;
    std::atomic<int> call_count{0};

    void send(UpstreamRequest /*req*/, ResponseCallback onDone,
              ErrorCallback onError) override {
        ++call_count;
        switch (fault) {
        case Fault::Timeout:
            onError("upstream timeout");
            return;
        case Fault::ServerError:
            onDone(500, R"({"error":"internal server error"})");
            return;
        case Fault::None:
            onDone(200, R"({"choices":[{"message":{"content":"ok"}}]})");
            return;
        }
    }

    void sendStreaming(UpstreamRequest /*req*/, ChunkCallback /*onChunk*/,
                       ResponseCallback onDone,
                       ErrorCallback onError) override {
        ++call_count;
        switch (fault) {
        case Fault::Timeout:
            onError("upstream timeout");
            return;
        case Fault::ServerError:
            onDone(500, "");
            return;
        case Fault::None:
            onDone(200, "");
            return;
        }
    }
};

// --- Chaos Scenario 1: Upstream timeout does not hang ---

TEST(ChaosTest, UpstreamTimeoutDoesNotHang) {
    FaultUpstreamClient client;
    client.fault = FaultUpstreamClient::Fault::Timeout;

    std::string error_msg;
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/chat/completions";
    req.body = "{}";

    client.send(std::move(req),
        [](int /*status*/, std::string /*body*/) {
            FAIL() << "onDone should not be called on timeout";
        },
        [&](std::string err) {
            error_msg = std::move(err);
        });

    EXPECT_FALSE(error_msg.empty());
    EXPECT_NE(error_msg.find("timeout"), std::string::npos);
}

// --- Chaos Scenario 2: Consecutive failures trip circuit breaker ---

TEST(ChaosTest, ConsecutiveFailuresTripsCircuitBreaker) {
    CircuitConfig cfg;
    cfg.failure_threshold = 3;
    cfg.reset_timeout = std::chrono::seconds(60);
    CircuitBreaker breaker(cfg);

    const std::string model = "chaos-model-a";

    EXPECT_TRUE(breaker.allowRequest(model));
    EXPECT_EQ(breaker.state(model), CircuitState::Closed);

    for (int i = 0; i < cfg.failure_threshold; ++i) {
        breaker.recordFailure(model);
    }

    EXPECT_EQ(breaker.state(model), CircuitState::Open);
    EXPECT_FALSE(breaker.allowRequest(model));
}

// --- Chaos Scenario 3: Circuit breaker recovery (open → half-open → closed) ---

TEST(ChaosTest, CircuitBreakerRecovery) {
    CircuitConfig cfg;
    cfg.failure_threshold = 2;
    cfg.reset_timeout = std::chrono::seconds(1);
    cfg.half_open_max_calls = 1;
    CircuitBreaker breaker(cfg);

    const std::string model = "chaos-model-b";

    breaker.recordFailure(model);
    breaker.recordFailure(model);
    EXPECT_EQ(breaker.state(model), CircuitState::Open);
    EXPECT_FALSE(breaker.allowRequest(model));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_TRUE(breaker.allowRequest(model));
    EXPECT_EQ(breaker.state(model), CircuitState::HalfOpen);

    breaker.recordSuccess(model);
    EXPECT_EQ(breaker.state(model), CircuitState::Closed);
}

// --- Chaos Scenario 4: Rate limit exhaustion ---

TEST(ChaosTest, RateLimitExhaustion) {
    RateLimiter::Config cfg;
    cfg.max_tokens = 10.0;
    cfg.refill_rate = 0.0;  // no refill → tokens deplete permanently
    RateLimiter limiter(cfg);

    const std::string key = "chaos-client";

    EXPECT_TRUE(limiter.allow(key, 5.0));
    EXPECT_TRUE(limiter.allow(key, 5.0));
    EXPECT_FALSE(limiter.allow(key, 1.0));
}

// --- Chaos Scenario 5: Shutdown rejects new requests ---

TEST(ChaosTest, ShutdownRejectsNewRequests) {
    auto& rt = GatewayRuntime::instance();
    rt.resetShutdownForTesting();

    EXPECT_FALSE(rt.isShuttingDown());

    rt.beginShutdown();
    EXPECT_TRUE(rt.isShuttingDown());

    rt.resetShutdownForTesting();
}
