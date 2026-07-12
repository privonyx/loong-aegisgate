#pragma once
#include "core/pipeline.h"
#include "core/feature_gate.h"
#include "storage/persistent_store.h"
#include <re2/re2.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <shared_mutex>

namespace aegisgate {

class AuditLogger;

namespace guard {
class GuardAdminController;
}  // namespace guard

enum class RuleAction { Block, Warn, Modify, Allow };

enum class ConditionType { RegexMatch, KeywordContains, LengthCheck, ModelMatch };

struct RuleCondition {
    ConditionType type;
    std::string value;
    std::unique_ptr<RE2> compiled_regex;
};

struct Rule {
    std::string id;
    std::string description;
    int priority = 0;
    std::vector<RuleCondition> conditions;
    RuleAction action;
    std::string action_param;
    bool enabled = true;
};

struct RuleResult {
    bool matched = false;
    std::string rule_id;
    RuleAction action = RuleAction::Allow;
    std::string detail;
    std::string action_param;
};

class RuleEngine : public PipelineStage {
public:
    explicit RuleEngine(const FeatureGate& gate);

    void addRule(const std::string& id, const std::string& description,
                 int priority, RuleAction action,
                 const std::string& action_param = "");
    void addConditionToRule(const std::string& rule_id,
                            ConditionType type, const std::string& value);

    bool loadFromYaml(const std::string& path);
    bool reloadRules(const std::string& path);

    bool loadFromStore(PersistentStore& store, const std::string& tenant_id);
    bool saveToStore(PersistentStore& store, const std::string& tenant_id);

    // TASK-20260702-01 P1-4：DB→引擎生效闭环。此前后台 createRuleSet/activateRuleSet
    // 写入 rule_sets 表，但运行时（装配 + reloadConfig）只从 config/rules/*.yaml
    // 加载，loadFromStore 从未接线 → 后台规则集永不生效。此方法优先加载 store 中
    // 该 scope 的激活规则集（DB 优先），无激活集时回退 YAML（向后兼容既有部署）。
    // 返回 true=从 store 加载，false=回退 YAML。
    // 注意：RuleEngine 是全局单实例，仅全局作用域（空 tenant）规则集在运行时生效；
    // per-tenant 逐请求强制评估仍属 backlog（见 memory-bank）。
    bool reloadFromStoreOrYaml(PersistentStore& store, const std::string& tenant_id,
                               const std::string& yaml_path);

    // TASK-20260702-02 P2-4（SR-4）：per-tenant 规则集请求期强制。装配/热重载遍历
    // 各租户激活集建 tenant_rules_ 桶；请求按 ctx.tenant_id 路由（无桶回退全局）。
    // loadAllTenants：全量重建所有租户桶（listTenants 遍历）。
    void loadAllTenants(PersistentStore& store);
    // reloadTenant：单租户桶刷新（activate 即时生效）。有激活集→建/替换；无→erase
    // 该桶回退全局。返回 true=建/替换，false=无激活集（已回退全局）。
    bool reloadTenant(PersistentStore& store, const std::string& tenant_id);

    RuleResult evaluate(const RequestContext& ctx) const;
    size_t ruleCount() const;

    // P1-C: borrowed AuditLogger; when set, a Block decision writes an audit
    // entry. Null-safe (no audit when unset).
    void setAuditLogger(AuditLogger* logger) { audit_logger_ = logger; }

    // TASK-20260708-03 / REV20260707-C2: borrowed GuardAdminController; when
    // set, a Block decision also records a structured `GuardExplanation`
    // (L2 layer, rule id + detail) for admin lookup. Nullable = no-op.
    void setGuardAdminController(guard::GuardAdminController* controller) {
        guard_admin_controller_ = controller;
    }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "RuleEngine"; }

private:
    using RuleVec = std::vector<std::unique_ptr<Rule>>;

    static bool parseYaml(const std::string& path, RuleVec& out);
    static bool parseConditionType(const std::string& s, ConditionType& out);
    static bool parseAction(const std::string& s, RuleAction& out);
    // TASK-20260702-02 P2-4：store 规则集 JSON → RuleVec（loadFromStore/loadAllTenants
    // /reloadTenant 共用；含解析 + 优先级降序排序）。返回 false=解析失败。
    static bool parseRuleSetJson(const std::string& rules_json, RuleVec& out);

    bool evaluateCondition(const RuleCondition& cond,
                           const RequestContext& ctx) const;
    std::string extractText(const RequestContext& ctx) const;
    RuleResult evaluateImpl(const RequestContext& ctx,
                            const RuleVec& rules) const;
    StageResult processImpl(RequestContext& ctx, const RuleVec& rules);

    mutable std::shared_mutex rules_mutex_;
    RuleVec rules_;
    // TASK-20260702-02 P2-4（SR-4）：per-tenant 激活规则集桶（key=tenant_id）。
    // 无桶的租户回退全局 rules_。与 rules_ 共用 rules_mutex_。
    std::map<std::string, RuleVec> tenant_rules_;
    const FeatureGate& gate_;
    AuditLogger* audit_logger_ = nullptr;  // P1-C: borrowed, may be null
    // TASK-20260708-03 / REV20260707-C2: borrowed, may be null.
    guard::GuardAdminController* guard_admin_controller_ = nullptr;
};

} // namespace aegisgate
