#pragma once
#include "storage/cache_store.h"
#include "storage/connection_pool.h"
#include <hiredis/hiredis.h>
#include <atomic>
#include <string>
#include <vector>

namespace aegisgate {

struct RedisConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    size_t pool_size = 4;
    int connect_timeout_ms = 3000;
    int command_timeout_ms = 1000;
};

class RedisCacheStore : public CacheStore {
public:
    explicit RedisCacheStore(const RedisConfig& config);
    ~RedisCacheStore() override;

    bool initialize() override;
    void close() override;
    bool isHealthy() const override;
    bool isReady() const override;
    std::string backendName() const override;

    bool set(const std::string& key, const std::string& value,
             std::chrono::seconds ttl = std::chrono::seconds(0)) override;
    std::optional<std::string> get(const std::string& key) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) const override;
    int64_t size() const override;
    void clear() override;

    std::vector<std::string> keys(const std::string& prefix = "",
                                   int limit = 1000);

    std::optional<std::string> executeRaw(const std::string& command);
    std::optional<int64_t> executeRawInt(const std::string& command);
    std::optional<int64_t> evalInt(const std::string& script,
                                   const std::string& key,
                                   const std::vector<std::string>& args);

    static constexpr const char* kNamespacePrefix = "aegisgate:";

private:
    static std::string prefixKey(const std::string& key);
    static std::string stripPrefix(const std::string& key);

    redisContext* createConnection();
    static void destroyConnection(redisContext* ctx);
    static bool checkConnection(redisContext* ctx);

    RedisConfig config_;
    std::unique_ptr<ConnectionPool<redisContext>> pool_;
    std::atomic<bool> initialized_{false};
};

} // namespace aegisgate
