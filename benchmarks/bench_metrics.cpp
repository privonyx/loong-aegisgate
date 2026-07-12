#include <benchmark/benchmark.h>
#include <string>
#include <vector>

#include "observe/metrics.h"

namespace {

std::vector<double> HistogramBuckets() {
    return {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
}

} // namespace

static void BM_Counter_Inc(benchmark::State& state) {
    aegisgate::Counter c("bench_counter_inc", "help");
    const aegisgate::LabelSet labels{{{"label", "a"}}, "", false};
    for (auto _ : state) {
        c.inc(labels);
        benchmark::DoNotOptimize(c.get(labels));
    }
}
BENCHMARK(BM_Counter_Inc);

static void BM_Counter_Inc_SameLabel_MT(benchmark::State& state) {
    static aegisgate::Counter c("bench_counter_inc_same_mt", "help");
    const aegisgate::LabelSet labels;
    for (auto _ : state) {
        c.inc(labels);
        benchmark::DoNotOptimize(c.get(labels));
    }
}
BENCHMARK(BM_Counter_Inc_SameLabel_MT)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);

static void BM_Counter_Inc_DiffLabels_MT(benchmark::State& state) {
    static aegisgate::Counter c("bench_counter_inc_diff_mt", "help");
    const aegisgate::LabelSet labels{
        {{ "thread", std::to_string(state.thread_index()) }}, "", false};
    for (auto _ : state) {
        c.inc(labels);
        benchmark::DoNotOptimize(c.get(labels));
    }
}
BENCHMARK(BM_Counter_Inc_DiffLabels_MT)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);

static void BM_Histogram_Observe(benchmark::State& state) {
    aegisgate::Histogram h("bench_histogram_observe", "help", HistogramBuckets());
    const aegisgate::LabelSet labels;
    for (auto _ : state) {
        h.observe(0.42, labels);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_Histogram_Observe);

static void BM_Histogram_Observe_MT(benchmark::State& state) {
    static aegisgate::Histogram h("bench_histogram_observe_mt", "help",
                                   HistogramBuckets());
    const aegisgate::LabelSet labels;
    for (auto _ : state) {
        h.observe(0.42, labels);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_Histogram_Observe_MT)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);

static void BM_Gauge_Set(benchmark::State& state) {
    aegisgate::Gauge g("bench_gauge_set", "help");
    const aegisgate::LabelSet labels;
    for (auto _ : state) {
        g.set(1.0, labels);
        benchmark::DoNotOptimize(g.get(labels));
    }
}
BENCHMARK(BM_Gauge_Set);

static void BM_Gauge_Inc_MT(benchmark::State& state) {
    static aegisgate::Gauge g("bench_gauge_inc_mt", "help");
    const aegisgate::LabelSet labels;
    for (auto _ : state) {
        g.inc(labels);
        benchmark::DoNotOptimize(g.get(labels));
    }
}
BENCHMARK(BM_Gauge_Inc_MT)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);

static void BM_Counter_Expose(benchmark::State& state) {
    aegisgate::Counter c("bench_counter_expose", "help");
    for (int i = 0; i < 10; ++i) {
        c.inc(
            aegisgate::LabelSet{{{"combo", std::to_string(i)}}, "", false});
    }
    for (auto _ : state) {
        std::string s = c.expose();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_Counter_Expose);
