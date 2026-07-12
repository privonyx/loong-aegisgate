#include "observe/savings_aggregator.h"
#include "observe/cost_tracker.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace aegisgate {

namespace {

// ISO-8601 UTC (matches PersistentStore / cost_records timestamp format).
std::string formatIso(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// Parse "YYYY-MM-DDTHH:MM:SSZ" → time_point. Returns false on malformed input
// so loadFromStore can skip bad rows without aborting startup (SR3/T3).
bool parseIso(const std::string& s, std::chrono::system_clock::time_point& out) {
    std::tm tm{};
    std::istringstream is(s);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (is.fail()) return false;
    time_t t = timegm(&tm);
    if (t == static_cast<time_t>(-1)) return false;
    out = std::chrono::system_clock::from_time_t(t);
    return true;
}

}  // namespace

SavingsAggregator::SavingsAggregator(CostTracker* tracker)
    : tracker_(tracker),
      started_at_(std::chrono::system_clock::now()),
      data_since_(started_at_) {
    // 预分配避免 hot path realloc。
    // sizeof(SavingsEvent) ~120-160 bytes（含 string SSO），
    // 100K * ~150 bytes ≈ 15MB（远低于设计 50MB cap）。
    events_.resize(kMaxEvents);
}

SavingsAggregator::~SavingsAggregator() {
    stopFlushThread();
}

double SavingsAggregator::computeCost(const std::string& model,
                                      int input_tokens,
                                      int output_tokens,
                                      bool& used_fallback) const {
    used_fallback = false;
    if (!tracker_) {
        used_fallback = true;
        return 0.0;
    }
    auto rec = tracker_->calculate(model, input_tokens, output_tokens);
    // CostTracker.calculate 在 model pricing 缺失时返回 cost=0；
    // 配合 token > 0 判定为 fallback（透明度合规：UI 暴露 fallback_count）。
    if (rec.total_cost == 0.0 && (input_tokens > 0 || output_tokens > 0)) {
        used_fallback = true;
    }
    return rec.total_cost;
}

void SavingsAggregator::appendLocked(SavingsEvent event) {
    events_[head_] = std::move(event);
    head_ = (head_ + 1) % kMaxEvents;
    if (head_ == 0) {
        wrapped_ = true;
    }
}

void SavingsAggregator::recordCacheHit(const std::string& model,
                                       int input_tokens,
                                       int output_tokens,
                                       const std::string& tenant_id) noexcept {
    try {
        bool fallback = false;
        double cost = computeCost(model, input_tokens, output_tokens, fallback);
        SavingsEvent ev;
        ev.type = SavingType::CacheHit;
        ev.model = model;
        ev.tenant_id = tenant_id;
        ev.tokens_saved = input_tokens + output_tokens;
        ev.cost_saved = cost;
        ev.fallback_pricing = fallback;
        ev.timestamp = std::chrono::system_clock::now();
        // 持久化记录在持锁前构建；enqueue 在释放 mutex_ 后进行（锁顺序）。
        PersistentStore::SavingsEventRecord rec;
        rec.type = static_cast<int>(ev.type);
        rec.model = ev.model;
        rec.tenant_id = ev.tenant_id;
        rec.tokens_saved = ev.tokens_saved;
        rec.cost_saved = ev.cost_saved;
        rec.fallback_pricing = ev.fallback_pricing;
        rec.timestamp = formatIso(ev.timestamp);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            appendLocked(std::move(ev));
        }
        enqueue(std::move(rec));
    } catch (...) {
        // SR-NEW4: 热路径吞异常，避免影响主请求链路
    }
}

void SavingsAggregator::recordCompression(const std::string& model,
                                          int tokens_saved_input,
                                          const std::string& tenant_id) noexcept {
    try {
        if (tokens_saved_input <= 0) {
            return;
        }
        bool fallback = false;
        double cost = computeCost(model, tokens_saved_input, 0, fallback);
        SavingsEvent ev;
        ev.type = SavingType::Compression;
        ev.model = model;
        ev.tenant_id = tenant_id;
        ev.tokens_saved = tokens_saved_input;
        ev.cost_saved = cost;
        ev.fallback_pricing = fallback;
        ev.timestamp = std::chrono::system_clock::now();
        PersistentStore::SavingsEventRecord rec;
        rec.type = static_cast<int>(ev.type);
        rec.model = ev.model;
        rec.tenant_id = ev.tenant_id;
        rec.tokens_saved = ev.tokens_saved;
        rec.cost_saved = ev.cost_saved;
        rec.fallback_pricing = ev.fallback_pricing;
        rec.timestamp = formatIso(ev.timestamp);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            appendLocked(std::move(ev));
        }
        enqueue(std::move(rec));
    } catch (...) {
        // SR-NEW4
    }
}

void SavingsAggregator::recordRouting(const std::string& current_model,
                                      const std::string& recommended_model,
                                      double potential_savings,
                                      const std::string& tenant_id) {
    SavingsEvent ev;
    ev.type = SavingType::Routing;
    ev.model = current_model + "->" + recommended_model;
    ev.tenant_id = tenant_id;
    ev.tokens_saved = 0;
    ev.cost_saved = potential_savings;
    ev.fallback_pricing = false;
    ev.timestamp = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    appendLocked(std::move(ev));
}

size_t SavingsAggregator::eventCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wrapped_ ? kMaxEvents : head_;
}

void SavingsAggregator::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    wrapped_ = false;
    started_at_ = std::chrono::system_clock::now();
    data_since_ = started_at_;
}

// --- write-behind 持久化（C2-A，TASK-20260617-02）---

void SavingsAggregator::setPersistentStore(PersistentStore* store) {
    // A18：二次调用 / nullptr 先停旧线程再切换，避免双线程。
    stopFlushThread();
    store_ = store;
    if (store_) {
        startFlushThread();
    }
}

int64_t SavingsAggregator::droppedCount() const {
    std::lock_guard<std::mutex> lock(wb_mutex_);
    return wb_dropped_;
}

void SavingsAggregator::enqueue(PersistentStore::SavingsEventRecord rec) noexcept {
    try {
        if (!store_ || !wb_running_.load()) return;
        std::lock_guard<std::mutex> lock(wb_mutex_);
        wb_queue_.push_back(std::move(rec));
        if (wb_queue_.size() > kMaxQueue) {
            wb_queue_.pop_front();  // SR4: drop-oldest，有界内存
            ++wb_dropped_;
        }
        wb_cv_.notify_one();
    } catch (...) {
        // 入队失败绝不传播到热路径（SR3）。
    }
}

void SavingsAggregator::startFlushThread() {
    if (wb_running_.exchange(true)) return;  // 已在运行
    wb_thread_ = std::thread([this] { flushLoop(); });
}

void SavingsAggregator::stopFlushThread() {
    if (!wb_running_.exchange(false)) {
        // 未运行：仍确保 thread 已 join（防御）。
        if (wb_thread_.joinable()) wb_thread_.join();
        return;
    }
    wb_cv_.notify_all();
    if (wb_thread_.joinable()) wb_thread_.join();
}

void SavingsAggregator::flushLoop() {
    std::unique_lock<std::mutex> lock(wb_mutex_);
    for (;;) {
        wb_cv_.wait_for(lock, kFlushInterval,
                        [this] { return !wb_queue_.empty() || !wb_running_.load(); });

        std::vector<PersistentStore::SavingsEventRecord> batch;
        while (!wb_queue_.empty() && batch.size() < kBatch) {
            batch.push_back(std::move(wb_queue_.front()));
            wb_queue_.pop_front();
        }
        const bool running = wb_running_.load();
        const bool drained = wb_queue_.empty();

        lock.unlock();
        for (const auto& rec : batch) {
            try {
                if (store_) store_->insertSavingsEvent(rec);
            } catch (...) {
                // store 抛异常不阻断 flush 线程（SR3）。
                spdlog::warn("savings write-behind: insertSavingsEvent threw, dropping 1 event");
            }
        }
        lock.lock();

        if (!running && drained) break;  // 关停且队列已排空 → 终次 drain 完成
    }
}

void SavingsAggregator::loadFromStore(int retention_days) {
    if (!store_) return;
    auto now = std::chrono::system_clock::now();
    std::string from_iso;
    if (retention_days > 0) {
        from_iso = formatIso(now - std::chrono::hours(24 * retention_days));
    }
    auto rows = store_->querySavingsEventsByDateRange(
        from_iso, /*to_iso=*/"", static_cast<int>(kMaxEvents));

    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    wrapped_ = false;
    int64_t skipped = 0;
    bool any = false;
    auto earliest = std::chrono::system_clock::time_point::max();
    for (const auto& r : rows) {
        SavingsEvent ev;
        if (!parseIso(r.timestamp, ev.timestamp)) {
            ++skipped;
            continue;  // 坏行跳过，绝不中断启动（SR3/T3）
        }
        ev.type = static_cast<SavingType>(r.type);
        ev.model = r.model;
        ev.tenant_id = r.tenant_id;
        ev.tokens_saved = r.tokens_saved;
        ev.cost_saved = r.cost_saved;
        ev.fallback_pricing = r.fallback_pricing;
        auto ts = ev.timestamp;
        appendLocked(std::move(ev));
        if (ts < earliest) earliest = ts;
        any = true;
    }
    if (skipped > 0) {
        spdlog::warn("savings loadFromStore: skipped {} malformed savings rows", skipped);
    }
    size_t replayed = wrapped_ ? kMaxEvents : head_;
    spdlog::info("SavingsAggregator loadFromStore: replayed {} savings events (window={}d)",
                 replayed, retention_days);
    // since 语义：有回放事件 → 数据实际覆盖起点；否则 = now（无历史）。
    data_since_ = any ? earliest : now;
}

namespace {

std::string formatDay(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

}  // namespace

SavingsSnapshot SavingsAggregator::snapshot(
    const std::string& tenant_id_filter,
    std::chrono::system_clock::time_point from,
    std::chrono::system_clock::time_point to) const {
    SavingsSnapshot snap;
    std::chrono::system_clock::time_point data_since;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_since = data_since_;
    }
    snap.since = data_since;
    snap.until = std::chrono::system_clock::now();

    std::map<std::string, SavingsBucketStats> day_map;  // sorted ASC，输出有序

    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = wrapped_ ? kMaxEvents : head_;

    for (size_t i = 0; i < count; ++i) {
        const auto& ev = events_[i];
        if (ev.timestamp < from || ev.timestamp > to) {
            continue;
        }
        if (!tenant_id_filter.empty() && ev.tenant_id != tenant_id_filter) {
            continue;
        }

        auto bump = [&](SavingsBucketStats& b) {
            b.event_count += 1;
            b.tokens_saved += ev.tokens_saved;
            b.cost_saved += ev.cost_saved;
            if (ev.fallback_pricing) {
                b.fallback_count += 1;
            }
        };
        bump(snap.total);
        bump(snap.by_type[static_cast<int>(ev.type)]);
        bump(snap.by_model[ev.model]);
        bump(snap.by_tenant[ev.tenant_id]);
        bump(day_map[formatDay(ev.timestamp)]);
    }

    snap.time_series_by_day.assign(day_map.begin(), day_map.end());
    return snap;
}

}  // namespace aegisgate
