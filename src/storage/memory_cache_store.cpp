#include "storage/memory_cache_store.h"

namespace aegisgate {

MemoryCacheStore::MemoryCacheStore(size_t max_size) : max_size_(max_size) {}

bool MemoryCacheStore::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return true;
}

void MemoryCacheStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    lru_list_.clear();
    lru_map_.clear();
    initialized_ = false;
}

bool MemoryCacheStore::isHealthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

std::string MemoryCacheStore::backendName() const { return "memory"; }

bool MemoryCacheStore::set(const std::string& key, const std::string& value,
                           std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.value = value;
        if (ttl.count() > 0) {
            it->second.expire_at = std::chrono::steady_clock::now() + ttl;
            it->second.has_ttl = true;
        } else {
            it->second.has_ttl = false;
        }
        touchLRU(key);
        return true;
    }

    while (data_.size() >= max_size_) {
        evictLRU();
    }

    Entry entry;
    entry.value = value;
    if (ttl.count() > 0) {
        entry.expire_at = std::chrono::steady_clock::now() + ttl;
        entry.has_ttl = true;
    }
    data_[key] = std::move(entry);
    lru_list_.push_front(key);
    lru_map_[key] = lru_list_.begin();
    return true;
}

std::optional<std::string> MemoryCacheStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return std::nullopt;

    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;

    if (isExpired(it->second)) {
        auto lru_it = lru_map_.find(key);
        if (lru_it != lru_map_.end()) {
            lru_list_.erase(lru_it->second);
            lru_map_.erase(lru_it);
        }
        data_.erase(it);
        return std::nullopt;
    }

    touchLRU(key);
    return it->second.value;
}

bool MemoryCacheStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    auto it = data_.find(key);
    if (it == data_.end()) return false;

    auto lru_it = lru_map_.find(key);
    if (lru_it != lru_map_.end()) {
        lru_list_.erase(lru_it->second);
        lru_map_.erase(lru_it);
    }
    data_.erase(it);
    return true;
}

bool MemoryCacheStore::exists(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    auto it = data_.find(key);
    if (it == data_.end()) return false;
    return !isExpired(it->second);
}

int64_t MemoryCacheStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t count = 0;
    for (const auto& kv : data_) {
        if (!isExpired(kv.second)) {
            ++count;
        }
    }
    return count;
}

void MemoryCacheStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    lru_list_.clear();
    lru_map_.clear();
}

bool MemoryCacheStore::isExpired(const Entry& e) const {
    if (!e.has_ttl) return false;
    return std::chrono::steady_clock::now() > e.expire_at;
}

void MemoryCacheStore::evictLRU() {
    if (lru_list_.empty()) return;
    auto oldest = lru_list_.back();
    lru_list_.pop_back();
    lru_map_.erase(oldest);
    data_.erase(oldest);
}

std::vector<std::string> MemoryCacheStore::keys(const std::string& prefix,
                                                  int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& [k, v] : data_) {
        if (!prefix.empty() && k.substr(0, prefix.size()) != prefix) continue;
        if (!isExpired(v)) {
            result.push_back(k);
            if (static_cast<int>(result.size()) >= limit) break;
        }
    }
    return result;
}

void MemoryCacheStore::touchLRU(const std::string& key) {
    auto it = lru_map_.find(key);
    if (it != lru_map_.end()) {
        lru_list_.erase(it->second);
    }
    lru_list_.push_front(key);
    lru_map_[key] = lru_list_.begin();
}

} // namespace aegisgate
