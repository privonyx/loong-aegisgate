#include "cache/milvus_vector_store.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <drogon/HttpClient.h>
#include <future>

namespace aegisgate {

MilvusVectorStore::MilvusVectorStore(const MilvusConfig& config)
    : config_(config) {}

std::string MilvusVectorStore::baseUrl() const {
    return "http://" + config_.host + ":" + std::to_string(config_.port);
}

std::string MilvusVectorStore::collectionName(
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

std::string MilvusVectorStore::httpPost(const std::string& path,
                                         const std::string& body) const {
    auto client = drogon::HttpClient::newHttpClient(baseUrl());
    client->setUserAgent("AegisGate/0.2");
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    if (!config_.token.empty()) {
        req->addHeader("Authorization", "Bearer " + config_.token);
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

std::string MilvusVectorStore::httpGet(const std::string& path) const {
    auto client = drogon::HttpClient::newHttpClient(baseUrl());
    client->setUserAgent("AegisGate/0.2");
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(path);
    if (!config_.token.empty()) {
        req->addHeader("Authorization", "Bearer " + config_.token);
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

bool MilvusVectorStore::ensureCollection(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (known_collections_.count(name)) return true;
    }

    if (!config_.auto_create_collection) {
        std::lock_guard<std::mutex> lock(mutex_);
        known_collections_[name] = true;
        return true;
    }

    nlohmann::json check_body;
    check_body["collectionName"] = name;
    auto check_resp = httpPost("/v2/vectordb/collections/has", check_body.dump());
    if (!check_resp.empty()) {
        try {
            auto j = nlohmann::json::parse(check_resp);
            if (j.contains("data") && j["data"].contains("has") &&
                j["data"]["has"].get<bool>()) {
                std::lock_guard<std::mutex> lock(mutex_);
                known_collections_[name] = true;
                return true;
            }
        } catch (...) {}
    }

    nlohmann::json create_body;
    create_body["collectionName"] = name;
    create_body["dimension"] = static_cast<int>(config_.dimension);
    create_body["metricType"] = config_.metric_type;

    nlohmann::json schema;
    schema["autoId"] = false;
    schema["enableDynamicField"] = true;
    nlohmann::json fields = nlohmann::json::array();

    nlohmann::json pk_field;
    pk_field["fieldName"] = "id";
    pk_field["dataType"] = "VarChar";
    pk_field["isPrimary"] = true;
    nlohmann::json pk_params;
    pk_params["max_length"] = 256;
    pk_field["elementTypeParams"] = pk_params;
    fields.push_back(pk_field);

    nlohmann::json vec_field;
    vec_field["fieldName"] = "vector";
    vec_field["dataType"] = "FloatVector";
    nlohmann::json vec_params;
    vec_params["dim"] = static_cast<int>(config_.dimension);
    vec_field["elementTypeParams"] = vec_params;
    fields.push_back(vec_field);

    schema["fields"] = fields;
    create_body["schema"] = schema;

    auto resp = httpPost("/v2/vectordb/collections/create", create_body.dump());
    if (resp.empty()) {
        spdlog::error("MilvusVectorStore: failed to create collection {}", name);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp);
        int code = j.value("code", -1);
        if (code == 0 || code == 65535) {
            std::lock_guard<std::mutex> lock(mutex_);
            known_collections_[name] = true;
            spdlog::info("MilvusVectorStore: collection '{}' ready", name);
            return true;
        }
        spdlog::error("MilvusVectorStore: create collection failed: {}", resp);
    } catch (const std::exception& e) {
        spdlog::error("MilvusVectorStore: create collection parse error: {}", e.what());
    }
    return false;
}

bool MilvusVectorStore::initialize() {
    auto resp = httpGet("/v2/vectordb/collections/list");
    if (resp.empty()) {
        spdlog::error("MilvusVectorStore: cannot connect to {}:{}", config_.host, config_.port);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(resp);
        if (j.value("code", -1) != 0) {
            spdlog::error("MilvusVectorStore: health check failed: {}", resp);
            return false;
        }
    } catch (...) {
        spdlog::error("MilvusVectorStore: invalid health check response");
        return false;
    }
    spdlog::info("MilvusVectorStore: connected to {}:{}", config_.host, config_.port);
    return true;
}

bool MilvusVectorStore::insert(const std::string& partition_key,
                                const std::string& id,
                                const std::vector<float>& vec) {
    auto coll = collectionName(partition_key);
    if (!ensureCollection(coll)) return false;

    nlohmann::json body;
    body["collectionName"] = coll;
    nlohmann::json data = nlohmann::json::array();
    nlohmann::json row;
    row["id"] = id;
    row["vector"] = vec;
    data.push_back(row);
    body["data"] = data;

    auto resp = httpPost("/v2/vectordb/entities/insert", body.dump());
    if (resp.empty()) {
        spdlog::warn("MilvusVectorStore: insert failed for id={}", id);
        return false;
    }
    try {
        auto j = nlohmann::json::parse(resp);
        if (j.value("code", -1) != 0) {
            spdlog::warn("MilvusVectorStore: insert error: {}", resp);
        } else {
            approx_size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    } catch (...) {
        spdlog::warn("MilvusVectorStore: insert parse error");
    }
    return false;
}

bool MilvusVectorStore::remove(const std::string& partition_key,
                                const std::string& id) {
    auto coll = collectionName(partition_key);

    nlohmann::json body;
    body["collectionName"] = coll;
    body["filter"] = "id == \"" + id + "\"";

    auto resp = httpPost("/v2/vectordb/entities/delete", body.dump());
    if (resp.empty()) return false;

    try {
        auto j = nlohmann::json::parse(resp);
        if (j.value("code", -1) == 0) {
            auto current = approx_size_.load(std::memory_order_relaxed);
            if (current > 0) {
                approx_size_.fetch_sub(1, std::memory_order_relaxed);
            }
            return true;
        }
    } catch (...) {}
    return false;
}

std::vector<VectorSearchResult> MilvusVectorStore::search(
    const std::string& partition_key,
    const std::vector<float>& query,
    size_t top_k,
    float threshold) const {
    auto coll = collectionName(partition_key);

    nlohmann::json body;
    body["collectionName"] = coll;
    body["data"] = nlohmann::json::array({query});
    body["limit"] = static_cast<int>(top_k);
    body["outputFields"] = nlohmann::json::array({"id"});

    nlohmann::json search_params;
    search_params["metric_type"] = config_.metric_type;
    search_params["params"] = nlohmann::json::object();
    body["searchParams"] = search_params;

    auto resp = httpPost("/v2/vectordb/entities/search", body.dump());
    if (resp.empty()) return {};

    std::vector<VectorSearchResult> results;
    try {
        auto j = nlohmann::json::parse(resp);
        if (j.value("code", -1) != 0) return {};

        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& item : j["data"]) {
                float score = item.value("distance", 0.0f);
                float similarity = (config_.metric_type == "IP")
                    ? score
                    : 1.0f / (1.0f + score);

                if (threshold > 0.0f && similarity < threshold) continue;

                std::string item_id = item.value("id", "");
                if (item_id.empty()) continue;
                results.push_back({item_id, similarity});
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("MilvusVectorStore: search parse error: {}", e.what());
    }
    return results;
}

size_t MilvusVectorStore::size() const {
    return approx_size_.load(std::memory_order_relaxed);
}

} // namespace aegisgate
