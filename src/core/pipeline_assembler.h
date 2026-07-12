#pragma once
#include "core/pipeline.h"
#include "core/config.h"
#include "core/feature_gate.h"
#include "guardrail/inbound/input_preprocessor.h"
#include "guardrail/inbound/prompt_template_stage.h"
#include "guardrail/inbound/injection.h"
#include "guardrail/inbound/guard_classifier.h"
#include "guardrail/inbound/pii_filter.h"
#include "guardrail/inbound/topic_guard.h"
#include "guardrail/inbound/external_safety_stage.h"
#include "guardrail/inbound/openai_moderation.h"
#include "guardrail/inbound/perspective_api.h"
#include "guardrail/outbound/content_filter.h"
#include "guardrail/outbound/hallucination.h"
#include "guardrail/audit.h"
#include "guardrail/rule_engine.h"
#include "observe/request_logger.h"
#include "observe/cost_tracker.h"
#include "observe/alerting.h"
#include "cache/semantic_cache.h"
#include "cache/embedder.h"
#include "cache/vector_store.h"
#include "gateway/prompt_compressor.h"
#include "gateway/smart_max_tokens.h"
#include "storage/cache_store.h"
#include "storage/persistent_store.h"
#include "plugin/plugin_loader.h"
#include "rag/retrieval_stage.h"
#include "rag/knowledge_base.h"
#include "observe/cost_attribution.h"
#include "observe/anomaly_detector.h"
#include "observe/quality_monitor.h"
#include "observe/cost_optimizer.h"
#include "auth/prompt_template_service.h"
#include "agent/tool_registry.h"
#include "agent/tool_sandbox.h"
#include "agent/orchestrator.h"
#include <memory>
#include <vector>

namespace aegisgate {

struct AssembledPipeline {
    ~AssembledPipeline();
    AssembledPipeline() = default;
    AssembledPipeline(AssembledPipeline&& o) noexcept;
    AssembledPipeline& operator=(AssembledPipeline&& o) noexcept;

    std::unique_ptr<Embedder> embedder;
    std::unique_ptr<VectorStore> vector_store;
    std::unique_ptr<FeatureGate> feature_gate;
    std::unique_ptr<CacheStore> cache_store;
    std::unique_ptr<PersistentStore> persistent_store;
    std::unique_ptr<PromptTemplateService> prompt_template_service;

    Pipeline inbound;
    Pipeline outbound;

    AuditLogger* audit_logger = nullptr;
    RequestLogger* request_logger = nullptr;
    CostTracker* cost_tracker = nullptr;
    SemanticCache* semantic_cache = nullptr;

    PromptTemplateStage* prompt_template_stage = nullptr;
    InputPreprocessor* input_preprocessor = nullptr;
    InjectionDetector* injection_detector = nullptr;
    // TASK-20260708-03 / REV20260707-C2: GuardClassifier raw pointer added
    // so GatewayRuntime can wire the GuardAdminController into it post-
    // assembly (only stage previously missing from AssembledPipeline).
    GuardClassifier* guard_classifier = nullptr;
    PIIFilter* pii_filter = nullptr;
    TopicGuard* topic_guard = nullptr;
    RuleEngine* rule_engine = nullptr;

    PromptCompressor* prompt_compressor = nullptr;
    SmartMaxTokens* smart_max_tokens = nullptr;
    ExternalSafetyStage* external_safety = nullptr;

    std::shared_ptr<AlertManager> alert_manager;
    std::unique_ptr<PluginLoader> plugin_loader;

    RetrievalStage* retrieval_stage = nullptr;
    std::unique_ptr<KnowledgeBase> knowledge_base;
    std::unique_ptr<CostAttribution> cost_attribution;
    std::unique_ptr<AnomalyDetector> anomaly_detector;
    std::unique_ptr<QualityMonitor> quality_monitor;
    std::unique_ptr<CostOptimizer> cost_optimizer;
    std::unique_ptr<ToolRegistry> tool_registry;
    std::unique_ptr<ToolSandbox> tool_sandbox;
    std::unique_ptr<Orchestrator> orchestrator;
};

class PipelineAssembler {
public:
    static AssembledPipeline assemble(const Config& config);

    // TASK-20260622-01 E1 (G1): 信任关键校验 —— 当请求的后端（非 memory）与实际
    // 装配到的后端不一致（编译期未编入 / 运行时初始化失败 → 静默回退 memory）时：
    //   strict=true  → spdlog::critical + 抛 std::runtime_error（fail-closed 拒绝启动）
    //   strict=false → spdlog::warn（降级可用）
    // 文案仅含 backend 名 + 编译开关名，绝不回显 url/host/password（SR2）。
    // 抽为静态方法以便独立单测（避免跑重量级全 assemble）。
    static void enforceBackendActive(const char* kind, const std::string& requested,
                                     const std::string& actual, const char* compile_flag,
                                     bool compiled_in, bool strict);
};

} // namespace aegisgate
