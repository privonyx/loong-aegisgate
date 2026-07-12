#pragma once

// Phase 11.3 TASK-20260523-02 — Generic ThreadPool used by WorkflowEngine.
//
// Decision D5=C ("thread pool + future") — bounded worker pool for parallel
// node dispatch. The Engine owns one pool; concurrency level is set by the
// runtime config and clamped to a sane minimum (1).
//
// API:
//   submit(fn)         -> std::future<R>
//   submitDetached(fn) -> void (no future for fire-and-forget)
//   wait_all()         -> blocks until all currently-enqueued + in-flight
//                          tasks drain.
//   shutdown()         -> stops accepting new tasks and joins workers.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace aegisgate::workflow {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename Fn>
    auto submit(Fn&& fn) -> std::future<decltype(fn())> {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        auto fut  = task->get_future();
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stopped_) {
                throw std::runtime_error("ThreadPool::submit after shutdown");
            }
            ++pending_;
            queue_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    template <typename Fn>
    void submitDetached(Fn&& fn) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (stopped_) {
                throw std::runtime_error("ThreadPool::submitDetached after shutdown");
            }
            ++pending_;
            queue_.emplace(std::function<void()>(std::forward<Fn>(fn)));
        }
        cv_.notify_one();
    }

    void wait_all();
    void shutdown();

    std::size_t worker_count() const noexcept { return workers_.size(); }

private:
    void workerLoop();

    std::vector<std::thread>                  workers_;
    std::queue<std::function<void()>>         queue_;
    std::mutex                                mu_;
    std::condition_variable                   cv_;
    std::condition_variable                   drain_cv_;
    std::atomic<bool>                         stopped_{false};
    int                                       pending_ = 0;
    int                                       active_  = 0;
};

} // namespace aegisgate::workflow
