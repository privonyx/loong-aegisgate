#pragma once
#include "core/pipeline.h"
#include "cache/embedder.h"
#include "cache/vector_store.h"
#include "cache/conversation_summarizer.h"
#include "cache/conversation_id_resolver.h"
#include <string>
#include <unordered_map>
#include <list>
#include <deque>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <functional>
#include <utility>
#include <atomic>

namespace aegisgate {

class CacheStore;
class PartitionedVectorIndex;

enum class ConversationHashMode {
    None,
    Full,
    Window,
};

struct ConversationHashConfig {
    ConversationHashMode mode = ConversationHashMode::None;
    size_t window_size = 4;
};

struct CacheEntry {
    std::string id;
    std::string prompt;
    std::string response;
    std::string model;
    std::string partition_key;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
};

struct CacheHit {
    std::string response;
    std::string original_prompt;
    float similarity;
};

struct CacheKeyInfo {
    std::string partition_key;
    std::string prompt;
    std::string conversation_hash;
};

struct AdaptiveThresholdConfig {
    bool enabled = false;
    float min_threshold = 0.85f;
    float max_threshold = 0.98f;
    float adjustment_rate = 0.01f;
    size_t window_size = 100;
    float target_hit_rate_low = 0.15f;
    float target_hit_rate_high = 0.60f;
};

struct CachePolicy {
    bool enabled = false;
    std::vector<std::string> skip_models;
    double max_temperature = 1.0;
    bool skip_streaming = false;
};

struct CacheStats {
    uint64_t hit_count = 0;
    uint64_t miss_count = 0;
    uint64_t put_count = 0;
    uint64_t entry_count = 0;
    float hit_rate = 0.0f;
    float current_threshold = 0.0f;

    // TASK-20260527-02 (MVP-5 case-study) — hit_by_type taxonomy (D6=A 3 类).
    // Always satisfies: hit_exact + hit_semantic + hit_conversation == hit_count.
    uint64_t hit_exact = 0;          // partition_key empty + max_sim ≥ 0.9999
    uint64_t hit_semantic = 0;        // partition_key empty + max_sim < 0.9999
    uint64_t hit_conversation = 0;    // partition_key non-empty (V2 conversation path)
};

struct CacheFeedback {
    std::string entry_id;
    double satisfaction = 1.0;
    std::string request_id;

    CacheFeedback() = default;
    CacheFeedback(std::string eid, double sat, std::string rid)
        : entry_id(std::move(eid)), satisfaction(sat), request_id(std::move(rid)) {}
};

struct QueryPattern {
    std::string prompt;
    size_t frequency = 0;
    std::chrono::steady_clock::time_point last_seen;

    QueryPattern() = default;
    QueryPattern(std::string p, size_t f)
        : prompt(std::move(p)), frequency(f),
          last_seen(std::chrono::steady_clock::now()) {}
};

struct CrossTenantConfig {
    bool enabled = false;
    float min_similarity_for_sharing = 0.95f;

    CrossTenantConfig() = default;
    CrossTenantConfig(bool en, float min_sim)
        : enabled(en), min_similarity_for_sharing(min_sim) {}
};

class SemanticCache : public PipelineStage {
public:
    SemanticCache(Embedder& embedder, VectorStore& store,
                  float threshold = 0.90f,
                  std::chrono::seconds ttl = std::chrono::seconds(3600),
                  size_t max_entries = 10000);

    void setCacheStore(CacheStore* store) { cache_store_ = store; }
    size_t warmUp();

    std::optional<CacheHit> get(const std::string& prompt,
                                 const std::string& model = "",
                                 const std::string& partition_key = "",
                                 bool conversation_scoped = false);
    void put(const std::string& prompt, const std::string& response,
             const std::string& model = "",
             const std::string& partition_key = "");
    void putFromContext(const std::vector<Message>& messages,
                        const std::string& response,
                        const std::string& model = "",
                        const std::string& tenant_id = "",
                        const std::string& conversation_id = "");

    std::string extractCacheKey(const std::vector<Message>& messages) const;
    CacheKeyInfo extractCacheKeyInfo(const std::vector<Message>& messages) const;

    void setContextAware(bool enabled) { context_aware_ = enabled; }
    bool contextAware() const { return context_aware_; }

    void setConversationHashConfig(const ConversationHashConfig& cfg) {
        conversation_hash_config_ = cfg;
    }
    const ConversationHashConfig& conversationHashConfig() const {
        return conversation_hash_config_;
    }
    size_t importFromJson(const std::string& json_str);
    void setThreshold(float threshold) { threshold_ = threshold; }
    void setCachePolicy(const CachePolicy& policy) { policy_ = policy; }
    const CachePolicy& cachePolicy() const { return policy_; }
    bool shouldCache(const RequestContext& ctx) const;

    void setAdaptiveConfig(const AdaptiveThresholdConfig& cfg) { adaptive_config_ = cfg; }
    const AdaptiveThresholdConfig& adaptiveConfig() const { return adaptive_config_; }
    float currentThreshold() const { return threshold_; }

    static std::string computeConversationHash(
        const std::vector<Message>& messages,
        const ConversationHashConfig& config);

    // V2 (D2=C + D3=B + SR1): sha256-based, optionally injects a summarizer
    // for richer multi-turn semantic context. Excludes the trailing user
    // message (treated as the current prompt embedded separately).
    static std::string computeConversationHashV2(
        const std::vector<Message>& messages,
        const ConversationHashConfig& config,
        ConversationSummarizer* summarizer);

    // V2 partition key derivation: sha256(tenant_id | conversation_id |
    //                                     sys_hash | conversation_hash_v2).
    // SR1: tenant_id is mixed in unconditionally so two tenants with the same
    //      client-supplied conversation_id can never collide.
    CacheKeyInfo extractCacheKeyInfoV2(
        const std::vector<Message>& messages,
        const std::string& tenant_id,
        const std::string& conversation_id = "",
        ConversationSummarizer* summarizer = nullptr) const;

    // Optional injectors for the runtime-assembled summarizer / id resolver.
    // When both are set, callers can pass an empty conversation_id and the
    // resolver will derive one from the request.
    void setConversationIdResolver(std::shared_ptr<ConversationIdResolver> r) {
        conversation_id_resolver_ = std::move(r);
    }
    void setConversationSummarizer(std::shared_ptr<ConversationSummarizer> s) {
        conversation_summarizer_ = std::move(s);
    }
    ConversationIdResolver* conversationIdResolver() const {
        return conversation_id_resolver_.get();
    }
    ConversationSummarizer* conversationSummarizer() const {
        return conversation_summarizer_.get();
    }

    void recordFeedback(const std::string& prompt, double satisfaction);
    double getAverageSatisfaction() const;

    void recordQueryPattern(const std::string& prompt);
    std::vector<QueryPattern> getTopPatterns(size_t limit = 50) const;

    void setCrossTenantConfig(const CrossTenantConfig& config);
    std::optional<CacheHit> getCrossTenant(const std::string& prompt,
                                            const std::string& model = "");

    CacheStats getStats() const;
    size_t size() const;
    void clear();
    void evictExpired();

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "SemanticCache"; }

    static constexpr const char* kCacheKeyPrefix = "sc:";

    // TASK-20260527-02: pure helper for hit_by_type taxonomy (D6=A 3 类).
    // Returns one of "exact" | "semantic" | "conversation".
    // - conversation_scoped (real multi-turn context) → "conversation"
    // - else max_similarity ≥ 0.9999 → "exact"
    // - else "semantic"
    // TASK-20260609-01 (P0-1 / SR-1, D1=C): the conversation signal is now an
    // explicit flag derived from a non-empty conversation_hash, decoupled from
    // the V2 partition_key (which is always non-empty once tenant_id is mixed
    // in for cross-tenant isolation). Without this, every production hit would
    // misreport as "conversation".
    // Defined as static so unit tests can verify the classification logic
    // without spinning up a full SemanticCache + VectorStore + Embedder.
    static std::string classifyHitType(float max_similarity,
                                        bool conversation_scoped);

private:
    std::string makeNamespace(const std::string& model) const;
    std::string makeEntryId(const std::string& ns, size_t seq) const;
    void evictLRU(std::vector<std::pair<std::string, std::string>>& removed_ids); // caller must hold mutex_
    void evictExpiredLocked(std::vector<std::pair<std::string, std::string>>& removed_ids); // caller must hold mutex_
    void persistEntry(const std::string& id, const CacheEntry& entry); // caller must hold mutex_; may acquire CacheStore lock (Layer 1 → 2)
    void recordHitMiss(bool is_hit, float max_similarity);
    void maybeAdjustThreshold(); // caller must hold mutex_
    float computeP50() const;   // caller must hold mutex_

    Embedder& embedder_;
    VectorStore& vector_store_;
    float threshold_;
    std::chrono::seconds ttl_;
    size_t max_entries_;

    CacheStore* cache_store_ = nullptr;
    bool context_aware_ = false;
    ConversationHashConfig conversation_hash_config_;
    CachePolicy policy_;
    AdaptiveThresholdConfig adaptive_config_;
    std::deque<bool> adaptive_hit_window_;
    std::deque<float> adaptive_similarity_window_;

    std::atomic<uint64_t> stat_hits_{0};
    std::atomic<uint64_t> stat_misses_{0};
    std::atomic<uint64_t> stat_puts_{0};

    // TASK-20260527-02 — hit_by_type taxonomy counters.
    std::atomic<uint64_t> stat_hit_exact_{0};
    std::atomic<uint64_t> stat_hit_semantic_{0};
    std::atomic<uint64_t> stat_hit_conversation_{0};

    std::unordered_map<std::string, CacheEntry> entries_;
    std::list<std::string> lru_list_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;
    size_t seq_counter_ = 0;
    size_t put_counter_ = 0;
    static constexpr size_t kEvictInterval = 100;

    std::deque<double> feedback_scores_;
    static constexpr size_t kMaxFeedbackWindow = 1000;

    std::unordered_map<std::string, QueryPattern> query_patterns_;
    static constexpr size_t kMaxQueryPatterns = 1000;

    CrossTenantConfig cross_tenant_config_;

    std::shared_ptr<ConversationIdResolver> conversation_id_resolver_;
    std::shared_ptr<ConversationSummarizer> conversation_summarizer_;

    mutable std::mutex mutex_; // Lock Layer 1 — see docs/LOCK_ORDERING.md
};

} // namespace aegisgate
