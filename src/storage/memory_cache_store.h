#pragma once
#include "storage/cache_store.h"
#include <mutex>
#include <unordered_map>
#include <list>

namespace aegisgate {

class MemoryCacheStore : public CacheStore {
public:
    explicit MemoryCacheStore(size_t max_size = 10000);

    bool initialize() override;
    void close() override;
    bool isHealthy() const override;
    std::string backendName() const override;

    bool set(const std::string& key, const std::string& value,
             std::chrono::seconds ttl = std::chrono::seconds(0)) override;
    std::optional<std::string> get(const std::string& key) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) const override;
    int64_t size() const override;
    void clear() override;
    std::vector<std::string> keys(const std::string& prefix = "",
                                   int limit = 1000) override;

private:
    struct Entry {
        std::string value;
        std::chrono::steady_clock::time_point expire_at;
        bool has_ttl = false;
    };

    bool isExpired(const Entry& e) const;
    void evictLRU(); // caller must hold mutex_
    void touchLRU(const std::string& key); // caller must hold mutex_

    size_t max_size_;
    bool initialized_ = false;
    std::unordered_map<std::string, Entry> data_;
    std::list<std::string> lru_list_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;
    mutable std::mutex mutex_; // Lock Layer 2 — see docs/LOCK_ORDERING.md
};

} // namespace aegisgate
