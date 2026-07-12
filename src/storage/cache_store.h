#pragma once
#include <string>
#include <optional>
#include <chrono>
#include <cstdint>
#include <vector>

namespace aegisgate {

class CacheStore {
public:
    virtual ~CacheStore() = default;
    virtual bool initialize() = 0;
    virtual void close() = 0;
    virtual bool isHealthy() const = 0;
    // P1-B: liveness probe for /health/ready. Default delegates to the passive
    // isHealthy(); pooled backends (Redis) override to actively PING the server.
    virtual bool isReady() const { return isHealthy(); }
    virtual std::string backendName() const = 0;

    virtual bool set(const std::string& key, const std::string& value,
                     std::chrono::seconds ttl = std::chrono::seconds(0)) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool del(const std::string& key) = 0;
    virtual bool exists(const std::string& key) const = 0;
    virtual int64_t size() const = 0;
    virtual void clear() = 0;

    virtual std::vector<std::string> keys(const std::string& prefix = "",
                                           int limit = 1000) = 0;
};

} // namespace aegisgate
