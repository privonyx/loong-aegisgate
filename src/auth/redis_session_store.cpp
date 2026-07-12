#include "auth/redis_session_store.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <sstream>

namespace aegisgate {

#ifdef AEGISGATE_ENABLE_REDIS

RedisSessionStore::RedisSessionStore(RedisCacheStore* redis, int session_ttl_seconds)
    : redis_(redis), session_ttl_seconds_(session_ttl_seconds) {}

bool RedisSessionStore::isAvailable() const {
    return redis_ && redis_->isHealthy();
}

std::string RedisSessionStore::sessionKey(const std::string& id) const {
    return "session:" + id;
}

std::string RedisSessionStore::userSessionsKey(const std::string& user_id) const {
    return "user_sessions:" + user_id;
}

std::string RedisSessionStore::serializeSession(const Session& s) const {
    nlohmann::json j;
    j["id"] = s.id;
    j["user_id"] = s.user_id;
    j["tenant_id"] = s.tenant_id;
    j["auth_method"] = s.auth_method;
    j["ip_address"] = s.ip_address;
    j["user_agent"] = s.user_agent;
    j["mfa_verified"] = s.mfa_verified;
    j["created_at"] = s.created_at;
    j["last_active_at"] = s.last_active_at;
    j["expires_at"] = s.expires_at;
    return j.dump();
}

std::optional<Session> RedisSessionStore::deserializeSession(const std::string& data) const {
    try {
        auto j = nlohmann::json::parse(data);
        Session s;
        s.id = j.value("id", "");
        s.user_id = j.value("user_id", "");
        s.tenant_id = j.value("tenant_id", "");
        s.auth_method = j.value("auth_method", "");
        s.ip_address = j.value("ip_address", "");
        s.user_agent = j.value("user_agent", "");
        s.mfa_verified = j.value("mfa_verified", false);
        s.created_at = j.value("created_at", "");
        s.last_active_at = j.value("last_active_at", "");
        s.expires_at = j.value("expires_at", "");
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

bool RedisSessionStore::insertSession(const Session& s) {
    if (!isAvailable()) {
        return false;
    }
    auto data = serializeSession(s);
    bool ok = redis_->set(sessionKey(s.id), data,
                          std::chrono::seconds(session_ttl_seconds_));
    if (ok && !s.user_id.empty()) {
        auto ukey = userSessionsKey(s.user_id);
        auto existing = redis_->get(ukey);
        std::string sessions_list = existing.value_or("");
        if (!sessions_list.empty()) {
            sessions_list += ",";
        }
        sessions_list += s.id;
        redis_->set(ukey, sessions_list, std::chrono::seconds(session_ttl_seconds_));
    }
    return ok;
}

std::optional<Session> RedisSessionStore::getSession(const std::string& id) {
    if (!isAvailable()) {
        return std::nullopt;
    }
    auto data = redis_->get(sessionKey(id));
    if (!data) {
        return std::nullopt;
    }
    return deserializeSession(*data);
}

bool RedisSessionStore::updateSessionActivity(const std::string& id, const std::string& ts) {
    if (!isAvailable()) {
        return false;
    }
    auto s = getSession(id);
    if (!s) {
        return false;
    }
    s->last_active_at = ts;
    return redis_->set(sessionKey(id), serializeSession(*s),
                       std::chrono::seconds(session_ttl_seconds_));
}

bool RedisSessionStore::deleteSession(const std::string& id) {
    if (!isAvailable()) {
        return false;
    }
    auto s = getSession(id);
    if (s && !s->user_id.empty()) {
        auto ukey = userSessionsKey(s->user_id);
        auto existing = redis_->get(ukey);
        if (existing) {
            std::string result;
            std::istringstream iss(*existing);
            std::string token;
            while (std::getline(iss, token, ',')) {
                if (token != id) {
                    if (!result.empty()) {
                        result += ",";
                    }
                    result += token;
                }
            }
            if (result.empty()) {
                redis_->del(ukey);
            } else {
                redis_->set(ukey, result, std::chrono::seconds(session_ttl_seconds_));
            }
        }
    }
    return redis_->del(sessionKey(id));
}

std::vector<Session> RedisSessionStore::listSessionsByUser(const std::string& user_id) {
    std::vector<Session> result;
    if (!isAvailable()) {
        return result;
    }
    auto ukey = userSessionsKey(user_id);
    auto list = redis_->get(ukey);
    if (!list || list->empty()) {
        return result;
    }
    std::istringstream iss(*list);
    std::string sid;
    while (std::getline(iss, sid, ',')) {
        if (sid.empty()) {
            continue;
        }
        auto s = getSession(sid);
        if (s) {
            result.push_back(std::move(*s));
        }
    }
    return result;
}

int64_t RedisSessionStore::deleteExpiredSessions() {
    // Redis TTL handles expiration automatically
    return 0;
}

int64_t RedisSessionStore::countSessionsByUser(const std::string& user_id) {
    auto sessions = listSessionsByUser(user_id);
    return static_cast<int64_t>(sessions.size());
}

bool RedisSessionStore::updateSessionMfaVerified(const std::string& id, bool verified) {
    if (!isAvailable()) {
        return false;
    }
    auto s = getSession(id);
    if (!s) {
        return false;
    }
    s->mfa_verified = verified;
    return redis_->set(sessionKey(id), serializeSession(*s),
                       std::chrono::seconds(session_ttl_seconds_));
}

#else

RedisSessionStore::RedisSessionStore(int session_ttl_seconds)
    : session_ttl_seconds_(session_ttl_seconds) {}

bool RedisSessionStore::isAvailable() const {
    (void)session_ttl_seconds_;
    return false;
}
bool RedisSessionStore::insertSession(const Session&) {
    return false;
}
std::optional<Session> RedisSessionStore::getSession(const std::string&) {
    return std::nullopt;
}
bool RedisSessionStore::updateSessionActivity(const std::string&, const std::string&) {
    return false;
}
bool RedisSessionStore::deleteSession(const std::string&) {
    return false;
}
std::vector<Session> RedisSessionStore::listSessionsByUser(const std::string&) {
    return {};
}
int64_t RedisSessionStore::deleteExpiredSessions() {
    return 0;
}
int64_t RedisSessionStore::countSessionsByUser(const std::string&) {
    return 0;
}
bool RedisSessionStore::updateSessionMfaVerified(const std::string&, bool) {
    return false;
}

#endif

}  // namespace aegisgate
