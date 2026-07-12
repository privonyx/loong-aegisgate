#include "cache/semantic_cache.h"
#include "core/crypto.h"
#include "observe/metrics.h"
#include "storage/cache_store.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <functional>
#include <numeric>

namespace aegisgate {

SemanticCache::SemanticCache(Embedder& embedder, VectorStore& store,
                              float threshold, std::chrono::seconds ttl,
                              size_t max_entries)
    : embedder_(embedder), vector_store_(store), threshold_(threshold),
      ttl_(ttl), max_entries_(max_entries) {}

std::string SemanticCache::makeNamespace(const std::string& model) const {
    std::hash<std::string> hasher;
    return "ns_" + std::to_string(hasher(model));
}

// TASK-20260527-02 (MVP-5 case-study) — hit_by_type classification helper.
// Static / pure / no side effects: testable without Embedder + VectorStore.
std::string SemanticCache::classifyHitType(float max_similarity,
                                           bool conversation_scoped) {
    if (conversation_scoped) return "conversation";
    if (max_similarity >= 0.9999f) return "exact";
    return "semantic";
}

std::string SemanticCache::makeEntryId(const std::string& ns, size_t seq) const {
    return ns + "_" + std::to_string(seq);
}

std::optional<CacheHit> SemanticCache::get(const std::string& prompt,
                                            const std::string& model,
                                            const std::string& partition_key,
                                            bool conversation_scoped) {
    auto vec = embedder_.embed(prompt);
    auto results = vector_store_.search(partition_key, vec, 1, threshold_);

    float max_sim = results.empty() ? 0.0f : results[0].score;

    if (results.empty()) {
        ++stat_misses_;
        recordHitMiss(false, max_sim);
        // D3=A / SR-6: a partition miss (e.g. the requesting tenant has no
        // entries at all) must still fall through to the cross_tenant path when
        // sharing is explicitly enabled — otherwise getCrossTenant stays a dead
        // path for the most common scenario. Off by default → nullopt (isolation
        // preserved, zero regression).
        if (cross_tenant_config_.enabled) {
            return getCrossTenant(prompt, model);
        }
        return std::nullopt;
    }

    std::string expired_id;
    std::optional<CacheHit> result;
    bool is_hit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(results[0].id);
        if (it == entries_.end()) {
            // entry disappeared
        } else {
            auto now = std::chrono::steady_clock::now();
            if (now - it->second.created_at > ttl_) {
                expired_id = results[0].id;
                auto lru_it = lru_map_.find(results[0].id);
                if (lru_it != lru_map_.end()) {
                    lru_list_.erase(lru_it->second);
                    lru_map_.erase(lru_it);
                }
                entries_.erase(it);
            } else if (!model.empty() && !it->second.model.empty() &&
                       it->second.model != model) {
                // model mismatch
            } else if (it->second.partition_key != partition_key) {
                // P0-B (TASK-20260701-01): cross-partition match. This is only
                // reachable once the PartitionedVectorIndex overflow bucket
                // merges distinct logical partitions into one physical index,
                // letting a search under partition B surface an id inserted
                // under partition A. Reject it to preserve tenant/conversation
                // isolation — the entry carries its originating partition_key,
                // so a key mismatch means a different logical owner. The
                // legitimate cross-tenant sharing path (when explicitly enabled)
                // is handled separately below via getCrossTenant().
            } else {
                auto lru_it = lru_map_.find(results[0].id);
                if (lru_it != lru_map_.end()) {
                    lru_list_.erase(lru_it->second);
                    lru_list_.push_front(results[0].id);
                    lru_it->second = lru_list_.begin();
                }
                it->second.last_accessed = now;
                result = CacheHit{it->second.response, it->second.prompt, results[0].score};
                is_hit = true;
            }
        }
    }

    if (!expired_id.empty()) {
        vector_store_.remove(partition_key, expired_id);
    }

    if (is_hit) {
        ++stat_hits_;
        // TASK-20260527-02 — bump per-type counter (D6=A 3 类).
        const auto hit_type = classifyHitType(max_sim, conversation_scoped);
        if (hit_type == "exact") {
            ++stat_hit_exact_;
        } else if (hit_type == "semantic") {
            ++stat_hit_semantic_;
        } else {  // conversation
            ++stat_hit_conversation_;
        }
    } else {
        ++stat_misses_;
    }
    recordHitMiss(is_hit, max_sim);

    if (!result.has_value() && cross_tenant_config_.enabled) {
        return getCrossTenant(prompt, model);
    }

    return result;
}

void SemanticCache::put(const std::string& prompt, const std::string& response,
                         const std::string& model,
                         const std::string& partition_key) {
    ++stat_puts_;
    auto vec = embedder_.embed(prompt);
    std::string ns = makeNamespace(model);
    std::string id;

    std::vector<std::pair<std::string, std::string>> removed_ids;

    // Phase 1: allocate ID and evict under SC lock (no VI calls)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++put_counter_;
        if (put_counter_ % kEvictInterval == 0) {
            evictExpiredLocked(removed_ids);
        }
        if (entries_.size() >= max_entries_) {
            evictLRU(removed_ids);
        }
        id = makeEntryId(ns, seq_counter_++);
    }

    // Phase 2: VectorStore operations outside SC lock
    for (const auto& [rid, rpk] : removed_ids) vector_store_.remove(rpk, rid);

    // P2-#1: only register the logical entry if the vector is actually durable
    // in the index. Previously the entry was committed unconditionally, so a
    // failed insert (dim mismatch / capacity / backend error) left a phantom in
    // entries_ that wastes a capacity slot, can never be matched by search, and
    // — once persisted — reloads into a split-brain cache.
    if (!vector_store_.insert(partition_key, id, vec)) {
        spdlog::warn("SemanticCache::put vector insert failed (id={}), "
                     "skipping logical entry to avoid split-brain", id);
        return;
    }

    // Phase 3: update local state under SC lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        CacheEntry entry;
        entry.id = id;
        entry.prompt = prompt;
        entry.response = response;
        entry.model = model;
        entry.partition_key = partition_key;
        entry.created_at = now;
        entry.last_accessed = now;

        entries_[id] = entry;
        lru_list_.push_front(id);
        lru_map_[id] = lru_list_.begin();

        persistEntry(id, entry);
    }
}

void SemanticCache::recordHitMiss(bool is_hit, float max_similarity) {
    if (!adaptive_config_.enabled) return;
    std::lock_guard<std::mutex> lock(mutex_);
    adaptive_hit_window_.push_back(is_hit);
    adaptive_similarity_window_.push_back(max_similarity);
    while (adaptive_hit_window_.size() > adaptive_config_.window_size) {
        adaptive_hit_window_.pop_front();
    }
    while (adaptive_similarity_window_.size() > adaptive_config_.window_size) {
        adaptive_similarity_window_.pop_front();
    }
    if (adaptive_hit_window_.size() == adaptive_config_.window_size) {
        maybeAdjustThreshold();
    }
}

float SemanticCache::computeP50() const {
    if (adaptive_similarity_window_.empty()) return 0.0f;
    auto copy = std::vector<float>(adaptive_similarity_window_.begin(),
                                    adaptive_similarity_window_.end());
    auto mid = copy.begin() + static_cast<long>(copy.size() / 2);
    std::nth_element(copy.begin(), mid, copy.end());
    return *mid;
}

void SemanticCache::maybeAdjustThreshold() {
    size_t hits = 0;
    for (bool h : adaptive_hit_window_) if (h) ++hits;
    float hit_rate = static_cast<float>(hits) /
                     static_cast<float>(adaptive_hit_window_.size());
    float p50_sim = computeP50();

    if (hit_rate < adaptive_config_.target_hit_rate_low &&
        p50_sim > threshold_ * 0.9f) {
        threshold_ = std::max(adaptive_config_.min_threshold,
                              threshold_ - adaptive_config_.adjustment_rate);
        spdlog::debug("AdaptiveThreshold: decreased to {:.4f} "
                      "(hit_rate={:.2f}, p50_sim={:.3f})",
                      threshold_, hit_rate, p50_sim);
    } else if (hit_rate > adaptive_config_.target_hit_rate_high &&
               p50_sim < threshold_ * 1.05f) {
        threshold_ = std::min(adaptive_config_.max_threshold,
                              threshold_ + adaptive_config_.adjustment_rate * 0.5f);
        spdlog::debug("AdaptiveThreshold: increased to {:.4f} "
                      "(hit_rate={:.2f}, p50_sim={:.3f})",
                      threshold_, hit_rate, p50_sim);
    }

    adaptive_hit_window_.clear();
    adaptive_similarity_window_.clear();
}

bool SemanticCache::shouldCache(const RequestContext& ctx) const {
    if (!policy_.enabled) return true;
    for (const auto& m : policy_.skip_models) {
        if (ctx.chat_request.model == m) return false;
    }
    if (ctx.chat_request.temperature.has_value() &&
        *ctx.chat_request.temperature > policy_.max_temperature) {
        return false;
    }
    if (policy_.skip_streaming && ctx.chat_request.stream) return false;
    return true;
}

CacheStats SemanticCache::getStats() const {
    CacheStats stats;
    stats.hit_count = stat_hits_.load(std::memory_order_relaxed);
    stats.miss_count = stat_misses_.load(std::memory_order_relaxed);
    stats.put_count = stat_puts_.load(std::memory_order_relaxed);
    stats.hit_exact = stat_hit_exact_.load(std::memory_order_relaxed);
    stats.hit_semantic = stat_hit_semantic_.load(std::memory_order_relaxed);
    stats.hit_conversation = stat_hit_conversation_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.entry_count = entries_.size();
    }
    auto total = stats.hit_count + stats.miss_count;
    stats.hit_rate = (total > 0)
        ? static_cast<float>(stats.hit_count) / static_cast<float>(total)
        : 0.0f;
    stats.current_threshold = threshold_;
    return stats;
}

size_t SemanticCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void SemanticCache::clear() {
    std::vector<std::pair<std::string, std::string>> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.reserve(entries_.size());
        for (auto& [id, entry] : entries_) ids.emplace_back(id, entry.partition_key);
        entries_.clear();
        lru_list_.clear();
        lru_map_.clear();
    }
    for (const auto& [id, pk] : ids) vector_store_.remove(pk, id);
}

void SemanticCache::evictExpired() {
    std::vector<std::pair<std::string, std::string>> expired_ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        evictExpiredLocked(expired_ids);
    }
    for (const auto& [id, pk] : expired_ids) vector_store_.remove(pk, id);
}

void SemanticCache::evictExpiredLocked(std::vector<std::pair<std::string, std::string>>& removed_ids) {
    auto now = std::chrono::steady_clock::now();
    auto it = entries_.begin();
    while (it != entries_.end()) {
        if (now - it->second.created_at > ttl_) {
            removed_ids.emplace_back(it->first, it->second.partition_key);
            auto lru_it = lru_map_.find(it->first);
            if (lru_it != lru_map_.end()) {
                lru_list_.erase(lru_it->second);
                lru_map_.erase(lru_it);
            }
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string SemanticCache::extractCacheKey(const std::vector<Message>& messages) const {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user") {
            return it->content;
        }
    }
    return "";
}

std::string SemanticCache::computeConversationHash(
    const std::vector<Message>& messages,
    const ConversationHashConfig& config) {

    if (config.mode == ConversationHashMode::None || messages.size() <= 1) {
        return "";
    }

    std::hash<std::string> hasher;
    size_t combined = 0;

    auto combine = [&](const std::string& role, const std::string& content) {
        combined ^= hasher(role) + 0x9e3779b9 + (combined << 6) + (combined >> 2);
        combined ^= hasher(content) + 0x9e3779b9 + (combined << 6) + (combined >> 2);
    };

    if (config.mode == ConversationHashMode::Full) {
        for (size_t i = 0; i + 1 < messages.size(); ++i) {
            combine(messages[i].role, messages[i].content);
        }
    } else {
        size_t ctx_count = messages.size() > 1 ? messages.size() - 1 : 0;
        size_t window = std::min(config.window_size, ctx_count);
        size_t start = ctx_count - window;
        for (size_t i = start; i < start + window; ++i) {
            combine(messages[i].role, messages[i].content);
        }
    }

    return "conv_" + std::to_string(combined);
}

CacheKeyInfo SemanticCache::extractCacheKeyInfo(const std::vector<Message>& messages) const {
    CacheKeyInfo info;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user" && info.prompt.empty()) {
            info.prompt = it->content;
        }
    }
    if (context_aware_) {
        for (const auto& msg : messages) {
            if (msg.role == "system" && !msg.content.empty()) {
                std::hash<std::string> hasher;
                info.partition_key = "sys_" + std::to_string(hasher(msg.content));
                break;
            }
        }
    }

    if (conversation_hash_config_.mode != ConversationHashMode::None) {
        info.conversation_hash = computeConversationHash(
            messages, conversation_hash_config_);
        if (!info.conversation_hash.empty()) {
            if (info.partition_key.empty()) {
                info.partition_key = info.conversation_hash;
            } else {
                info.partition_key += "|" + info.conversation_hash;
            }
        }
    }
    return info;
}

std::string SemanticCache::computeConversationHashV2(
    const std::vector<Message>& messages,
    const ConversationHashConfig& config,
    ConversationSummarizer* summarizer) {
    if (config.mode == ConversationHashMode::None || messages.empty()) {
        return "";
    }

    // Exclude trailing user message (current turn) so the hash represents
    // accumulated context only.
    size_t end = messages.size();
    if (end > 0 && messages.back().role == "user") --end;
    if (end == 0) return "";

    size_t start = 0;
    if (config.mode == ConversationHashMode::Window) {
        const size_t window = std::min(config.window_size, end);
        start = end - window;
    }

    std::string body;
    body.reserve(2048);
    for (size_t i = start; i < end; ++i) {
        body.append(messages[i].role);
        body.push_back('\x1f');
        body.append(messages[i].content);
        body.push_back('\x1e');
    }

    if (summarizer) {
        std::vector<Message> ctx(messages.begin() + static_cast<long>(start),
                                  messages.begin() + static_cast<long>(end));
        const auto summary = summarizer->summarize(ctx);
        if (!summary.empty()) {
            body.append("\x1d|summary|");
            body.append(summary);
        }
    }

    return crypto::sha256(body);
}

CacheKeyInfo SemanticCache::extractCacheKeyInfoV2(
    const std::vector<Message>& messages,
    const std::string& tenant_id,
    const std::string& conversation_id,
    ConversationSummarizer* summarizer) const {
    CacheKeyInfo info;
    if (messages.empty()) return info;

    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user" && info.prompt.empty()) {
            info.prompt = it->content;
            break;
        }
    }

    std::string sys_hash;
    for (const auto& m : messages) {
        if (m.role == "system" && !m.content.empty()) {
            sys_hash = crypto::sha256(m.content);
            break;
        }
    }

    info.conversation_hash = computeConversationHashV2(
        messages, conversation_hash_config_, summarizer);

    // SR1 cross-tenant hard isolation: tenant_id is mixed in unconditionally.
    // Field separator '\x1f' prevents adversarial concatenation attacks
    // (e.g., tenant="a|b" must not collide with tenant="a" + conv="b").
    std::string blob;
    blob.reserve(tenant_id.size() + conversation_id.size() + sys_hash.size() +
                 info.conversation_hash.size() + 8);
    blob.append(tenant_id);
    blob.push_back('\x1f');
    blob.append(conversation_id);
    blob.push_back('\x1f');
    blob.append(sys_hash);
    blob.push_back('\x1f');
    blob.append(info.conversation_hash);
    info.partition_key = crypto::sha256(blob);
    return info;
}

void SemanticCache::putFromContext(const std::vector<Message>& messages,
                                   const std::string& response,
                                   const std::string& model,
                                   const std::string& tenant_id,
                                   const std::string& conversation_id) {
    if (policy_.enabled) {
        for (const auto& m : policy_.skip_models) {
            if (model == m) return;
        }
    }
    // P0-1 / SR-1 (D1=C): derive a tenant-isolated partition key via V2 so the
    // write key aligns with the V2 read key in process(). tenant_id is mixed in
    // unconditionally → two tenants can never share a partition.
    auto info = extractCacheKeyInfoV2(messages, tenant_id, conversation_id,
                                      conversation_summarizer_.get());
    if (!info.prompt.empty()) {
        put(info.prompt, response, model, info.partition_key);
    }
}

void SemanticCache::evictLRU(std::vector<std::pair<std::string, std::string>>& removed_ids) {
    if (lru_list_.empty()) return;
    auto oldest_id = lru_list_.back();
    lru_list_.pop_back();
    auto eit = entries_.find(oldest_id);
    std::string pk = (eit != entries_.end()) ? eit->second.partition_key : "";
    if (eit != entries_.end()) entries_.erase(eit);
    lru_map_.erase(oldest_id);
    removed_ids.emplace_back(oldest_id, pk);
}

void SemanticCache::persistEntry(const std::string& id, const CacheEntry& entry) {
    if (!cache_store_) return;
    try {
        nlohmann::json j;
        j["id"] = entry.id;
        j["prompt"] = entry.prompt;
        j["response"] = entry.response;
        j["model"] = entry.model;
        j["partition_key"] = entry.partition_key;
        std::string store_key = std::string(kCacheKeyPrefix) + id;
        cache_store_->set(store_key, j.dump(), ttl_);
    } catch (const std::exception& e) {
        spdlog::warn("SemanticCache persist failed for {}: {}", id, e.what());
    }
}

size_t SemanticCache::warmUp() {
    if (!cache_store_) return 0;
    auto stored_keys = cache_store_->keys(kCacheKeyPrefix, static_cast<int>(max_entries_));
    size_t loaded = 0;

    static constexpr size_t kMaxWarmUpValueSize = 1024 * 1024;
    for (const auto& store_key : stored_keys) {
        auto val = cache_store_->get(store_key);
        if (!val) continue;
        if (val->size() > kMaxWarmUpValueSize) {
            spdlog::warn("SemanticCache warmUp: skipping oversized entry {} ({} bytes)",
                         store_key, val->size());
            continue;
        }
        try {
            auto j = nlohmann::json::parse(*val);
            std::string id = j.value("id", "");
            std::string prompt = j.value("prompt", "");
            std::string response = j.value("response", "");
            std::string model = j.value("model", "");
            std::string pk = j.value("partition_key", "");
            if (id.empty() || prompt.empty()) continue;

            auto vec = embedder_.embed(prompt);
            auto now = std::chrono::steady_clock::now();

            bool should_insert = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!entries_.count(id) && entries_.size() < max_entries_) {
                    CacheEntry entry;
                    entry.id = id;
                    entry.prompt = prompt;
                    entry.response = response;
                    entry.model = model;
                    entry.partition_key = pk;
                    entry.created_at = now;
                    entry.last_accessed = now;
                    entries_[id] = std::move(entry);
                    lru_list_.push_back(id);
                    lru_map_[id] = std::prev(lru_list_.end());
                    should_insert = true;
                    ++loaded;
                }
            }
            if (should_insert) {
                vector_store_.insert(pk, id, vec);
            }
        } catch (const std::exception& e) {
            spdlog::warn("SemanticCache warmUp parse error: {}", e.what());
        }
    }
    if (loaded > 0) {
        spdlog::info("SemanticCache warmed up {} entries from CacheStore", loaded);
    }
    return loaded;
}

size_t SemanticCache::importFromJson(const std::string& json_str) {
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(json_str);
    } catch (const std::exception& e) {
        spdlog::warn("importFromJson: invalid JSON: {}", e.what());
        return 0;
    }
    if (!data.is_array()) {
        spdlog::warn("importFromJson: expected JSON array");
        return 0;
    }

    size_t imported = 0;
    for (const auto& item : data) {
        auto prompt = item.value("prompt", "");
        auto response = item.value("response", "");
        auto model = item.value("model", "");
        if (prompt.empty() || response.empty()) continue;
        put(prompt, response, model);
        ++imported;
    }
    if (imported > 0) {
        spdlog::info("importFromJson: imported {} entries", imported);
    }
    return imported;
}

StageResult SemanticCache::process(RequestContext& ctx) {
    if (ctx.chat_request.messages.empty()) return StageResult::Continue;
    if (ctx.has_tools) return StageResult::Continue;
    if (!shouldCache(ctx)) return StageResult::Continue;

    // P0-1 / SR-1 (D1=C): use the tenant-isolated V2 key on the read path.
    // conversation_id comes from the injected resolver (or "" when absent),
    // matching the write path in putFromContext / gateway cache store.
    std::string conv_id = conversation_id_resolver_
        ? conversation_id_resolver_->resolve(ctx.chat_request) : "";
    auto info = extractCacheKeyInfoV2(ctx.chat_request.messages, ctx.tenant_id,
                                      conv_id, conversation_summarizer_.get());
    if (info.prompt.empty()) return StageResult::Continue;

    auto hit = get(info.prompt, ctx.chat_request.model, info.partition_key,
                   !info.conversation_hash.empty());
    if (hit.has_value()) {
        ctx.cache_hit = true;
        ctx.cached_response = hit->response;
        spdlog::info("Cache hit for request {}: similarity={:.3f}",
                     ctx.request_id, hit->similarity);
        return StageResult::ShortCircuit;
    }

    recordQueryPattern(info.prompt);

    return StageResult::Continue;
}

void SemanticCache::recordFeedback(const std::string& prompt, double satisfaction) {
    // P2-#5: count every feedback signal so cache_feedback_total is no longer
    // a permanently-zero metric.
    MetricsRegistry::instance().cacheFeedbackTotal().inc();

    auto vec = embedder_.embed(prompt);
    auto results = vector_store_.search("", vec, 1, 0.0f);

    std::lock_guard<std::mutex> lock(mutex_);

    feedback_scores_.push_back(satisfaction);
    while (feedback_scores_.size() > kMaxFeedbackWindow) {
        feedback_scores_.pop_front();
    }

    if (satisfaction < 0.5 && !results.empty()) {
        auto it = entries_.find(results[0].id);
        if (it != entries_.end()) {
            auto halved_ttl = ttl_ / 2;
            auto target_time = it->second.last_accessed - (ttl_ - halved_ttl);
            it->second.created_at = target_time;
        }
    }
}

double SemanticCache::getAverageSatisfaction() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feedback_scores_.empty()) return 0.0;
    double sum = std::accumulate(feedback_scores_.begin(),
                                  feedback_scores_.end(), 0.0);
    return sum / static_cast<double>(feedback_scores_.size());
}

void SemanticCache::recordQueryPattern(const std::string& prompt) {
    std::hash<std::string> hasher;
    std::string key = std::to_string(hasher(prompt));

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = query_patterns_.find(key);
    if (it != query_patterns_.end()) {
        it->second.frequency++;
        it->second.last_seen = std::chrono::steady_clock::now();
        return;
    }

    if (query_patterns_.size() >= kMaxQueryPatterns) {
        auto min_it = query_patterns_.begin();
        for (auto cur = query_patterns_.begin(); cur != query_patterns_.end(); ++cur) {
            if (cur->second.frequency < min_it->second.frequency) {
                min_it = cur;
            }
        }
        query_patterns_.erase(min_it);
    }

    query_patterns_.emplace(key, QueryPattern(prompt, 1));
}

std::vector<QueryPattern> SemanticCache::getTopPatterns(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<QueryPattern> all;
    all.reserve(query_patterns_.size());
    for (const auto& [_, pattern] : query_patterns_) {
        all.push_back(pattern);
    }

    std::sort(all.begin(), all.end(),
              [](const QueryPattern& a, const QueryPattern& b) {
                  return a.frequency > b.frequency;
              });

    if (all.size() > limit) {
        all.resize(limit);
    }
    return all;
}

void SemanticCache::setCrossTenantConfig(const CrossTenantConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    cross_tenant_config_ = config;
}

std::optional<CacheHit> SemanticCache::getCrossTenant(
    const std::string& prompt, const std::string& model) {

    float min_sim = cross_tenant_config_.min_similarity_for_sharing;
    auto vec = embedder_.embed(prompt);
    // D3=A: genuine cross-partition scan. The previous search("", …) only hit
    // the empty partition (never populated in production) → cross_tenant was a
    // dead path. searchAllPartitions crosses tenant boundaries by design; the
    // min_sim floor is the sharing gate (SR-6).
    auto results = vector_store_.searchAllPartitions(vec, 1, min_sim);

    if (results.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(results[0].id);
    if (it == entries_.end()) return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    if (now - it->second.created_at > ttl_) return std::nullopt;

    if (!model.empty() && !it->second.model.empty() &&
        it->second.model != model) {
        return std::nullopt;
    }

    // D3=A / SR-6: every cross-tenant hit is audited. The candidate already
    // cleared min_similarity_for_sharing (vector_store_ search floor above), so
    // reaching here means one tenant is served another tenant's cached content.
    // Emit metadata-only telemetry — never the prompt/response plaintext.
    MetricsRegistry::instance().crossTenantCacheHitsTotal().inc();
    spdlog::info("cross_tenant cache hit sim={:.4f} model={}",
                 results[0].score,
                 it->second.model.empty() ? std::string("<any>")
                                          : it->second.model);

    return CacheHit{it->second.response, it->second.prompt, results[0].score};
}

} // namespace aegisgate
