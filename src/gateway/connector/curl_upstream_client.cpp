#ifdef AEGISGATE_ENABLE_CURL

#include "curl_upstream_client.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

CurlUpstreamClient::CurlUpstreamClient(const std::string& base_url,
                                       Dispatcher dispatcher)
    : base_url_(base_url), dispatcher_(std::move(dispatcher)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi_ = curl_multi_init();
    running_.store(true);
    worker_ = std::thread(&CurlUpstreamClient::workerLoop, this);
}

CurlUpstreamClient::~CurlUpstreamClient() {
    shutdown();
}

void CurlUpstreamClient::shutdown() {
    if (!running_.exchange(false)) return;

    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    for (auto& t : active_) {
        if (t->easy) {
            curl_multi_remove_handle(multi_, t->easy);
            curl_easy_cleanup(t->easy);
        }
        if (t->headers) curl_slist_free_all(t->headers);
    }
    active_.clear();

    if (multi_) {
        curl_multi_cleanup(multi_);
        multi_ = nullptr;
    }
}

UpstreamClient::ResponseCallback
CurlUpstreamClient::wrapResponse(ResponseCallback cb) {
    if (!dispatcher_) return cb;
    auto d = dispatcher_;
    auto shared = std::make_shared<ResponseCallback>(std::move(cb));
    return [d, cb = std::move(shared)](int status, std::string body) {
        d([cb, status, body = std::move(body)]() mutable {
            (*cb)(status, std::move(body));
        });
    };
}

UpstreamClient::ErrorCallback
CurlUpstreamClient::wrapError(ErrorCallback cb) {
    if (!dispatcher_) return cb;
    auto d = dispatcher_;
    auto shared = std::make_shared<ErrorCallback>(std::move(cb));
    return [d, cb = std::move(shared)](std::string err) {
        d([cb, err = std::move(err)]() mutable {
            (*cb)(std::move(err));
        });
    };
}

UpstreamClient::ChunkCallback
CurlUpstreamClient::wrapChunk(ChunkCallback cb) {
    if (!dispatcher_) return cb;
    auto d = dispatcher_;
    auto shared = std::make_shared<ChunkCallback>(std::move(cb));
    return [d, cb = std::move(shared)](std::string_view chunk) {
        std::string owned(chunk);
        d([cb, owned = std::move(owned)]() {
            (*cb)(owned);
        });
    };
}

void CurlUpstreamClient::send(UpstreamRequest req,
                                ResponseCallback onDone,
                                ErrorCallback onError) {
    PendingRequest pr;
    pr.req = std::move(req);
    pr.streaming = false;
    pr.on_done = wrapResponse(std::move(onDone));
    pr.on_error = wrapError(std::move(onError));
    enqueue(std::move(pr));
}

void CurlUpstreamClient::sendStreaming(UpstreamRequest req,
                                        ChunkCallback onChunk,
                                        ResponseCallback onDone,
                                        ErrorCallback onError) {
    PendingRequest pr;
    pr.req = std::move(req);
    pr.streaming = true;
    pr.on_chunk = wrapChunk(std::move(onChunk));
    pr.on_done = wrapResponse(std::move(onDone));
    pr.on_error = wrapError(std::move(onError));
    enqueue(std::move(pr));
}

void CurlUpstreamClient::enqueue(PendingRequest pr) {
    {
        std::lock_guard lk(queue_mutex_);
        pending_.push(std::move(pr));
    }
    queue_cv_.notify_one();
}

size_t CurlUpstreamClient::writeCallback(char* ptr, size_t size, size_t nmemb,
                                           void* userdata) {
    auto* transfer = static_cast<ActiveTransfer*>(userdata);
    size_t total = size * nmemb;

    transfer->accumulated_body.append(ptr, total);

    if (transfer->streaming && transfer->on_chunk) {
        transfer->line_buffer.append(ptr, total);
        while (true) {
            auto nl = transfer->line_buffer.find('\n');
            if (nl == std::string::npos) break;
            std::string_view line(transfer->line_buffer.data(), nl);
            if (!line.empty() && line.back() == '\r') {
                line = line.substr(0, line.size() - 1);
            }
            if (!line.empty()) {
                transfer->on_chunk(line);
            }
            transfer->line_buffer.erase(0, nl + 1);
        }
    }

    return total;
}

void CurlUpstreamClient::startTransfer(PendingRequest& pr) {
    auto transfer = std::make_unique<ActiveTransfer>();
    transfer->streaming = pr.streaming;
    transfer->on_chunk = std::move(pr.on_chunk);
    transfer->on_done = std::move(pr.on_done);
    transfer->on_error = std::move(pr.on_error);
    // 把 body 移入 transfer 持有：CURLOPT_POSTFIELDS 不复制数据，而 pr 是
    // workerLoop 的局部变量，startTransfer 返回后即析构。若直接指向 pr.req.body，
    // 异步 curl_multi_perform 真正发送时指针已悬空，会发出空 body，上游报
    // "Failed to parse the request body as JSON"。
    transfer->request_body = std::move(pr.req.body);

    CURL* easy = curl_easy_init();
    transfer->easy = easy;

    std::string url = base_url_ + pr.req.path;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, transfer->request_body.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(transfer->request_body.size()));

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    for (const auto& [k, v] : pr.req.headers) {
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    transfer->headers = headers;

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(pr.req.timeout_seconds));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);

    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());

    curl_multi_add_handle(multi_, easy);
    active_.push_back(std::move(transfer));
}

void CurlUpstreamClient::finishTransfer(CURL* easy, CURLcode result) {
    ActiveTransfer* transfer = nullptr;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &transfer);
    if (!transfer) return;

    if (result != CURLE_OK) {
        if (transfer->on_error) {
            transfer->on_error(std::string("curl error: ") + curl_easy_strerror(result));
        }
    } else {
        if (transfer->streaming && transfer->on_chunk &&
            !transfer->line_buffer.empty()) {
            transfer->on_chunk(transfer->line_buffer);
            transfer->line_buffer.clear();
        }
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        if (transfer->on_done) {
            transfer->on_done(static_cast<int>(status),
                              std::move(transfer->accumulated_body));
        }
    }

    curl_multi_remove_handle(multi_, easy);
    curl_easy_cleanup(easy);
    if (transfer->headers) curl_slist_free_all(transfer->headers);
    transfer->headers = nullptr;
    transfer->easy = nullptr;

    active_.erase(
        std::remove_if(active_.begin(), active_.end(),
                        [transfer](const auto& t) { return t.get() == transfer; }),
        active_.end());
}

void CurlUpstreamClient::workerLoop() {
    while (running_.load()) {
        {
            std::unique_lock lk(queue_mutex_);
            queue_cv_.wait_for(lk, std::chrono::milliseconds(10), [this] {
                return !pending_.empty() || !running_.load();
            });

            while (!pending_.empty()) {
                auto pr = std::move(pending_.front());
                pending_.pop();
                lk.unlock();
                startTransfer(pr);
                lk.lock();
            }
        }

        if (!running_.load() && active_.empty()) break;

        int still_running = 0;
        curl_multi_perform(multi_, &still_running);

        CURLMsg* msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(multi_, &msgs_left)) != nullptr) {
            if (msg->msg == CURLMSG_DONE) {
                finishTransfer(msg->easy_handle, msg->data.result);
            }
        }

        if (still_running > 0) {
            curl_multi_poll(multi_, nullptr, 0, 100, nullptr);
        }
    }
}

} // namespace aegisgate

#endif // AEGISGATE_ENABLE_CURL
