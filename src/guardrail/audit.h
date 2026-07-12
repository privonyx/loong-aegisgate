#pragma once
#include "core/pipeline.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <future>
#include <chrono>
#include <unordered_map>

namespace aegisgate {

class PersistentStore;
class Encryption;

struct AuditEntry {
    std::string request_id;
    std::string timestamp;
    std::string tenant_id;
    std::string action;
    std::string stage_name;
    std::string detail;
    std::string input_hash;
    std::string output_hash;
    std::string chain_hash;
};

using AuditSink = std::function<void(const AuditEntry&)>;

class AuditLogger : public PipelineStage {
public:
    AuditLogger();
    ~AuditLogger() override;

    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    void setSink(AuditSink sink);
    void setPersistentStore(PersistentStore* store);
    void setEncryption(const Encryption* encryption);
    void log(const AuditEntry& entry);
    void logAction(const std::string& request_id,
                   const std::string& tenant_id,
                   const std::string& stage,
                   const std::string& action,
                   const std::string& detail = "");

    bool flush(std::chrono::seconds timeout = std::chrono::seconds{5});
    void shutdown();

    std::vector<AuditEntry> entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }
    void setMaxEntries(size_t max) { max_entries_ = max; }
    void clear();

    using LogSubscriber = std::function<void(const AuditEntry&)>;
    size_t subscribe(LogSubscriber callback);
    void unsubscribe(size_t id);

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "AuditLogger"; }

    static std::string computeChainHash(const AuditEntry& entry,
                                        const std::string& prev_chain_hash);

    // TASK-20260702-01 P1-3：detail 落库前对称加密（见 log()），但读路径此前无
    // 解密回读，开启加密时 /admin/audits 查询与合规导出会返回密文。此静态 helper
    // 供读路径做对称解密：加密不可用 / enc 为空 / 值本就是明文（认证解密失败）时
    // 原样返回，兼容未加密历史数据。
    static std::string decryptDetail(const std::string& stored, const Encryption* enc);
    bool verifyChain() const;
    bool verifyChain(const std::vector<AuditEntry>& entries) const;

private:
    std::string currentTimestamp() const;
    std::string hashString(const std::string& input) const;
    void writerLoop();
    void ensureWriterThread(); // starts writer thread on first need
    void drainBatch(std::deque<AuditEntry>& batch); // called without locks held
    void notifyFlushWaiters(); // caller must hold queue_mutex_

    std::vector<AuditEntry> entries_;
    AuditSink sink_;
    mutable std::mutex mutex_; // Lock Layer 3 — protects entries_ and sink_
    size_t max_entries_ = 100000;
    std::string last_chain_hash_; // guarded by mutex_

    const Encryption* encryption_ = nullptr;
    static constexpr const char* kAuditEncryptionPurpose = "audit_log";

    PersistentStore* store_ = nullptr;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<AuditEntry> pending_queue_;
    bool draining_ = false; // true while drainBatch runs outside lock; guarded by queue_mutex_
    std::atomic<bool> stop_{false};
    std::thread writer_thread_;
    bool writer_started_ = false; // guarded by queue_mutex_

    std::atomic<uint64_t> flush_epoch_{0};
    std::atomic<uint64_t> completed_epoch_{0};
    std::vector<std::pair<uint64_t, std::promise<void>>> flush_waiters_; // guarded by queue_mutex_

    std::mutex subscribers_mutex_;
    std::unordered_map<size_t, LogSubscriber> subscribers_;
    std::atomic<size_t> next_subscriber_id_{1};

    static constexpr size_t kMaxQueueSize = 10000;
    static constexpr size_t kBatchSize = 1000;
    static constexpr auto kFlushInterval = std::chrono::milliseconds{100};
};

} // namespace aegisgate
