#pragma once
#include "auth/auth_models.h"
#include "auth/crypto_utils.h"
#include "auth/session_manager.h"
#include "core/config.h"
#include "core/feature_gate.h"
#include "storage/persistent_store.h"
#include <optional>
#include <string>

namespace aegisgate {

class AuthService {
public:
    AuthService(PersistentStore* store, const Config* config, const FeatureGate* gate);

    std::optional<AuthContext> resolve(const std::string& bearer_token) const;
    bool isRbacEnabled() const;

    std::optional<AuthContext> resolveSession(const std::string& session_id) const;
    bool isSsoEnabled(const std::string& tenant_id) const;
    bool isMfaRequired(const AuthContext& ctx) const;

    SessionManager& sessionManager();

private:
    std::optional<AuthContext> resolveRbac(const std::string& token) const;
    std::optional<AuthContext> resolveLegacy(const std::string& token) const;

    PersistentStore* store_;
    const Config* config_;
    const FeatureGate* gate_;
    SessionManager session_mgr_;
};

} // namespace aegisgate
