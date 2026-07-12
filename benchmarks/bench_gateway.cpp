#include <benchmark/benchmark.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gateway/circuit_breaker.h"
#include "gateway/balancer.h"

using aegisgate::Balancer;
using aegisgate::CircuitBreaker;

static void BM_CircuitBreaker_AllowClosed(benchmark::State& state) {
    CircuitBreaker cb;
    const std::string model = "model";
    cb.recordSuccess(model);
    for (auto _ : state) {
        benchmark::DoNotOptimize(cb.allowRequest(model));
    }
}

static void BM_CircuitBreaker_RecordSuccess(benchmark::State& state) {
    CircuitBreaker cb;
    const std::string model = "model";
    for (auto _ : state) {
        cb.recordSuccess(model);
        benchmark::DoNotOptimize(cb.state(model));
    }
}

static void BM_CircuitBreaker_AllowClosed_MT(benchmark::State& state) {
    static CircuitBreaker cb;
    static std::once_flag once;
    std::call_once(once, [] {
        cb.recordSuccess("m");
    });
    for (auto _ : state) {
        benchmark::DoNotOptimize(cb.allowRequest("m"));
    }
}

static void BM_CircuitBreaker_AllowDiffModels_MT(benchmark::State& state) {
    static CircuitBreaker cb;
    const std::string model = "m" + std::to_string(state.thread_index());
    cb.recordSuccess(model);
    for (auto _ : state) {
        benchmark::DoNotOptimize(cb.allowRequest(model));
    }
}

static void BM_Balancer_NextKey_3(benchmark::State& state) {
    Balancer b({{"a", 1}, {"b", 2}, {"c", 3}});
    for (auto _ : state) {
        benchmark::DoNotOptimize(b.nextKey());
    }
}

static void BM_Balancer_NextKey_10(benchmark::State& state) {
    std::vector<std::pair<std::string, int>> keys;
    keys.reserve(10);
    for (int i = 0; i < 10; ++i) {
        keys.emplace_back("k" + std::to_string(i), 1);
    }
    Balancer b(keys);
    for (auto _ : state) {
        benchmark::DoNotOptimize(b.nextKey());
    }
}

static void BM_Balancer_NextKey_MT(benchmark::State& state) {
    static Balancer b({
        {"k0", 1},
        {"k1", 1},
        {"k2", 1},
        {"k3", 1},
        {"k4", 1},
    });
    for (auto _ : state) {
        benchmark::DoNotOptimize(b.nextKey());
    }
}

static void BM_Balancer_ReportSuccess(benchmark::State& state) {
    Balancer b({{"k0", 1}, {"k1", 2}, {"k2", 3}});
    for (auto _ : state) {
        b.reportSuccess("k1");
    }
}

static void BM_Balancer_ReportFailure(benchmark::State& state) {
    Balancer b({{"k0", 1}, {"k1", 2}, {"k2", 3}});
    for (auto _ : state) {
        b.reportFailure("k1");
        b.reportSuccess("k1");
    }
}

static void BM_Balancer_ReportSuccess_MT(benchmark::State& state) {
    static Balancer b({{"k0", 1}, {"k1", 1}, {"k2", 1}, {"k3", 1}, {"k4", 1}});
    const std::string key = "k" + std::to_string(state.thread_index() % 5);
    for (auto _ : state) {
        b.reportSuccess(key);
    }
}

BENCHMARK(BM_CircuitBreaker_AllowClosed);
BENCHMARK(BM_CircuitBreaker_RecordSuccess);
BENCHMARK(BM_CircuitBreaker_AllowClosed_MT)->Threads(1)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK(BM_CircuitBreaker_AllowDiffModels_MT)->Threads(1)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK(BM_Balancer_NextKey_3);
BENCHMARK(BM_Balancer_NextKey_10);
BENCHMARK(BM_Balancer_NextKey_MT)->Threads(1)->Threads(2)->Threads(4)->Threads(8);
BENCHMARK(BM_Balancer_ReportSuccess);
BENCHMARK(BM_Balancer_ReportFailure);
BENCHMARK(BM_Balancer_ReportSuccess_MT)->Threads(1)->Threads(2)->Threads(4)->Threads(8);
