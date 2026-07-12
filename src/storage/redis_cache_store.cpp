#include "storage/redis_cache_store.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <string>

namespace aegisgate {

namespace {

int64_t scanCountMatching(redisContext* ctx, const std::string& pattern) {
    int64_t count = 0;
    std::string cursor = "0";
    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx, "SCAN %b MATCH %b COUNT 100",
                         cursor.c_str(), cursor.size(),
                         pattern.c_str(), pattern.size()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            break;
        }

        cursor = reply->element[0]->str ? reply->element[0]->str : "0";
        auto* keys_arr = reply->element[1];
        if (keys_arr && keys_arr->type == REDIS_REPLY_ARRAY) {
            count += static_cast<int64_t>(keys_arr->elements);
        }
        freeReplyObject(reply);
    } while (cursor != "0");
    return count;
}

void scanDeleteMatching(redisContext* ctx, const std::string& pattern) {
    std::string cursor = "0";
    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx, "SCAN %b MATCH %b COUNT 100",
                         cursor.c_str(), cursor.size(),
                         pattern.c_str(), pattern.size()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            break;
        }

        cursor = reply->element[0]->str ? reply->element[0]->str : "0";
        auto* keys_arr = reply->element[1];
        if (keys_arr && keys_arr->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < keys_arr->elements; ++i) {
                auto* el = keys_arr->element[i];
                if (el->type == REDIS_REPLY_STRING && el->str) {
                    auto* del_r = static_cast<redisReply*>(
                        redisCommand(ctx, "DEL %b", el->str, el->len));
                    if (del_r) freeReplyObject(del_r);
                }
            }
        }
        freeReplyObject(reply);
    } while (cursor != "0");
}

} // namespace

std::string RedisCacheStore::prefixKey(const std::string& key) {
    return std::string(kNamespacePrefix) + key;
}

std::string RedisCacheStore::stripPrefix(const std::string& key) {
    const std::string prefix(kNamespacePrefix);
    if (key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0) {
        return key.substr(prefix.size());
    }
    return key;
}

RedisCacheStore::RedisCacheStore(const RedisConfig& config)
    : config_(config) {}

RedisCacheStore::~RedisCacheStore() {
    close();
}

redisContext* RedisCacheStore::createConnection() {
    struct timeval tv;
    tv.tv_sec = config_.connect_timeout_ms / 1000;
    tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

    redisContext* ctx = redisConnectWithTimeout(
        config_.host.c_str(), config_.port, tv);
    if (!ctx || ctx->err) {
        if (ctx) {
            spdlog::error("Redis connect error: {}", ctx->errstr);
            redisFree(ctx);
        }
        return nullptr;
    }

    struct timeval cmd_tv;
    cmd_tv.tv_sec = config_.command_timeout_ms / 1000;
    cmd_tv.tv_usec = (config_.command_timeout_ms % 1000) * 1000;
    redisSetTimeout(ctx, cmd_tv);

    if (!config_.password.empty()) {
        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx, "AUTH %b",
                         config_.password.c_str(), config_.password.size()));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            spdlog::error("Redis AUTH failed");
            if (reply) freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    if (config_.db != 0) {
        auto* reply = static_cast<redisReply*>(
            redisCommand(ctx, "SELECT %d", config_.db));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            spdlog::error("Redis SELECT failed");
            if (reply) freeReplyObject(reply);
            redisFree(ctx);
            return nullptr;
        }
        freeReplyObject(reply);
    }

    return ctx;
}

void RedisCacheStore::destroyConnection(redisContext* ctx) {
    if (ctx) redisFree(ctx);
}

bool RedisCacheStore::checkConnection(redisContext* ctx) {
    if (!ctx || ctx->err) return false;
    auto* reply = static_cast<redisReply*>(redisCommand(ctx, "PING"));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_STATUS &&
               std::strcmp(reply->str, "PONG") == 0);
    freeReplyObject(reply);
    return ok;
}

bool RedisCacheStore::initialize() {
    if (initialized_) return true;
    try {
        auto self = this;
        pool_ = std::make_unique<ConnectionPool<redisContext>>(
            config_.pool_size,
            [self]() { return self->createConnection(); },
            destroyConnection,
            checkConnection);
        initialized_ = true;
        spdlog::info("RedisCacheStore initialized: {}:{} db={} pool={}",
                     config_.host, config_.port, config_.db, config_.pool_size);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("RedisCacheStore init failed: {}", e.what());
        return false;
    }
}

void RedisCacheStore::close() {
    pool_.reset();
    initialized_ = false;
}

bool RedisCacheStore::isHealthy() const {
    return initialized_ && pool_ && pool_->isHealthy();
}

bool RedisCacheStore::isReady() const {
    // P1-B: actively PING the Redis server so a post-startup outage surfaces
    // in /health/ready instead of being masked by the passive pool check.
    return initialized_ && pool_ && pool_->activeHealthCheck();
}

std::string RedisCacheStore::backendName() const {
    return "redis";
}

bool RedisCacheStore::set(const std::string& key, const std::string& value,
                           std::chrono::seconds ttl) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return false;

    auto rkey = prefixKey(key);
    redisReply* reply;
    if (ttl.count() > 0) {
        reply = static_cast<redisReply*>(
            redisCommand(handle->get(), "SETEX %b %lld %b",
                         rkey.c_str(), rkey.size(),
                         static_cast<long long>(ttl.count()),
                         value.c_str(), value.size()));
    } else {
        reply = static_cast<redisReply*>(
            redisCommand(handle->get(), "SET %b %b",
                         rkey.c_str(), rkey.size(),
                         value.c_str(), value.size()));
    }

    if (!reply) return false;
    bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

std::optional<std::string> RedisCacheStore::get(const std::string& key) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return std::nullopt;

    auto rkey = prefixKey(key);
    auto* reply = static_cast<redisReply*>(
        redisCommand(handle->get(), "GET %b", rkey.c_str(), rkey.size()));
    if (!reply) return std::nullopt;

    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING && reply->str) {
        result = std::string(reply->str, reply->len);
    }
    freeReplyObject(reply);
    return result;
}

bool RedisCacheStore::del(const std::string& key) {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return false;

    auto rkey = prefixKey(key);
    auto* reply = static_cast<redisReply*>(
        redisCommand(handle->get(), "DEL %b", rkey.c_str(), rkey.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return ok;
}

bool RedisCacheStore::exists(const std::string& key) const {
    if (!initialized_) return false;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return false;

    auto rkey = prefixKey(key);
    auto* reply = static_cast<redisReply*>(
        redisCommand(handle->get(), "EXISTS %b", rkey.c_str(), rkey.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return ok;
}

int64_t RedisCacheStore::size() const {
    if (!initialized_) return 0;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return 0;

    std::string pattern = std::string(kNamespacePrefix) + "*";
    return scanCountMatching(handle->get(), pattern);
}

void RedisCacheStore::clear() {
    if (!initialized_) return;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return;

    std::string pattern = std::string(kNamespacePrefix) + "*";
    scanDeleteMatching(handle->get(), pattern);
}

std::vector<std::string> RedisCacheStore::keys(const std::string& prefix,
                                                 int limit) {
    std::vector<std::string> result;
    if (!initialized_) return result;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return result;

    std::string rprefix = prefixKey(prefix.empty() ? "" : prefix);
    std::string pattern = rprefix + "*";
    std::string cursor = "0";

    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(handle->get(), "SCAN %b MATCH %b COUNT 100",
                         cursor.c_str(), cursor.size(),
                         pattern.c_str(), pattern.size()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            break;
        }

        cursor = reply->element[0]->str;
        auto* keys_arr = reply->element[1];
        for (size_t i = 0; i < keys_arr->elements; ++i) {
            if (keys_arr->element[i]->type == REDIS_REPLY_STRING) {
                std::string raw(keys_arr->element[i]->str,
                                keys_arr->element[i]->len);
                result.push_back(stripPrefix(raw));
            }
            if (static_cast<int>(result.size()) >= limit) break;
        }
        freeReplyObject(reply);
    } while (cursor != "0" && static_cast<int>(result.size()) < limit);

    return result;
}

std::optional<std::string> RedisCacheStore::executeRaw(const std::string& command) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return std::nullopt;

    auto* reply = static_cast<redisReply*>(
        redisCommand(handle->get(), command.c_str()));
    if (!reply) return std::nullopt;

    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING && reply->str) {
        result = std::string(reply->str, reply->len);
    } else if (reply->type == REDIS_REPLY_STATUS && reply->str) {
        result = std::string(reply->str, reply->len);
    } else if (reply->type == REDIS_REPLY_INTEGER) {
        result = std::to_string(reply->integer);
    }
    freeReplyObject(reply);
    return result;
}

std::optional<int64_t> RedisCacheStore::executeRawInt(const std::string& command) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return std::nullopt;

    auto* reply = static_cast<redisReply*>(
        redisCommand(handle->get(), command.c_str()));
    if (!reply) return std::nullopt;

    std::optional<int64_t> result;
    if (reply->type == REDIS_REPLY_INTEGER) {
        result = reply->integer;
    }
    freeReplyObject(reply);
    return result;
}

std::optional<int64_t> RedisCacheStore::evalInt(
    const std::string& script, const std::string& key,
    const std::vector<std::string>& args) {
    if (!initialized_) return std::nullopt;
    auto handle = pool_->acquire(std::chrono::milliseconds(config_.command_timeout_ms));
    if (!handle) return std::nullopt;

    std::vector<const char*> argv;
    std::vector<size_t> argvlen;

    static const char* eval_cmd = "EVAL";
    argv.push_back(eval_cmd);
    argvlen.push_back(4);

    argv.push_back(script.c_str());
    argvlen.push_back(script.size());

    static const char* one = "1";
    argv.push_back(one);
    argvlen.push_back(1);

    argv.push_back(key.c_str());
    argvlen.push_back(key.size());

    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
        argvlen.push_back(arg.size());
    }

    auto* reply = static_cast<redisReply*>(
        redisCommandArgv(handle->get(),
                         static_cast<int>(argv.size()),
                         argv.data(), argvlen.data()));
    if (!reply) return std::nullopt;

    std::optional<int64_t> result;
    if (reply->type == REDIS_REPLY_INTEGER) {
        result = reply->integer;
    }
    freeReplyObject(reply);
    return result;
}

} // namespace aegisgate
