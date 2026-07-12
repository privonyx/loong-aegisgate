#include "server/admin_governance_controller.h"
#include "server/admin_handler_helpers.h"
#include "observe/compliance_reporter.h"
#include "guardrail/audit.h"
#include "auth/encryption.h"

namespace aegisgate {

// TASK-20260605-05 Epic E6 — 治理域方法体逐字迁移自 AdminController（零行为变化）。

// --- Prompt Template management ---

AdminResult AdminGovernanceController::createPromptTemplate(
    const AuthContext& ctx, const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::TenantAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    std::string tenant_id = body.value("tenant_id", ctx.tenant_id);
    if (!auth::requireTenantAccess(ctx, tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    PersistentStore::PromptTemplateRecord rec;
    rec.id = generateId();
    rec.tenant_id = tenant_id;
    rec.name = body.value("name", "");
    rec.content = body.value("content", "");
    rec.version = body.value("version", 1);
    rec.weight = body.value("weight", 100);
    rec.is_active = body.value("is_active", true);
    rec.created_at = nowTimestamp();
    rec.updated_at = rec.created_at;

    if (rec.name.empty() || rec.content.empty())
        return AdminResult::error(ErrorCode::InvalidRequest, "Missing 'name' or 'content'");

    if (!store_->insertPromptTemplate(rec))
        return AdminResult::error(ErrorCode::InternalError, "Failed to insert template");

    auditAction(ctx, "admin.create_prompt_template", "id=" + rec.id + " name=" + rec.name);
    return AdminResult::ok({
        {"id", rec.id}, {"tenant_id", rec.tenant_id}, {"name", rec.name},
        {"content", rec.content}, {"version", rec.version}, {"weight", rec.weight},
        {"is_active", rec.is_active}, {"created_at", rec.created_at}
    }, 201);
}

AdminResult AdminGovernanceController::listPromptTemplates(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    if (!auth::requireRole(ctx, Role::Viewer))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto eff = effectiveTenantId(ctx, tenant_id);
    auto templates = store_->listPromptTemplates(eff, limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : templates) {
        arr.push_back({
            {"id", t.id}, {"tenant_id", t.tenant_id}, {"name", t.name},
            {"content", t.content}, {"version", t.version}, {"weight", t.weight},
            {"is_active", t.is_active}, {"created_at", t.created_at},
            {"updated_at", t.updated_at}
        });
    }
    // P0-E/SR-3：total 经 eff 过滤。
    return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                            {"total", store_->promptTemplateCount(eff)}});
}

AdminResult AdminGovernanceController::getPromptTemplate(
    const AuthContext& ctx, const std::string& id) {
    if (!auth::requireRole(ctx, Role::Viewer))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto t = store_->getPromptTemplate(id);
    if (!t) return AdminResult::error(ErrorCode::InvalidRequest, "Template not found");
    if (!auth::requireTenantAccess(ctx, t->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    return AdminResult::ok({
        {"id", t->id}, {"tenant_id", t->tenant_id}, {"name", t->name},
        {"content", t->content}, {"version", t->version}, {"weight", t->weight},
        {"is_active", t->is_active}, {"created_at", t->created_at},
        {"updated_at", t->updated_at}
    });
}

AdminResult AdminGovernanceController::updatePromptTemplate(
    const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::TenantAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto existing = store_->getPromptTemplate(id);
    if (!existing) return AdminResult::error(ErrorCode::InvalidRequest, "Template not found");
    if (!auth::requireTenantAccess(ctx, existing->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    if (body.contains("name")) existing->name = body["name"].get<std::string>();
    if (body.contains("content")) existing->content = body["content"].get<std::string>();
    if (body.contains("weight")) existing->weight = body["weight"].get<int>();
    if (body.contains("is_active")) existing->is_active = body["is_active"].get<bool>();
    existing->updated_at = nowTimestamp();

    if (!store_->updatePromptTemplate(*existing))
        return AdminResult::error(ErrorCode::InternalError, "Failed to update template");

    auditAction(ctx, "admin.update_prompt_template", "id=" + id);
    return AdminResult::ok({
        {"id", existing->id}, {"tenant_id", existing->tenant_id}, {"name", existing->name},
        {"content", existing->content}, {"version", existing->version},
        {"weight", existing->weight}, {"is_active", existing->is_active},
        {"updated_at", existing->updated_at}
    });
}

AdminResult AdminGovernanceController::deletePromptTemplate(
    const AuthContext& ctx, const std::string& id) {
    if (!auth::requireRole(ctx, Role::TenantAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto existing = store_->getPromptTemplate(id);
    if (!existing) return AdminResult::error(ErrorCode::InvalidRequest, "Template not found");
    if (!auth::requireTenantAccess(ctx, existing->tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    if (!store_->deletePromptTemplate(id))
        return AdminResult::error(ErrorCode::InternalError, "Failed to delete template");

    auditAction(ctx, "admin.delete_prompt_template", "id=" + id);
    return AdminResult::ok({{"deleted", true}, {"id", id}});
}

// --- Rule Set management ---

AdminResult AdminGovernanceController::listRuleSets(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    if (!auth::requireRole(ctx, Role::Viewer))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto eff = effectiveTenantId(ctx, tenant_id);
    auto versions = store_->listRuleSetVersions(eff, limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : versions) {
        arr.push_back({
            {"version", v.version}, {"tenant_id", v.tenant_id},
            {"created_at", v.created_at}, {"is_active", v.is_active},
            {"rules_json", v.rules_json}
        });
    }
    // SR-2：total 经 effectiveTenantId 过滤，非 super 仅见本租户计数（不泄漏跨租户）。
    return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                            {"total", store_->ruleSetCount(eff)}});
}

AdminResult AdminGovernanceController::getActiveRuleSet(
    const AuthContext& ctx, const std::string& tenant_id) {
    if (!auth::requireRole(ctx, Role::Viewer))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto eff = effectiveTenantId(ctx, tenant_id);
    auto active = store_->getActiveRuleSet(eff);
    if (!active)
        return AdminResult::error(ErrorCode::InvalidRequest, "No active rule set");
    return AdminResult::ok({
        {"version", active->version}, {"tenant_id", active->tenant_id},
        {"created_at", active->created_at}, {"is_active", active->is_active},
        {"rules_json", active->rules_json}
    });
}

AdminResult AdminGovernanceController::createRuleSet(
    const AuthContext& ctx, const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::TenantAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    std::string tenant_id = body.value("tenant_id", ctx.tenant_id);
    if (!auth::requireTenantAccess(ctx, tenant_id))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    std::string rules_json;
    if (body.contains("rules") && body["rules"].is_array()) {
        rules_json = body["rules"].dump();
    } else if (body.contains("rules_json") && body["rules_json"].is_string()) {
        rules_json = body["rules_json"].get<std::string>();
    } else {
        return AdminResult::error(ErrorCode::InvalidRequest, "Missing 'rules' or 'rules_json'");
    }

    try {
        auto parsed = nlohmann::json::parse(rules_json);
        (void)parsed;
    } catch (...) {
        return AdminResult::error(ErrorCode::InvalidRequest, "Invalid rules JSON");
    }

    auto existing = store_->listRuleSetVersions(tenant_id, 1);
    int64_t next_version = existing.empty() ? 1 : existing[0].version + 1;

    PersistentStore::RuleSetRecord record;
    record.version = next_version;
    record.tenant_id = tenant_id;
    record.rules_json = rules_json;
    record.created_at = nowTimestamp();
    record.is_active = true;

    if (!store_->insertRuleSet(tenant_id, record))
        return AdminResult::error(ErrorCode::InternalError, "Failed to insert rule set");

    auditAction(ctx, "admin.create_rule_set",
                "tenant=" + tenant_id + " version=" + std::to_string(next_version));
    return AdminResult::ok({
        {"version", record.version}, {"tenant_id", tenant_id},
        {"created_at", record.created_at}, {"is_active", true}
    }, 201);
}

AdminResult AdminGovernanceController::activateRuleSet(
    const AuthContext& ctx, const std::string& tenant_id, int64_t version) {
    if (!auth::requireRole(ctx, Role::TenantAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    auto eff = effectiveTenantId(ctx, tenant_id);
    if (!auth::requireTenantAccess(ctx, eff))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    if (!store_->activateRuleSetVersion(eff, version))
        return AdminResult::error(ErrorCode::InvalidRequest, "Version not found");

    // P2-4（SR-4）：激活即时生效 —— 刷新该租户运行时规则集桶。
    if (rule_set_activation_hook_) rule_set_activation_hook_(eff);

    auditAction(ctx, "admin.activate_rule_set",
                "tenant=" + eff + " version=" + std::to_string(version));
    return AdminResult::ok({{"activated", true}, {"version", version}});
}

// --- Audit query & export ---

// TASK-20260602-01 Epic 5 — RBAC role gate 用 admin::withRbac decorator 包裹（D3）。
// tenant scope 校验 (requireTenantAccess) 保留 inline，因为它是 data-scope 不是
// role-based，且与具体 endpoint 的 tenant_id 参数耦合。
AdminResult AdminGovernanceController::queryAudits(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {
        if (!tenant_id.empty() && !auth::requireTenantAccess(ctx, tenant_id))
            return AdminResult::error(ErrorCode::InsufficientPermissions);
        auto eff = effectiveTenantId(ctx, tenant_id);

        auto audits = store_->queryAudits(eff, limit, offset);
        const Encryption* enc = &Encryption::instance();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : audits) {
            // P1-3（TASK-20260702-01）：detail 落库前加密，读路径须对称解密回明文。
            arr.push_back({
                {"request_id", a.request_id}, {"timestamp", a.timestamp},
                {"tenant_id", a.tenant_id}, {"action", a.action},
                {"stage_name", a.stage_name},
                {"detail", AuditLogger::decryptDetail(a.detail, enc)}
            });
        }
        // P0-E/SR-3：total 经 eff 过滤。
        return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                                {"total", store_->auditCount(eff)}});
    });
}

AdminResult AdminGovernanceController::exportAuditReport(const AuthContext& ctx,
                                                          const std::string& from,
                                                          const std::string& to,
                                                          const std::string& tenant_id,
                                                          const std::string& format) {
    if (!auth::requireRole(ctx, Role::TenantAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    std::string effective_tenant = tenant_id;
    if (ctx.role != Role::SuperAdmin && !tenant_id.empty() && tenant_id != ctx.tenant_id) {
        return AdminResult::error(ErrorCode::InsufficientPermissions,
                                   "Cannot export other tenant's data");
    }
    if (ctx.role != Role::SuperAdmin) {
        effective_tenant = ctx.tenant_id;
    }

    ComplianceReporter reporter(store_);
    if (format == "json") {
        auto data = reporter.exportAuditsJson(from, to, effective_tenant);
        nlohmann::json result;
        result["format"] = "json";
        result["data"] = nlohmann::json::parse(data);
        auditAction(ctx, "admin.export_audit_report", "format=json");
        return AdminResult::ok(result);
    }

    auto csv = reporter.exportAuditsCsv(from, to, effective_tenant);
    nlohmann::json result;
    result["format"] = "csv";
    result["data"] = csv;
    auditAction(ctx, "admin.export_audit_report", "format=csv");
    return AdminResult::ok(result);
}

} // namespace aegisgate
