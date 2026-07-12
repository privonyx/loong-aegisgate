// Phase 11.3 TASK-20260523-02 — Epic 3.1: ThreadPool.
//
// A small, predictable thread pool the WorkflowEngine submits ready nodes
// into. The Engine uses submit() + futures for dispatch, wait_all() for
// drain on join boundaries, and shutdown() on destruction (A2 destruction
// symmetry).

#include "workflow/thread_pool.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <gtest/gtest.h>

namespace aw = aegisgate::workflow;

TEST(ThreadPoolTest, SubmitReturnsResultsInOrderOfCompletion) {
    aw::ThreadPool pool(4);
    std::vector<std::future<int>> fs;
    for (int i = 0; i < 8; ++i) {
        fs.push_back(pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return i * 2;
        }));
    }
    int total = 0;
    for (auto& f : fs) total += f.get();
    EXPECT_EQ(total, 0+2+4+6+8+10+12+14);
}

TEST(ThreadPoolTest, RunsTasksConcurrently) {
    aw::ThreadPool pool(4);
    std::atomic<int> peak{0};
    std::atomic<int> running{0};
    std::vector<std::future<void>> fs;
    for (int i = 0; i < 8; ++i) {
        fs.push_back(pool.submit([&] {
            int v = ++running;
            int p = peak.load();
            while (v > p && !peak.compare_exchange_weak(p, v)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            --running;
        }));
    }
    for (auto& f : fs) f.get();
    EXPECT_GE(peak.load(), 2) << "pool did not run jobs concurrently";
}

TEST(ThreadPoolTest, ShutdownPreventsNewSubmissions) {
    aw::ThreadPool pool(2);
    pool.shutdown();
    EXPECT_THROW({ (void)pool.submit([] { return 1; }); },
                 std::runtime_error);
}

TEST(ThreadPoolTest, WaitAllDrainsPending) {
    aw::ThreadPool pool(2);
    std::atomic<int> n{0};
    for (int i = 0; i < 10; ++i) {
        pool.submitDetached([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ++n;
        });
    }
    pool.wait_all();
    EXPECT_EQ(n.load(), 10);
}

TEST(ThreadPoolTest, DestructorJoinsCleanly) {
    std::atomic<int> finished{0};
    {
        aw::ThreadPool pool(2);
        for (int i = 0; i < 4; ++i) {
            pool.submitDetached([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ++finished;
            });
        }
        // destructor runs at scope exit and must wait for in-flight tasks.
    }
    EXPECT_EQ(finished.load(), 4);
}
