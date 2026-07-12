#pragma once
#include "auth/auth_models.h"
#include "control_plane/config_version_record.h"
#include "control_plane/rollout/rollout_record.h"
#include "guardrail/audit.h"
#include "observe/cost_tracker.h"
#include "storage/approval_proposal_record.h"
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace aegisgate {

class PersistentStore {
public:
    virtual ~PersistentStore() = default;
    virtual bool initialize() = 0;
    virtual void close() = 0;
    virtual bool isHealthy() const = 0;
    // P1-B: liveness probe for /health/ready. Default delegates to the passive
    // isHealthy(); pooled backends (PG) override to actively validate a real
    // connection so a post-startup DB outage is not masked.
    virtual bool isReady() const { return isHealthy(); }
    virtual std::string backendName() const = 0;

    // --- Audit ---
    virtual bool insertAudit(const AuditEntry& entry) = 0;
    virtual std::vector<AuditEntry> queryAudits(
        const std::string& tenant_id = "",
        int limit = 100, int offset = 0) = 0;
    virtual int64_t auditCount(const std::string& tenant_id = "") = 0;

    // --- Cost ---
    virtual bool insertCostRecord(const CostRecord& record) = 0;
    // TASK-20260604-01 P0-E/D1=A：新增尾部 tenant_id 过滤（DB 级 / 折叠 P1-11），
    // 为空时不过滤（向后兼容既有 model-only 调用）。
    virtual std::vector<CostRecord> queryCosts(
        const std::string& model = "",
        int limit = 100, int offset = 0,
        const std::string& tenant_id = "") = 0;
    virtual int64_t costRecordCount(const std::string& model = "",
                                    const std::string& tenant_id = "") = 0;

    // TASK-20260702-01 P1-2：total_cost 聚合。此前 dashboard 用
    // queryCosts("",10000,0) 内存累加，>10000 条被静默截断（金额低估）。
    // 非纯虚 + 默认 scan 实现保持 source 兼容（沿用 savings_events 模式），
    // 真实后端（memory/sqlite/pg）override 为 DB 级 SUM。tenant_id 为空 = 全局。
    virtual double costTotal(const std::string& tenant_id = "") {
        double sum = 0.0;
        auto rows = queryCosts("", 1000000, 0, tenant_id);
        for (const auto& r : rows) sum += r.total_cost;
        return sum;
    }

    virtual int64_t pruneAudits(int retention_days) = 0;
    virtual int64_t pruneCostRecords(int retention_days) = 0;

    // --- Cost date range query (usage prediction) ---
    virtual std::vector<CostRecord> queryCostsByDateRange(
        const std::string& tenant_id,
        const std::string& from,
        const std::string& to) {
        (void)tenant_id; (void)from; (void)to;
        return {};
    }

    // --- Savings events (dashboard persistence, TASK-20260617-02) ---
    //
    // Non-pure virtuals with no-op defaults so backends without the
    // savings_events table stay source-compatible (mirrors Rollout /
    // ApprovalProposal three-backend pattern). Memory + SQLite override;
    // PG optional. Only model / tenant_id / numeric fields are persisted —
    // NEVER raw prompt or response text (SR2: 敏感数据不落盘).
    struct SavingsEventRecord {
        int type = 0;                 // (int)SavingType: 0=CacheHit/1=Compression/2=Routing
        std::string model;
        std::string tenant_id;
        int tokens_saved = 0;
        double cost_saved = 0.0;
        bool fallback_pricing = false;
        std::string timestamp;        // ISO-8601 UTC
    };
    virtual bool insertSavingsEvent(const SavingsEventRecord& ev) {
        (void)ev; return false;
    }
    // Returns rows with from_iso <= timestamp <= to_iso, ordered by timestamp
    // ascending, capped at `limit`. Empty bound = open on that side.
    virtual std::vector<SavingsEventRecord> querySavingsEventsByDateRange(
        const std::string& from_iso, const std::string& to_iso,
        int limit = 100000) {
        (void)from_iso; (void)to_iso; (void)limit; return {};
    }
    virtual int64_t savingsEventCount() { return 0; }
    virtual int64_t pruneSavingsEvents(int retention_days) {
        (void)retention_days; return 0;
    }

    // --- Tenant CRUD (RBAC) ---
    virtual bool insertTenant(const Tenant& tenant) { (void)tenant; return false; }
    virtual std::optional<Tenant> getTenant(const std::string& id) { (void)id; return std::nullopt; }
    virtual std::vector<Tenant> listTenants(int limit = 100, int offset = 0) { (void)limit; (void)offset; return {}; }
    virtual bool updateTenant(const Tenant& tenant) { (void)tenant; return false; }
    virtual bool deleteTenant(const std::string& id) { (void)id; return false; }

    // --- User CRUD (RBAC) ---
    virtual bool insertUser(const User& user) { (void)user; return false; }
    virtual std::optional<User> getUser(const std::string& id) { (void)id; return std::nullopt; }
    virtual std::optional<User> getUserByUsername(const std::string& tenant_id, const std::string& username) {
        (void)tenant_id; (void)username; return std::nullopt;
    }
    virtual std::vector<User> listUsers(const std::string& tenant_id, int limit = 100, int offset = 0) {
        (void)tenant_id; (void)limit; (void)offset; return {};
    }
    virtual bool updateUser(const User& user) { (void)user; return false; }
    virtual bool deleteUser(const std::string& id) { (void)id; return false; }

    // --- API Key CRUD (RBAC) ---
    virtual bool insertApiKey(const ApiKeyRecord& key) { (void)key; return false; }
    virtual std::optional<ApiKeyRecord> getApiKeyByHash(const std::string& key_hash) { (void)key_hash; return std::nullopt; }
    virtual std::vector<ApiKeyRecord> getApiKeysByPrefix(const std::string& prefix) { (void)prefix; return {}; }
    virtual std::vector<ApiKeyRecord> listApiKeys(const std::string& tenant_id, int limit = 100, int offset = 0) {
        (void)tenant_id; (void)limit; (void)offset; return {};
    }
    virtual bool updateApiKey(const ApiKeyRecord& key) { (void)key; return false; }
    virtual bool revokeApiKey(const std::string& id) { (void)id; return false; }

    // --- Pagination total counts (TASK-20260604-01 P0-E/D1=A) ---
    // 真实总数，供 list 端点返回 `total`（区别于当前页 `count`）。默认 0；
    // 三后端 override。count 必须经调用方 effectiveTenantId 过滤（SR-3）。
    virtual int64_t tenantCount() { return 0; }
    virtual int64_t userCount(const std::string& tenant_id) {
        (void)tenant_id; return 0;
    }
    virtual int64_t apiKeyCount(const std::string& tenant_id) {
        (void)tenant_id; return 0;
    }
    virtual int64_t promptTemplateCount(const std::string& tenant_id) {
        (void)tenant_id; return 0;
    }
    virtual int64_t ruleSetCount(const std::string& tenant_id) {
        (void)tenant_id; return 0;
    }

    // --- Tenant cost aggregation ---
    virtual double getTenantCostInPeriod(const std::string& tenant_id,
        const std::string& start, const std::string& end) {
        (void)tenant_id; (void)start; (void)end; return 0.0;
    }

    // --- SSO Provider ---
    virtual bool insertSsoProvider(const SsoProvider& p) { (void)p; return false; }
    virtual std::optional<SsoProvider> getSsoProvider(const std::string& id) { (void)id; return std::nullopt; }
    virtual std::optional<SsoProvider> getSsoProviderByTenant(const std::string& tenant_id) { (void)tenant_id; return std::nullopt; }
    virtual std::vector<SsoProvider> listSsoProviders(int limit = 100, int offset = 0) { (void)limit; (void)offset; return {}; }
    virtual int64_t ssoProviderCount() { return 0; }
    virtual bool updateSsoProvider(const SsoProvider& p) { (void)p; return false; }
    virtual bool deleteSsoProvider(const std::string& id) { (void)id; return false; }

    // --- Identity Mapping ---
    virtual bool insertIdentityMapping(const IdentityMapping& m) { (void)m; return false; }
    virtual std::optional<IdentityMapping> getIdentityMapping(
        const std::string& external_subject, const std::string& external_issuer) {
        (void)external_subject; (void)external_issuer; return std::nullopt;
    }
    virtual std::vector<IdentityMapping> listIdentityMappings(const std::string& tenant_id, int limit = 100, int offset = 0) {
        (void)tenant_id; (void)limit; (void)offset; return {};
    }
    virtual bool deleteIdentityMapping(const std::string& id) { (void)id; return false; }
    virtual bool updateIdentityMappingLastLogin(const std::string& id, const std::string& ts) {
        (void)id; (void)ts; return false;
    }

    // --- Session ---
    virtual bool insertSession(const Session& s) { (void)s; return false; }
    virtual std::optional<Session> getSession(const std::string& id) { (void)id; return std::nullopt; }
    virtual std::vector<Session> listSessionsByUser(const std::string& user_id) { (void)user_id; return {}; }
    virtual bool updateSessionActivity(const std::string& id, const std::string& ts) { (void)id; (void)ts; return false; }
    virtual bool deleteSession(const std::string& id) { (void)id; return false; }
    virtual int64_t deleteExpiredSessions() { return 0; }
    virtual int64_t countSessionsByUser(const std::string& user_id) { (void)user_id; return 0; }
    virtual bool updateSessionMfaVerified(const std::string& id, bool verified) { (void)id; (void)verified; return false; }

    // --- MFA ---
    virtual bool upsertMfaSecret(const MfaSecret& m) { (void)m; return false; }
    virtual std::optional<MfaSecret> getMfaSecret(const std::string& user_id) { (void)user_id; return std::nullopt; }
    virtual bool deleteMfaSecret(const std::string& user_id) { (void)user_id; return false; }

    // --- MFA 失败锁定（TASK-20260702-02 P2-2 / SR-2）---
    // 原子记录一次失败：窗口内自增计数（now - first_fail_at > window_sec 则重置为
    // 1），达 max_failures 时置 locked_until = now + window_sec。返回是否已锁定。
    // 非纯虚 + 默认 no-op（沿用 savings/costTotal 模式），真实后端 override。
    virtual bool recordMfaFailure(const std::string& user_id, int max_failures,
                                  int window_sec) {
        (void)user_id; (void)max_failures; (void)window_sec; return false;
    }
    // 返回 locked_until（epoch 秒；0 或已过期由调用方判定为未锁）。
    virtual int64_t getMfaLockedUntil(const std::string& user_id) {
        (void)user_id; return 0;
    }
    // 验证成功后清零失败计数与锁定。
    virtual bool clearMfaFailures(const std::string& user_id) {
        (void)user_id; return false;
    }

    // --- SCIM Group ---
    struct ScimGroupRecord {
        std::string id;
        std::string tenant_id;
        std::string display_name;
        std::string created_at;
        std::string updated_at;
    };
    virtual bool insertScimGroup(const std::string& id, const std::string& tenant_id,
                                  const std::string& display_name) {
        (void)id; (void)tenant_id; (void)display_name; return false;
    }
    virtual std::optional<ScimGroupRecord> getScimGroup(const std::string& id) { (void)id; return std::nullopt; }
    virtual bool updateScimGroup(const std::string& id, const std::string& display_name,
                                  const std::vector<std::string>& member_ids) {
        (void)id; (void)display_name; (void)member_ids; return false;
    }
    virtual bool deleteScimGroup(const std::string& id) { (void)id; return false; }
    virtual std::vector<ScimGroupRecord> listScimGroups(const std::string& tenant_id) { (void)tenant_id; return {}; }
    virtual std::vector<std::string> getScimGroupMembers(const std::string& group_id) { (void)group_id; return {}; }

    // --- SCIM Token ---
    virtual bool insertScimToken(const ScimToken& t) { (void)t; return false; }
    virtual std::optional<ScimToken> getScimTokenByHash(const std::string& hash) { (void)hash; return std::nullopt; }
    virtual std::vector<ScimToken> listScimTokens(const std::string& tenant_id) { (void)tenant_id; return {}; }
    virtual bool deleteScimToken(const std::string& id) { (void)id; return false; }

    // --- Prompt Template ---
    struct PromptTemplateRecord {
        std::string id;
        std::string tenant_id;
        std::string name;
        std::string content;
        int version = 1;
        int weight = 100;
        bool is_active = true;
        std::string created_at;
        std::string updated_at;
    };
    virtual bool insertPromptTemplate(const PromptTemplateRecord& tpl) {
        (void)tpl; return false;
    }
    virtual std::optional<PromptTemplateRecord> getPromptTemplate(const std::string& id) {
        (void)id; return std::nullopt;
    }
    virtual bool updatePromptTemplate(const PromptTemplateRecord& tpl) {
        (void)tpl; return false;
    }
    virtual bool deletePromptTemplate(const std::string& id) { (void)id; return false; }
    virtual std::vector<PromptTemplateRecord> listPromptTemplates(
        const std::string& tenant_id, int limit = 100, int offset = 0) {
        (void)tenant_id; (void)limit; (void)offset; return {};
    }
    virtual std::vector<PromptTemplateRecord> listPromptTemplatesByName(
        const std::string& tenant_id, const std::string& name) {
        (void)tenant_id; (void)name; return {};
    }

    // --- Rule Set (versioned) ---
    struct RuleSetRecord {
        int64_t version = 0;
        std::string tenant_id;
        std::string rules_json;
        std::string created_at;
        bool is_active = false;
    };
    virtual bool insertRuleSet(const std::string& tenant_id, const RuleSetRecord& record) {
        (void)tenant_id; (void)record; return false;
    }
    virtual std::optional<RuleSetRecord> getActiveRuleSet(const std::string& tenant_id) {
        (void)tenant_id; return std::nullopt;
    }
    virtual std::vector<RuleSetRecord> listRuleSetVersions(const std::string& tenant_id, int limit = 20, int offset = 0) {
        (void)tenant_id; (void)limit; (void)offset; return {};
    }
    virtual bool activateRuleSetVersion(const std::string& tenant_id, int64_t version) {
        (void)tenant_id; (void)version; return false;
    }

    // --- ConfigBundle Versioning (Phase 9.3 control plane) ---
    //
    // Kept as non-pure virtuals with no-op defaults so backends not carrying
    // the control_versions table remain source-compatible. Backends wanting
    // to serve the control plane (Memory/SQLite/PG) override all six.
    virtual bool insertConfigVersion(const ConfigVersionRecord& rec) {
        (void)rec;
        return false;
    }
    virtual bool updateConfigStatus(const std::string& version_id,
                                    ConfigStatus new_status,
                                    const std::string& actor,
                                    const std::string& comment,
                                    std::int64_t timestamp_ms) {
        (void)version_id; (void)new_status; (void)actor; (void)comment;
        (void)timestamp_ms;
        return false;
    }
    virtual std::optional<ConfigVersionRecord> getConfigVersion(
        const std::string& version_id) {
        (void)version_id;
        return std::nullopt;
    }
    virtual std::vector<ConfigVersionRecord> listConfigVersions(
        const ConfigVersionQuery& q) {
        (void)q;
        return {};
    }
    virtual std::optional<ConfigVersionRecord> getActiveConfig() {
        return std::nullopt;
    }
    // activateConfig must be atomic: transition the previous ACTIVE (if any)
    // to SUPERSEDED and the target to ACTIVE in a single transaction.
    virtual bool activateConfig(const std::string& new_active_version_id,
                                const std::string& activator,
                                std::int64_t activate_ms) {
        (void)new_active_version_id; (void)activator; (void)activate_ms;
        return false;
    }

    // --- Rollout versioning (Phase 9.3.4 TASK-20260422-01) ---
    //
    // Non-pure virtuals with no-op defaults so backends without the rollouts/
    // rollout_stage_events tables stay source-compatible. Memory/SQLite
    // backends override all seven in Epic A.4 / A.5; PG backend keeps the
    // defaults as documented in design §9 (PG not part of MVP).
    virtual bool insertRollout(const RolloutRecord& rec) {
        (void)rec;
        return false;
    }
    virtual bool updateRollout(const RolloutRecord& rec) {
        (void)rec;
        return false;
    }
    virtual std::optional<RolloutRecord> getRollout(const std::string& rollout_id) {
        (void)rollout_id;
        return std::nullopt;
    }
    virtual std::vector<RolloutRecord> listRollouts(const RolloutQuery& q) {
        (void)q;
        return {};
    }
    // findActiveRolloutByTarget enforces "at most one PENDING/PROGRESSING/PAUSED
    // rollout per target_version_id" invariant; backends implement via UNIQUE
    // partial index (SQLite) or equivalent in-memory bookkeeping.
    virtual std::optional<RolloutRecord> findActiveRolloutByTarget(
        const std::string& target_version_id) {
        (void)target_version_id;
        return std::nullopt;
    }
    virtual bool appendRolloutStageEvent(const RolloutStageEvent& ev) {
        (void)ev;
        return false;
    }
    virtual std::vector<RolloutStageEvent> listRolloutStageEvents(
        const std::string& rollout_id) {
        (void)rollout_id;
        return {};
    }

    // --- Autonomy approval proposals (Phase 11.5 TASK-20260518-02) ---------
    //
    // Non-pure virtuals with no-op defaults so backends that don't carry
    // the autonomy_proposals table stay source-compatible. Memory/SQLite/PG
    // backends override all five in Epic 1.0 of design spec §4.1 / creative
    // C5 §1.3 (three-backend strategy, mirrors Rollout pattern).
    virtual bool insertApprovalProposal(const ApprovalProposalRecord& rec) {
        (void)rec;
        return false;
    }
    virtual std::optional<ApprovalProposalRecord> getApprovalProposal(
        const std::string& id) {
        (void)id;
        return std::nullopt;
    }
    virtual bool updateApprovalProposal(const ApprovalProposalRecord& rec) {
        (void)rec;
        return false;
    }
    virtual std::vector<ApprovalProposalRecord> listApprovalProposals(
        const ApprovalProposalQuery& q) {
        (void)q;
        return {};
    }
    virtual std::int64_t pruneApprovalProposals(int retention_days) {
        (void)retention_days;
        return 0;
    }
};

} // namespace aegisgate
