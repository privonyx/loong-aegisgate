add_library(aegisgate_core
    core/pipeline.cpp
    core/crypto.cpp
    core/config.cpp
    core/rollout_resolve.cpp
    core/feature_gate.cpp
    gateway/connector/openai.cpp
    gateway/connector/claude.cpp
    gateway/connector/drogon_upstream_client.cpp
    gateway/connector/factory.cpp
    gateway/connector/registry.cpp
    gateway/provider_spec/provider_manifest.cpp
    gateway/router.cpp
    gateway/ab_test_router.cpp
    gateway/geo_router.cpp
    gateway/ml_router.cpp
    gateway/multi_objective_router.cpp
    gateway/routing_strategy_catalog.cpp
    gateway/bandit_router.cpp
    gateway/balancer.cpp
    gateway/fallback.cpp
    gateway/circuit_breaker.cpp
    gateway/rate_limiter.cpp
    gateway/abuse_detector.cpp
    gateway/prompt_compressor.cpp
    gateway/smart_max_tokens.cpp
    observe/token_estimator.cpp
    guardrail/inbound/injection.cpp
    guardrail/inbound/pii_filter.cpp
    guardrail/inbound/topic_guard.cpp
    guardrail/inbound/encoding_detector.cpp
    guardrail/inbound/unicode_normalizer.cpp
    guardrail/inbound/input_preprocessor.cpp
    guardrail/inbound/prompt_template_stage.cpp
    guardrail/inbound/guard_classifier.cpp
    guardrail/inbound/openai_moderation.cpp
    guardrail/inbound/perspective_api.cpp
    guardrail/inbound/external_safety_stage.cpp
    guardrail/outbound/content_filter.cpp
    guardrail/outbound/hallucination.cpp
    guardrail/audit.cpp
    guardrail/rule_engine.cpp
    guardrail/model/memory_guard_model_registry.cpp
    guardrail/model/sqlite_guard_model_registry.cpp
    guardrail/feedback/guard_feedback_sink.cpp
    guardrail/feedback/guard_feedback_rate_limiter.cpp
    guardrail/feedback/guard_feedback_anomaly_detector.cpp
    guardrail/guard_explanation_builder.cpp
    guardrail/training/guard_trainer.cpp
    guardrail/autonomy/guard_model_applier.cpp
    guardrail/admin/guard_admin_controller.cpp
    workflow/workflow_dsl.cpp
    workflow/workflow_dsl_parser.cpp
    workflow/memory_workflow_state_store.cpp
    workflow/sqlite_workflow_state_store.cpp
    workflow/thread_pool.cpp
    workflow/workflow_engine.cpp
    workflow/human_approval_node_handler.cpp
    workflow/workflow_approval_applier.cpp
    observe/crash_handler.cpp
    observe/request_logger.cpp
    observe/metrics.cpp
    observe/feedback_event.cpp
    observe/feedback_bus.cpp
    observe/metrics_feedback_subscriber.cpp
    observe/cost_tracker.cpp
    observe/router_outcome_replayer.cpp
    cli/replay_cli.cpp
    observe/alerting.cpp
    observe/alert_dispatcher.cpp
    observe/alert_channels.cpp
    observe/compliance_reporter.cpp
    observe/quality_scorer.cpp
    observe/usage_predictor.cpp
    observe/autonomy/approval_proposal.cpp
    observe/autonomy/approval_queue.cpp
    observe/autonomy/approval_workflow.cpp
    observe/autonomy/base_autonomy_applier.cpp
    observe/autonomy/cost_autonomy_applier.cpp
    observe/autonomy/bandit_autonomy_applier.cpp
    cache/embedder.cpp
    cache/tokenizer.cpp
    cache/vector_index.cpp
    cache/partitioned_vector_index.cpp
    cache/hnsw_vector_store.cpp
    cache/semantic_cache.cpp
    cache/conversation_id_resolver.cpp
    cache/rule_based_summarizer.cpp
    cache/onnx_summarizer.cpp
    cache/composite_summarizer.cpp
    cache/summarizer_factory.cpp
    cache/conversation_cache_evictor.cpp
    cache/cache_migrator.cpp
    multimodal/modality.cpp
    multimodal/modality_router.cpp
    multimodal/openai_modality_handlers.cpp
    multimodal/modality_rate_limiter.cpp
    auth/crypto_utils.cpp
    auth/auth_service.cpp
    auth/jwt_utils.cpp
    auth/encryption.cpp
    auth/session_manager.cpp
    auth/redis_session_store.cpp
    auth/identity_mapper.cpp
    auth/oidc_client.cpp
    auth/totp_service.cpp
    auth/scim_service.cpp
    auth/prompt_template_service.cpp
    server/admin_controller_base.cpp
    server/admin_iam_controller.cpp
    server/admin_governance_controller.cpp
    server/admin_controller.cpp
    server/admin_session.cpp
    server/case_study_builder.cpp
    storage/memory_cache_store.cpp
    storage/memory_persistent_store.cpp
    storage/sqlite_persistent_store.cpp
    storage/rollout_schema.cpp
    common/scope_matcher.cpp
    common/clock.cpp
    control_plane/rollout/rollout_metrics_provider.cpp
    control_plane/rollout/rollout_state_machine.cpp
    control_plane/rollout/rollout_ticker.cpp
    control_plane/rollout/rollout_audit_bridge.cpp
    control_plane/rollout/rollout_controller.cpp
    control_plane/state_machine.cpp
    control_plane/ulid.cpp
    control_plane/sensitive_scanner.cpp
    control_plane/diff_engine.cpp
    control_plane/audit_bridge.cpp
    control_plane/config_service_core.cpp
    control_plane/bootstrap_helper.cpp
    core/pipeline_assembler.cpp
    plugin/plugin_loader.cpp
    plugin/rule_pack_manager.cpp
    server/gateway_runtime.cpp
    server/budget_guard_stage.cpp
    server/modality_quota_enforcer.cpp
    server/sse_response.cpp
    cli/migrate.cpp
    cli/cache_cli.cpp
    cli/estimate_cli.cpp
    agent/tool_registry.cpp
    agent/tool_sandbox.cpp
    agent/orchestrator.cpp
    rag/knowledge_base.cpp
    rag/retrieval_stage.cpp
    observe/cost_attribution.cpp
    observe/anomaly_detector.cpp
    observe/quality_monitor.cpp
    observe/cost_optimizer.cpp
    observe/savings_aggregator.cpp
    observe/recovery/auto_recovery_controller.cpp
    observe/recovery/log_ringbuffer_sink.cpp
    observe/recovery/feedback_event_history.cpp
    observe/recovery/recovery_applier.cpp
    observe/recovery/root_cause_analyzer.cpp
    observe/recovery/capacity_predictor.cpp
    observe/recovery/runbook_engine.cpp
)

target_include_directories(aegisgate_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(aegisgate_core PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    Drogon::Drogon
    re2::re2
    hnswlib::hnswlib
    OpenSSL::SSL
    OpenSSL::Crypto
    unofficial::sqlite3::sqlite3
    ${CMAKE_DL_LIBS}
)

if(ENABLE_ONNX)
    target_sources(aegisgate_core PRIVATE cache/onnx_embedder.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_ONNX)
    target_link_libraries(aegisgate_core PUBLIC onnxruntime::onnxruntime)
endif()

if(ENABLE_GUARD_MODEL)
    target_sources(aegisgate_core PRIVATE guardrail/inbound/spm_tokenizer.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_GUARD)
    target_link_libraries(aegisgate_core PUBLIC PkgConfig::SENTENCEPIECE)
    # sentencepiece(overlay port，SPM_ABSL_PROVIDER=package)用外部 abseil 实现其
    # --minloglevel 等命令行 flag（error.cc 引用 absl::flags_internal::*），但它的
    # pkg-config(.pc) 不会把 abseil flags 库带进来，直接链 PkgConfig::SENTENCEPIECE 会
    # 报 absl::flags_internal::{AbslParseFlag,RegisterCommandLineFlag,FlagImpl} 未定义。
    # 补链 sentencepiece 声明依赖的这几个 absl 组件（abseil 随 protobuf/grpc 已安装）。
    find_package(absl CONFIG REQUIRED)
    target_link_libraries(aegisgate_core PUBLIC
        absl::flags absl::flags_parse absl::strings absl::log absl::check)
    if(NOT ENABLE_ONNX)
        target_link_libraries(aegisgate_core PUBLIC onnxruntime::onnxruntime)
    endif()
endif()

if(ENABLE_REDIS)
    target_sources(aegisgate_core PRIVATE
        storage/redis_cache_store.cpp
        cluster/redis_state_store.cpp
    )
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_REDIS)
    target_link_libraries(aegisgate_core PUBLIC hiredis::hiredis)
endif()

if(ENABLE_PG)
    target_sources(aegisgate_core PRIVATE storage/pg_persistent_store.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_PG)
    target_link_libraries(aegisgate_core PUBLIC PostgreSQL::PostgreSQL)
endif()

if(ENABLE_CURL)
    target_sources(aegisgate_core PRIVATE gateway/connector/curl_upstream_client.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_CURL)
    target_link_libraries(aegisgate_core PUBLIC CURL::libcurl)
endif()

if(ENABLE_MILVUS)
    target_sources(aegisgate_core PRIVATE cache/milvus_vector_store.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_MILVUS)
    message(STATUS "Milvus vector store backend enabled")
endif()

if(ENABLE_QDRANT)
    target_sources(aegisgate_core PRIVATE cache/qdrant_vector_store.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_QDRANT)
    message(STATUS "Qdrant vector store backend enabled")
endif()

if(ENABLE_OPENTELEMETRY)
    target_sources(aegisgate_core PRIVATE observe/tracing.cpp)
    target_compile_definitions(aegisgate_core PUBLIC AEGISGATE_ENABLE_OTEL)
    target_link_libraries(aegisgate_core PUBLIC
        opentelemetry-cpp::trace
        opentelemetry-cpp::otlp_http_exporter
        opentelemetry-cpp::in_memory_span_exporter
        opentelemetry-cpp::resources
    )
endif()

file(READ ${CMAKE_SOURCE_DIR}/docs/openapi.yaml OPENAPI_SPEC_CONTENT)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/server/openapi_spec.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/openapi_spec.h
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/server/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)

add_executable(aegisgate
    main.cpp
    server/api_controller.cpp
    server/admin_http_helpers.cpp
    server/admin_http_controller.cpp
    server/scim_http_controller.cpp
    server/admin_ws_controller.cpp
)

target_include_directories(aegisgate PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 导出动态符号表（== -rdynamic），让 backtrace_symbols_fd 在崩溃日志中
# 解析出函数名（崩溃 dump 日志 / TASK-20260615-05）。
set_target_properties(aegisgate PROPERTIES ENABLE_EXPORTS ON)

target_link_libraries(aegisgate PRIVATE
    aegisgate_core
    Drogon::Drogon
)

include(GNUInstallDirs)
install(TARGETS aegisgate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/aegisgate)
