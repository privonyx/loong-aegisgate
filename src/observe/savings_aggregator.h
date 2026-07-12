#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/persistent_store.h"

namespace aegisgate {

class CostTracker;

// 节省事件的来源类别。CacheHit / Compression 是 hot path 实际节省；
// Routing 是潜在节省（基于 CostOptimizer 推荐，未真实发生），用于运营展示。
enum class SavingType {
    CacheHit = 0,
    Compression = 1,
    Routing = 2,
};

struct SavingsEvent {
    SavingType type = SavingType::CacheHit;
    std::string model;
    std::string tenant_id;
    int tokens_saved = 0;
    double cost_saved = 0.0;
    bool fallback_pricing = false;
    std::chrono::system_clock::time_point timestamp;
};

struct SavingsBucketStats {
    int64_t event_count = 0;
    int64_t tokens_saved = 0;
    double cost_saved = 0.0;
    int64_t fallback_count = 0;
};

struct SavingsSnapshot {
    SavingsBucketStats total;
    std::unordered_map<int, SavingsBucketStats> by_type;          // key = (int)SavingType
    std::unordered_map<std::string, SavingsBucketStats> by_model;
    std::unordered_map<std::string, SavingsBucketStats> by_tenant;
    std::vector<std::pair<std::string, SavingsBucketStats>> time_series_by_day;
    std::chrono::system_clock::time_point since;
    std::chrono::system_clock::time_point until;
};

// 进程内、线程安全的省钱事件聚合器。
//
// 设计要点（详见 docs/specs/2026-05-10-admin-savings-dashboard-design.md）：
//   - 不引入新存储 schema，进程重启即重置（UI 通过 since 字段标注）
//   - FIFO 100K 环形缓冲（kMaxEvents），写满淘汰最早事件，~50MB 内存上限
//   - hot path 的两个 record* 接口标 noexcept 并 try/catch 吞异常（SR-NEW4）
//   - recordRouting 在 admin API 主动获取时调用，非 hot path，可抛
//   - snapshot 接受 tenant 过滤 + 时间窗口，admin API 套 365 天上限做 DoS 防护
class SavingsAggregator {
public:
    explicit SavingsAggregator(CostTracker* tracker);
    ~SavingsAggregator();  // A18: 必须 stopFlushThread（join + 终次 flush）

    // TASK-20260617-02: 注入持久化 store（非 owning）。非空时启动 write-behind
    // flush 线程；二次调用 / 传 nullptr 先停旧线程再切换（避免双线程）。
    void setPersistentStore(PersistentStore* store);
    // 启动回放：从 store 回读近 retention_days 天事件重建内存态。
    // 坏行（脏 timestamp）跳过 + 计数，绝不中断启动（SR3/T3）。
    void loadFromStore(int retention_days);
    // write-behind 溢出丢弃计数（SR4 可观测 / 测试用）。
    int64_t droppedCount() const;

    // hot path: noexcept + try/catch
    void recordCacheHit(const std::string& model,
                        int input_tokens,
                        int output_tokens,
                        const std::string& tenant_id) noexcept;
    void recordCompression(const std::string& model,
                           int tokens_saved_input,
                           const std::string& tenant_id) noexcept;

    // 非 hot path（admin API 主动获取，调用方需处理异常）
    void recordRouting(const std::string& current_model,
                       const std::string& recommended_model,
                       double potential_savings,
                       const std::string& tenant_id);

    SavingsSnapshot snapshot(const std::string& tenant_id_filter,
                             std::chrono::system_clock::time_point from,
                             std::chrono::system_clock::time_point to) const;

    std::chrono::system_clock::time_point startedAt() const { return started_at_; }
    size_t eventCount() const;
    void clear();  // 仅测试用

    static constexpr size_t kMaxEvents = 100000;

private:
    void appendLocked(SavingsEvent event);
    double computeCost(const std::string& model, int input_tokens,
                       int output_tokens, bool& used_fallback) const;

    // write-behind（C2-A）：热路径仅入队，IO 全异步。
    void enqueue(PersistentStore::SavingsEventRecord rec) noexcept;
    void startFlushThread();
    void stopFlushThread();  // wb_running_=false + notify + join + 终次 drain
    void flushLoop();

    CostTracker* tracker_;
    mutable std::mutex mutex_;          // Lock Layer 3 — see docs/LOCK_ORDERING.md
    std::vector<SavingsEvent> events_;  // 预分配 kMaxEvents，避免 hot path realloc
    size_t head_ = 0;                   // 下次写入位置（环形）
    bool wrapped_ = false;
    std::chrono::system_clock::time_point started_at_;
    // 数据覆盖起点：默认 = started_at_；loadFromStore 后 = 回放最早事件时间。
    std::chrono::system_clock::time_point data_since_;

    // write-behind 队列（独立 wb_mutex_，绝不与 mutex_ 同时持有 → 无锁环）。
    PersistentStore* store_ = nullptr;  // 非 owning
    std::deque<PersistentStore::SavingsEventRecord> wb_queue_;
    mutable std::mutex wb_mutex_;       // Lock Layer 3b — 仅护 wb_queue_/wb_dropped_
    std::condition_variable wb_cv_;
    std::thread wb_thread_;
    std::atomic<bool> wb_running_{false};
    int64_t wb_dropped_ = 0;
    static constexpr size_t kMaxQueue = 10000;            // 有界 + drop-oldest
    static constexpr size_t kBatch = 256;                 // 单批最大出队
    static constexpr auto kFlushInterval = std::chrono::seconds(1);
};

}  // namespace aegisgate
