#pragma once
#include "auth/auth_models.h"
#include "core/config.h"
#include "storage/persistent_store.h"
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

class SessionManager {
public:
    SessionManager(PersistentStore* store, const Config* config);

    std::optional<Session> createSession(const std::string& user_id,
                                          const std::string& tenant_id,
                                          const std::string& auth_method,
                                          const std::string& ip_address,
                                          const std::string& user_agent);

    std::optional<Session> getSession(const std::string& session_id);

    bool touchSession(const std::string& session_id);

    bool deleteSession(const std::string& session_id);

    int64_t deleteExpiredSessions();

    bool setMfaVerified(const std::string& session_id, bool verified);

    std::vector<Session> listUserSessions(const std::string& user_id);

    int64_t deleteAllUserSessions(const std::string& user_id);

private:
    std::string generateSessionId() const;
    std::string nowIso() const;
    std::string expiresAtIso() const;
    bool isExpired(const Session& s) const;
    bool isIdleTimeout(const Session& s) const;
    void enforceLimit(const std::string& user_id);

    PersistentStore* store_;
    const Config* config_;
};

} // namespace aegisgate
