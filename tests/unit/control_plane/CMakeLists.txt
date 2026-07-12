# Phase 9.3 control plane unit tests.
#
# Layered by dependency:
#   * Pure C++ business logic (state machine, ULID, sensitive scanner,
#     diff engine, ConfigServiceCore) — always compiled so the OFF path
#     exercises the same invariants as ON.
#   * Proto / gRPC code — only when ENABLE_CONTROL_PLANE is on since it
#     depends on the generated control_plane_proto library.

# ---- Always-on pure-C++ tests ----

add_executable(test_state_machine test_state_machine.cpp)
target_link_libraries(test_state_machine
    PRIVATE
        aegisgate_core
        GTest::gtest_main
)
add_test(NAME ControlPlaneStateMachineTest COMMAND test_state_machine)

add_executable(test_ulid test_ulid.cpp)
target_link_libraries(test_ulid
    PRIVATE
        aegisgate_core
        GTest::gtest_main
)
add_test(NAME ControlPlaneUlidTest COMMAND test_ulid)

add_executable(test_sensitive_scanner test_sensitive_scanner.cpp)
target_link_libraries(test_sensitive_scanner
    PRIVATE
        aegisgate_core
        GTest::gtest_main
)
add_test(NAME ControlPlaneSensitiveScannerTest COMMAND test_sensitive_scanner)

add_executable(test_diff_engine test_diff_engine.cpp)
target_link_libraries(test_diff_engine
    PRIVATE
        aegisgate_core
        GTest::gtest_main
)
add_test(NAME ControlPlaneDiffEngineTest COMMAND test_diff_engine)

add_executable(test_config_service_submit test_config_service_submit.cpp)
target_link_libraries(test_config_service_submit
    PRIVATE
        aegisgate_core
        GTest::gtest_main
)
add_test(NAME ControlPlaneConfigServiceSubmitTest
         COMMAND test_config_service_submit)

add_executable(test_config_service_review test_config_service_review.cpp)
target_link_libraries(test_config_service_review
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneConfigServiceReviewTest
         COMMAND test_config_service_review)

add_executable(test_config_service_activate test_config_service_activate.cpp)
target_link_libraries(test_config_service_activate
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneConfigServiceActivateTest
         COMMAND test_config_service_activate)

add_executable(test_config_service_rollback test_config_service_rollback.cpp)
target_link_libraries(test_config_service_rollback
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneConfigServiceRollbackTest
         COMMAND test_config_service_rollback)

add_executable(test_config_service_read test_config_service_read.cpp)
target_link_libraries(test_config_service_read
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneConfigServiceReadTest
         COMMAND test_config_service_read)

add_executable(test_audit_bridge test_audit_bridge.cpp)
target_link_libraries(test_audit_bridge
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneAuditBridgeTest COMMAND test_audit_bridge)

add_executable(test_submit_rate_limit test_submit_rate_limit.cpp)
target_link_libraries(test_submit_rate_limit
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneSubmitRateLimitTest COMMAND test_submit_rate_limit)

add_executable(test_security_end_to_end test_security_end_to_end.cpp)
target_link_libraries(test_security_end_to_end
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneSecurityEndToEndTest COMMAND test_security_end_to_end)

add_executable(test_bootstrap_helper test_bootstrap_helper.cpp)
target_link_libraries(test_bootstrap_helper
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneBootstrapHelperTest COMMAND test_bootstrap_helper)

# ---- Phase 9.3.4 RolloutController (TASK-20260422-01) ----
# Pure-business rollout logic lives in aegisgate_core and must be testable
# on both OFF and ON builds. The gRPC adapter + end-to-end rollout tests are
# under the ENABLE_CONTROL_PLANE block below.

add_executable(test_rollout_state_machine rollout/test_rollout_state_machine.cpp)
target_link_libraries(test_rollout_state_machine
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneRolloutStateMachineTest COMMAND test_rollout_state_machine)

add_executable(test_rollout_metrics_provider
               rollout/test_rollout_metrics_provider.cpp)
target_link_libraries(test_rollout_metrics_provider
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneRolloutMetricsProviderTest
         COMMAND test_rollout_metrics_provider)

add_executable(test_rollout_ticker rollout/test_rollout_ticker.cpp)
target_link_libraries(test_rollout_ticker
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneRolloutTickerTest COMMAND test_rollout_ticker)

add_executable(test_rollout_audit_bridge rollout/test_rollout_audit_bridge.cpp)
target_link_libraries(test_rollout_audit_bridge
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneRolloutAuditBridgeTest
         COMMAND test_rollout_audit_bridge)

add_executable(test_rollout_controller rollout/test_rollout_controller.cpp)
target_link_libraries(test_rollout_controller
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneRolloutControllerTest
         COMMAND test_rollout_controller)

add_executable(test_rollout_security_sr13_17
               rollout/test_rollout_security_sr13_17.cpp)
target_link_libraries(test_rollout_security_sr13_17
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ControlPlaneRolloutSecuritySR13_17Test
         COMMAND test_rollout_security_sr13_17)

# ---- gRPC-dependent tests ----
if(ENABLE_CONTROL_PLANE)
    add_executable(test_proto_roundtrip test_proto_roundtrip.cpp)
    target_link_libraries(test_proto_roundtrip
        PRIVATE
            control_plane_proto
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneProtoRoundtripTest COMMAND test_proto_roundtrip)

    add_executable(test_grpc_adapter test_grpc_adapter.cpp)
    target_link_libraries(test_grpc_adapter
        PRIVATE
            control_plane_server
            aegisgate_core
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneGrpcAdapterTest COMMAND test_grpc_adapter)

    add_executable(test_auth_interceptor test_auth_interceptor.cpp)
    target_link_libraries(test_auth_interceptor
        PRIVATE
            control_plane_server
            aegisgate_core
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneAuthInterceptorTest COMMAND test_auth_interceptor)

    add_executable(test_server_bootstrap test_server_bootstrap.cpp)
    target_link_libraries(test_server_bootstrap
        PRIVATE
            control_plane_server
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneServerBootstrapTest COMMAND test_server_bootstrap)

    # Epic 7 — client-side gRPC stub.
    add_executable(test_grpc_client test_grpc_client.cpp)
    target_link_libraries(test_grpc_client
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneGrpcClientTest COMMAND test_grpc_client)

    add_executable(test_config_cli_review test_config_cli_review.cpp)
    target_link_libraries(test_config_cli_review
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneConfigCliReviewTest COMMAND test_config_cli_review)

    add_executable(test_config_cli_activate test_config_cli_activate.cpp)
    target_link_libraries(test_config_cli_activate
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneConfigCliActivateTest COMMAND test_config_cli_activate)

    add_executable(test_config_cli_rollback test_config_cli_rollback.cpp)
    target_link_libraries(test_config_cli_rollback
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneConfigCliRollbackTest COMMAND test_config_cli_rollback)

    add_executable(test_config_cli_read test_config_cli_read.cpp)
    target_link_libraries(test_config_cli_read
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneConfigCliReadTest COMMAND test_config_cli_read)

    add_executable(test_config_cli_flags test_config_cli_flags.cpp)
    target_link_libraries(test_config_cli_flags
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneConfigCliFlagsTest COMMAND test_config_cli_flags)

    # SR-NEW1 (TASK-20260515-01 C1) — JSON redaction coverage tests.
    add_executable(test_config_cli_json_redact test_config_cli_json_redact.cpp)
    target_link_libraries(test_config_cli_json_redact
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneConfigCliJsonRedactTest
             COMMAND test_config_cli_json_redact)

    add_executable(test_rollout_grpc_adapter
                   rollout/test_rollout_grpc_adapter.cpp)
    target_link_libraries(test_rollout_grpc_adapter
        PRIVATE
            control_plane_server
            aegisgate_core
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneRolloutGrpcAdapterTest
             COMMAND test_rollout_grpc_adapter)

    add_executable(test_rollout_cli test_rollout_cli.cpp)
    target_link_libraries(test_rollout_cli
        PRIVATE
            aegisctl_controlplane
            GTest::gtest_main
    )
    add_test(NAME ControlPlaneRolloutCliTest COMMAND test_rollout_cli)
endif()
