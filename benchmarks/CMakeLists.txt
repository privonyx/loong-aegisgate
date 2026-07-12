find_package(benchmark REQUIRED)

set(BENCH_TARGETS
    bench_rate_limiter
    bench_gateway
    bench_guardrail
    bench_cache
    bench_metrics
    bench_pipeline
)

foreach(bench ${BENCH_TARGETS})
    add_executable(${bench} ${bench}.cpp)
    target_link_libraries(${bench} PRIVATE
        aegisgate_core
        benchmark::benchmark
        benchmark::benchmark_main
    )
endforeach()
