add_executable(test_e2e test_e2e.cpp)
target_link_libraries(test_e2e PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME E2ETest COMMAND test_e2e WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_security test_security.cpp)
target_link_libraries(test_security PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME SecurityTest COMMAND test_security WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(SecurityTest PROPERTIES TIMEOUT 60)

add_executable(test_streaming test_streaming.cpp)
target_link_libraries(test_streaming PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME StreamingTest COMMAND test_streaming WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_chaos test_chaos.cpp)
target_link_libraries(test_chaos PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ChaosTest COMMAND test_chaos WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# TASK-20260613-01 — 见 tests/unit/server/CMakeLists.txt 注释。ChaosTest 通过
# GatewayRuntime::instance() 装配整条 runtime 并复用共享磁盘状态，挂同名锁
# "gateway_runtime" 防止与 server 重测试并行时争用 / OOM。
set_tests_properties(ChaosTest PROPERTIES RESOURCE_LOCK "gateway_runtime")

add_executable(test_rbac_e2e test_rbac_e2e.cpp)
target_link_libraries(test_rbac_e2e PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME RbacE2ETest COMMAND test_rbac_e2e)

add_executable(test_tenant_isolation test_tenant_isolation.cpp)
target_link_libraries(test_tenant_isolation PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME TenantIsolationTest COMMAND test_tenant_isolation)

add_executable(test_external_safety test_external_safety.cpp)
target_link_libraries(test_external_safety PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ExternalSafetyTest COMMAND test_external_safety WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Phase 11.5 (TASK-20260518-02 E4.3) — GatewayRuntime autonomy wiring tests.
add_executable(test_gateway_runtime_autonomy test_gateway_runtime_autonomy.cpp)
target_link_libraries(test_gateway_runtime_autonomy PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME GatewayRuntimeAutonomyTest
         COMMAND test_gateway_runtime_autonomy
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(GatewayRuntimeAutonomyTest PROPERTIES TIMEOUT 60)
# TASK-20260613-01 — 同 ChaosTest，挂同名锁防止与 server 重 runtime 测试并行。
set_tests_properties(GatewayRuntimeAutonomyTest PROPERTIES RESOURCE_LOCK "gateway_runtime")

# Phase 11.2 (TASK-20260521-03 Epic 5) — BanditRouter shadow-to-live integration.
add_executable(test_bandit_shadow_to_live test_bandit_shadow_to_live.cpp)
target_link_libraries(test_bandit_shadow_to_live PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME BanditShadowToLiveIntegrationTest
         COMMAND test_bandit_shadow_to_live
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(BanditShadowToLiveIntegrationTest PROPERTIES TIMEOUT 60)
