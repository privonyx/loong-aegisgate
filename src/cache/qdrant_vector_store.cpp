#include "cache/qdrant_vector_store.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <drogon/HttpClient.h>
#include <future>
#include <functional>

namespace aegisgate {

QdrantVectorStore::QdrantVectorStore(const QdrantConfig& config)
    : config_(config) {}

std::string QdrantVectorStore::baseUrl() const {
    return "http://" + config_.host + ":" + std::to_string(config_.port);
}

std::string QdrantVectorStore::collectionName(
    const std::string& partition_key) const {
    if (partition_key.empty()) {
        return config_.collection_prefix + "_default";
    }
    std::string safe_key;
    safe_key.reserve(partition_key.size());
    for (char c : partition_key) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            safe_key += c;
        } else {
            safe_key += '_';
        }
    }
    return config_.collection_prefix + "_" + safe_key;
}

std::string QdrantVectorStore::httpRequest(const std::string& method,
                                            const std::string& path,
                                            const std::string& body) const {
    auto client = drogon::HttpClient::newHttpClient(baseUrl());
    client->setUserAgent("AegisGate/0.2");
    auto req = drogon::HttpRequest::newHttpRequest();

    if (method == "PUT") req->setMethod(drogon::Put);
    else if (method == "POST") req->setMethod(drogon::Post);
    else if (method == "DELETE") req->setMethod(drogon::Delete);
    else req->setMethod(drogon::Get);

    req->setPath(path);
    if (!body.empty()) {
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(body);
    }
    if (!config_.api_key.empty()) {
        req->addHeader("api-key", config_.api_key);
    }

    std::promise<std::string> promise;
    auto future = promise.get_future();
    client->sendRequest(
        req,
        [&promise](drogon::ReqResult result,
                   const drogon::HttpResponsePtr& resp) {
            if (result == drogon::ReqResult::Ok && resp) {
                promise.set_value(std::string(resp->body()));
            } else {
                promise.set_value("");
            }
        },
        static_cast<double>(config_.request_timeout_ms) / 1000.0);

    return future.get();
}

bool QdrantVectorStore::ensureCollection(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (known_collections_.count(name)) return true;
    }

    if (!config_.auto_create_collection) {
        std::lock_guard<std::mutex> lock(mutex_);
        known_collections_[name] = true;
        return true;
    }

    auto check = httpRequest("GET", "/collections/" + name);
    if (!check.empty()) {
        try {
            auto j = nlohmann::json::parse(check);
            if (j.contains("result") && j["result"].contains("status")) {
                std::lock_guard<std::mutex> lock(mutex_);
                known_collections_[name] = true;
                return true;
            }
        } catch (...) {}
    }

    nlohmann::json create_body;
    nlohmann::json vectors_config;
    vectors_config["size"] = static_cast<int>(config_.dimension);
    vectors_config["distance"] = config_.distance;
    create_body["vectors"] = vectors_config;

    auto resp = httpRequest("PUT", "/collections/" + name, create_body.dump());
    if (resp.empty()) {
        spdlog::error("QdrantVectorStore: failed to create collection {}", name);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp);
        if (j.value("result", false) || j.contains("status")) {
            std::lock_guard<std::mutex> lock(mutex_);
            known_collections_[name] = true;
            spdlog::info("QdrantVectorStore: collection '{}' ready", name);
            return true;
        }
        spdlog::error("QdrantVectorStore: create collection failed: {}", resp);
    } catch (const std::exception& e) {
        spdlog::error("QdrantVectorStore: create collection parse error: {}",
                       e.what());
    }
    return false;
}

bool QdrantVectorStore::initialize() {
    auto resp = httpRequest("GET", "/collections");
    if (resp.empty()) {
        spdlog::error("QdrantVectorStore: cannot connect to {}:{}",
                       config_.host, config_.port);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(resp);
        if (!j.contains("result")) {
            spdlog::error("QdrantVectorStore: unexpected response: {}", resp);
            return false;
        }
    } catch (...) {
        spdlog::error("QdrantVectorStore: invalid health check response");
        return false;
    }
    spdlog::info("QdrantVectorStore: connected to {}:{}", config_.host, config_.port);
    return true;
}

bool QdrantVectorStore::insert(const std::string& partition_key,
                                const std::string& id,
                                const std::vector<float>& vec) {
    auto coll = collectionName(partition_key);
    if (!ensureCollection(coll)) return false;

    nlohmann::json body;
    nlohmann::json point;
    std::hash<std::string> hasher;
    uint64_t point_id = hasher(id);
    point["id"] = point_id;
    point["vector"] = vec;
    nlohmann::json payload;
    payload["external_id"] = id;
    point["payload"] = payload;
    body["points"] = nlohmann::json::array({point});

    auto resp = httpRequest("PUT",
        "/collections/" + coll + "/points?wait=true", body.dump());
    if (resp.empty()) {
        spdlog::warn("QdrantVectorStore: insert failed for id={}", id);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(resp);
        auto status = j.value("status", "");
        if (status == "ok" || status == "completed") {
            approx_size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        spdlog::warn("QdrantVectorStore: insert unexpected status: {}", resp);
    } catch (...) {
        spdlog::warn("QdrantVectorStore: insert parse error");
    }
    return false;
}

bool QdrantVectorStore::remove(const std::string& partition_key,
                                const std::string& id) {
    auto coll = collectionName(partition_key);

    std::hash<std::string> hasher;
    uint64_t point_id = hasher(id);
    nlohmann::json body;
    body["points"] = nlohmann::json::array({point_id});

    auto resp = httpRequest("POST",
        "/collections/" + coll + "/points/delete?wait=true", body.dump());
    if (resp.empty()) return false;

    try {
        auto j = nlohmann::json::parse(resp);
        auto status = j.value("status", "");
        if (status == "ok" || status == "completed") {
            auto current = approx_size_.load(std::memory_order_relaxed);
            if (current > 0) {
                approx_size_.fetch_sub(1, std::memory_order_relaxed);
            }
            return true;
        }
    } catch (...) {}
    return false;
}

std::vector<VectorSearchResult> QdrantVectorStore::search(
    const std::string& partition_key,
    const std::vector<float>& query,
    size_t top_k,
    float threshold) const {
    auto coll = collectionName(partition_key);

    nlohmann::json body;
    body["vector"] = query;
    body["limit"] = static_cast<int>(top_k);
    body["with_payload"] = true;
    if (threshold > 0.0f) {
        body["score_threshold"] = threshold;
    }

    auto resp = httpRequest("POST",
        "/collections/" + coll + "/points/search", body.dump());
    if (resp.empty()) return {};

    std::vector<VectorSearchResult> results;
    try {
        auto j = nlohmann::json::parse(resp);
        if (!j.contains("result")) return {};

        for (const auto& item : j["result"]) {
            float score = item.value("score", 0.0f);
            std::string ext_id;
            if (item.contains("payload") &&
                item["payload"].contains("external_id")) {
                ext_id = item["payload"]["external_id"].get<std::string>();
            }
            if (ext_id.empty()) continue;
            results.push_back({ext_id, score});
        }
    } catch (const std::exception& e) {
        spdlog::warn("QdrantVectorStore: search parse error: {}", e.what());
    }
    return results;
}

size_t QdrantVectorStore::size() const {
    return approx_size_.load(std::memory_order_relaxed);
}

} // namespace aegisgate
