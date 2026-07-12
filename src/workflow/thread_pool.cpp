#include "workflow/thread_pool.h"

#include <algorithm>

namespace aegisgate::workflow {

ThreadPool::ThreadPool(std::size_t worker_count) {
    std::size_t n = std::max<std::size_t>(worker_count, 1);
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopped_ || !queue_.empty(); });
            if (stopped_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop();
            ++active_;
        }
        try {
            job();
        } catch (...) {
            // Tasks own their own error reporting (futures will surface
            // exceptions to the caller). A detached task that escapes
            // exceptions is its own bug — we swallow here to keep the worker
            // alive.
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            --active_;
            --pending_;
            if (pending_ == 0) drain_cv_.notify_all();
        }
    }
}

void ThreadPool::wait_all() {
    std::unique_lock<std::mutex> lk(mu_);
    drain_cv_.wait(lk, [&] { return pending_ == 0; });
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopped_) return;
        stopped_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

} // namespace aegisgate::workflow
