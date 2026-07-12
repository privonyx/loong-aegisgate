#include <gtest/gtest.h>

#ifdef AEGISGATE_ENABLE_CURL

#include "gateway/connector/curl_upstream_client.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace aegisgate;

class CurlUpstreamClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client_ = std::make_unique<CurlUpstreamClient>("http://127.0.0.1:1");
    }
    void TearDown() override {
        client_.reset();
    }
    std::unique_ptr<CurlUpstreamClient> client_;
};

TEST_F(CurlUpstreamClientTest, ConstructAndDestroy) {
    auto c = std::make_unique<CurlUpstreamClient>("http://127.0.0.1:1");
    EXPECT_NE(c, nullptr);
}

TEST_F(CurlUpstreamClientTest, SendErrorOnUnreachable) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    std::string error_msg;

    UpstreamRequest req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.body = R"({"model":"test"})";
    req.timeout_seconds = 2.0;

    client_->send(
        req,
        [](int, std::string) { FAIL() << "Should not receive response"; },
        [&](std::string err) {
            std::lock_guard lk(mtx);
            error_msg = std::move(err);
            done = true;
            cv.notify_one();
        });

    std::unique_lock lk(mtx);
    EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; }));
    EXPECT_FALSE(error_msg.empty());
}

TEST_F(CurlUpstreamClientTest, SendStreamingErrorOnUnreachable) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    std::string error_msg;

    UpstreamRequest req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.body = R"({"model":"test","stream":true})";
    req.timeout_seconds = 2.0;

    client_->sendStreaming(
        req,
        [](std::string_view) { FAIL() << "Should not receive chunk"; },
        [](int, std::string) { FAIL() << "Should not receive response"; },
        [&](std::string err) {
            std::lock_guard lk(mtx);
            error_msg = std::move(err);
            done = true;
            cv.notify_one();
        });

    std::unique_lock lk(mtx);
    EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; }));
    EXPECT_FALSE(error_msg.empty());
}

TEST_F(CurlUpstreamClientTest, ShutdownDuringPending) {
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/v1/test";
    req.body = "{}";
    req.timeout_seconds = 30.0;

    std::atomic<bool> callback_called{false};
    client_->send(
        req,
        [&](int, std::string) { callback_called.store(true); },
        [&](std::string) { callback_called.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client_->shutdown();
    client_.reset();
}

TEST_F(CurlUpstreamClientTest, MultipleSequentialRequests) {
    std::atomic<int> error_count{0};
    std::mutex mtx;
    std::condition_variable cv;

    for (int i = 0; i < 3; ++i) {
        UpstreamRequest req;
        req.method = "POST";
        req.path = "/v1/test";
        req.body = "{}";
        req.timeout_seconds = 2.0;

        client_->send(
            req,
            [](int, std::string) {},
            [&](std::string) {
                error_count.fetch_add(1);
                cv.notify_all();
            });
    }

    std::unique_lock lk(mtx);
    cv.wait_for(lk, std::chrono::seconds(15), [&] { return error_count.load() >= 3; });
    EXPECT_EQ(error_count.load(), 3);
}

TEST_F(CurlUpstreamClientTest, DispatcherReceivesCallbacks) {
    std::mutex dispatch_mtx;
    std::vector<std::function<void()>> dispatched;

    CurlUpstreamClient::Dispatcher dispatcher =
        [&](std::function<void()> f) {
            std::lock_guard lk(dispatch_mtx);
            dispatched.push_back(std::move(f));
        };

    auto dc = std::make_unique<CurlUpstreamClient>("http://127.0.0.1:1", dispatcher);

    std::atomic<bool> error_called{false};
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/v1/test";
    req.body = "{}";
    req.timeout_seconds = 2.0;

    dc->send(
        req,
        [](int, std::string) { FAIL() << "Should not succeed"; },
        [&](std::string) { error_called.store(true); });

    std::this_thread::sleep_for(std::chrono::seconds(3));

    EXPECT_FALSE(error_called.load());

    size_t count;
    {
        std::lock_guard lk(dispatch_mtx);
        count = dispatched.size();
    }
    EXPECT_GE(count, 1u);

    {
        std::lock_guard lk(dispatch_mtx);
        for (auto& f : dispatched) f();
    }

    EXPECT_TRUE(error_called.load());
    dc->shutdown();
}

TEST_F(CurlUpstreamClientTest, StreamingDispatcherReceivesCallbacks) {
    std::mutex dispatch_mtx;
    std::vector<std::function<void()>> dispatched;

    CurlUpstreamClient::Dispatcher dispatcher =
        [&](std::function<void()> f) {
            std::lock_guard lk(dispatch_mtx);
            dispatched.push_back(std::move(f));
        };

    auto dc = std::make_unique<CurlUpstreamClient>("http://127.0.0.1:1", dispatcher);

    std::atomic<bool> error_called{false};
    UpstreamRequest req;
    req.method = "POST";
    req.path = "/v1/test";
    req.body = R"({"stream":true})";
    req.timeout_seconds = 2.0;

    dc->sendStreaming(
        req,
        [](std::string_view) { FAIL() << "Should not receive chunk"; },
        [](int, std::string) { FAIL() << "Should not succeed"; },
        [&](std::string) { error_called.store(true); });

    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_FALSE(error_called.load());

    {
        std::lock_guard lk(dispatch_mtx);
        EXPECT_GE(dispatched.size(), 1u);
        for (auto& f : dispatched) f();
    }

    EXPECT_TRUE(error_called.load());
    dc->shutdown();
}

#else

TEST(CurlUpstreamClientSkipped, DisabledBuild) {
    GTEST_SKIP() << "ENABLE_CURL is OFF";
}

#endif
