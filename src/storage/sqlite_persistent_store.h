#pragma once
#include "storage/persistent_store.h"
#include <sqlite3.h>
#include <mutex>

namespace aegisgate {

class SQLitePersistentStore : public PersistentStore {
public:
    explicit SQLitePersistentStore(const std::string& db_path,
                                    bool wal_mode = true);
    ~SQLitePersistentStore() override;

    bool initialize() override;
    void close() override;
    bool isHealthy() const override;
    std::string backendName() const override;

    bool insertAudit(const AuditEntry& entry) override;
    std::vector<AuditEntry> queryAudits(
        const std::string& tenant_id = "",
        int limit = 100, int offset = 0) override;
    int64_t auditCount(const std::string& tenant_id = "") override;

    bool insertCostRecord(const CostRecord& record) override;
    std::vector<CostRecord> queryCosts(
        const std::string& model = "",
        int limit = 100, int offset = 0,
        const std::string& tenant_id = "") override;
    double costTotal(const std::string& tenant_id = "") override;
    int64_t costRecordCount(const std::string& model = "",
                            const std::string& tenant_id = "") override;

    // TASK-20260604-01 P0-E：分页总数。
    int64_t tenantCount() override;
    int64_t userCount(const std::string& tenant_id) override;
    int64_t apiKeyCount(const std::string& tenant_id) override;
    int64_t promptTemplateCount(const std::string& tenant_id) override;
    int64_t ruleSetCount(const std::string& tenant_id) override;

    int64_t pruneAudits(int retention_days) override;
    int64_t pruneCostRecords(int retention_days) override;

    // --- RBAC: Tenant ---
    bool insertTenant(const Tenant& tenant) override;
    std::optional<Tenant> getTenant(const std::string& id) override;
    std::vector<Tenant> listTenants(int limit = 100, int offset = 0) override;
    bool updateTenant(const Tenant& tenant) override;
    bool deleteTenant(const std::string& id) override;

    // --- RBAC: User ---
    bool insertUser(const User& user) override;
    std::optional<User> getUser(const std::string& id) override;
    std::optional<User> getUserByUsername(const std::string& tenant_id, const std::string& username) override;
    std::vector<User> listUsers(const std::string& tenant_id, int limit = 100, int offset = 0) override;
    bool updateUser(const User& user) override;
    bool deleteUser(const std::string& id) override;

    // --- RBAC: API Key ---
    bool insertApiKey(const ApiKeyRecord& key) override;
    std::optional<ApiKeyRecord> getApiKeyByHash(const std::string& key_hash) override;
    std::vector<ApiKeyRecord> getApiKeysByPrefix(const std::string& prefix) override;
    std::vector<ApiKeyRecord> listApiKeys(const std::string& tenant_id, int limit = 100, int offset = 0) override;
    bool updateApiKey(const ApiKeyRecord& key) override;
    bool revokeApiKey(const std::string& id) override;

    // --- Tenant cost aggregation ---
    double getTenantCostInPeriod(const std::string& tenant_id,
        const std::string& start, const std::string& end) override;
    std::vector<CostRecord> queryCostsByDateRange(
        const std::string& tenant_id,
        const std::string& from,
        const std::string& to) override;

    // --- Savings events (dashboard persistence, TASK-20260617-02) ---
    bool insertSavingsEvent(const SavingsEventRecord& ev) override;
    std::vector<SavingsEventRecord> querySavingsEventsByDateRange(
        const std::string& from_iso, const std::string& to_iso,
        int limit = 100000) override;
    int64_t savingsEventCount() override;
    int64_t pruneSavingsEvents(int retention_days) override;

    // --- SSO Provider ---
    bool insertSsoProvider(const SsoProvider& p) override;
    std::optional<SsoProvider> getSsoProvider(const std::string& id) override;
    std::optional<SsoProvider> getSsoProviderByTenant(const std::string& tenant_id) override;
    std::vector<SsoProvider> listSsoProviders(int limit = 100, int offset = 0) override;
    int64_t ssoProviderCount() override;
    bool updateSsoProvider(const SsoProvider& p) override;
    bool deleteSsoProvider(const std::string& id) override;

    // --- Identity Mapping ---
    bool insertIdentityMapping(const IdentityMapping& m) override;
    std::optional<IdentityMapping> getIdentityMapping(
        const std::string& external_subject, const std::string& external_issuer) override;
    std::vector<IdentityMapping> listIdentityMappings(const std::string& tenant_id, int limit = 100, int offset = 0) override;
    bool deleteIdentityMapping(const std::string& id) override;
    bool updateIdentityMappingLastLogin(const std::string& id, const std::string& ts) override;

    // --- Session ---
    bool insertSession(const Session& s) override;
    std::optional<Session> getSession(const std::string& id) override;
    std::vector<Session> listSessionsByUser(const std::string& user_id) override;
    bool updateSessionActivity(const std::string& id, const std::string& ts) override;
    bool deleteSession(const std::string& id) override;
    int64_t deleteExpiredSessions() override;
    int64_t countSessionsByUser(const std::string& user_id) override;
    bool updateSessionMfaVerified(const std::string& id, bool verified) override;

    // --- MFA ---
    bool upsertMfaSecret(const MfaSecret& m) override;
    std::optional<MfaSecret> getMfaSecret(const std::string& user_id) override;
    bool deleteMfaSecret(const std::string& user_id) override;
    // TASK-20260702-02 P2-2（SR-2）：MFA 失败锁定。
    bool recordMfaFailure(const std::string& user_id, int max_failures,
                          int window_sec) override;
    int64_t getMfaLockedUntil(const std::string& user_id) override;
    bool clearMfaFailures(const std::string& user_id) override;

    // --- SCIM Group ---
    bool insertScimGroup(const std::string& id, const std::string& tenant_id,
                          const std::string& display_name) override;
    std::optional<ScimGroupRecord> getScimGroup(const std::string& id) override;
    bool updateScimGroup(const std::string& id, const std::string& display_name,
                          const std::vector<std::string>& member_ids) override;
    bool deleteScimGroup(const std::string& id) override;
    std::vector<ScimGroupRecord> listScimGroups(const std::string& tenant_id) override;
    std::vector<std::string> getScimGroupMembers(const std::string& group_id) override;

    // --- SCIM Token ---
    bool insertScimToken(const ScimToken& t) override;
    std::optional<ScimToken> getScimTokenByHash(const std::string& hash) override;
    std::vector<ScimToken> listScimTokens(const std::string& tenant_id) override;
    bool deleteScimToken(const std::string& id) override;

    // --- Prompt Template ---
    bool insertPromptTemplate(const PromptTemplateRecord& tpl) override;
    std::optional<PromptTemplateRecord> getPromptTemplate(const std::string& id) override;
    bool updatePromptTemplate(const PromptTemplateRecord& tpl) override;
    bool deletePromptTemplate(const std::string& id) override;
    std::vector<PromptTemplateRecord> listPromptTemplates(
        const std::string& tenant_id, int limit = 100, int offset = 0) override;
    std::vector<PromptTemplateRecord> listPromptTemplatesByName(
        const std::string& tenant_id, const std::string& name) override;

    // --- Rule Set ---
    bool insertRuleSet(const std::string& tenant_id, const RuleSetRecord& record) override;
    std::optional<RuleSetRecord> getActiveRuleSet(const std::string& tenant_id) override;
    std::vector<RuleSetRecord> listRuleSetVersions(const std::string& tenant_id, int limit = 20, int offset = 0) override;
    bool activateRuleSetVersion(const std::string& tenant_id, int64_t version) override;

    // --- ConfigBundle Versioning (Phase 9.3) ---
    bool insertConfigVersion(const ConfigVersionRecord& rec) override;
    bool updateConfigStatus(const std::string& version_id,
                            ConfigStatus new_status,
                            const std::string& actor,
                            const std::string& comment,
                            std::int64_t timestamp_ms) override;
    std::optional<ConfigVersionRecord> getConfigVersion(
        const std::string& version_id) override;
    std::vector<ConfigVersionRecord> listConfigVersions(
        const ConfigVersionQuery& q) override;
    std::optional<ConfigVersionRecord> getActiveConfig() override;
    bool activateConfig(const std::string& version_id,
                        const std::string& activator,
                        std::int64_t activate_ms) override;

    // --- Rollout versioning (Phase 9.3.4 TASK-20260422-01) ---
    bool insertRollout(const RolloutRecord& rec) override;
    bool updateRollout(const RolloutRecord& rec) override;
    std::optional<RolloutRecord> getRollout(const std::string& rollout_id) override;
    std::vector<RolloutRecord> listRollouts(const RolloutQuery& q) override;
    std::optional<RolloutRecord> findActiveRolloutByTarget(
        const std::string& target_version_id) override;
    bool appendRolloutStageEvent(const RolloutStageEvent& ev) override;
    std::vector<RolloutStageEvent> listRolloutStageEvents(
        const std::string& rollout_id) override;

    // --- Autonomy approval proposals (Phase 11.5 TASK-20260518-02) ---
    bool insertApprovalProposal(const ApprovalProposalRecord& rec) override;
    std::optional<ApprovalProposalRecord> getApprovalProposal(
        const std::string& id) override;
    bool updateApprovalProposal(const ApprovalProposalRecord& rec) override;
    std::vector<ApprovalProposalRecord> listApprovalProposals(
        const ApprovalProposalQuery& q) override;
    std::int64_t pruneApprovalProposals(int retention_days) override;

private:
    bool createTables(); // caller must hold mutex_
    bool enableWAL(); // caller must hold mutex_
    void closeLocked(); // caller must hold mutex_

    std::string db_path_;
    bool wal_mode_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_; // Lock Layer 2 — see docs/LOCK_ORDERING.md
};

} // namespace aegisgate
