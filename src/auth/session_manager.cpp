#include "auth/session_manager.h"
#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace aegisgate {

SessionManager::SessionManager(PersistentStore* store, const Config* config)
    : store_(store), config_(config) {}

std::string SessionManager::generateSessionId() const {
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error("RAND_bytes failed generating session ID");
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : buf) oss << std::setw(2) << static_cast<int>(b);
    return oss.str();
}

std::string SessionManager::nowIso() const {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string SessionManager::expiresAtIso() const {
    int timeout = config_ ? config_->sessionAbsoluteTimeoutSeconds() : 28800;
    auto now = std::chrono::system_clock::now();
    auto expires = now + std::chrono::seconds(timeout);
    auto tt = std::chrono::system_clock::to_time_t(expires);
    std::tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

static std::chrono::system_clock::time_point parseIso(const std::string& ts) {
    std::tm tm{};
    std::istringstream iss(ts);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

bool SessionManager::isExpired(const Session& s) const {
    if (s.expires_at.empty()) return false;
    auto expires = parseIso(s.expires_at);
    auto now = std::chrono::system_clock::now();
    return now >= expires;
}

bool SessionManager::isIdleTimeout(const Session& s) const {
    int timeout = config_ ? config_->sessionIdleTimeoutSeconds() : 3600;
    if (timeout <= 0) return false;
    std::string ts = s.last_active_at.empty() ? s.created_at : s.last_active_at;
    if (ts.empty()) return false;
    auto last = parseIso(ts);
    auto deadline = last + std::chrono::seconds(timeout);
    auto now = std::chrono::system_clock::now();
    return now >= deadline;
}

void SessionManager::enforceLimit(const std::string& user_id) {
    int max_concurrent = config_ ? config_->sessionMaxConcurrent() : 5;
    if (max_concurrent <= 0) return;

    auto count = store_->countSessionsByUser(user_id);
    if (count < max_concurrent) return;

    auto sessions = store_->listSessionsByUser(user_id);
    // listSessionsByUser returns ORDER BY created_at DESC, so oldest are at the end
    std::sort(sessions.begin(), sessions.end(),
              [](const Session& a, const Session& b) {
                  return a.created_at < b.created_at;
              });

    int64_t to_delete = count - max_concurrent + 1;
    for (int64_t i = 0; i < to_delete && i < static_cast<int64_t>(sessions.size()); ++i) {
        store_->deleteSession(sessions[static_cast<size_t>(i)].id);
        spdlog::debug("Evicted oldest session {} for user {}", sessions[static_cast<size_t>(i)].id, user_id);
    }
}

std::optional<Session> SessionManager::createSession(const std::string& user_id,
                                                      const std::string& tenant_id,
                                                      const std::string& auth_method,
                                                      const std::string& ip_address,
                                                      const std::string& user_agent) {
    enforceLimit(user_id);

    Session s;
    s.id = generateSessionId();
    s.user_id = user_id;
    s.tenant_id = tenant_id;
    s.auth_method = auth_method;
    s.ip_address = ip_address;
    s.user_agent = user_agent;
    s.mfa_verified = false;
    s.created_at = nowIso();
    s.last_active_at = s.created_at;
    s.expires_at = expiresAtIso();

    if (!store_->insertSession(s)) {
        spdlog::error("Failed to insert session for user {}", user_id);
        return std::nullopt;
    }

    spdlog::info("Created session {} for user {} (method={})", s.id, user_id, auth_method);
    return s;
}

std::optional<Session> SessionManager::getSession(const std::string& session_id) {
    auto s = store_->getSession(session_id);
    if (!s) return std::nullopt;

    if (isExpired(*s)) {
        spdlog::debug("Session {} expired", session_id);
        return std::nullopt;
    }

    if (isIdleTimeout(*s)) {
        spdlog::debug("Session {} idle timeout", session_id);
        return std::nullopt;
    }

    touchSession(session_id);
    s->last_active_at = nowIso();
    return s;
}

bool SessionManager::touchSession(const std::string& session_id) {
    return store_->updateSessionActivity(session_id, nowIso());
}

bool SessionManager::deleteSession(const std::string& session_id) {
    return store_->deleteSession(session_id);
}

int64_t SessionManager::deleteExpiredSessions() {
    return store_->deleteExpiredSessions();
}

bool SessionManager::setMfaVerified(const std::string& session_id, bool verified) {
    return store_->updateSessionMfaVerified(session_id, verified);
}

std::vector<Session> SessionManager::listUserSessions(const std::string& user_id) {
    return store_->listSessionsByUser(user_id);
}

int64_t SessionManager::deleteAllUserSessions(const std::string& user_id) {
    auto sessions = store_->listSessionsByUser(user_id);
    int64_t deleted = 0;
    for (const auto& s : sessions) {
        if (store_->deleteSession(s.id)) ++deleted;
    }
    return deleted;
}

} // namespace aegisgate
