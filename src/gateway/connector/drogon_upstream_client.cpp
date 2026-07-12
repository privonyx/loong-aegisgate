#include "drogon_upstream_client.h"
#include <spdlog/spdlog.h>
#include <sstream>

namespace aegisgate {

DrogonUpstreamClient::DrogonUpstreamClient(const std::string& base_url)
    : client_(drogon::HttpClient::newHttpClient(base_url)) {}

void DrogonUpstreamClient::send(UpstreamRequest req,
                                 ResponseCallback onDone,
                                 ErrorCallback onError) {
    auto dReq = drogon::HttpRequest::newHttpRequest();
    auto method_str = req.method;
    dReq->setMethod(method_str == "GET" ? drogon::Get : drogon::Post);
    dReq->setPath(req.path);

    auto ct_it = req.headers.find("Content-Type");
    if (ct_it != req.headers.end()) {
        dReq->setContentTypeString(ct_it->second);
    } else {
        dReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }

    dReq->setBody(req.body);
    for (const auto& [k, v] : req.headers) {
        if (k == "Content-Type") continue;
        dReq->addHeader(k, v);
    }

    client_->sendRequest(
        dReq,
        [onDone = std::move(onDone), onError = std::move(onError)](
            drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
            if (result != drogon::ReqResult::Ok || !resp) {
                onError("Failed to reach upstream");
                return;
            }
            onDone(static_cast<int>(resp->statusCode()),
                   std::string(resp->body()));
        },
        req.timeout_seconds);
}

void DrogonUpstreamClient::sendStreaming(UpstreamRequest req,
                                          ChunkCallback onChunk,
                                          ResponseCallback onDone,
                                          ErrorCallback onError) {
    auto dReq = drogon::HttpRequest::newHttpRequest();
    dReq->setMethod(drogon::Post);
    dReq->setPath(req.path);
    dReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    dReq->setBody(req.body);
    for (const auto& [k, v] : req.headers) {
        dReq->addHeader(k, v);
    }

    client_->sendRequest(
        dReq,
        [onChunk = std::move(onChunk), onDone = std::move(onDone),
         onError = std::move(onError)](drogon::ReqResult result,
                                        const drogon::HttpResponsePtr& resp) {
            if (result != drogon::ReqResult::Ok || !resp) {
                onError("Failed to reach upstream");
                return;
            }
            auto status = static_cast<int>(resp->statusCode());
            auto body = std::string(resp->body());
            std::istringstream stream(body);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty() || line == "\r") continue;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                onChunk(line);
            }
            onDone(status, std::move(body));
        },
        req.timeout_seconds);
}

} // namespace aegisgate
