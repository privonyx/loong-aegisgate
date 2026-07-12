#include "server/admin_controller.h"
#include "core/feature_gate.h"
#include "server/admin_handler_helpers.h"
#include "server/case_study_builder.h"
#include "cache/semantic_cache.h"
#include "observe/autonomy/approval_queue.h"
#include "observe/autonomy/approval_workflow.h"
#include "observe/compliance_reporter.h"
#include "observe/cost_optimizer.h"
#include "observe/cost_tracker.h"
#include "observe/metrics.h"
#include "observe/quality_monitor.h"
#include "observe/savings_aggregator.h"
#include "observe/usage_predictor.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisgate {

AdminController::AdminController(PersistentStore* store, AuthService* auth_service,
                                 AuditLogger* audit,
                                 OidcClient* oidc_client,
                                 IdentityMapper* identity_mapper,
                                 SavingsAggregator* savings_aggregator,
                                 SemanticCache* semantic_cache,
                                 const FeatureGate* feature_gate)
    : AdminControllerBase(store, audit),
      iam_(store, audit),
      governance_(store, audit),
      auth_service_(auth_service),
      oidc_client_(oidc_client), identity_mapper_(identity_mapper),
      savings_aggregator_(savings_aggregator),
      semantic_cache_(semantic_cache),
      // TASK-20260711-01 / REV20260707-I13: AdvancedRouting license gate.
      feature_gate_(feature_gate) {}

// effectiveTenantId / auditAction / auditCrossTenantAction / nowTimestamp /
// generateId 已上移 AdminControllerBase（TASK-20260605-05 Epic 0）。

// --- IAM 域（租户 / 用户 / API 密钥）---
// TASK-20260605-05 Epic E2：方法体迁至 AdminIamController；Facade 薄委托。
// tenantToJson / userToJson / apiKeyToJson 随方法体一并迁出。

AdminResult AdminController::createTenant(const AuthContext& ctx, const nlohmann::json& body) {
    return iam_.createTenant(ctx, body);
}
AdminResult AdminController::listTenants(const AuthContext& ctx, int limit, int offset) {
    return iam_.listTenants(ctx, limit, offset);
}
AdminResult AdminController::getTenant(const AuthContext& ctx, const std::string& id) {
    return iam_.getTenant(ctx, id);
}
AdminResult AdminController::updateTenant(const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    return iam_.updateTenant(ctx, id, body);
}
AdminResult AdminController::deleteTenant(const AuthContext& ctx, const std::string& id) {
    return iam_.deleteTenant(ctx, id);
}

AdminResult AdminController::createUser(const AuthContext& ctx, const nlohmann::json& body) {
    return iam_.createUser(ctx, body);
}
AdminResult AdminController::listUsers(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    return iam_.listUsers(ctx, tenant_id, limit, offset);
}
AdminResult AdminController::getUser(const AuthContext& ctx, const std::string& id) {
    return iam_.getUser(ctx, id);
}
AdminResult AdminController::updateUser(
    const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    return iam_.updateUser(ctx, id, body);
}
AdminResult AdminController::deleteUser(const AuthContext& ctx, const std::string& id) {
    return iam_.deleteUser(ctx, id);
}

AdminResult AdminController::createApiKey(const AuthContext& ctx, const nlohmann::json& body) {
    return iam_.createApiKey(ctx, body);
}
AdminResult AdminController::listApiKeys(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    return iam_.listApiKeys(ctx, tenant_id, limit, offset);
}
AdminResult AdminController::revokeApiKey(const AuthContext& ctx, const std::string& id) {
    return iam_.revokeApiKey(ctx, id);
}
AdminResult AdminController::rotateApiKey(const AuthContext& ctx, const std::string& id) {
    return iam_.rotateApiKey(ctx, id);
}

// --- Prompt Template / Rule Set / Audit query ---
// TASK-20260605-05 Epic E6：方法体迁至 AdminGovernanceController；Facade 薄委托。

AdminResult AdminController::createPromptTemplate(
    const AuthContext& ctx, const nlohmann::json& body) {
    return governance_.createPromptTemplate(ctx, body);
}
AdminResult AdminController::listPromptTemplates(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    return governance_.listPromptTemplates(ctx, tenant_id, limit, offset);
}
AdminResult AdminController::getPromptTemplate(
    const AuthContext& ctx, const std::string& id) {
    return governance_.getPromptTemplate(ctx, id);
}
AdminResult AdminController::updatePromptTemplate(
    const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    return governance_.updatePromptTemplate(ctx, id, body);
}
AdminResult AdminController::deletePromptTemplate(
    const AuthContext& ctx, const std::string& id) {
    return governance_.deletePromptTemplate(ctx, id);
}

AdminResult AdminController::listRuleSets(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    return governance_.listRuleSets(ctx, tenant_id, limit, offset);
}
AdminResult AdminController::getActiveRuleSet(
    const AuthContext& ctx, const std::string& tenant_id) {
    return governance_.getActiveRuleSet(ctx, tenant_id);
}
AdminResult AdminController::createRuleSet(
    const AuthContext& ctx, const nlohmann::json& body) {
    return governance_.createRuleSet(ctx, body);
}
AdminResult AdminController::activateRuleSet(
    const AuthContext& ctx, const std::string& tenant_id, int64_t version) {
    return governance_.activateRuleSet(ctx, tenant_id, version);
}

// --- Audit & Cost queries ---

AdminResult AdminController::queryAudits(
    const AuthContext& ctx, const std::string& tenant_id, int limit, int offset) {
    return governance_.queryAudits(ctx, tenant_id, limit, offset);
}

AdminResult AdminController::queryCosts(
    const AuthContext& ctx, const std::string& tenant_id,
    const std::string& model, int limit, int offset) {
    return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {
        if (!tenant_id.empty() && !auth::requireTenantAccess(ctx, tenant_id))
            return AdminResult::error(ErrorCode::InsufficientPermissions);
        auto eff = effectiveTenantId(ctx, tenant_id);

        // P0-E/D1=A：tenant 过滤下沉 DB（折叠 P1-11），修正翻页（旧实现
        // 在 app 层过滤 DB 已分页结果 → 页内条数错乱）。
        auto costs = store_->queryCosts(model, limit, offset, eff);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : costs) {
            arr.push_back({
                {"request_id", c.request_id}, {"tenant_id", c.tenant_id},
                {"model", c.model}, {"total_cost", c.total_cost},
                {"input_tokens", c.input_tokens}, {"output_tokens", c.output_tokens},
                {"timestamp", c.timestamp}
            });
        }
        // P0-E/SR-3：total 经 model + eff 过滤。
        return AdminResult::ok({{"data", arr}, {"count", arr.size()},
                                {"total", store_->costRecordCount(model, eff)}});
    });
}

// --- Dashboard ---

// TASK-20260602-01 Epic 5 — RBAC 用 admin::withRbac decorator 包裹（D3）。
AdminResult AdminController::dashboardSummary(const AuthContext& ctx) {
    return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {

    // SR-3（TASK-20260702-01）：计数须经 effectiveTenantId 过滤，非 super 只见
    // 本租户计数（super → "" 全局）。此前走全局 count 会跨租户泄漏计数。
    const std::string count_scope = effectiveTenantId(ctx, "");
    int64_t total_requests = store_->auditCount(count_scope);
    int64_t total_cost_records = store_->costRecordCount("", count_scope);

    auto tenants = store_->listTenants(10000, 0);
    int active_tenants = 0;
    for (const auto& t : tenants) {
        if (t.status == "active") ++active_tenants;
    }

    // P1-2（TASK-20260702-01）：改 DB 级 SUM，避免此前 queryCosts("",10000,0)
    // 内存累加对 >10000 条的静默截断（金额低估）。scope 经 effectiveTenantId
    // 过滤，非 super 只计本租户（与 count_scope 同源）。
    double total_cost = store_->costTotal(count_scope);

    nlohmann::json summary;
    summary["total_requests"] = total_requests;
    summary["active_tenants"] = active_tenants;
    summary["total_cost"] = total_cost;
    summary["total_cost_records"] = total_cost_records;

    // 缓存命中率：来自 SemanticCache::getStats() 实时统计；
    // 注入缺失时为 null（兼容老调用方 / 测试 stub）。
    if (semantic_cache_) {
        auto stats = semantic_cache_->getStats();
        summary["cache_hit_rate"] = static_cast<double>(stats.hit_rate);
        summary["cache_hit_count"] = static_cast<uint64_t>(stats.hit_count);
        summary["cache_miss_count"] = static_cast<uint64_t>(stats.miss_count);
    } else {
        summary["cache_hit_rate"] = nullptr;
    }

    // 已节省金额（近 30 天，per-tenant 视角）：来自 SavingsAggregator。
    // 进程内聚合，重启重置；UI 通过 aggregator_since 标注。
    summary["cost_saved_30d"] = 0.0;
    summary["aggregator_since"] = nullptr;
    if (savings_aggregator_) {
        auto now = std::chrono::system_clock::now();
        auto from = now - std::chrono::hours(24 * 30);
        const std::string scope_tenant =
            (ctx.role == Role::SuperAdmin) ? std::string() : ctx.tenant_id;
        auto snap = savings_aggregator_->snapshot(scope_tenant, from, now);
        summary["cost_saved_30d"] = snap.total.cost_saved;
        std::time_t since_t = std::chrono::system_clock::to_time_t(snap.since);
        std::tm since_tm{};
        gmtime_r(&since_t, &since_tm);
        char since_buf[32];
        std::strftime(since_buf, sizeof(since_buf), "%Y-%m-%dT%H:%M:%SZ", &since_tm);
        summary["aggregator_since"] = std::string(since_buf);
    }

    return AdminResult::ok(summary);
    });
}

// --- Case Study Numbers (TASK-20260527-02 / MVP-5 prep) ---
//
// 拼装 3 头条数字（spec §3.5 Row 4 / D4=A）：
//   1. saved_vs_baseline ($X)  — CostTracker totalSummary / summaryByTenant
//   2. cache_hit_by_type Y%    — SemanticCache::getStats hit_exact/semantic/conversation
//   3. quality_reason            — QualityMonitor::getTrends 聚合 reason taxonomy
//
// SR1 RBAC: SuperAdmin → 全局聚合（scope="global"）；其他角色 → 限本租户
//          （scope="tenant"，effectiveTenantId）。
// SR2 baseline_cost 不接受 HTTP 入参 — 此端点为 GET，没有 body 输入路径。
// TASK-20260602-01 Epic 2 — caseStudyHeadline 改调共享 CaseStudySnapshotBuilder。
// 原 ~100 行内联逻辑提取至 src/server/case_study_builder.cpp，与
// admin_ws_controller::caseStudyTimer 共用 → 消除双份构造的 schema 漂移风险。
// 业务等价由 test_admin_case_study_endpoint.cpp 既有 4 测试用例继续保护。
// TASK-20260602-01 Epic 5 — RBAC 闸门用 admin::withRbac decorator 包裹（D3）。
AdminResult AdminController::caseStudyHeadline(const AuthContext& ctx) {
    return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {
        admin::CaseStudyInputs in;
        in.cost_tracker = cost_tracker_;
        in.semantic_cache = semantic_cache_;
        in.quality_monitor = quality_monitor_;
        in.savings_aggregator = savings_aggregator_;
        in.is_super = (ctx.role == Role::SuperAdmin);
        in.tenant_id = ctx.tenant_id;
        in.include_envelope = true;
        return AdminResult::ok(admin::buildCaseStudySnapshot(in));
    });
}

namespace {

// 解析 ISO 8601 (UTC) 字符串到 system_clock::time_point；失败返回 def。
// 仅接受 "YYYY-MM-DDTHH:MM:SS"（可选末尾 'Z'），与 nowTimestamp 输出兼容。
std::chrono::system_clock::time_point parseIsoOrDefault(
    const std::string& iso, std::chrono::system_clock::time_point def) {
    if (iso.empty()) return def;
    std::tm tm{};
    std::istringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return def;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

std::string formatIso(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

const char* savingTypeName(SavingType t) {
    switch (t) {
        case SavingType::CacheHit:    return "cache_hit";
        case SavingType::Compression: return "compression";
        case SavingType::Routing:     return "routing_potential";
    }
    return "unknown";
}

}  // namespace

// TASK-20260602-01 Epic 5 — RBAC 用 admin::withRbac decorator 包裹（D3）。
AdminResult AdminController::getSavingsSummary(
    const AuthContext& ctx, const std::string& from_iso,
    const std::string& to_iso, const std::string& tenant_id) {
    return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {

    // TASK-20260711-01 / REV20260707-I13: AdvancedRouting license gate.
    // Savings analytics (ROI / routing_recommendations / ...) are an
    // Enterprise-only capability per feature-list.md and the Editions
    // matrix. Community-edition Viewers pass RBAC but must be rejected
    // here so the license model is enforced.
    //
    // Nullable-safe: unset feature_gate_ (legacy unit tests / test
    // stubs / harnesses using the 7-arg AdminController constructor)
    // falls open to preserve backward compatibility. Message calls out
    // the license (not a role denial) so front-ends can render an
    // upgrade CTA instead of "contact an admin".
    if (feature_gate_ && !feature_gate_->isEnabled(Feature::AdvancedRouting)) {
        return AdminResult::error(
            ErrorCode::InsufficientPermissions,
            "Savings summary requires the AdvancedRouting feature "
            "(Enterprise edition license required)");
    }

    // SR1：多租户隔离。非 SuperAdmin 显式指定其它 tenant_id 时拒绝；
    // SuperAdmin 允许跨租户。
    if (!tenant_id.empty() && !auth::requireTenantAccess(ctx, tenant_id)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    const std::string effective_tenant = effectiveTenantId(ctx, tenant_id);

    auto now = std::chrono::system_clock::now();
    auto def_from = now - std::chrono::hours(24 * 7);  // 默认近 7 天
    auto from = parseIsoOrDefault(from_iso, def_from);
    auto to = parseIsoOrDefault(to_iso, now);

    // SR-NEW3：时间窗口 ≤ 365 天（防 DoS / 防意外全表扫）。
    if (to > from && (to - from) > std::chrono::hours(24 * 365)) {
        return AdminResult::error(
            ErrorCode::InvalidRequest,
            "time_range too large (max 365 days)");
    }

    nlohmann::json body;
    body["from"] = from_iso.empty() ? formatIso(from) : from_iso;
    body["to"] = to_iso.empty() ? formatIso(to) : to_iso;
    body["aggregator_since"] = nullptr;
    body["total_cost_saved"] = 0.0;
    body["total_cost_actual"] = 0.0;
    body["roi_percent"] = 0.0;
    body["total_tokens_saved"] = 0;
    body["total_cache_hits"] = 0;
    body["fallback_pricing_count"] = 0;
    body["by_type"] = nlohmann::json::array();
    body["by_model"] = nlohmann::json::array();
    body["time_series"] = nlohmann::json::array();
    body["top_tenants"] = nlohmann::json::array();
    body["routing_recommendations"] = nlohmann::json::array();

    if (!savings_aggregator_) {
        // 聚合器未注入（如纯单元测试 stub）→ 返回空骨架，UI 仍能渲染。
        return AdminResult::ok(body);
    }

    auto snap = savings_aggregator_->snapshot(effective_tenant, from, to);
    body["aggregator_since"] = formatIso(snap.since);
    body["total_cost_saved"] = snap.total.cost_saved;
    body["total_tokens_saved"] = snap.total.tokens_saved;
    body["fallback_pricing_count"] = snap.total.fallback_count;

    // 缓存命中事件数 = by_type[CacheHit].event_count
    auto it_cache = snap.by_type.find(static_cast<int>(SavingType::CacheHit));
    if (it_cache != snap.by_type.end()) {
        body["total_cache_hits"] = it_cache->second.event_count;
    }

    // total_cost_actual：从 cost_records 倒推（按 effective_tenant 过滤），
    // 用于计算 ROI（cost_saved / (cost_saved + cost_actual)）。
    double total_actual = 0.0;
    if (store_) {
        auto from_str = formatIso(from);
        auto to_str = formatIso(to);
        auto costs = store_->queryCostsByDateRange(effective_tenant, from_str, to_str);
        for (const auto& c : costs) {
            total_actual += c.total_cost;
        }
    }
    body["total_cost_actual"] = total_actual;
    double roi_denom = snap.total.cost_saved + total_actual;
    body["roi_percent"] = (roi_denom > 0.0)
        ? snap.total.cost_saved * 100.0 / roi_denom
        : 0.0;

    // by_type 数组化
    for (const auto& [type_int, stats] : snap.by_type) {
        body["by_type"].push_back({
            {"type", savingTypeName(static_cast<SavingType>(type_int))},
            {"cost_saved", stats.cost_saved},
            {"tokens_saved", stats.tokens_saved},
            {"event_count", stats.event_count},
        });
    }

    // by_model 数组化
    for (const auto& [model, stats] : snap.by_model) {
        body["by_model"].push_back({
            {"model", model},
            {"cost_saved", stats.cost_saved},
            {"tokens_saved", stats.tokens_saved},
            {"request_count", stats.event_count},
        });
    }

    // time_series 数组化（已按日期升序，map 排序保证）
    for (const auto& [day, stats] : snap.time_series_by_day) {
        body["time_series"].push_back({
            {"date", day},
            {"cost_saved", stats.cost_saved},
            {"tokens_saved", stats.tokens_saved},
            {"requests", stats.event_count},
        });
    }

    // top_tenants：仅 SuperAdmin（SR1：非 SuperAdmin 排行数组保持空）
    if (ctx.role == Role::SuperAdmin) {
        std::vector<std::pair<std::string, SavingsBucketStats>> sorted_tenants(
            snap.by_tenant.begin(), snap.by_tenant.end());
        std::sort(sorted_tenants.begin(), sorted_tenants.end(),
                  [](const auto& a, const auto& b) {
                      return a.second.cost_saved > b.second.cost_saved;
                  });
        const int n = std::min<int>(static_cast<int>(sorted_tenants.size()), 10);
        for (int i = 0; i < n; ++i) {
            body["top_tenants"].push_back({
                {"tenant_id", sorted_tenants[i].first},
                {"cost_saved", sorted_tenants[i].second.cost_saved},
                {"tokens_saved", sorted_tenants[i].second.tokens_saved},
                {"event_count", sorted_tenants[i].second.event_count},
            });
        }
    }

    // routing_recommendations：snapshot.by_model 中所有 "X->Y" 形式的潜在路由
    auto it_routing = snap.by_type.find(static_cast<int>(SavingType::Routing));
    if (it_routing != snap.by_type.end() && it_routing->second.event_count > 0) {
        for (const auto& [model_pair, stats] : snap.by_model) {
            if (model_pair.find("->") == std::string::npos) continue;
            body["routing_recommendations"].push_back({
                {"route", model_pair},
                {"potential_savings", stats.cost_saved},
                {"event_count", stats.event_count},
            });
        }
    }

    return AdminResult::ok(body);
    });
}

// TASK-20260602-01 Epic 5 — RBAC 用 admin::withRbac decorator 包裹（D3）。
AdminResult AdminController::getSecurityEvents(const AuthContext& ctx) {
    return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {

    // 注：MetricsRegistry counter 是全局累积值（自启动以来），
    // 当前没有 per-tenant label 维度，统一返回全局视角。
    // 多租户敏感度：仅 SuperAdmin 看到原始计数；TenantAdmin / Viewer
    // 看到归一化"严重程度"分级（避免暴露其它租户安全态势）。
    auto& m = MetricsRegistry::instance();

    nlohmann::json events;
    events["timestamp"] = nowTimestamp();
    events["scope"] = (ctx.role == Role::SuperAdmin) ? "global" : "tenant";

    const double guardrail = m.guardrailBlocksTotal().get();
    const double normalized = m.preprocessorNormalizedTotal().get();
    const double rate_limited = m.rateLimitedTotal().get();
    const double cache_hits = m.cacheHitsTotal().get();
    const double cache_queries = m.cacheQueriesTotal().get();

    if (ctx.role == Role::SuperAdmin) {
        events["guardrail_blocks_total"] = guardrail;
        events["preprocessor_normalized_total"] = normalized;
        events["rate_limited_total"] = rate_limited;
        events["cache_hits_total"] = cache_hits;
        events["cache_queries_total"] = cache_queries;
    } else {
        // 非 SuperAdmin：只暴露 0 / low / medium / high 严重等级
        auto severity = [](double v) {
            if (v <= 0.0) return "none";
            if (v < 10.0) return "low";
            if (v < 100.0) return "medium";
            return "high";
        };
        events["guardrail_blocks_severity"] = severity(guardrail);
        events["rate_limited_severity"] = severity(rate_limited);
    }

    return AdminResult::ok(events);
    });
}

// --- Compliance reports ---

// TASK-20260605-05 Epic E6：exportAuditReport 迁至 AdminGovernanceController；薄委托。
AdminResult AdminController::exportAuditReport(const AuthContext& ctx,
                                                 const std::string& from,
                                                 const std::string& to,
                                                 const std::string& tenant_id,
                                                 const std::string& format) {
    return governance_.exportAuditReport(ctx, from, to, tenant_id, format);
}

AdminResult AdminController::exportCostReport(const AuthContext& ctx,
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
        auto data = reporter.exportCostsJson(from, to, effective_tenant);
        nlohmann::json result;
        result["format"] = "json";
        result["data"] = nlohmann::json::parse(data);
        auditAction(ctx, "admin.export_cost_report", "format=json");
        return AdminResult::ok(result);
    }

    auto csv = reporter.exportCostsCsv(from, to, effective_tenant);
    nlohmann::json result;
    result["format"] = "csv";
    result["data"] = csv;
    auditAction(ctx, "admin.export_cost_report", "format=csv");
    return AdminResult::ok(result);
}

// --- SSO flow ---

nlohmann::json AdminController::ssoProviderToJson(const SsoProvider& p) {
    nlohmann::json j;
    j["id"] = p.id;
    j["tenant_id"] = p.tenant_id;
    j["name"] = p.name;
    j["issuer_url"] = p.issuer_url;
    j["client_id"] = p.client_id;
    j["has_client_secret"] = !p.client_secret_enc.empty();
    j["redirect_uri"] = p.redirect_uri;
    j["scopes"] = p.scopes;
    j["jit_provisioning"] = p.jit_provisioning;
    j["default_role"] = p.default_role;
    j["enabled"] = p.enabled;
    j["created_at"] = p.created_at;
    j["updated_at"] = p.updated_at;

    if (!p.claim_mapping_json.empty()) {
        auto parsed = nlohmann::json::parse(p.claim_mapping_json, nullptr, false);
        if (!parsed.is_discarded()) j["claim_mapping"] = parsed;
    }
    if (!p.group_role_mapping_json.empty()) {
        auto parsed = nlohmann::json::parse(p.group_role_mapping_json, nullptr, false);
        if (!parsed.is_discarded()) j["group_role_mapping"] = parsed;
    }
    return j;
}

SsoLoginResult AdminController::initiateSsoLogin(const std::string& tenant_id) {
    SsoLoginResult result;

    if (!oidc_client_) {
        result.error = "OIDC client not configured";
        return result;
    }

    auto provider = store_->getSsoProviderByTenant(tenant_id);
    if (!provider || !provider->enabled) {
        result.error = "No SSO provider configured for tenant";
        return result;
    }

    auto discovery = oidc_client_->discover(provider->issuer_url);
    if (!discovery) {
        result.error = "OIDC discovery failed";
        return result;
    }

    auto scopes = provider->scopes.empty()
        ? std::vector<std::string>{"openid", "profile", "email"}
        : provider->scopes;

    auto auth_result = oidc_client_->generateAuthUrl(
        discovery->authorization_endpoint,
        provider->client_id,
        provider->redirect_uri,
        scopes);

    result.redirect_url = auth_result.url;
    result.state = auth_result.state;
    result.nonce = auth_result.nonce;
    result.code_verifier = auth_result.code_verifier;
    return result;
}

SsoCallbackResult AdminController::handleSsoCallback(
    const std::string& code,
    const std::string& code_verifier,
    const std::string& nonce,
    const std::string& tenant_id,
    const std::string& ip_address,
    const std::string& user_agent) {

    SsoCallbackResult result;

    if (!oidc_client_ || !identity_mapper_ || !auth_service_) {
        result.error = "SSO services not configured";
        return result;
    }

    auto provider = store_->getSsoProviderByTenant(tenant_id);
    if (!provider) {
        result.error = "No SSO provider configured for tenant";
        return result;
    }

    auto discovery = oidc_client_->discover(provider->issuer_url);
    if (!discovery) {
        result.error = "OIDC discovery failed";
        return result;
    }

    std::string client_secret;
    if (!provider->client_secret_enc.empty()) {
        auto decrypted = Encryption::instance().decrypt(
            provider->client_secret_enc, "sso_client_secret");
        client_secret = decrypted.value_or(provider->client_secret_enc);
    }

    auto token_resp = oidc_client_->exchangeCode(
        discovery->token_endpoint,
        code, code_verifier,
        provider->client_id, client_secret,
        provider->redirect_uri);

    if (!token_resp) {
        result.error = "Token exchange failed";
        return result;
    }

    oidc_client_->fetchJwks(discovery->jwks_uri, provider->issuer_url);

    auto claims = oidc_client_->verifyIdToken(
        token_resp->id_token,
        provider->client_id,
        discovery->issuer);

    if (!claims) {
        result.error = "ID token verification failed";
        return result;
    }

    if (claims->contains("nonce") &&
        (*claims)["nonce"].get<std::string>() != nonce) {
        result.error = "Nonce mismatch";
        return result;
    }

    auto mapped = identity_mapper_->mapIdentity(*claims, *provider);
    if (!mapped) {
        result.error = "Identity mapping failed";
        return result;
    }

    auto session = auth_service_->sessionManager().createSession(
        mapped->user.id, provider->tenant_id,
        "sso", ip_address, user_agent);

    if (!session) {
        result.error = "Session creation failed";
        return result;
    }

    result.success = true;
    result.session = *session;
    return result;
}

std::string AdminController::handleSsoLogout(const std::string& session_id,
                                              const std::string& tenant_id) {
    if (auth_service_ && !session_id.empty()) {
        auth_service_->sessionManager().deleteSession(session_id);
    }

    if (!oidc_client_) return "";

    auto provider = store_->getSsoProviderByTenant(tenant_id);
    if (!provider) return "";

    auto discovery = oidc_client_->discover(provider->issuer_url);
    if (!discovery) return "";

    return discovery->end_session_endpoint;
}

// --- SSO Provider CRUD ---

AdminResult AdminController::createSsoProvider(const AuthContext& ctx,
                                                const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto tid = body.value("tenant_id", std::string{});
    auto issuer = body.value("issuer_url", std::string{});
    auto cid = body.value("client_id", std::string{});
    if (tid.empty() || issuer.empty() || cid.empty())
        return AdminResult::error(ErrorCode::MissingRequiredField,
            "Fields 'tenant_id', 'issuer_url', 'client_id' are required");

    SsoProvider p;
    p.id = generateId();
    p.tenant_id = tid;
    p.name = body.value("name", "");
    p.issuer_url = issuer;
    p.client_id = cid;

    auto secret = body.value("client_secret", std::string{});
    if (!secret.empty()) {
        if (Encryption::instance().isAvailable()) {
            p.client_secret_enc = Encryption::instance().encrypt(secret, "sso_client_secret");
        } else {
            p.client_secret_enc = secret;
        }
    }

    p.redirect_uri = body.value("redirect_uri", std::string{});
    if (body.contains("scopes") && body["scopes"].is_array())
        p.scopes = body["scopes"].get<std::vector<std::string>>();
    if (body.contains("claim_mapping"))
        p.claim_mapping_json = body["claim_mapping"].dump();
    if (body.contains("group_role_mapping"))
        p.group_role_mapping_json = body["group_role_mapping"].dump();
    p.jit_provisioning = body.value("jit_provisioning", true);
    p.default_role = body.value("default_role", "viewer");
    p.enabled = body.value("enabled", true);
    p.created_at = nowTimestamp();
    p.updated_at = p.created_at;

    if (!store_->insertSsoProvider(p))
        return AdminResult::error(ErrorCode::InternalError, "Failed to create SSO provider");

    auditAction(ctx, "admin.create_sso_provider", "provider_id=" + p.id);
    return AdminResult::ok(ssoProviderToJson(p), 201);
}

AdminResult AdminController::listSsoProviders(const AuthContext& ctx,
                                               int limit, int offset) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto providers = store_->listSsoProviders(limit, offset);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : providers) arr.push_back(ssoProviderToJson(p));
    return AdminResult::ok({{"data", arr},
                            {"count", arr.size()},
                            {"total", store_->ssoProviderCount()}});
}

AdminResult AdminController::getSsoProvider(const AuthContext& ctx,
                                             const std::string& id) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto p = store_->getSsoProvider(id);
    if (!p) return AdminResult::error(ErrorCode::InvalidRequest, "SSO provider not found");
    return AdminResult::ok(ssoProviderToJson(*p));
}

AdminResult AdminController::updateSsoProvider(const AuthContext& ctx,
                                                const std::string& id,
                                                const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    auto p = store_->getSsoProvider(id);
    if (!p) return AdminResult::error(ErrorCode::InvalidRequest, "SSO provider not found");

    if (body.contains("tenant_id")) p->tenant_id = body["tenant_id"].get<std::string>();
    if (body.contains("name")) p->name = body["name"].get<std::string>();
    if (body.contains("issuer_url")) p->issuer_url = body["issuer_url"].get<std::string>();
    if (body.contains("client_id")) p->client_id = body["client_id"].get<std::string>();
    if (body.contains("client_secret")) {
        auto secret = body["client_secret"].get<std::string>();
        if (Encryption::instance().isAvailable()) {
            p->client_secret_enc = Encryption::instance().encrypt(secret, "sso_client_secret");
        } else {
            p->client_secret_enc = secret;
        }
    }
    if (body.contains("redirect_uri")) p->redirect_uri = body["redirect_uri"].get<std::string>();
    if (body.contains("scopes") && body["scopes"].is_array())
        p->scopes = body["scopes"].get<std::vector<std::string>>();
    if (body.contains("claim_mapping"))
        p->claim_mapping_json = body["claim_mapping"].dump();
    if (body.contains("group_role_mapping"))
        p->group_role_mapping_json = body["group_role_mapping"].dump();
    if (body.contains("jit_provisioning")) p->jit_provisioning = body["jit_provisioning"].get<bool>();
    if (body.contains("default_role")) p->default_role = body["default_role"].get<std::string>();
    if (body.contains("enabled")) p->enabled = body["enabled"].get<bool>();
    p->updated_at = nowTimestamp();

    if (!store_->updateSsoProvider(*p))
        return AdminResult::error(ErrorCode::InternalError, "Failed to update SSO provider");

    auditAction(ctx, "admin.update_sso_provider", "provider_id=" + id);
    return AdminResult::ok(ssoProviderToJson(*p));
}

AdminResult AdminController::deleteSsoProvider(const AuthContext& ctx,
                                                const std::string& id) {
    if (!auth::requireRole(ctx, Role::SuperAdmin))
        return AdminResult::error(ErrorCode::InsufficientPermissions);

    if (!store_->deleteSsoProvider(id))
        return AdminResult::error(ErrorCode::InvalidRequest, "SSO provider not found");

    auditAction(ctx, "admin.delete_sso_provider", "provider_id=" + id);
    return AdminResult::ok({{"deleted", true}});
}

// --- Usage prediction ---

AdminResult AdminController::predictUsage(const AuthContext& ctx,
                                           const std::string& tenant_id,
                                           int history_days, int forecast_days) {
    auto eff_tid = effectiveTenantId(ctx, tenant_id);
    if (ctx.role != Role::SuperAdmin && ctx.role != Role::TenantAdmin) {
        return AdminResult::error(ErrorCode::InsufficientPermissions, "Insufficient permissions");
    }
    if (ctx.role == Role::TenantAdmin && eff_tid != ctx.tenant_id) {
        return AdminResult::error(ErrorCode::CrossTenantDenied, "Cannot predict for other tenants");
    }

    UsagePredictor predictor(store_);
    auto pred = predictor.predict(eff_tid, history_days, forecast_days);

    nlohmann::json body;
    body["daily_trend"] = pred.daily_trend;
    body["r_squared"] = pred.r_squared;
    body["historical"] = nlohmann::json::array();
    for (const auto& h : pred.historical) {
        body["historical"].push_back({
            {"date", h.date}, {"total_cost", h.total_cost},
            {"request_count", h.request_count}
        });
    }
    body["predicted"] = nlohmann::json::array();
    for (const auto& p : pred.predicted) {
        body["predicted"].push_back({
            {"date", p.date}, {"total_cost", p.total_cost}
        });
    }
    return AdminResult::ok(std::move(body));
}

AdminResult AdminController::predictBudgetExhaustion(
    const AuthContext& ctx, const std::string& tenant_id,
    double budget, int history_days) {
    auto eff_tid = effectiveTenantId(ctx, tenant_id);
    if (ctx.role != Role::SuperAdmin && ctx.role != Role::TenantAdmin) {
        return AdminResult::error(ErrorCode::InsufficientPermissions, "Insufficient permissions");
    }
    if (ctx.role == Role::TenantAdmin && eff_tid != ctx.tenant_id) {
        return AdminResult::error(ErrorCode::CrossTenantDenied, "Cannot predict for other tenants");
    }

    UsagePredictor predictor(store_);
    auto pred = predictor.predictBudgetExhaustion(eff_tid, budget, history_days);

    nlohmann::json body;
    body["budget"] = budget;
    body["budget_exhaustion_date"] = pred.budget_exhaustion_date;
    body["daily_trend"] = pred.daily_trend;
    body["r_squared"] = pred.r_squared;
    return AdminResult::ok(std::move(body));
}

// --- MFA management ---

AdminResult AdminController::setupMfa(const AuthContext& ctx) {
    auto existing = store_->getMfaSecret(ctx.user_id);
    if (existing && existing->enabled) {
        return AdminResult::error(ErrorCode::InvalidRequest, "MFA is already enabled");
    }

    auto secret = TotpService::generateSecret();

    std::string secret_enc = secret;
    if (Encryption::instance().isAvailable()) {
        secret_enc = Encryption::instance().encrypt(secret, "mfa_totp_secret");
    }

    auto recovery_codes =
        TotpService::generateRecoveryCodes(mfa_recovery_code_count_);

    std::vector<std::string> recovery_hashes;
    for (const auto& code : recovery_codes) {
        recovery_hashes.push_back(TotpService::hashRecoveryCode(code));
    }

    MfaSecret mfa;
    mfa.user_id = ctx.user_id;
    mfa.secret_enc = secret_enc;
    mfa.enabled = false;
    mfa.recovery_codes_hash = recovery_hashes;
    mfa.created_at = nowTimestamp();

    if (!store_->upsertMfaSecret(mfa)) {
        return AdminResult::error(ErrorCode::InternalError, "Failed to store MFA secret");
    }

    auto user = store_->getUser(ctx.user_id);
    std::string username = user ? user->username : ctx.user_id;

    auto qr_uri = TotpService::generateOtpAuthUri(secret, username, "AegisGate");

    nlohmann::json result;
    result["secret"] = secret;
    result["qr_uri"] = qr_uri;
    result["recovery_codes"] = recovery_codes;

    auditAction(ctx, "admin.mfa_setup", "user_id=" + ctx.user_id);
    return AdminResult::ok(result);
}

AdminResult AdminController::verifyMfa(const AuthContext& ctx, const nlohmann::json& body) {
    auto code = body.value("code", std::string{});
    if (code.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField, "Field 'code' is required");
    }

    auto mfa = store_->getMfaSecret(ctx.user_id);
    if (!mfa) {
        return AdminResult::error(ErrorCode::MfaNotSetup);
    }

    // P2-2（SR-2）：验证前先查锁定态，阻断在线爆破。
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (store_->getMfaLockedUntil(ctx.user_id) > now) {
        return AdminResult::error(ErrorCode::RateLimitExceeded,
                                  "Too many failed MFA attempts; try again later");
    }

    std::string secret = mfa->secret_enc;
    if (Encryption::instance().isAvailable()) {
        auto decrypted = Encryption::instance().decrypt(mfa->secret_enc, "mfa_totp_secret");
        if (decrypted) secret = *decrypted;
    }

    if (!TotpService::verifyCode(secret, code)) {
        store_->recordMfaFailure(ctx.user_id, mfa_lockout_max_failures_,
                                 mfa_lockout_window_sec_);
        return AdminResult::error(ErrorCode::MfaInvalidCode);
    }

    store_->clearMfaFailures(ctx.user_id);

    if (!mfa->enabled) {
        mfa->enabled = true;
        store_->upsertMfaSecret(*mfa);
    }

    if (!ctx.session_id.empty() && auth_service_) {
        auth_service_->sessionManager().setMfaVerified(ctx.session_id, true);
    }

    auditAction(ctx, "admin.mfa_verify", "user_id=" + ctx.user_id);
    return AdminResult::ok({{"verified", true}, {"mfa_enabled", true}});
}

AdminResult AdminController::disableMfa(const AuthContext& ctx, const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::TenantAdmin)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }

    auto code = body.value("code", std::string{});
    if (code.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField, "Field 'code' is required");
    }

    auto mfa = store_->getMfaSecret(ctx.user_id);
    if (!mfa || !mfa->enabled) {
        return AdminResult::error(ErrorCode::MfaNotSetup);
    }

    std::string secret = mfa->secret_enc;
    if (Encryption::instance().isAvailable()) {
        auto decrypted = Encryption::instance().decrypt(mfa->secret_enc, "mfa_totp_secret");
        if (decrypted) secret = *decrypted;
    }

    if (!TotpService::verifyCode(secret, code)) {
        return AdminResult::error(ErrorCode::MfaInvalidCode);
    }

    store_->deleteMfaSecret(ctx.user_id);

    auditAction(ctx, "admin.mfa_disable", "user_id=" + ctx.user_id);
    return AdminResult::ok({{"disabled", true}});
}

AdminResult AdminController::recoveryMfa(const AuthContext& ctx, const nlohmann::json& body) {
    auto code = body.value("code", std::string{});
    if (code.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField, "Field 'code' is required");
    }

    auto mfa = store_->getMfaSecret(ctx.user_id);
    if (!mfa || !mfa->enabled) {
        return AdminResult::error(ErrorCode::MfaNotSetup);
    }

    // P2-2（SR-2）：恢复码路径同样受失败锁定保护。
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (store_->getMfaLockedUntil(ctx.user_id) > now) {
        return AdminResult::error(ErrorCode::RateLimitExceeded,
                                  "Too many failed MFA attempts; try again later");
    }

    auto hashes = mfa->recovery_codes_hash;
    if (!TotpService::verifyRecoveryCode(hashes, code)) {
        store_->recordMfaFailure(ctx.user_id, mfa_lockout_max_failures_,
                                 mfa_lockout_window_sec_);
        return AdminResult::error(ErrorCode::MfaInvalidCode, "Invalid recovery code");
    }

    store_->clearMfaFailures(ctx.user_id);

    mfa->recovery_codes_hash = hashes;
    store_->upsertMfaSecret(*mfa);

    if (!ctx.session_id.empty() && auth_service_) {
        auth_service_->sessionManager().setMfaVerified(ctx.session_id, true);
    }

    auditAction(ctx, "admin.mfa_recovery", "user_id=" + ctx.user_id);
    return AdminResult::ok({{"verified", true}, {"remaining_codes", static_cast<int>(hashes.size())}});
}

// ============================================================================
// Phase 11.5 (TASK-20260518-02 E5.1) — Autonomy approval workflow endpoints
// ============================================================================
//
// All five handlers share these invariants (design spec §4.2 + plan §D 5.1):
//   1. RBAC gate: TenantAdmin minimum (auth::requireRole) — Viewer/Developer
//      see AEGIS-1002 (InsufficientPermissions).
//   2. Wiring gate: if autonomy_workflow_ OR autonomy_queue_ is null we
//      return AEGIS-6002 (AutonomyDisabled). This covers both "config
//      disabled" and "env kill switch tripped" cases without leaking the
//      reason — operators read the runtime log to disambiguate.
//   3. Audit trail: every handler calls auditAction() on the happy path so
//      Test #6 (all endpoints write audit) is one assertion against
//      audit_->entries().size().
//
// State filter / source filter for listAutonomyProposals are
// case-sensitive strings matching ApprovalState / AutonomySource
// toString().  Empty string == no filter.

namespace {

nlohmann::json proposalToJson(const autonomy::ApprovalProposal& p) {
    nlohmann::json out;
    out["id"] = p.id;
    out["source"] = autonomy::toString(p.source);
    out["subject"] = p.subject;
    out["state"] = autonomy::toString(p.state);
    out["proposer_user_id"] = p.proposer_user_id;
    out["proposed_at_ms"] = p.proposed_at_ms;
    out["reviewer_user_id"] = p.reviewer_user_id;
    out["reviewed_at_ms"] = p.reviewed_at_ms;
    out["reject_reason"] = p.reject_reason;
    out["payload"] = p.payload;
    out["decision_trace"] = p.decision_trace;
    out["payload_sha256"] = p.payload_sha256;
    return out;
}

} // namespace

AdminResult AdminController::listAutonomyProposals(
    const AuthContext& ctx,
    const std::string& state_filter,
    const std::string& source_filter,
    int limit, int offset) {
    if (!auth::requireRole(ctx, Role::TenantAdmin)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    if (!autonomy_workflow_ || !autonomy_queue_) {
        return AdminResult::error(ErrorCode::AutonomyDisabled);
    }

    std::optional<autonomy::ApprovalState> sf;
    if (!state_filter.empty()) {
        sf = autonomy::approvalStateFromString(state_filter);
        if (!sf.has_value()) {
            return AdminResult::error(ErrorCode::InvalidRequest,
                "Unknown state filter: " + state_filter);
        }
    }
    std::optional<autonomy::AutonomySource> srcf;
    if (!source_filter.empty()) {
        srcf = autonomy::autonomySourceFromString(source_filter);
        if (!srcf.has_value()) {
            return AdminResult::error(ErrorCode::InvalidRequest,
                "Unknown source filter: " + source_filter);
        }
    }

    if (limit <= 0)  limit = 100;
    if (offset < 0)  offset = 0;
    auto items = autonomy_workflow_->list(sf, srcf, limit, offset);

    nlohmann::json data = nlohmann::json::array();
    for (const auto& p : items) data.push_back(proposalToJson(p));

    nlohmann::json body;
    body["data"] = std::move(data);
    body["limit"] = limit;
    body["offset"] = offset;
    body["total"] = autonomy_workflow_->count(sf, srcf);

    auditAction(ctx, "autonomy.list",
                "state=" + state_filter +
                " source=" + source_filter +
                " count=" + std::to_string(items.size()));
    return AdminResult::ok(std::move(body));
}

AdminResult AdminController::approveAutonomyProposal(
    const AuthContext& ctx, const std::string& id) {
    if (!auth::requireRole(ctx, Role::TenantAdmin)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    if (!autonomy_workflow_ || !autonomy_queue_) {
        return AdminResult::error(ErrorCode::AutonomyDisabled);
    }
    if (id.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField,
            "Proposal id is required");
    }

    auto existing = autonomy_queue_->get(id);
    if (!existing.has_value()) {
        return AdminResult::error(ErrorCode::ApprovalNotFound);
    }
    if (existing->state != autonomy::ApprovalState::PROPOSED) {
        return AdminResult::error(ErrorCode::ApprovalStateInvalid,
            "Cannot approve from state " + autonomy::toString(existing->state));
    }

    const auto reviewer = ctx.user_id.empty() ? "admin" : ctx.user_id;
    const bool ok = autonomy_workflow_->approve(id, reviewer);
    if (!ok) {
        // Workflow rejected (race / state changed). Re-query for the
        // freshest reason.
        auto cur = autonomy_queue_->get(id);
        return AdminResult::error(ErrorCode::ApprovalStateInvalid,
            cur ? ("Approve refused, current state " +
                   autonomy::toString(cur->state))
                : "Approve refused");
    }

    auditAction(ctx, "autonomy.approve",
                "id=" + id + " reviewer=" + reviewer);

    auto after = autonomy_queue_->get(id);
    nlohmann::json body;
    body["id"] = id;
    body["state"] = after ? autonomy::toString(after->state) : "APPROVED";
    body["reviewer_user_id"] = reviewer;
    return AdminResult::ok(std::move(body));
}

AdminResult AdminController::rejectAutonomyProposal(
    const AuthContext& ctx, const std::string& id, const nlohmann::json& body) {
    if (!auth::requireRole(ctx, Role::TenantAdmin)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    if (!autonomy_workflow_ || !autonomy_queue_) {
        return AdminResult::error(ErrorCode::AutonomyDisabled);
    }
    if (id.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField,
            "Proposal id is required");
    }
    auto reason = body.value("reason", std::string{});
    if (reason.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField,
            "Field 'reason' is required for reject");
    }

    auto existing = autonomy_queue_->get(id);
    if (!existing.has_value()) {
        return AdminResult::error(ErrorCode::ApprovalNotFound);
    }
    if (existing->state != autonomy::ApprovalState::PROPOSED &&
        existing->state != autonomy::ApprovalState::APPROVED) {
        return AdminResult::error(ErrorCode::ApprovalStateInvalid,
            "Cannot reject from state " + autonomy::toString(existing->state));
    }

    const auto reviewer = ctx.user_id.empty() ? "admin" : ctx.user_id;
    const bool ok = autonomy_workflow_->reject(id, reviewer, reason);
    if (!ok) {
        return AdminResult::error(ErrorCode::ApprovalStateInvalid,
            "Reject refused");
    }

    auditAction(ctx, "autonomy.reject",
                "id=" + id + " reviewer=" + reviewer + " reason=" + reason);

    nlohmann::json out;
    out["id"] = id;
    out["state"] = "REJECTED";
    out["reviewer_user_id"] = reviewer;
    out["reject_reason"] = reason;
    return AdminResult::ok(std::move(out));
}

AdminResult AdminController::rollbackAutonomyProposal(
    const AuthContext& ctx, const std::string& id) {
    if (!auth::requireRole(ctx, Role::TenantAdmin)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    if (!autonomy_workflow_ || !autonomy_queue_) {
        return AdminResult::error(ErrorCode::AutonomyDisabled);
    }
    if (id.empty()) {
        return AdminResult::error(ErrorCode::MissingRequiredField,
            "Proposal id is required");
    }

    auto existing = autonomy_queue_->get(id);
    if (!existing.has_value()) {
        return AdminResult::error(ErrorCode::ApprovalNotFound);
    }
    if (existing->state != autonomy::ApprovalState::APPLIED) {
        return AdminResult::error(ErrorCode::ApprovalStateInvalid,
            "Rollback only allowed from APPLIED; current state " +
            autonomy::toString(existing->state));
    }

    const bool ok = autonomy_workflow_->rollback(id);
    if (!ok) {
        return AdminResult::error(ErrorCode::InternalError,
            "Rollback failed; see audit log");
    }

    auditAction(ctx, "autonomy.rollback",
                "id=" + id + " user=" + ctx.user_id);
    nlohmann::json out;
    out["id"] = id;
    out["state"] = "ROLLED_BACK";
    return AdminResult::ok(std::move(out));
}

AdminResult AdminController::autonomyReport(const AuthContext& ctx) {
    if (!auth::requireRole(ctx, Role::TenantAdmin)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    if (!autonomy_workflow_ || !autonomy_queue_) {
        return AdminResult::error(ErrorCode::AutonomyDisabled);
    }

    // Aggregate by source × state. We pull "all proposals, last 1000" so
    // the report is bounded for ops review; deeper analytics is the
    // domain of SavingsAggregator / dashboards.
    auto items = autonomy_workflow_->list(std::nullopt, std::nullopt, 1000, 0);

    std::unordered_map<std::string, std::unordered_map<std::string, int>>
        by_source;
    std::unordered_map<std::string, int> totals;
    double estimated_savings_24h_usd = 0.0;
    for (const auto& p : items) {
        const auto src = autonomy::toString(p.source);
        const auto st  = autonomy::toString(p.state);
        by_source[src][st]++;
        totals[st]++;
        if (p.state == autonomy::ApprovalState::APPLIED) {
            if (p.payload.contains("estimated_savings_usd_24h") &&
                p.payload["estimated_savings_usd_24h"].is_number()) {
                estimated_savings_24h_usd +=
                    p.payload["estimated_savings_usd_24h"].get<double>();
            }
        }
    }

    nlohmann::json by_source_json = nlohmann::json::object();
    for (const auto& [src, counts] : by_source) {
        nlohmann::json c = nlohmann::json::object();
        for (const auto& [st, n] : counts) c[st] = n;
        by_source_json[src] = c;
    }
    nlohmann::json totals_json = nlohmann::json::object();
    for (const auto& [st, n] : totals) totals_json[st] = n;

    nlohmann::json out;
    out["totals"] = std::move(totals_json);
    out["by_source"] = std::move(by_source_json);
    out["estimated_savings_24h_usd"] = estimated_savings_24h_usd;
    out["sample_size"] = static_cast<int>(items.size());

    auditAction(ctx, "autonomy.report",
                "sample=" + std::to_string(items.size()));
    return AdminResult::ok(std::move(out));
}

} // namespace aegisgate
