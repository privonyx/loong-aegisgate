#pragma once
#include "auth/auth_models.h"
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

#ifdef AEGISGATE_ENABLE_REDIS
#include "storage/redis_cache_store.h"
#endif

namespace aegisgate {

class RedisSessionStore {
public:
#ifdef AEGISGATE_ENABLE_REDIS
    explicit RedisSessionStore(RedisCacheStore* redis, int session_ttl_seconds = 28800);
#else
    explicit RedisSessionStore(int session_ttl_seconds = 28800);
#endif

    bool insertSession(const Session& s);
    std::optional<Session> getSession(const std::string& id);
    bool updateSessionActivity(const std::string& id, const std::string& ts);
    bool deleteSession(const std::string& id);
    std::vector<Session> listSessionsByUser(const std::string& user_id);
    int64_t deleteExpiredSessions();
    int64_t countSessionsByUser(const std::string& user_id);
    bool updateSessionMfaVerified(const std::string& id, bool verified);

    bool isAvailable() const;

private:
#ifdef AEGISGATE_ENABLE_REDIS
    std::string sessionKey(const std::string& id) const;
    std::string userSessionsKey(const std::string& user_id) const;
    std::string serializeSession(const Session& s) const;
    std::optional<Session> deserializeSession(const std::string& data) const;

    RedisCacheStore* redis_;
#endif
    int session_ttl_seconds_;
};

}  // namespace aegisgate
