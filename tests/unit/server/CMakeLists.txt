add_executable(test_gateway_runtime test_gateway_runtime.cpp)
target_link_libraries(test_gateway_runtime PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME GatewayRuntimeTest COMMAND test_gateway_runtime WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# TASK-20260708-02 / REV20260707-C1 — SR-1 Redis rate limiter wiring predicate.
add_executable(test_gateway_runtime_rate_limiter_wiring
               test_gateway_runtime_rate_limiter_wiring.cpp)
target_link_libraries(test_gateway_runtime_rate_limiter_wiring PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME GatewayRuntimeRateLimiterWiringTest
         COMMAND test_gateway_runtime_rate_limiter_wiring
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# TASK-20260711-01 / REV20260707-I13 — AdvancedRouting edition gate predicate.
add_executable(test_gateway_runtime_edition_gate
               test_gateway_runtime_edition_gate.cpp)
target_link_libraries(test_gateway_runtime_edition_gate PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME GatewayRuntimeEditionGateTest
         COMMAND test_gateway_runtime_edition_gate
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_budget_guard_stage test_budget_guard_stage.cpp)
target_link_libraries(test_budget_guard_stage PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME BudgetGuardStageTest COMMAND test_budget_guard_stage)

add_executable(test_sse_response test_sse_response.cpp)
target_link_libraries(test_sse_response PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME SseResponseTest COMMAND test_sse_response)

add_executable(test_feedback_bus_wiring test_feedback_bus_wiring.cpp)
target_link_libraries(test_feedback_bus_wiring PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME FeedbackBusWiringTest COMMAND test_feedback_bus_wiring)

add_executable(test_runtime_lifecycle test_runtime_lifecycle.cpp)
target_link_libraries(test_runtime_lifecycle PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME RuntimeLifecycleTest COMMAND test_runtime_lifecycle WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# TASK-20260703-04 Epic 1 (D1/SR-1) — /v1/agent auth + opt-in gate contract.
add_executable(test_agent_endpoint test_agent_endpoint.cpp)
target_link_libraries(test_agent_endpoint PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AgentEndpointTest COMMAND test_agent_endpoint WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_admin_controller test_admin_controller.cpp)
target_link_libraries(test_admin_controller PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AdminControllerTest COMMAND test_admin_controller)

# Phase 11.5 (TASK-20260518-02 E5.1) — AdminController autonomy endpoints.
add_executable(test_admin_controller_autonomy test_admin_controller_autonomy.cpp)
target_link_libraries(test_admin_controller_autonomy PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AdminControllerAutonomyTest COMMAND test_admin_controller_autonomy)

add_executable(test_sso_integration test_sso_integration.cpp)
target_link_libraries(test_sso_integration PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME SsoIntegrationTest COMMAND test_sso_integration)

add_executable(test_mfa_integration test_mfa_integration.cpp)
target_link_libraries(test_mfa_integration PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME MfaIntegrationTest COMMAND test_mfa_integration)

add_executable(test_phase3_integration test_phase3_integration.cpp)
target_link_libraries(test_phase3_integration PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME Phase3IntegrationTest COMMAND test_phase3_integration)

add_executable(test_proxy_security test_proxy_security.cpp)
target_link_libraries(test_proxy_security PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ProxySecurityTest COMMAND test_proxy_security WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_router_outcome_publisher test_router_outcome_publisher.cpp)
target_link_libraries(test_router_outcome_publisher PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME RouterOutcomePublisherTest COMMAND test_router_outcome_publisher)

add_executable(test_admin_savings test_admin_savings.cpp)
target_link_libraries(test_admin_savings PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AdminSavingsTest COMMAND test_admin_savings)

# TASK-20260527-02 — MVP-5 prep / Case Study Numbers endpoint.
add_executable(test_admin_case_study_endpoint test_admin_case_study_endpoint.cpp)
target_link_libraries(test_admin_case_study_endpoint PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AdminCaseStudyTest COMMAND test_admin_case_study_endpoint)

# TASK-20260602-01 Epic 2 — CaseStudySnapshotBuilder shared helper.
add_executable(test_case_study_builder test_case_study_builder.cpp)
target_link_libraries(test_case_study_builder PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME CaseStudyBuilderTest COMMAND test_case_study_builder)

# TASK-20260603-02 — admin_session shared resolver + scope predicates (P0-1/2/3).
add_executable(test_admin_session test_admin_session.cpp)
target_link_libraries(test_admin_session PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AdminSessionTest COMMAND test_admin_session)

# TASK-20260702-02 P2-1 — CORS decision pure function (wildcard vs credentials).
add_executable(test_admin_http_helpers test_admin_http_helpers.cpp)
target_link_libraries(test_admin_http_helpers PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AdminHttpHelpersTest COMMAND test_admin_http_helpers)

add_executable(test_modality_quota_enforcer test_modality_quota_enforcer.cpp)
target_link_libraries(test_modality_quota_enforcer PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ModalityQuotaEnforcerTest COMMAND test_modality_quota_enforcer)

# TASK-20260609-02 P1-4 — proxy / multimodal billing helper.
add_executable(test_proxy_billing test_proxy_billing.cpp)
target_link_libraries(test_proxy_billing PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ProxyBillingTest COMMAND test_proxy_billing WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# TASK-20260609-02 P1-10 — cache savings decoupled from prompt compression.
add_executable(test_cache_savings test_cache_savings.cpp)
target_link_libraries(test_cache_savings PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME CacheSavingsTest COMMAND test_cache_savings)

# TASK-20260609-02 P1-8/P1-9 — response header passthrough (budget/AB).
add_executable(test_response_headers test_response_headers.cpp)
target_link_libraries(test_response_headers PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ResponseHeadersTest COMMAND test_response_headers)

# TASK-20260609-02 P1-9 — A/B experiment attribution (metric + header).
add_executable(test_ab_attribution test_ab_attribution.cpp)
target_link_libraries(test_ab_attribution PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AbAttributionTest COMMAND test_ab_attribution)

# TASK-20260613-01 — ctest -j 并行争用根治。
# 这些测试装配整条 GatewayRuntime pipeline（GatewayRuntime::instance()）并在
# 共享 WORKING_DIRECTORY(${CMAKE_SOURCE_DIR}) 下读写固定路径的磁盘状态
# （审计日志 / sqlite / 语义缓存索引）。并行运行会争用同一磁盘状态 + 叠加
# 内存峰值，导致 ctest -j2 卡死或 OOM(137)。FeedbackBusWiringTest 的 drain
# 计时断言在系统抖动下同样卡死。统一挂同名 RESOURCE_LOCK "gateway_runtime"
# （与 tests/integration/CMakeLists.txt 共享同名锁），令调度器保证这些测试两
# 两不并行；其余轻量测试仍可与其并行，整体 -j2 安全。
set_tests_properties(
    ProxySecurityTest
    RuntimeLifecycleTest
    AgentEndpointTest
    FeedbackBusWiringTest
    PROPERTIES RESOURCE_LOCK "gateway_runtime")
