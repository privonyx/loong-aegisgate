#include "storage/memory_persistent_store.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace aegisgate {

bool MemoryPersistentStore::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return true;
}

void MemoryPersistentStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    audits_.clear();
    costs_.clear();
    initialized_ = false;
}

bool MemoryPersistentStore::isHealthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

std::string MemoryPersistentStore::backendName() const { return "memory"; }

bool MemoryPersistentStore::insertAudit(const AuditEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (audits_.size() >= max_audits_) audits_.erase(audits_.begin());
    audits_.push_back(entry);
    return true;
}

std::vector<AuditEntry> MemoryPersistentStore::queryAudits(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuditEntry> result;
    int skipped = 0;
    // C11 ②：newest-first（DESC），对齐 PG/SQLite audits。audits_ 按插入序 push_back
    // → 反向遍历即最新在前。
    for (auto it = audits_.rbegin(); it != audits_.rend(); ++it) {
        const auto& a = *it;
        if (!tenant_id.empty() && a.tenant_id != tenant_id) continue;
        if (skipped < offset) { ++skipped; continue; }
        result.push_back(a);
        if (static_cast<int>(result.size()) >= limit) break;
    }
    return result;
}

int64_t MemoryPersistentStore::auditCount(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tenant_id.empty()) return static_cast<int64_t>(audits_.size());
    return std::count_if(audits_.begin(), audits_.end(),
        [&](const AuditEntry& a) { return a.tenant_id == tenant_id; });
}

bool MemoryPersistentStore::insertCostRecord(const CostRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (costs_.size() >= max_costs_) costs_.erase(costs_.begin());
    costs_.push_back(record);
    return true;
}

std::vector<CostRecord> MemoryPersistentStore::queryCosts(
    const std::string& model, int limit, int offset, const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CostRecord> result;
    int skipped = 0;
    // C11 ②：newest-first（DESC），对齐 PG/SQLite。反向遍历插入序容器。
    for (auto it = costs_.rbegin(); it != costs_.rend(); ++it) {
        const auto& c = *it;
        if (!model.empty() && c.model != model) continue;
        if (!tenant_id.empty() && c.tenant_id != tenant_id) continue;
        if (skipped < offset) { ++skipped; continue; }
        result.push_back(c);
        if (static_cast<int>(result.size()) >= limit) break;
    }
    return result;
}

int64_t MemoryPersistentStore::costRecordCount(const std::string& model,
                                               const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::count_if(costs_.begin(), costs_.end(),
        [&](const CostRecord& c) {
            return (model.empty() || c.model == model) &&
                   (tenant_id.empty() || c.tenant_id == tenant_id);
        });
}

// TASK-20260702-01 P1-2 — total_cost DB 级聚合（无 10k 截断）。tenant 空 = 全局。
double MemoryPersistentStore::costTotal(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    double sum = 0.0;
    for (const auto& c : costs_) {
        if (!tenant_id.empty() && c.tenant_id != tenant_id) continue;
        sum += c.total_cost;
    }
    return sum;
}

// TASK-20260604-01 P0-E/D1=A — 分页总数。tenant 为空 = 全局（SR-3）。
int64_t MemoryPersistentStore::tenantCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int64_t>(tenants_.size());
}

int64_t MemoryPersistentStore::userCount(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tenant_id.empty()) return static_cast<int64_t>(users_.size());
    return std::count_if(users_.begin(), users_.end(),
        [&](const auto& kv) { return kv.second.tenant_id == tenant_id; });
}

int64_t MemoryPersistentStore::apiKeyCount(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tenant_id.empty()) return static_cast<int64_t>(api_keys_.size());
    return std::count_if(api_keys_.begin(), api_keys_.end(),
        [&](const auto& kv) { return kv.second.tenant_id == tenant_id; });
}

int64_t MemoryPersistentStore::promptTemplateCount(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tenant_id.empty()) return static_cast<int64_t>(prompt_templates_.size());
    return std::count_if(prompt_templates_.begin(), prompt_templates_.end(),
        [&](const auto& kv) { return kv.second.tenant_id == tenant_id; });
}

int64_t MemoryPersistentStore::ruleSetCount(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tenant_id.empty()) {
        auto it = rule_sets_.find(tenant_id);
        return it == rule_sets_.end() ? 0 : static_cast<int64_t>(it->second.size());
    }
    int64_t total = 0;
    for (const auto& kv : rule_sets_) total += static_cast<int64_t>(kv.second.size());
    return total;
}

namespace {
std::string cutoffTimestamp(int retention_days) {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retention_days);
    auto tt = std::chrono::system_clock::to_time_t(cutoff);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    std::ostringstream oss;
    oss << std::put_time(&buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
} // namespace

int64_t MemoryPersistentStore::pruneAudits(int retention_days) {
    if (retention_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cutoff = cutoffTimestamp(retention_days);
    auto it = std::remove_if(audits_.begin(), audits_.end(),
        [&](const AuditEntry& a) { return a.timestamp < cutoff; });
    int64_t removed = std::distance(it, audits_.end());
    audits_.erase(it, audits_.end());
    return removed;
}

int64_t MemoryPersistentStore::pruneCostRecords(int retention_days) {
    if (retention_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cutoff = cutoffTimestamp(retention_days);
    auto it = std::remove_if(costs_.begin(), costs_.end(),
        [&](const CostRecord& c) { return c.timestamp < cutoff; });
    int64_t removed = std::distance(it, costs_.end());
    costs_.erase(it, costs_.end());
    return removed;
}

// --- RBAC: Tenant ---

bool MemoryPersistentStore::insertTenant(const Tenant& tenant) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tenants_.count(tenant.id)) return false;
    tenants_[tenant.id] = tenant;
    return true;
}

std::optional<Tenant> MemoryPersistentStore::getTenant(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tenants_.find(id);
    if (it == tenants_.end()) return std::nullopt;
    return it->second;
}

std::vector<Tenant> MemoryPersistentStore::listTenants(int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tenant> result;
    int skipped = 0;
    for (const auto& [_, t] : tenants_) {
        if (skipped < offset) { ++skipped; continue; }
        result.push_back(t);
        if (static_cast<int>(result.size()) >= limit) break;
    }
    return result;
}

bool MemoryPersistentStore::updateTenant(const Tenant& tenant) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tenants_.find(tenant.id);
    if (it == tenants_.end()) return false;
    it->second = tenant;
    return true;
}

bool MemoryPersistentStore::deleteTenant(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return tenants_.erase(id) > 0;
}

// --- RBAC: User ---

bool MemoryPersistentStore::insertUser(const User& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_.count(user.id)) return false;
    for (const auto& [_, u] : users_) {
        if (u.tenant_id == user.tenant_id && u.username == user.username)
            return false;
    }
    users_[user.id] = user;
    return true;
}

std::optional<User> MemoryPersistentStore::getUser(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = users_.find(id);
    if (it == users_.end()) return std::nullopt;
    return it->second;
}

std::optional<User> MemoryPersistentStore::getUserByUsername(
    const std::string& tenant_id, const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, u] : users_) {
        if (u.tenant_id == tenant_id && u.username == username) return u;
    }
    return std::nullopt;
}

std::vector<User> MemoryPersistentStore::listUsers(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<User> result;
    int skipped = 0;
    for (const auto& [_, u] : users_) {
        if (!tenant_id.empty() && u.tenant_id != tenant_id) continue;
        if (skipped < offset) { ++skipped; continue; }
        result.push_back(u);
        if (static_cast<int>(result.size()) >= limit) break;
    }
    return result;
}

bool MemoryPersistentStore::updateUser(const User& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = users_.find(user.id);
    if (it == users_.end()) return false;
    it->second = user;
    return true;
}

bool MemoryPersistentStore::deleteUser(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return users_.erase(id) > 0;
}

// --- RBAC: API Key ---

bool MemoryPersistentStore::insertApiKey(const ApiKeyRecord& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (api_keys_.count(key.id)) return false;
    api_keys_[key.id] = key;
    return true;
}

std::optional<ApiKeyRecord> MemoryPersistentStore::getApiKeyByHash(const std::string& key_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, k] : api_keys_) {
        if (k.key_hash == key_hash) return k;
    }
    return std::nullopt;
}

std::vector<ApiKeyRecord> MemoryPersistentStore::getApiKeysByPrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApiKeyRecord> result;
    for (const auto& [_, k] : api_keys_) {
        if (k.key_prefix == prefix) result.push_back(k);
    }
    return result;
}

std::vector<ApiKeyRecord> MemoryPersistentStore::listApiKeys(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApiKeyRecord> result;
    int skipped = 0;
    for (const auto& [_, k] : api_keys_) {
        if (!tenant_id.empty() && k.tenant_id != tenant_id) continue;
        if (skipped < offset) { ++skipped; continue; }
        result.push_back(k);
        if (static_cast<int>(result.size()) >= limit) break;
    }
    return result;
}

bool MemoryPersistentStore::updateApiKey(const ApiKeyRecord& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = api_keys_.find(key.id);
    if (it == api_keys_.end()) return false;
    it->second = key;
    return true;
}

bool MemoryPersistentStore::revokeApiKey(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = api_keys_.find(id);
    if (it == api_keys_.end()) return false;
    it->second.status = "revoked";
    return true;
}

// --- Tenant cost aggregation ---

double MemoryPersistentStore::getTenantCostInPeriod(
    const std::string& tenant_id, const std::string& start, const std::string& end) {
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0.0;
    for (const auto& c : costs_) {
        if (c.tenant_id != tenant_id) continue;
        if (!start.empty() && c.timestamp < start) continue;
        if (!end.empty() && c.timestamp > end) continue;
        total += c.total_cost;
    }
    return total;
}

bool MemoryPersistentStore::recordMfaFailure(const std::string& user_id,
                                             int max_failures, int window_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& row = mfa_failures_[user_id];
    if (row.first_fail_at == 0 || now - row.first_fail_at > window_sec) {
        row.first_fail_at = now;
        row.fail_count = 1;
    } else {
        row.fail_count += 1;
    }
    if (row.fail_count >= max_failures) {
        row.locked_until = now + window_sec;
    }
    return row.locked_until > now;
}

int64_t MemoryPersistentStore::getMfaLockedUntil(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mfa_failures_.find(user_id);
    return it == mfa_failures_.end() ? 0 : it->second.locked_until;
}

bool MemoryPersistentStore::clearMfaFailures(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    mfa_failures_.erase(user_id);
    return true;
}

std::vector<CostRecord> MemoryPersistentStore::queryCostsByDateRange(
    const std::string& tenant_id, const std::string& from,
    const std::string& to) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CostRecord> result;
    for (const auto& c : costs_) {
        if (!tenant_id.empty() && c.tenant_id != tenant_id) continue;
        if (!from.empty() && c.timestamp < from) continue;
        if (!to.empty() && c.timestamp > to) continue;
        result.push_back(c);
    }
    return result;
}

// --- Savings events (dashboard persistence, TASK-20260617-02) ---

bool MemoryPersistentStore::insertSavingsEvent(const SavingsEventRecord& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (savings_events_.size() >= max_savings_events_)
        savings_events_.erase(savings_events_.begin());
    savings_events_.push_back(ev);
    return true;
}

std::vector<PersistentStore::SavingsEventRecord>
MemoryPersistentStore::querySavingsEventsByDateRange(
    const std::string& from_iso, const std::string& to_iso, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SavingsEventRecord> result;
    for (const auto& e : savings_events_) {
        if (!from_iso.empty() && e.timestamp < from_iso) continue;
        if (!to_iso.empty() && e.timestamp > to_iso) continue;
        result.push_back(e);
    }
    std::sort(result.begin(), result.end(),
        [](const SavingsEventRecord& a, const SavingsEventRecord& b) {
            return a.timestamp < b.timestamp;
        });
    if (limit >= 0 && static_cast<int>(result.size()) > limit)
        result.resize(limit);
    return result;
}

int64_t MemoryPersistentStore::savingsEventCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int64_t>(savings_events_.size());
}

int64_t MemoryPersistentStore::pruneSavingsEvents(int retention_days) {
    if (retention_days <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cutoff = cutoffTimestamp(retention_days);
    auto it = std::remove_if(savings_events_.begin(), savings_events_.end(),
        [&](const SavingsEventRecord& e) { return e.timestamp < cutoff; });
    int64_t removed = std::distance(it, savings_events_.end());
    savings_events_.erase(it, savings_events_.end());
    return removed;
}

// ===========================================================================
// TASK-20260703-02 C11 ③ / D3=A — SSO Provider / Identity Mapping / Session /
// SCIM Token 三后端对齐（此前 Memory 未 override → 基类 no-op）。语义严格镜像
// SQLite：insert 主键冲突返 false；getSsoProviderByTenant 仅返 enabled；
// listSessionsByUser 按 created_at DESC；deleteExpiredSessions 按 ISO expires_at；
// getScimTokenByHash 按 token_hash。所有方法持 mutex_（Lock Layer 2）。
// ===========================================================================

// --- SSO Provider ---

bool MemoryPersistentStore::insertSsoProvider(const SsoProvider& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (sso_providers_.count(p.id)) return false;  // 镜像 PK 冲突
    sso_providers_[p.id] = p;
    return true;
}

std::optional<SsoProvider> MemoryPersistentStore::getSsoProvider(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sso_providers_.find(id);
    if (it == sso_providers_.end()) return std::nullopt;
    return it->second;
}

std::optional<SsoProvider> MemoryPersistentStore::getSsoProviderByTenant(
    const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : sso_providers_) {
        if (kv.second.tenant_id == tenant_id && kv.second.enabled) return kv.second;
    }
    return std::nullopt;
}

std::vector<SsoProvider> MemoryPersistentStore::listSsoProviders(int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SsoProvider> all;
    all.reserve(sso_providers_.size());
    for (const auto& kv : sso_providers_) all.push_back(kv.second);
    // 稳定顺序（unordered_map 遍历序不定）：按 id 升序。
    std::sort(all.begin(), all.end(),
              [](const SsoProvider& a, const SsoProvider& b) { return a.id < b.id; });
    std::vector<SsoProvider> result;
    for (int i = offset; i < static_cast<int>(all.size()) &&
                         static_cast<int>(result.size()) < limit; ++i) {
        result.push_back(all[i]);
    }
    return result;
}

int64_t MemoryPersistentStore::ssoProviderCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int64_t>(sso_providers_.size());
}

bool MemoryPersistentStore::updateSsoProvider(const SsoProvider& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sso_providers_.find(p.id);
    if (it == sso_providers_.end()) return false;
    it->second = p;
    return true;
}

bool MemoryPersistentStore::deleteSsoProvider(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sso_providers_.erase(id) > 0;
}

// --- Identity Mapping ---

bool MemoryPersistentStore::insertIdentityMapping(const IdentityMapping& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (identity_mappings_.count(m.id)) return false;
    identity_mappings_[m.id] = m;
    return true;
}

std::optional<IdentityMapping> MemoryPersistentStore::getIdentityMapping(
    const std::string& external_subject, const std::string& external_issuer) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : identity_mappings_) {
        if (kv.second.external_subject == external_subject &&
            kv.second.external_issuer == external_issuer) {
            return kv.second;
        }
    }
    return std::nullopt;
}

std::vector<IdentityMapping> MemoryPersistentStore::listIdentityMappings(
    const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IdentityMapping> all;
    for (const auto& kv : identity_mappings_) {
        if (kv.second.tenant_id == tenant_id) all.push_back(kv.second);
    }
    std::sort(all.begin(), all.end(),
              [](const IdentityMapping& a, const IdentityMapping& b) { return a.id < b.id; });
    std::vector<IdentityMapping> result;
    for (int i = offset; i < static_cast<int>(all.size()) &&
                         static_cast<int>(result.size()) < limit; ++i) {
        result.push_back(all[i]);
    }
    return result;
}

bool MemoryPersistentStore::deleteIdentityMapping(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_mappings_.erase(id) > 0;
}

bool MemoryPersistentStore::updateIdentityMappingLastLogin(const std::string& id,
                                                           const std::string& ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = identity_mappings_.find(id);
    if (it == identity_mappings_.end()) return false;
    it->second.last_login_at = ts;
    return true;
}

// --- Session ---

bool MemoryPersistentStore::insertSession(const Session& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (sessions_.count(s.id)) return false;
    sessions_[s.id] = s;
    return true;
}

std::optional<Session> MemoryPersistentStore::getSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}

std::vector<Session> MemoryPersistentStore::listSessionsByUser(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Session> result;
    for (const auto& kv : sessions_) {
        if (kv.second.user_id == user_id) result.push_back(kv.second);
    }
    // ORDER BY created_at DESC（镜像 SQLite）。
    std::sort(result.begin(), result.end(),
              [](const Session& a, const Session& b) { return a.created_at > b.created_at; });
    return result;
}

bool MemoryPersistentStore::updateSessionActivity(const std::string& id,
                                                  const std::string& ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    it->second.last_active_at = ts;
    return true;
}

bool MemoryPersistentStore::deleteSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.erase(id) > 0;
}

int64_t MemoryPersistentStore::deleteExpiredSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    // C11 ①：now 用 ISO8601（与 expires_at 写入格式一致）。cutoffTimestamp(0) = now。
    std::string now_iso = cutoffTimestamp(0);
    int64_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.expires_at < now_iso) {
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

int64_t MemoryPersistentStore::countSessionsByUser(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::count_if(sessions_.begin(), sessions_.end(),
        [&](const auto& kv) { return kv.second.user_id == user_id; });
}

bool MemoryPersistentStore::updateSessionMfaVerified(const std::string& id, bool verified) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    it->second.mfa_verified = verified;
    return true;
}

// --- SCIM Token ---

bool MemoryPersistentStore::insertScimToken(const ScimToken& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (scim_tokens_.count(t.id)) return false;
    scim_tokens_[t.id] = t;
    return true;
}

std::optional<ScimToken> MemoryPersistentStore::getScimTokenByHash(const std::string& hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : scim_tokens_) {
        if (kv.second.token_hash == hash) return kv.second;
    }
    return std::nullopt;
}

std::vector<ScimToken> MemoryPersistentStore::listScimTokens(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScimToken> result;
    for (const auto& kv : scim_tokens_) {
        if (kv.second.tenant_id == tenant_id) result.push_back(kv.second);
    }
    std::sort(result.begin(), result.end(),
              [](const ScimToken& a, const ScimToken& b) { return a.id < b.id; });
    return result;
}

bool MemoryPersistentStore::deleteScimToken(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return scim_tokens_.erase(id) > 0;
}

// --- SCIM Group ---

bool MemoryPersistentStore::insertScimGroup(const std::string& id,
                                             const std::string& tenant_id,
                                             const std::string& display_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scim_groups_.count(id)) return false;
    auto now = std::to_string(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
    scim_groups_[id] = {id, tenant_id, display_name, now, now};
    return true;
}

std::optional<PersistentStore::ScimGroupRecord>
MemoryPersistentStore::getScimGroup(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = scim_groups_.find(id);
    if (it == scim_groups_.end()) return std::nullopt;
    return it->second;
}

bool MemoryPersistentStore::updateScimGroup(const std::string& id,
                                             const std::string& display_name,
                                             const std::vector<std::string>& member_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = scim_groups_.find(id);
    if (it == scim_groups_.end()) return false;
    it->second.display_name = display_name;
    it->second.updated_at = std::to_string(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    scim_group_members_[id] = member_ids;
    return true;
}

bool MemoryPersistentStore::deleteScimGroup(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto erased = scim_groups_.erase(id);
    scim_group_members_.erase(id);
    return erased > 0;
}

std::vector<PersistentStore::ScimGroupRecord>
MemoryPersistentStore::listScimGroups(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScimGroupRecord> result;
    for (const auto& [id, g] : scim_groups_) {
        if (g.tenant_id == tenant_id) result.push_back(g);
    }
    return result;
}

std::vector<std::string>
MemoryPersistentStore::getScimGroupMembers(const std::string& group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = scim_group_members_.find(group_id);
    if (it == scim_group_members_.end()) return {};
    return it->second;
}

// --- Prompt Template ---

bool MemoryPersistentStore::insertPromptTemplate(const PromptTemplateRecord& tpl) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (prompt_templates_.count(tpl.id)) return false;
    prompt_templates_[tpl.id] = tpl;
    return true;
}

std::optional<PersistentStore::PromptTemplateRecord>
MemoryPersistentStore::getPromptTemplate(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = prompt_templates_.find(id);
    if (it == prompt_templates_.end()) return std::nullopt;
    return it->second;
}

bool MemoryPersistentStore::updatePromptTemplate(const PromptTemplateRecord& tpl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = prompt_templates_.find(tpl.id);
    if (it == prompt_templates_.end()) return false;
    it->second = tpl;
    return true;
}

bool MemoryPersistentStore::deletePromptTemplate(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return prompt_templates_.erase(id) > 0;
}

std::vector<PersistentStore::PromptTemplateRecord>
MemoryPersistentStore::listPromptTemplates(const std::string& tenant_id,
                                            int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PromptTemplateRecord> result;
    int skipped = 0;
    for (const auto& [_, t] : prompt_templates_) {
        if (t.tenant_id != tenant_id) continue;
        if (skipped < offset) { ++skipped; continue; }
        result.push_back(t);
        if (static_cast<int>(result.size()) >= limit) break;
    }
    return result;
}

std::vector<PersistentStore::PromptTemplateRecord>
MemoryPersistentStore::listPromptTemplatesByName(const std::string& tenant_id,
                                                  const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PromptTemplateRecord> result;
    for (const auto& [_, t] : prompt_templates_) {
        if (t.tenant_id == tenant_id && t.name == name) {
            result.push_back(t);
        }
    }
    return result;
}

// --- Rule Set ---

bool MemoryPersistentStore::insertRuleSet(const std::string& tenant_id,
                                           const RuleSetRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& versions = rule_sets_[tenant_id];
    for (const auto& v : versions) {
        if (v.version == record.version) return false;
    }
    RuleSetRecord rec = record;
    rec.tenant_id = tenant_id;
    if (rec.is_active) {
        for (auto& v : versions) v.is_active = false;
    }
    versions.push_back(rec);
    return true;
}

std::optional<PersistentStore::RuleSetRecord>
MemoryPersistentStore::getActiveRuleSet(const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rule_sets_.find(tenant_id);
    if (it == rule_sets_.end()) return std::nullopt;
    for (const auto& v : it->second) {
        if (v.is_active) return v;
    }
    return std::nullopt;
}

std::vector<PersistentStore::RuleSetRecord>
MemoryPersistentStore::listRuleSetVersions(const std::string& tenant_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rule_sets_.find(tenant_id);
    if (it == rule_sets_.end()) return {};
    std::vector<RuleSetRecord> result;
    auto& versions = it->second;
    // 最新版本在前；先跳过 offset 个最新版本，再取 limit 个（与 SQL ORDER BY version DESC 一致）。
    int top = static_cast<int>(versions.size()) - 1 - std::max(0, offset);
    int count = 0;
    for (int i = top; i >= 0 && count < limit; --i, ++count) {
        result.push_back(versions[static_cast<size_t>(i)]);
    }
    return result;
}

bool MemoryPersistentStore::activateRuleSetVersion(const std::string& tenant_id,
                                                    int64_t version) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rule_sets_.find(tenant_id);
    if (it == rule_sets_.end()) return false;
    bool found = false;
    for (auto& v : it->second) {
        if (v.version == version) {
            v.is_active = true;
            found = true;
        } else {
            v.is_active = false;
        }
    }
    return found;
}

// --- ConfigBundle Versioning (Phase 9.3) ---

bool MemoryPersistentStore::insertConfigVersion(const ConfigVersionRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rec.version_id.empty()) return false;
    if (config_versions_.count(rec.version_id)) return false;
    config_versions_.emplace(rec.version_id, rec);
    return true;
}

bool MemoryPersistentStore::updateConfigStatus(
    const std::string& version_id,
    ConfigStatus new_status,
    const std::string& actor,
    const std::string& comment,
    std::int64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_versions_.find(version_id);
    if (it == config_versions_.end()) return false;
    auto& rec = it->second;
    rec.status = new_status;
    // APPROVED / REJECTED are both the reviewer's verdict; activation goes
    // through activateConfig() instead.
    if (new_status == ConfigStatus::APPROVED ||
        new_status == ConfigStatus::REJECTED) {
        rec.reviewer = actor;
        rec.reviewer_comment = comment;
        rec.reviewed_at = timestamp_ms;
    }
    return true;
}

std::optional<ConfigVersionRecord>
MemoryPersistentStore::getConfigVersion(const std::string& version_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_versions_.find(version_id);
    if (it == config_versions_.end()) return std::nullopt;
    return it->second;
}

std::vector<ConfigVersionRecord>
MemoryPersistentStore::listConfigVersions(const ConfigVersionQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigVersionRecord> all;
    all.reserve(config_versions_.size());
    for (const auto& [_, r] : config_versions_) {
        if (!q.statuses.empty()) {
            bool keep = false;
            for (ConfigStatus s : q.statuses) {
                if (s == r.status) { keep = true; break; }
            }
            if (!keep) continue;
        }
        if (q.since_millis > 0 && r.submitted_at < q.since_millis) continue;
        all.push_back(r);
    }
    // Order: submitted_at DESC, version_id DESC as tie-breaker (ULID is
    // time-sortable, so lexicographic DESC matches recency).
    std::sort(all.begin(), all.end(),
              [](const ConfigVersionRecord& a, const ConfigVersionRecord& b) {
                  if (a.submitted_at != b.submitted_at)
                      return a.submitted_at > b.submitted_at;
                  return a.version_id > b.version_id;
              });
    // Cursor: skip until after page_token (exclusive).
    auto start = all.begin();
    if (!q.page_token.empty()) {
        start = std::find_if(all.begin(), all.end(),
            [&](const ConfigVersionRecord& r) {
                return r.version_id == q.page_token;
            });
        if (start != all.end()) ++start;
    }
    std::vector<ConfigVersionRecord> out;
    const int cap = q.limit > 0 ? q.limit : 50;
    for (auto it = start; it != all.end() && static_cast<int>(out.size()) < cap; ++it) {
        out.push_back(*it);
    }
    return out;
}

std::optional<ConfigVersionRecord>
MemoryPersistentStore::getActiveConfig() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_config_version_id_.empty()) return std::nullopt;
    auto it = config_versions_.find(active_config_version_id_);
    if (it == config_versions_.end()) return std::nullopt;
    return it->second;
}

bool MemoryPersistentStore::activateConfig(
    const std::string& version_id,
    const std::string& activator,
    std::int64_t activate_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_versions_.find(version_id);
    if (it == config_versions_.end()) return false;
    auto& target = it->second;

    // Only APPROVED or SUPERSEDED (R2 rollback) can be activated. An
    // already-ACTIVE version is a no-op success (idempotent).
    if (target.status == ConfigStatus::ACTIVE &&
        active_config_version_id_ == version_id) {
        return true;
    }
    if (target.status != ConfigStatus::APPROVED &&
        target.status != ConfigStatus::SUPERSEDED) {
        return false;
    }

    // Atomically demote previous ACTIVE -> SUPERSEDED, promote target -> ACTIVE.
    if (!active_config_version_id_.empty() &&
        active_config_version_id_ != version_id) {
        auto prev_it = config_versions_.find(active_config_version_id_);
        if (prev_it != config_versions_.end()) {
            prev_it->second.status = ConfigStatus::SUPERSEDED;
            prev_it->second.deactivated_at = activate_ms;
        }
    }
    target.status = ConfigStatus::ACTIVE;
    target.activator = activator;
    target.activated_at = activate_ms;
    target.deactivated_at = 0;
    active_config_version_id_ = version_id;
    return true;
}

// =====================================================================
// Phase 9.3.4 RolloutController — rollout CRUD + stage-event log.
// Status taxonomy (see §5.1 of design doc):
//   ACTIVE  = {PENDING, PROGRESSING, PAUSED}   ← at most one per target
//   TERMINAL = {COMPLETED, FAILED, ABORTED}    ← unbounded, read-only
// Invariant: at most one ACTIVE rollout per target_version_id.
// =====================================================================

namespace {

bool isActiveStatus(RolloutStatus s) {
    switch (s) {
        case RolloutStatus::PENDING:
        case RolloutStatus::PROGRESSING:
        case RolloutStatus::PAUSED:
            return true;
        case RolloutStatus::COMPLETED:
        case RolloutStatus::FAILED:
        case RolloutStatus::ABORTED:
            return false;
    }
    return false;
}

}  // namespace

bool MemoryPersistentStore::insertRollout(const RolloutRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (rec.rollout_id.empty()) return false;
    if (rollouts_.find(rec.rollout_id) != rollouts_.end()) return false;
    if (isActiveStatus(rec.status)) {
        for (const auto& kv : rollouts_) {
            if (kv.second.target_version_id == rec.target_version_id &&
                isActiveStatus(kv.second.status)) {
                return false;
            }
        }
    }
    rollouts_.emplace(rec.rollout_id, rec);
    return true;
}

bool MemoryPersistentStore::updateRollout(const RolloutRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    auto it = rollouts_.find(rec.rollout_id);
    if (it == rollouts_.end()) return false;
    // If the update is keeping the rollout active, the invariant must still
    // hold against other rollouts on the same target.
    if (isActiveStatus(rec.status)) {
        for (const auto& kv : rollouts_) {
            if (kv.first == rec.rollout_id) continue;
            if (kv.second.target_version_id == rec.target_version_id &&
                isActiveStatus(kv.second.status)) {
                return false;
            }
        }
    }
    it->second = rec;
    return true;
}

std::optional<RolloutRecord> MemoryPersistentStore::getRollout(
    const std::string& rollout_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rollouts_.find(rollout_id);
    if (it == rollouts_.end()) return std::nullopt;
    return it->second;
}

std::optional<RolloutRecord> MemoryPersistentStore::findActiveRolloutByTarget(
    const std::string& target_version_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : rollouts_) {
        if (kv.second.target_version_id == target_version_id &&
            isActiveStatus(kv.second.status)) {
            return kv.second;
        }
    }
    return std::nullopt;
}

std::vector<RolloutRecord> MemoryPersistentStore::listRollouts(
    const RolloutQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RolloutRecord> out;
    out.reserve(rollouts_.size());
    for (const auto& kv : rollouts_) {
        const auto& r = kv.second;
        if (!q.statuses.empty()) {
            bool match = false;
            for (auto s : q.statuses) { if (s == r.status) { match = true; break; } }
            if (!match) continue;
        }
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(),
        [](const RolloutRecord& a, const RolloutRecord& b) {
            if (a.started_at != b.started_at) return a.started_at > b.started_at;
            return a.rollout_id > b.rollout_id;  // stable tiebreaker
        });
    // page_token = last rollout_id returned by previous page. Skip through
    // (including) that id, then return up to `limit` items.
    std::vector<RolloutRecord> page;
    bool skipping = !q.page_token.empty();
    int limit = q.limit > 0 ? q.limit : static_cast<int>(out.size());
    for (auto& r : out) {
        if (skipping) {
            if (r.rollout_id == q.page_token) skipping = false;
            continue;
        }
        page.push_back(std::move(r));
        if (static_cast<int>(page.size()) >= limit) break;
    }
    return page;
}

bool MemoryPersistentStore::appendRolloutStageEvent(
    const RolloutStageEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (ev.event_id.empty() || ev.rollout_id.empty()) return false;
    rollout_events_[ev.rollout_id].push_back(ev);
    return true;
}

std::vector<RolloutStageEvent> MemoryPersistentStore::listRolloutStageEvents(
    const std::string& rollout_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rollout_events_.find(rollout_id);
    if (it == rollout_events_.end()) return {};
    auto copy = it->second;
    std::sort(copy.begin(), copy.end(),
        [](const RolloutStageEvent& a, const RolloutStageEvent& b) {
            if (a.at_millis != b.at_millis) return a.at_millis < b.at_millis;
            return a.event_id < b.event_id;
        });
    return copy;
}

// =========================================================================
// Phase 11.5 AutonomyApprovalWorkflow — TASK-20260518-02 Epic 1.0
// =========================================================================

bool MemoryPersistentStore::insertApprovalProposal(
    const ApprovalProposalRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    if (rec.id.empty()) return false;
    if (approval_proposals_.find(rec.id) != approval_proposals_.end()) {
        return false;  // duplicate id rejected
    }
    approval_proposals_[rec.id] = rec;
    return true;
}

std::optional<ApprovalProposalRecord>
MemoryPersistentStore::getApprovalProposal(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = approval_proposals_.find(id);
    if (it == approval_proposals_.end()) return std::nullopt;
    return it->second;
}

bool MemoryPersistentStore::updateApprovalProposal(
    const ApprovalProposalRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;
    auto it = approval_proposals_.find(rec.id);
    if (it == approval_proposals_.end()) return false;
    it->second = rec;
    return true;
}

std::vector<ApprovalProposalRecord>
MemoryPersistentStore::listApprovalProposals(const ApprovalProposalQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApprovalProposalRecord> out;
    out.reserve(approval_proposals_.size());
    for (const auto& kv : approval_proposals_) {
        const auto& rec = kv.second;
        if (!q.state_filter.empty() && rec.state != q.state_filter) continue;
        if (!q.source_filter.empty() && rec.source != q.source_filter) continue;
        out.push_back(rec);
    }
    // newest first (proposed_at_ms DESC), then id DESC for stable tiebreak
    std::sort(out.begin(), out.end(),
        [](const ApprovalProposalRecord& a, const ApprovalProposalRecord& b) {
            if (a.proposed_at_ms != b.proposed_at_ms)
                return a.proposed_at_ms > b.proposed_at_ms;
            return a.id > b.id;
        });
    // Apply limit + offset
    int offset = std::max(0, q.offset);
    int limit  = q.limit > 0 ? q.limit : static_cast<int>(out.size());
    if (offset >= static_cast<int>(out.size())) return {};
    auto begin = out.begin() + offset;
    auto end   = (offset + limit >= static_cast<int>(out.size()))
                  ? out.end()
                  : begin + limit;
    return std::vector<ApprovalProposalRecord>(begin, end);
}

std::int64_t MemoryPersistentStore::pruneApprovalProposals(int retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || retention_days <= 0) return 0;
    // Cutoff = now - retention_days. Compute via system_clock so prune is
    // wall-clock aware (mirrors pruneAudits semantics).
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::int64_t cutoff_ms = now_ms -
        static_cast<std::int64_t>(retention_days) * 86400LL * 1000LL;
    std::int64_t pruned = 0;
    for (auto it = approval_proposals_.begin();
         it != approval_proposals_.end(); ) {
        if (it->second.proposed_at_ms < cutoff_ms) {
            it = approval_proposals_.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    return pruned;
}

} // namespace aegisgate
