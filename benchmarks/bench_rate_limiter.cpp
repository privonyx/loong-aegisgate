#include <benchmark/benchmark.h>
#include <string>

#include "gateway/rate_limiter.h"

namespace {

aegisgate::RateLimiter::Config HugeBucketConfig() {
    return {1e9, 1e9};
}

} // namespace

static void BM_RateLimiter_Allow_SingleKey(benchmark::State& state) {
    aegisgate::RateLimiter limiter(HugeBucketConfig());
    const std::string key = "k";
    for (auto _ : state) {
        auto ok = limiter.allow(key);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_Allow_SingleKey);

static void BM_RateLimiter_Allow_SameKey_MT(benchmark::State& state) {
    static aegisgate::RateLimiter limiter(HugeBucketConfig());
    const std::string key = "shared";
    for (auto _ : state) {
        auto ok = limiter.allow(key);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_Allow_SameKey_MT)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16);

static void BM_RateLimiter_Allow_DiffKeys_MT(benchmark::State& state) {
    static aegisgate::RateLimiter limiter(HugeBucketConfig());
    const std::string key = "t" + std::to_string(state.thread_index());
    for (auto _ : state) {
        auto ok = limiter.allow(key);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_Allow_DiffKeys_MT)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16);

static void BM_RateLimiter_SetKeyConfig(benchmark::State& state) {
    aegisgate::RateLimiter limiter(HugeBucketConfig());
    const std::string key = "k";
    const auto cfg = HugeBucketConfig();
    for (auto _ : state) {
        limiter.setKeyConfig(key, cfg);
        benchmark::DoNotOptimize(limiter);
    }
}
BENCHMARK(BM_RateLimiter_SetKeyConfig);

static void BM_RateLimiter_Remaining(benchmark::State& state) {
    aegisgate::RateLimiter limiter(HugeBucketConfig());
    const std::string key = "k";
    for (auto _ : state) {
        auto r = limiter.remaining(key);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_RateLimiter_Remaining);
