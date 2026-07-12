# Phase 9.3.4 RolloutController (TASK-20260422-01) Epic B.1.
add_executable(test_scope_matcher test_scope_matcher.cpp)
target_link_libraries(test_scope_matcher PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ScopeMatcherTest COMMAND test_scope_matcher)

# Epic B.4.
add_executable(test_clock test_clock.cpp)
target_link_libraries(test_clock PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ClockTest COMMAND test_clock)
