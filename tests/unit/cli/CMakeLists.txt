add_executable(test_migrate test_migrate.cpp)
target_link_libraries(test_migrate PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME MigrateTest COMMAND test_migrate)

add_executable(test_bench_stats test_bench_stats.cpp)
target_link_libraries(test_bench_stats PRIVATE GTest::gtest_main)
target_include_directories(test_bench_stats PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME BenchStatsTest COMMAND test_bench_stats)

add_executable(test_replay test_replay.cpp)
target_link_libraries(test_replay PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ReplayTest COMMAND test_replay)

add_executable(test_cache_cli test_cache_cli.cpp)
target_link_libraries(test_cache_cli PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME CacheCliTest COMMAND test_cache_cli)

add_executable(test_estimate_cli test_estimate_cli.cpp)
target_link_libraries(test_estimate_cli PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME EstimateCliTest COMMAND test_estimate_cli)
