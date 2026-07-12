#include "guardrail/audit.h"
#include "core/crypto.h"
#include "auth/encryption.h"
#include "storage/persistent_store.h"
#include <spdlog/spdlog.h>
#include <functional>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace aegisgate {

AuditLogger::AuditLogger() = default;

AuditLogger::~AuditLogger() {
    shutdown();
}

void AuditLogger::setSink(AuditSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void AuditLogger::setPersistentStore(PersistentStore* store) {
    store_ = store;
    if (store) {
        ensureWriterThread();
    }
}

void AuditLogger::setEncryption(const Encryption* encryption) {
    encryption_ = encryption;
    if (encryption && encryption->isAvailable()) {
        spdlog::info("AuditLogger: encryption enabled for detail field");
    }
}

void AuditLogger::ensureWriterThread() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (writer_started_ || stop_.load(std::memory_order_relaxed)) return;
    writer_started_ = true;
    writer_thread_ = std::thread(&AuditLogger::writerLoop, this);
}

std::string AuditLogger::computeChainHash(const AuditEntry& entry,
                                           const std::string& prev_chain_hash) {
    std::string payload = prev_chain_hash + "|"
        + entry.timestamp + "|"
        + entry.request_id + "|"
        + entry.tenant_id + "|"
        + entry.stage_name + "|"
        + entry.action + "|"
        + entry.detail + "|"
        + entry.input_hash;
    return crypto::sha256(payload);
}

bool AuditLogger::verifyChain() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return verifyChain(entries_);
}

bool AuditLogger::verifyChain(const std::vector<AuditEntry>& entries) const {
    std::string prev_hash;
    for (const auto& entry : entries) {
        auto expected = computeChainHash(entry, prev_hash);
        if (entry.chain_hash != expected) return false;
        prev_hash = entry.chain_hash;
    }
    return true;
}

void AuditLogger::log(const AuditEntry& entry) {
    AuditEntry chained_entry = entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chained_entry.chain_hash = computeChainHash(chained_entry, last_chain_hash_);
        last_chain_hash_ = chained_entry.chain_hash;
        if (entries_.size() >= max_entries_) {
            entries_.erase(entries_.begin());
        }
        entries_.push_back(chained_entry);
        if (sink_) {
            sink_(chained_entry);
        }
    }

    if (store_) {
        AuditEntry persist_entry = chained_entry;
        if (encryption_ && encryption_->isAvailable() && !persist_entry.detail.empty()) {
            auto encrypted = encryption_->encrypt(persist_entry.detail,
                                                   kAuditEncryptionPurpose);
            if (!encrypted.empty()) {
                persist_entry.detail = encrypted;
            }
        }
        std::lock_guard<std::mutex> qlock(queue_mutex_);
        if (pending_queue_.size() >= kMaxQueueSize) {
            pending_queue_.pop_front();
            spdlog::warn("Audit queue overflow, oldest entry dropped");
        }
        pending_queue_.push_back(persist_entry);
        queue_cv_.notify_one();
    }

    spdlog::debug("Audit: req={} stage={} action={} detail={}",
                  chained_entry.request_id, chained_entry.stage_name,
                  chained_entry.action, chained_entry.detail);

    // P0-D (TASK-20260701-01): snapshot subscriber callbacks under the lock,
    // then invoke them WITHOUT holding subscribers_mutex_. Subscribers (e.g.
    // the /admin/logs/stream SSE handler) legitimately call unsubscribe() from
    // inside their delivery callback when the client connection drops. Invoking
    // under the lock re-entered this non-recursive mutex (deadlock) and mutated
    // the map mid range-for (UB), hanging the entire audit log path the moment
    // a streaming admin client disconnected.
    std::vector<LogSubscriber> subs_snapshot;
    {
        std::lock_guard<std::mutex> sub_lock(subscribers_mutex_);
        subs_snapshot.reserve(subscribers_.size());
        for (const auto& [id, cb] : subscribers_) {
            subs_snapshot.push_back(cb);
        }
    }
    for (const auto& cb : subs_snapshot) {
        try { cb(chained_entry); } catch (...) {}
    }
}

std::string AuditLogger::decryptDetail(const std::string& stored, const Encryption* enc) {
    if (stored.empty() || !enc || !enc->isAvailable()) return stored;
    auto dec = enc->decrypt(stored, kAuditEncryptionPurpose);
    return dec.value_or(stored);
}

void AuditLogger::logAction(const std::string& request_id,
                             const std::string& tenant_id,
                             const std::string& stage,
                             const std::string& action,
                             const std::string& detail) {
    AuditEntry entry;
    entry.request_id = request_id;
    entry.timestamp = currentTimestamp();
    entry.tenant_id = tenant_id;
    entry.stage_name = stage;
    entry.action = action;
    entry.detail = detail;
    log(entry);
}

bool AuditLogger::flush(std::chrono::seconds timeout) {
    if (!store_) return true;

    uint64_t target;
    std::future<void> fut;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (pending_queue_.empty() && !draining_ && flush_waiters_.empty()) {
            return true;
        }
        target = ++flush_epoch_;
        std::promise<void> p;
        fut = p.get_future();
        flush_waiters_.emplace_back(target, std::move(p));
    }
    queue_cv_.notify_one();
    auto status = fut.wait_for(timeout);
    if (status == std::future_status::timeout) {
        spdlog::warn("AuditLogger::flush timed out after {}s", timeout.count());
        return false;
    }
    return true;
}

void AuditLogger::shutdown() {
    if (stop_.exchange(true)) return;
    queue_cv_.notify_one();
    if (writer_thread_.joinable()) writer_thread_.join();
}

void AuditLogger::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

StageResult AuditLogger::process(RequestContext& ctx) {
    std::string input_text;
    for (const auto& msg : ctx.chat_request.messages) {
        input_text += msg.content;
    }

    AuditEntry entry;
    entry.request_id = ctx.request_id;
    entry.timestamp = currentTimestamp();
    entry.tenant_id = ctx.tenant_id;
    entry.stage_name = "pipeline";
    entry.action = "request_received";
    entry.input_hash = hashString(input_text);
    log(entry);

    return StageResult::Continue;
}

void AuditLogger::writerLoop() {
    std::deque<AuditEntry> batch;
    while (!stop_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, kFlushInterval, [this] {
                return !pending_queue_.empty() ||
                       !flush_waiters_.empty() ||
                       stop_.load(std::memory_order_relaxed);
            });
            if (pending_queue_.empty()) {
                notifyFlushWaiters();
                continue;
            }
            const size_t n = std::min(pending_queue_.size(), kBatchSize);
            batch.assign(
                std::make_move_iterator(pending_queue_.begin()),
                std::make_move_iterator(pending_queue_.begin() +
                                        static_cast<std::ptrdiff_t>(n)));
            pending_queue_.erase(
                pending_queue_.begin(),
                pending_queue_.begin() + static_cast<std::ptrdiff_t>(n));
            draining_ = true;
        }
        drainBatch(batch);
        batch.clear();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            draining_ = false;
            if (pending_queue_.empty()) {
                notifyFlushWaiters();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        batch.swap(pending_queue_);
    }
    if (!batch.empty()) {
        drainBatch(batch);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        draining_ = false;
        notifyFlushWaiters();
    }
}

// Caller must hold queue_mutex_
void AuditLogger::notifyFlushWaiters() {
    if (flush_waiters_.empty()) return;
    auto epoch = completed_epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto it = flush_waiters_.begin();
    while (it != flush_waiters_.end()) {
        if (it->first <= epoch) {
            it->second.set_value();
            it = flush_waiters_.erase(it);
        } else {
            ++it;
        }
    }
}

void AuditLogger::drainBatch(std::deque<AuditEntry>& batch) {
    if (!store_ || batch.empty()) return;
    for (const auto& entry : batch) {
        if (!store_->insertAudit(entry)) {
            spdlog::warn("Audit batch persist failed: req={}",
                         entry.request_id);
        }
    }
}

std::string AuditLogger::currentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

size_t AuditLogger::subscribe(LogSubscriber callback) {
    size_t id = next_subscriber_id_.fetch_add(1);
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_[id] = std::move(callback);
    return id;
}

void AuditLogger::unsubscribe(size_t id) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(id);
}

std::string AuditLogger::hashString(const std::string& input) const {
    return crypto::sha256(input);
}

} // namespace aegisgate
