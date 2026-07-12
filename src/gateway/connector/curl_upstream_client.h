#pragma once
#ifdef AEGISGATE_ENABLE_CURL

#include "upstream_client.h"
#include <curl/curl.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace aegisgate {

class CurlUpstreamClient : public UpstreamClient {
public:
    using Dispatcher = std::function<void(std::function<void()>)>;

    explicit CurlUpstreamClient(const std::string& base_url,
                                Dispatcher dispatcher = {});
    ~CurlUpstreamClient() override;

    CurlUpstreamClient(const CurlUpstreamClient&) = delete;
    CurlUpstreamClient& operator=(const CurlUpstreamClient&) = delete;

    void send(UpstreamRequest req, ResponseCallback onDone,
              ErrorCallback onError) override;

    void sendStreaming(UpstreamRequest req, ChunkCallback onChunk,
                       ResponseCallback onDone,
                       ErrorCallback onError) override;

    void shutdown();

private:
    struct ActiveTransfer {
        CURL* easy = nullptr;
        struct curl_slist* headers = nullptr;
        // CURLOPT_POSTFIELDS 不复制 body，必须由调用方保证其内存在传输完成前一直有效。
        // ActiveTransfer 的生命周期由 active_ 持有，覆盖整个异步传输，故在此持有 body。
        std::string request_body;
        std::string accumulated_body;
        std::string line_buffer;
        bool streaming = false;
        ChunkCallback on_chunk;
        ResponseCallback on_done;
        ErrorCallback on_error;
    };

    struct PendingRequest {
        UpstreamRequest req;
        bool streaming = false;
        ChunkCallback on_chunk;
        ResponseCallback on_done;
        ErrorCallback on_error;
    };

    void enqueue(PendingRequest pr);
    void workerLoop();
    void startTransfer(PendingRequest& pr);
    void finishTransfer(CURL* easy, CURLcode result);

    ResponseCallback wrapResponse(ResponseCallback cb);
    ErrorCallback wrapError(ErrorCallback cb);
    ChunkCallback wrapChunk(ChunkCallback cb);

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

    std::string base_url_;
    Dispatcher dispatcher_;
    CURLM* multi_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<PendingRequest> pending_;

    std::vector<std::unique_ptr<ActiveTransfer>> active_;
};

} // namespace aegisgate

#endif // AEGISGATE_ENABLE_CURL
