#include "core/pipeline_assembler.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include "observe/quality_scorer.h"
#include "observe/alert_dispatcher.h"
#include "observe/alert_channels.h"
#include "plugin/plugin_loader.h"
#include "storage/memory_cache_store.h"
#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"
#ifdef AEGISGATE_ENABLE_REDIS
#include "storage/redis_cache_store.h"
#endif
#ifdef AEGISGATE_ENABLE_PG
#include "storage/pg_persistent_store.h"
#endif
#ifdef AEGISGATE_ENABLE_ONNX
#include "cache/onnx_embedder.h"
#endif
#include "cache/hnsw_vector_store.h"
#ifdef AEGISGATE_ENABLE_MILVUS
#include "cache/milvus_vector_store.h"
#endif
#ifdef AEGISGATE_ENABLE_QDRANT
#include "cache/qdrant_vector_store.h"
#endif
#include "cache/summarizer_factory.h"
#include "cache/conversation_id_resolver.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <memory>

namespace aegisgate {

AssembledPipeline::~AssembledPipeline() {
    if (audit_logger) {
        audit_logger->shutdown();
    }
}

AssembledPipeline::AssembledPipeline(AssembledPipeline&& o) noexcept
    : embedder(std::move(o.embedder))
    , vector_store(std::move(o.vector_store))
    , feature_gate(std::move(o.feature_gate))
    , cache_store(std::move(o.cache_store))
    , persistent_store(std::move(o.persistent_store))
    , prompt_template_service(std::move(o.prompt_template_service))
    , inbound(std::move(o.inbound))
    , outbound(std::move(o.outbound))
    , audit_logger(std::exchange(o.audit_logger, nullptr))
    , request_logger(std::exchange(o.request_logger, nullptr))
    , cost_tracker(std::exchange(o.cost_tracker, nullptr))
    , semantic_cache(std::exchange(o.semantic_cache, nullptr))
    , prompt_template_stage(std::exchange(o.prompt_template_stage, nullptr))
    , input_preprocessor(std::exchange(o.input_preprocessor, nullptr))
    , injection_detector(std::exchange(o.injection_detector, nullptr))
    , pii_filter(std::exchange(o.pii_filter, nullptr))
    , topic_guard(std::exchange(o.topic_guard, nullptr))
    , prompt_compressor(std::exchange(o.prompt_compressor, nullptr))
    , smart_max_tokens(std::exchange(o.smart_max_tokens, nullptr))
    , external_safety(std::exchange(o.external_safety, nullptr))
    , alert_manager(std::move(o.alert_manager))
    , plugin_loader(std::move(o.plugin_loader))
    , retrieval_stage(std::exchange(o.retrieval_stage, nullptr))
    , knowledge_base(std::move(o.knowledge_base))
    , cost_attribution(std::move(o.cost_attribution))
    , anomaly_detector(std::move(o.anomaly_detector))
    , quality_monitor(std::move(o.quality_monitor))
    , cost_optimizer(std::move(o.cost_optimizer))
    , tool_registry(std::move(o.tool_registry))
    , tool_sandbox(std::move(o.tool_sandbox))
    , orchestrator(std::move(o.orchestrator))
{}

AssembledPipeline& AssembledPipeline::operator=(AssembledPipeline&& o) noexcept {
    if (this != &o) {
        if (audit_logger) audit_logger->shutdown();

        embedder = std::move(o.embedder);
        vector_store = std::move(o.vector_store);
        feature_gate = std::move(o.feature_gate);
        cache_store = std::move(o.cache_store);
        persistent_store = std::move(o.persistent_store);
        prompt_template_service = std::move(o.prompt_template_service);
        inbound = std::move(o.inbound);
        outbound = std::move(o.outbound);
        audit_logger = std::exchange(o.audit_logger, nullptr);
        request_logger = std::exchange(o.request_logger, nullptr);
        cost_tracker = std::exchange(o.cost_tracker, nullptr);
        semantic_cache = std::exchange(o.semantic_cache, nullptr);
        prompt_template_stage = std::exchange(o.prompt_template_stage, nullptr);
        input_preprocessor = std::exchange(o.input_preprocessor, nullptr);
        injection_detector = std::exchange(o.injection_detector, nullptr);
        pii_filter = std::exchange(o.pii_filter, nullptr);
        topic_guard = std::exchange(o.topic_guard, nullptr);
        prompt_compressor = std::exchange(o.prompt_compressor, nullptr);
        smart_max_tokens = std::exchange(o.smart_max_tokens, nullptr);
        external_safety = std::exchange(o.external_safety, nullptr);
        alert_manager = std::move(o.alert_manager);
        plugin_loader = std::move(o.plugin_loader);
        retrieval_stage = std::exchange(o.retrieval_stage, nullptr);
        knowledge_base = std::move(o.knowledge_base);
        cost_attribution = std::move(o.cost_attribution);
        anomaly_detector = std::move(o.anomaly_detector);
        quality_monitor = std::move(o.quality_monitor);
        cost_optimizer = std::move(o.cost_optimizer);
        tool_registry = std::move(o.tool_registry);
        tool_sandbox = std::move(o.tool_sandbox);
        orchestrator = std::move(o.orchestrator);
    }
    return *this;
}

namespace {

#ifdef AEGISGATE_ENABLE_ONNX
bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}
#endif

/// Owns a shared AlertManager so AssembledPipeline can configure it while the
/// outbound pipeline holds a default unique_ptr<PipelineStage>.
class SharedAlertManagerStage : public PipelineStage {
public:
    explicit SharedAlertManagerStage(std::shared_ptr<AlertManager> impl)
        : impl_(std::move(impl)) {}

    StageResult process(RequestContext& ctx) override { return impl_->process(ctx); }

    StageResult processChunk(RequestContext& ctx, std::string_view chunk) override {
        return impl_->processChunk(ctx, chunk);
    }

    std::string name() const override { return impl_->name(); }

private:
    std::shared_ptr<AlertManager> impl_;
};

} // namespace

void PipelineAssembler::enforceBackendActive(const char* kind, const std::string& requested,
                                             const std::string& actual, const char* compile_flag,
                                             bool compiled_in, bool strict) {
    // 显式请求 memory（社区默认）或请求与实际一致 → 非误配，放行
    if (requested == "memory" || requested == actual) return;

    // 仅含 backend 名 + 编译开关名，绝不回显 url/host/password（SR2）
    const std::string reason = compiled_in
        ? std::string("backend initialization failed at startup (dependency unreachable?)")
        : std::string("binary built WITHOUT ") + compile_flag +
              " (rebuild with scripts/build.sh -t Release)";
    const std::string msg = std::string("Requested ") + kind + " backend '" + requested +
        "' is NOT active (running on '" + actual + "'): " + reason;

    if (strict) {
        spdlog::critical("{} [storage.strict_backends=true -> refusing to start]", msg);
        throw std::runtime_error(msg);
    }
    spdlog::warn("{} [storage.strict_backends=false -> degraded, NOT production-grade]", msg);
}

AssembledPipeline PipelineAssembler::assemble(const Config& config) {
    AssembledPipeline ap;
    ap.feature_gate = std::make_unique<FeatureGate>(config.edition());
    ap.feature_gate->loadLicense(config.licensePath());

    // --- Cache backend ---
    auto cache_backend = config.cacheBackend();
#ifdef AEGISGATE_ENABLE_REDIS
    if (cache_backend == "redis") {
        RedisConfig rcfg;
        rcfg.host = config.redisHost();
        rcfg.port = config.redisPort();
        rcfg.password = config.redisPassword();
        rcfg.db = config.redisDb();
        rcfg.pool_size = static_cast<size_t>(config.redisPoolSize());
        rcfg.connect_timeout_ms = config.redisConnectTimeout();
        rcfg.command_timeout_ms = config.redisCommandTimeout();
        ap.cache_store = std::make_unique<RedisCacheStore>(rcfg);
    }
#endif
    if (!ap.cache_store) {
        ap.cache_store = std::make_unique<MemoryCacheStore>(10000);
    }
    if (!ap.cache_store->initialize()) {
        spdlog::error("Failed to initialize cache store ({}), falling back to memory",
                      cache_backend);
        ap.cache_store = std::make_unique<MemoryCacheStore>(10000);
        ap.cache_store->initialize();
    }
    spdlog::info("Cache store: {}", ap.cache_store->backendName());
    {
        bool redis_compiled =
#ifdef AEGISGATE_ENABLE_REDIS
            true;
#else
            false;
#endif
        enforceBackendActive("cache", cache_backend, ap.cache_store->backendName(),
                             "ENABLE_REDIS", redis_compiled, config.strictBackends());
    }

    // --- Persistent backend ---
    auto persistent_backend = config.persistentBackend();
#ifdef AEGISGATE_ENABLE_PG
    if (persistent_backend == "postgres") {
        PgConfig pcfg;
        pcfg.url = config.pgUrl();
        pcfg.pool_size = static_cast<size_t>(config.pgPoolSize());
        pcfg.connect_timeout_ms = config.pgConnectTimeout();
        ap.persistent_store = std::make_unique<PgPersistentStore>(pcfg);
    }
#endif
    if (!ap.persistent_store) {
        if (persistent_backend == "sqlite") {
            auto db_path = config.sqlitePath();
            auto parent = std::filesystem::path(db_path).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            ap.persistent_store = std::make_unique<SQLitePersistentStore>(
                db_path, config.sqliteWalMode());
        } else {
            ap.persistent_store = std::make_unique<MemoryPersistentStore>();
        }
    }
    if (!ap.persistent_store->initialize()) {
        spdlog::error("Failed to initialize persistent store ({}), falling back to memory",
                      persistent_backend);
        ap.persistent_store = std::make_unique<MemoryPersistentStore>();
        ap.persistent_store->initialize();
    }
    spdlog::info("Persistent store: {}", ap.persistent_store->backendName());
    {
        bool pg_compiled =
#ifdef AEGISGATE_ENABLE_PG
            true;
#else
            false;
#endif
        // sqlite 恒编入；postgres 需 ENABLE_PG。memory/一致 时 enforce 内部直接放行。
        bool persist_compiled = (persistent_backend == "postgres") ? pg_compiled : true;
        enforceBackendActive("persistent", persistent_backend, ap.persistent_store->backendName(),
                             "ENABLE_PG", persist_compiled, config.strictBackends());
    }

    // --- Audit retention pruning ---
    int retention_days = config.auditRetentionDays();
    if (retention_days > 0) {
        auto pruned_a = ap.persistent_store->pruneAudits(retention_days);
        auto pruned_c = ap.persistent_store->pruneCostRecords(retention_days);
        // TASK-20260617-02: savings_events 与 cost_records 同一保留策略。
        auto pruned_s = ap.persistent_store->pruneSavingsEvents(retention_days);
        if (pruned_a > 0 || pruned_c > 0 || pruned_s > 0) {
            spdlog::info("Pruned {} audits, {} cost records, {} savings events (retention={}d)",
                         pruned_a, pruned_c, pruned_s, retention_days);
        }
    }

    // --- Inbound pipeline ---

    auto audit = std::make_unique<AuditLogger>();
    audit->setPersistentStore(ap.persistent_store.get());
    ap.audit_logger = audit.get();
    ap.inbound.addStage(std::move(audit));

    // TASK-20260709-01 / REV20260707-I5 D6: PromptTemplateStage after Audit,
    // before InputPreprocessor so injected system text is normalized/scanned.
    ap.prompt_template_service =
        std::make_unique<PromptTemplateService>(ap.persistent_store.get());
    auto tpl_stage = std::make_unique<PromptTemplateStage>();
    tpl_stage->setService(ap.prompt_template_service.get());
    ap.prompt_template_stage = tpl_stage.get();
    ap.inbound.addStage(std::move(tpl_stage));

    // 2. Input preprocessor (Unicode normalization + encoding detection)
    auto preprocessor = std::make_unique<InputPreprocessor>(
        config.encodingMinBase64Length());
    preprocessor->setUnicodeNormalization(config.unicodeNormalizationEnabled());
    preprocessor->setEncodingDetection(config.encodingDetectionEnabled());
    // TASK-20260707-03 / REV20260707-N19: cap data: URI decode in the vision
    // image-reference scan surface (SR-4 DoS guard).
    preprocessor->setMaxImageDecodeBytes(config.imageScanMaxDecodeBytes());
    ap.input_preprocessor = preprocessor.get();

    // 3. Injection detection
    auto injection = std::make_unique<InjectionDetector>();
    injection->loadPatterns("config/rules/injection_patterns.yaml");
    injection->setAuditLogger(ap.audit_logger);  // P1-1: audit block decisions
    injection->setFailOpen(config.injectionFailOpen());  // P0-2: degradation policy
    ap.injection_detector = injection.get();

    // Link preprocessor to injection detector for shadow analysis
    preprocessor->setInjectionDetector(ap.injection_detector);

    ap.inbound.addStage(std::move(preprocessor));
    ap.inbound.addStage(std::move(injection));

    // 4. Guard classifier (optional, requires guard model)
    if (config.guardModelEnabled()) {
        auto guard = std::make_unique<GuardClassifier>(
            config.guardModelPath(), config.guardModelVocabPath(),
            config.guardModelSpmPath());
        guard->setThreshold(static_cast<float>(config.guardModelThreshold()));
        guard->setFailClosed(config.guardModelFailPolicy() == "closed");  // C3
        guard->setAuditLogger(ap.audit_logger);  // P1-1/P1-2: audit unsafe reject
        if (guard->isReady()) {
            spdlog::info("GuardClassifier: local ONNX guard model active");
        } else {
            spdlog::info("GuardClassifier: L3 disabled (no model / inference "
                         "unavailable) — running fail-open pass-through");
        }
        // TASK-20260708-03 / REV20260707-C2: expose raw pointer so
        // GatewayRuntime can wire GuardAdminController into this stage
        // post-assembly (mirrors ap.injection_detector / ap.rule_engine /
        // ap.external_safety exposure pattern).
        ap.guard_classifier = guard.get();
        ap.inbound.addStage(std::move(guard));
    }

    // 4b. External Safety APIs (L4 — optional, requires CURL)
    if (config.externalSafetyEnabled()) {
        ExternalSafetyStageConfig es_cfg;
        auto mode_str = config.externalSafetyMode();
        if (mode_str == "all") es_cfg.mode = ExternalSafetyMode::All;
        else if (mode_str == "majority") es_cfg.mode = ExternalSafetyMode::Majority;
        else es_cfg.mode = ExternalSafetyMode::Any;

        auto fp_str = config.externalSafetyFailPolicy();
        es_cfg.fail_policy = (fp_str == "closed") ?
            ExternalSafetyFailPolicy::Closed : ExternalSafetyFailPolicy::Open;
        es_cfg.async_parallel = config.externalSafetyParallel();
        // Phase 6.3 (Epic 4.2): wire SR3+SR6 knobs into the stage so YAML
        // config controls shadow rollout without code changes.
        es_cfg.shadow_mode = config.externalSafetyShadowMode();
        es_cfg.shadow_max_inflight = static_cast<size_t>(
            std::max(0, config.externalSafetyShadowMaxInflight()));
        es_cfg.shadow_audit_ttl = std::chrono::seconds{
            config.externalSafetyShadowAuditTtlSeconds()};

        auto ext_stage = std::make_unique<ExternalSafetyStage>(es_cfg);
        // SR3: shadow worker must reach the audit log. AuditLogger is owned
        // by the inbound pipeline (added above), but we keep a non-owning
        // pointer for the shadow worker — its lifetime outlives the stage.
        ext_stage->setAuditLogger(ap.audit_logger);

        if (config.openaiModerationEnabled()) {
            OpenAIModerationConfig oai_cfg;
            oai_cfg.api_key = config.openaiModerationApiKey();
            oai_cfg.base_url = config.openaiModerationBaseUrl();
            oai_cfg.model = config.openaiModerationModel();
            oai_cfg.timeout_ms = config.openaiModerationTimeout();
            ext_stage->addProvider(std::make_unique<OpenAIModeration>(oai_cfg));
            spdlog::info("ExternalSafety: OpenAI Moderation enabled (model={})", oai_cfg.model);
        }

        if (config.perspectiveApiEnabled()) {
            PerspectiveConfig persp_cfg;
            persp_cfg.api_key = config.perspectiveApiKey();
            persp_cfg.base_url = config.perspectiveBaseUrl();
            persp_cfg.threshold = config.perspectiveThreshold();
            persp_cfg.attributes = config.perspectiveAttributes();
            persp_cfg.timeout_ms = config.perspectiveTimeout();
            ext_stage->addProvider(std::make_unique<PerspectiveApi>(persp_cfg));
            spdlog::info("ExternalSafety: Google Perspective enabled (threshold={:.2f})",
                         persp_cfg.threshold);
        }

        if (ext_stage->providerCount() > 0) {
            ap.external_safety = ext_stage.get();
            const bool shadow = es_cfg.shadow_mode;
            ap.inbound.addStage(std::move(ext_stage));
            spdlog::info(
                "ExternalSafetyStage: L4 active with {} provider(s), mode={},"
                " fail={}, shadow={} (cap={}, ttl={}s)",
                ap.external_safety->providerCount(), mode_str, fp_str,
                shadow ? "on" : "off", es_cfg.shadow_max_inflight,
                es_cfg.shadow_audit_ttl.count());
        }
    }

    // 5. PII filter
    auto pii = std::make_unique<PIIFilter>();
    ap.pii_filter = pii.get();
    ap.inbound.addStage(std::move(pii));

    // 6. Topic guard
    auto topic = std::make_unique<TopicGuard>();
    topic->loadConfig("config/rules/topic_whitelist.yaml");
    topic->setAuditLogger(ap.audit_logger);  // P1-1: audit block decisions
    ap.topic_guard = topic.get();
    ap.inbound.addStage(std::move(topic));

    // 7. Rule engine (enterprise only, skipped automatically in community)
    auto rule_engine = std::make_unique<RuleEngine>(*ap.feature_gate);
    // P1-4（TASK-20260702-01）：优先加载 store 中全局作用域（空 tenant）的激活
    // 规则集（后台 createRuleSet/activateRuleSet 写入），无激活集时回退 YAML。
    // 使后台规则集在运行时真正生效（reloadConfig 走同一路径）。
    if (ap.persistent_store) {
        rule_engine->reloadFromStoreOrYaml(*ap.persistent_store, "",
                                           "config/rules/custom_rules.yaml");
        // P2-4（TASK-20260702-02 / SR-4）：装配期加载各租户激活集，令请求按
        // ctx.tenant_id 命中各自规则集（无桶回退全局）。
        rule_engine->loadAllTenants(*ap.persistent_store);
    } else {
        rule_engine->loadFromYaml("config/rules/custom_rules.yaml");
    }
    rule_engine->setAuditLogger(ap.audit_logger);  // P1-C: audit Block decisions
    ap.rule_engine = rule_engine.get();
    ap.inbound.addStage(std::move(rule_engine));

    // 8. Prompt compressor (token optimization — before cache for better hit rate)
    {
        PromptCompressor::Config pc_cfg;
        pc_cfg.enabled = config.promptCompressionEnabled();
        pc_cfg.max_context_messages = static_cast<size_t>(
            config.promptCompressionMaxContextMessages());
        pc_cfg.compress_whitespace = config.promptCompressionCompressWhitespace();
        pc_cfg.dedup_system_prompts = config.promptCompressionDedupSystem();
        auto compressor = std::make_unique<PromptCompressor>(pc_cfg);
        ap.prompt_compressor = compressor.get();
        ap.inbound.addStage(std::move(compressor));
    }

    // 9. Smart max tokens (token optimization — auto-set max_tokens)
    {
        SmartMaxTokens::Config smt_cfg;
        smt_cfg.enabled = config.smartMaxTokensEnabled();
        smt_cfg.default_max_output = config.smartMaxTokensDefaultOutput();
        smt_cfg.max_output_ratio = config.smartMaxTokensOutputRatio();
        smt_cfg.min_output_tokens = config.smartMaxTokensMinOutput();
        auto smart_mt = std::make_unique<SmartMaxTokens>(smt_cfg);
        ap.smart_max_tokens = smart_mt.get();
        ap.inbound.addStage(std::move(smart_mt));
    }

    // 9b. RAG Retrieval stage — moved below: it depends on ap.embedder and
    // ap.vector_store, which are not constructed until section 10. Assembling
    // it here dereferenced null pointers (P0-1 startup crash when RAGPipeline
    // was enabled). The stage is now added after the vector store is ready but
    // before the SemanticCache stage, preserving "retrieval before cache".

    // 10. Semantic cache — prefer ONNX embedder when available
    size_t embed_dim = 128;
#ifdef AEGISGATE_ENABLE_ONNX
    const std::string model_path = config.embeddingModelPath();
    const std::string vocab_path = config.embeddingVocabPath();
    if (!model_path.empty() && !vocab_path.empty() &&
        fileExists(model_path) && fileExists(vocab_path)) {
        auto onnx = std::make_unique<OnnxEmbedder>(model_path, vocab_path);
        if (onnx->isReady()) {
            embed_dim = onnx->dimension();
            ap.embedder = std::move(onnx);
            spdlog::info("Semantic cache: ONNX embedder (dim={})", embed_dim);
        }
    }
#endif
    if (!ap.embedder) {
        embed_dim = 128;
        ap.embedder = std::make_unique<HashEmbedder>(embed_dim);
        spdlog::info("Semantic cache: HashEmbedder fallback (dim={})", embed_dim);
    }

    auto cache_max = static_cast<size_t>(config.cacheMaxEntries());
    auto cache_partitions = static_cast<size_t>(config.cacheMaxPartitions());

    auto vs_backend = config.vectorStoreBackend();
#ifdef AEGISGATE_ENABLE_MILVUS
    if (vs_backend == "milvus") {
        MilvusConfig mc;
        mc.host = config.milvusHost();
        mc.port = config.milvusPort();
        mc.collection_prefix = config.milvusCollectionPrefix();
        mc.dimension = embed_dim;
        mc.metric_type = config.milvusMetricType();
        mc.token = config.milvusToken();
        mc.connect_timeout_ms = config.milvusConnectTimeout();
        mc.request_timeout_ms = config.milvusRequestTimeout();
        mc.auto_create_collection = config.milvusAutoCreateCollection();
        ap.vector_store = std::make_unique<MilvusVectorStore>(mc);
    }
#endif
#ifdef AEGISGATE_ENABLE_QDRANT
    if (vs_backend == "qdrant") {
        QdrantConfig qc;
        qc.host = config.qdrantHost();
        qc.port = config.qdrantPort();
        qc.collection_prefix = config.qdrantCollectionPrefix();
        qc.dimension = embed_dim;
        qc.distance = config.qdrantDistance();
        qc.api_key = config.qdrantApiKey();
        qc.connect_timeout_ms = config.qdrantConnectTimeout();
        qc.request_timeout_ms = config.qdrantRequestTimeout();
        qc.auto_create_collection = config.qdrantAutoCreateCollection();
        ap.vector_store = std::make_unique<QdrantVectorStore>(qc);
    }
#endif
    if (!ap.vector_store) {
        auto hnsw = std::make_unique<HnswVectorStore>(
            embed_dim, cache_max, cache_partitions);
        hnsw->initialize();
        ap.vector_store = std::move(hnsw);
        spdlog::info("Vector store: hnswlib (in-process)");
    } else {
        if (!ap.vector_store->initialize()) {
            spdlog::error("Failed to initialize {} vector store, falling back to hnswlib",
                          vs_backend);
            auto hnsw = std::make_unique<HnswVectorStore>(
                embed_dim, cache_max, cache_partitions);
            hnsw->initialize();
            ap.vector_store = std::move(hnsw);
        } else {
            spdlog::info("Vector store: {}", ap.vector_store->backendName());
        }
    }

    // 9b (relocated). RAG Retrieval stage — runs before the cache stage, but is
    // assembled here so ap.embedder / ap.vector_store are already constructed.
    if (ap.feature_gate && ap.feature_gate->isEnabled(Feature::RAGPipeline)) {
        ap.knowledge_base = std::make_unique<KnowledgeBase>(
            *ap.embedder, *ap.vector_store);
        // I23/I32 (TASK-20260703-04)：RAG 参数从 config 装配（此前硬编码
        // top_k=3/min_relevance=0.7，且 max_context_tokens/injection_position 从不
        // 生效）。缺省与 config getter 默认一致，保持零回归。
        RetrievalConfig ret_cfg;
        ret_cfg.enabled = true;
        ret_cfg.top_k = config.ragTopK();
        ret_cfg.min_relevance = config.ragMinRelevance();
        ret_cfg.max_context_tokens = config.ragMaxContextTokens();
        const std::string pos = config.ragInjectionPosition();
        if (pos == "before_system")
            ret_cfg.injection_position = InjectionPosition::BeforeSystem;
        else if (pos == "after_system")
            ret_cfg.injection_position = InjectionPosition::AfterSystem;
        else
            ret_cfg.injection_position = InjectionPosition::BeforeUser;
        auto retrieval = std::make_unique<RetrievalStage>(ret_cfg);
        retrieval->setKnowledgeBase(ap.knowledge_base.get());
        ap.retrieval_stage = retrieval.get();
        ap.inbound.addStage(std::move(retrieval));
        spdlog::info("RetrievalStage: RAG pipeline active "
                     "(top_k={}, min_relevance={}, max_context_tokens={}, pos={})",
                     ret_cfg.top_k, ret_cfg.min_relevance,
                     ret_cfg.max_context_tokens, pos);
    }

    auto cache = std::make_unique<SemanticCache>(
        *ap.embedder, *ap.vector_store,
        config.cacheThreshold(),
        std::chrono::seconds(config.cacheTtlSeconds()),
        cache_max);
    cache->setCacheStore(ap.cache_store.get());
    cache->setContextAware(config.cacheContextAware());
    {
        ConversationHashConfig conv_cfg;
        auto mode_str = config.cacheConversationHashMode();
        if (mode_str == "full") {
            conv_cfg.mode = ConversationHashMode::Full;
        } else if (mode_str == "window") {
            conv_cfg.mode = ConversationHashMode::Window;
        } else {
            conv_cfg.mode = ConversationHashMode::None;
        }
        conv_cfg.window_size = static_cast<size_t>(config.cacheConversationHashWindow());
        cache->setConversationHashConfig(conv_cfg);
        if (conv_cfg.mode != ConversationHashMode::None) {
            spdlog::info("SemanticCache: conversation hash mode={}, window={}",
                         mode_str, conv_cfg.window_size);
        }
    }
    {
        CachePolicy policy;
        policy.enabled = config.cachePolicyEnabled();
        policy.skip_models = config.cachePolicySkipModels();
        policy.max_temperature = config.cachePolicyMaxTemperature();
        policy.skip_streaming = config.cachePolicySkipStreaming();
        cache->setCachePolicy(policy);
    }
    // D3=A / SR-6 (TASK-20260703-04): cross_tenant sharing装配（此前 setter 从不
    // 调用 → getCrossTenant 死路）。fail-safe：config 缺省 enabled=false → 跨租户
    // 永不命中，隔离与现状逐字节一致（零回归基线）。启用后仅 similarity ≥ 门槛
    // （缺省 0.95 高）跨命中，且每次命中在 getCrossTenant 内强制审计。
    cache->setCrossTenantConfig(CrossTenantConfig{
        config.cacheCrossTenantEnabled(),
        config.cacheCrossTenantMinSimilarity()});
    if (config.cacheCrossTenantEnabled()) {
        spdlog::warn("SemanticCache: cross_tenant sharing ENABLED "
                     "(min_similarity={}) — cross-tenant hits will be audited",
                     config.cacheCrossTenantMinSimilarity());
    }
    {
        AdaptiveThresholdConfig adaptive;
        adaptive.enabled = config.cacheAdaptiveEnabled();
        adaptive.min_threshold = config.cacheAdaptiveMinThreshold();
        adaptive.max_threshold = config.cacheAdaptiveMaxThreshold();
        adaptive.adjustment_rate = config.cacheAdaptiveAdjustmentRate();
        adaptive.window_size = static_cast<size_t>(config.cacheAdaptiveWindowSize());
        cache->setAdaptiveConfig(adaptive);
    }
    // Phase 6.4 wire (Epic 5.1b) — ConversationIdResolver + SummarizerFactory.
    // Both default-disabled: the SemanticCache continues to behave exactly as
    // before unless cache.conversation_cache.enabled flips to true. PIIFilter
    // (ap.pii_filter) is forwarded into the summarizer for SR4.
    if (config.conversationCacheEnabled()) {
        if (config.conversationIdResolverEnabled()) {
            cache->setConversationIdResolver(
                std::make_shared<ConversationIdResolver>());
            spdlog::info("SemanticCache: ConversationIdResolver active "
                         "(client metadata.conversation_id wins, hash fallback)");
        }
        SummarizerConfig sum_cfg;
        const auto sum_type = config.conversationSummarizerType();
        sum_cfg.type = (sum_type == "onnx") ? "onnx" : "rule";
        sum_cfg.onnx_model_path = config.conversationSummarizerOnnxModelPath();
        sum_cfg.onnx_timeout_ms = config.conversationSummarizerMaxSummaryMs();
        cache->setConversationSummarizer(
            std::shared_ptr<ConversationSummarizer>(
                makeSummarizer(sum_cfg, ap.pii_filter)));
        spdlog::info("SemanticCache: ConversationSummarizer wired (type={}, "
                     "max_summary_ms={})",
                     sum_cfg.type, sum_cfg.onnx_timeout_ms);
    }

    auto warmed = cache->warmUp();
    if (warmed > 0) {
        spdlog::info("SemanticCache: restored {} entries from {}", warmed,
                     ap.cache_store->backendName());
    }
    ap.semantic_cache = cache.get();
    ap.inbound.addStage(std::move(cache));

    // --- Outbound pipeline (runs after model invocation) ---

    // 1. Content filter
    auto content = std::make_unique<ContentFilter>();
    content->addDefaultPatterns();
    ap.outbound.addStage(std::move(content));

    // 1b. Outbound PII filter (P1-3 / SR-2): redact PII in the model response
    // for both streaming (processChunk) and non-streaming (process over
    // accumulated_response) paths, achieving parity with inbound masking.
    auto outbound_pii = std::make_unique<PIIFilter>();
    outbound_pii->setOutbound(true);
    ap.outbound.addStage(std::move(outbound_pii));

    // 2. Hallucination detector
    ap.outbound.addStage(std::make_unique<HallucinationDetector>());

    // P0-5: create the per-model quality EMA and per-app cost attribution
    // aggregators *before* the outbound stages that feed them, so the scorer /
    // tracker can be wired. (Previously these were created far below at the
    // Phase-8 section, after the stages had already been moved into the
    // pipeline, leaving them permanently unfed dead components.)
    QualityMonitorConfig quality_config;
    quality_config.enabled = config.qualityMonitoringEnabled();
    quality_config.alert_threshold = config.qualityMonitoringAlertThreshold();
    ap.quality_monitor = std::make_unique<QualityMonitor>(quality_config);
    ap.cost_attribution = std::make_unique<CostAttribution>();
    AnomalyDetectorConfig anomaly_config;
    anomaly_config.enabled = config.anomalyDetectionEnabled();
    anomaly_config.z_score_threshold = config.anomalyDetectionZScoreThreshold();
    anomaly_config.window_size =
        static_cast<size_t>(config.anomalyDetectionWindowSize());
    ap.anomaly_detector = std::make_unique<AnomalyDetector>(anomaly_config);

    CostOptimizerConfig optimizer_config;
    optimizer_config.enabled = config.costOptimizationEnabled();
    ap.cost_optimizer = std::make_unique<CostOptimizer>(optimizer_config);

    // 3. Quality scorer (before cost tracker so alerts can reference quality)
    auto quality_scorer = std::make_unique<QualityScorer>();
    quality_scorer->setQualityMonitor(ap.quality_monitor.get());  // P0-5
    ap.outbound.addStage(std::move(quality_scorer));

    auto cost = std::make_unique<CostTracker>();
    cost->setPersistentStore(ap.persistent_store.get());
    cost->setCostAttribution(ap.cost_attribution.get());  // P0-5
    if (config.anomalyDetectionEnabled()) {
        cost->setAnomalyDetector(ap.anomaly_detector.get());
    }
    if (config.costOptimizationEnabled()) {
        cost->setCostOptimizer(ap.cost_optimizer.get());
    }
    // TASK-20260617-02: 启动回读历史成本记录，使重启后仪表盘成本聚合恢复。
    if (config.dashboardPersistenceEnabled()) {
        cost->loadFromStore(config.dashboardReloadDays(), /*cap=*/0);
    }
    ap.cost_tracker = cost.get();
    ap.outbound.addStage(std::move(cost));

    // 4. Alert manager (metrics rules — after cost, before request log)
    ap.alert_manager = std::make_shared<AlertManager>(*ap.feature_gate);
    // Load alert rules from config. Without this the rule set stays empty and
    // the alerting stage evaluates nothing in production (P0-G residual,
    // TASK-20260701-02) — channels alone never fire.
    for (const auto& rc : config.alertRules()) {
        AlertRule rule;
        rule.id = rc.id;
        rule.description = rc.description;
        rule.metric_name = rc.metric_name;
        rule.threshold = rc.threshold;
        rule.enabled = rc.enabled;
        rule.severity = parseAlertSeverity(rc.severity);
        rule.cooldown_seconds = rc.cooldown_seconds;
        ap.alert_manager->addRule(rule);
        spdlog::info("Alert rule registered: {} (metric={} threshold={})",
                     rule.id, rule.metric_name, rule.threshold);
    }
    {
        auto channel_configs = config.alertChannels();
        if (!channel_configs.empty()) {
            auto dispatcher = std::make_shared<AlertDispatcher>();
            for (const auto& ch_cfg : channel_configs) {
                if (ch_cfg.type == "webhook") {
                    dispatcher->addChannel(ch_cfg.name,
                        makeWebhookChannel(ch_cfg.url, ch_cfg.secret));
                } else if (ch_cfg.type == "dingtalk") {
                    dispatcher->addChannel(ch_cfg.name,
                        makeDingTalkChannel(ch_cfg.url));
                } else if (ch_cfg.type == "feishu") {
                    dispatcher->addChannel(ch_cfg.name,
                        makeFeishuChannel(ch_cfg.url));
                } else if (ch_cfg.type == "slack") {
                    dispatcher->addChannel(ch_cfg.name,
                        makeSlackChannel(ch_cfg.url));
                } else {
                    spdlog::warn("Unknown alert channel type: {}", ch_cfg.type);
                    continue;
                }
                spdlog::info("Alert channel registered: {} ({})", ch_cfg.name, ch_cfg.type);
            }
            ap.alert_manager->setChannel(
                [dispatcher](const Alert& a) { dispatcher->dispatch(a); });
        }
    }
    ap.outbound.addStage(
        std::make_unique<SharedAlertManagerStage>(ap.alert_manager));

    // 5. Request logger (last stage — records final state)
    auto logger = std::make_unique<RequestLogger>();
    ap.request_logger = logger.get();
    ap.outbound.addStage(std::move(logger));

    // --- Plugin stages (dynamic .so loading) ---
    if (config.pluginEnabled()) {
        ap.plugin_loader = std::make_unique<PluginLoader>();
        auto search_path = config.pluginSearchPath();
        for (const auto& pcfg : config.pluginStages()) {
            std::string so_path = pcfg.path;
            if (so_path.find('/') == std::string::npos) {
                so_path = search_path + "/" + so_path;
            }
            if (!ap.plugin_loader->load(so_path)) {
                spdlog::warn("Failed to load plugin: {}", pcfg.name);
                continue;
            }
            auto stage = ap.plugin_loader->createStage(pcfg.name);
            if (!stage) continue;

            if (pcfg.position == "inbound") {
                ap.inbound.addStage(std::move(stage));
                spdlog::info("Plugin '{}' added to inbound pipeline", pcfg.name);
            } else {
                ap.outbound.addStage(std::move(stage));
                spdlog::info("Plugin '{}' added to outbound pipeline", pcfg.name);
            }
        }
    }

    // --- Phase 8: Agent orchestration (enterprise only) ---
    if (ap.feature_gate && ap.feature_gate->isEnabled(Feature::AgentOrchestration)) {
        ap.tool_registry = std::make_unique<ToolRegistry>();
        ap.tool_sandbox = std::make_unique<ToolSandbox>(ap.tool_registry.get());
        ap.orchestrator = std::make_unique<Orchestrator>();
        ap.orchestrator->setToolRegistry(ap.tool_registry.get());
        ap.orchestrator->setToolSandbox(ap.tool_sandbox.get());
        spdlog::info("AgentOrchestration: tool registry + sandbox + orchestrator active");
    }

    spdlog::info("Pipeline assembled: edition={}, inbound stages={}, outbound stages={}",
                 config.edition() == Edition::Enterprise ? "enterprise" : "community",
                 ap.inbound.stageCount(), ap.outbound.stageCount());
    return ap;
}

} // namespace aegisgate
