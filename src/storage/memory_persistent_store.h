#pragma once
#include "storage/persistent_store.h"
#include <mutex>
#include <unordered_map>

namespace aegisgate {

class MemoryPersistentStore : public PersistentStore {
public:
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

    // --- Cost date range query ---
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

    // --- MFA 失败锁定（TASK-20260702-02 P2-2 / SR-2）---
    bool recordMfaFailure(const std::string& user_id, int max_failures,
                          int window_sec) override;
    int64_t getMfaLockedUntil(const std::string& user_id) override;
    bool clearMfaFailures(const std::string& user_id) override;

    // --- SSO Provider (TASK-20260703-02 C11 ③ / D3=A) ---
    bool insertSsoProvider(const SsoProvider& p) override;
    std::optional<SsoProvider> getSsoProvider(const std::string& id) override;
    std::optional<SsoProvider> getSsoProviderByTenant(const std::string& tenant_id) override;
    std::vector<SsoProvider> listSsoProviders(int limit = 100, int offset = 0) override;
    int64_t ssoProviderCount() override;
    bool updateSsoProvider(const SsoProvider& p) override;
    bool deleteSsoProvider(const std::string& id) override;

    // --- Identity Mapping (C11 ③) ---
    bool insertIdentityMapping(const IdentityMapping& m) override;
    std::optional<IdentityMapping> getIdentityMapping(
        const std::string& external_subject, const std::string& external_issuer) override;
    std::vector<IdentityMapping> listIdentityMappings(
        const std::string& tenant_id, int limit = 100, int offset = 0) override;
    bool deleteIdentityMapping(const std::string& id) override;
    bool updateIdentityMappingLastLogin(const std::string& id, const std::string& ts) override;

    // --- Session (C11 ③) ---
    bool insertSession(const Session& s) override;
    std::optional<Session> getSession(const std::string& id) override;
    std::vector<Session> listSessionsByUser(const std::string& user_id) override;
    bool updateSessionActivity(const std::string& id, const std::string& ts) override;
    bool deleteSession(const std::string& id) override;
    int64_t deleteExpiredSessions() override;
    int64_t countSessionsByUser(const std::string& user_id) override;
    bool updateSessionMfaVerified(const std::string& id, bool verified) override;

    // --- SCIM Token (C11 ③) ---
    bool insertScimToken(const ScimToken& t) override;
    std::optional<ScimToken> getScimTokenByHash(const std::string& hash) override;
    std::vector<ScimToken> listScimTokens(const std::string& tenant_id) override;
    bool deleteScimToken(const std::string& id) override;

    // --- SCIM Group ---
    bool insertScimGroup(const std::string& id, const std::string& tenant_id,
                          const std::string& display_name) override;
    std::optional<ScimGroupRecord> getScimGroup(const std::string& id) override;
    bool updateScimGroup(const std::string& id, const std::string& display_name,
                          const std::vector<std::string>& member_ids) override;
    bool deleteScimGroup(const std::string& id) override;
    std::vector<ScimGroupRecord> listScimGroups(const std::string& tenant_id) override;
    std::vector<std::string> getScimGroupMembers(const std::string& group_id) override;

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
    bool initialized_ = false;
    std::vector<AuditEntry> audits_;
    std::vector<CostRecord> costs_;
    // TASK-20260617-02: durable savings events for dashboard reload.
    std::vector<SavingsEventRecord> savings_events_;
    std::unordered_map<std::string, Tenant> tenants_;
    std::unordered_map<std::string, User> users_;
    std::unordered_map<std::string, ApiKeyRecord> api_keys_;
    std::unordered_map<std::string, ScimGroupRecord> scim_groups_;
    std::unordered_map<std::string, std::vector<std::string>> scim_group_members_;
    // TASK-20260703-02 C11 ③：SSO/身份映射/会话/SCIM Token 持久化（keyed by id）。
    std::unordered_map<std::string, SsoProvider> sso_providers_;
    std::unordered_map<std::string, IdentityMapping> identity_mappings_;
    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<std::string, ScimToken> scim_tokens_;
    std::unordered_map<std::string, PromptTemplateRecord> prompt_templates_;
    std::unordered_map<std::string, std::vector<RuleSetRecord>> rule_sets_;
    // Phase 9.3: global ConfigBundle versioning (no tenant axis).
    std::unordered_map<std::string, ConfigVersionRecord> config_versions_;
    std::string active_config_version_id_;  // empty => no ACTIVE version
    // Phase 9.3.4: rollouts keyed by rollout_id; events keyed by rollout_id.
    std::unordered_map<std::string, RolloutRecord> rollouts_;
    std::unordered_map<std::string, std::vector<RolloutStageEvent>>
        rollout_events_;
    // Phase 11.5: autonomy approval proposals keyed by ULID id.
    std::unordered_map<std::string, ApprovalProposalRecord> approval_proposals_;
    // TASK-20260702-02 P2-2: MFA failure lockout keyed by user_id.
    struct MfaFailRow {
        int fail_count = 0;
        int64_t first_fail_at = 0;   // epoch 秒，窗口起点
        int64_t locked_until = 0;    // epoch 秒，0=未锁
    };
    std::unordered_map<std::string, MfaFailRow> mfa_failures_;
    mutable std::mutex mutex_;  // Lock Layer 2 — see docs/LOCK_ORDERING.md
    size_t max_audits_ = 100000;
    size_t max_costs_ = 100000;
    size_t max_savings_events_ = 100000;
};

} // namespace aegisgate
