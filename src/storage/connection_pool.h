#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace aegisgate {

template<typename Conn>
class ConnectionPool {
public:
    class Handle {
    public:
        Handle() noexcept = default;
        Handle(Conn* conn, ConnectionPool* pool) noexcept
            : conn_(conn), pool_(pool) {}
        ~Handle() { release(); }

        Handle(Handle&& other) noexcept
            : conn_(other.conn_), pool_(other.pool_) {
            other.conn_ = nullptr;
            other.pool_ = nullptr;
        }
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                release();
                conn_ = other.conn_;
                pool_ = other.pool_;
                other.conn_ = nullptr;
                other.pool_ = nullptr;
            }
            return *this;
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        Conn* operator->() { return conn_; }
        const Conn* operator->() const { return conn_; }
        Conn* get() { return conn_; }
        const Conn* get() const { return conn_; }

    private:
        void release() noexcept {
            if (conn_ != nullptr && pool_ != nullptr) {
                if (pool_->closed_.load(std::memory_order_acquire)) {
                    pool_->deleter_(conn_);
                } else {
                    pool_->returnConn(conn_);
                }
                conn_ = nullptr;
                pool_ = nullptr;
            }
        }
        Conn* conn_ = nullptr;
        ConnectionPool* pool_ = nullptr;
    };

    static constexpr size_t kMaxPoolSize = 256;

    ConnectionPool(size_t pool_size,
                   std::function<Conn*()> factory,
                   std::function<void(Conn*)> deleter,
                   std::function<bool(Conn*)> health_check)
        : pool_size_(std::min(pool_size, kMaxPoolSize)), factory_(std::move(factory)),
          deleter_(std::move(deleter)), health_check_(std::move(health_check)) {
        for (size_t i = 0; i < pool_size_; ++i) {
            Conn* c = nullptr;
            try {
                c = factory_();
            } catch (...) {
                for (auto* existing : idle_) deleter_(existing);
                idle_.clear();
                throw;
            }
            if (!c) {
                for (auto* existing : idle_) deleter_(existing);
                idle_.clear();
                throw std::runtime_error("ConnectionPool: factory returned nullptr");
            }
            idle_.push_back(c);
        }
    }

    ~ConnectionPool() {
        closed_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* c : idle_) {
            deleter_(c);
        }
        idle_.clear();
    }

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    std::optional<Handle> acquire(std::chrono::milliseconds timeout) {
        Conn* conn = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto deadline = std::chrono::steady_clock::now() + timeout;

            while (idle_.empty()) {
                if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    if (idle_.empty()) return std::nullopt;
                }
            }

            conn = idle_.back();
            idle_.pop_back();
        }

        if (!health_check_(conn)) {
            deleter_(conn);
            conn = factory_();
            if (!conn) return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++active_count_;
        }
        return Handle(conn, this);
    }

    bool isHealthy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !idle_.empty() || active_count_ > 0;
    }

    // P1-B: active liveness probe. Unlike isHealthy() (which only checks that the
    // pool still holds connection objects), this borrows a connection and runs the
    // real backend validator (SELECT 1 / PING) via acquire(); a dead connection is
    // transparently rebuilt. Returns false if no usable connection can be obtained
    // within `timeout` (backend genuinely down). Used by /health/ready so a
    // memory-fallback or a silently-dropped DB cannot mask an outage.
    bool activeHealthCheck(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
        try {
            auto handle = acquire(timeout);
            return handle.has_value() && handle->get() != nullptr;
        } catch (...) {
            return false;
        }
    }

    size_t activeCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_count_;
    }

    size_t idleCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_.size();
    }

    size_t poolSize() const {
        return pool_size_;
    }

private:
    void returnConn(Conn* conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_count_;
        idle_.push_back(conn);
        cv_.notify_one();
    }

    size_t pool_size_;
    std::function<Conn*()> factory_;
    std::function<void(Conn*)> deleter_;
    std::function<bool(Conn*)> health_check_;

    std::vector<Conn*> idle_;
    size_t active_count_ = 0;
    mutable std::mutex mutex_; // Lock Layer 2 — see docs/LOCK_ORDERING.md
    std::condition_variable cv_;
    std::atomic<bool> closed_{false};
};

} // namespace aegisgate
