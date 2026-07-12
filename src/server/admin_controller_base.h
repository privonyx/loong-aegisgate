#pragma once
#include "auth/auth_models.h"
#include "storage/persistent_store.h"
#include "guardrail/audit.h"
#include <aegisgate/error_codes.h>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace aegisgate {

// 业务层统一返回类型（HTTP 层 forwardAdminResult 据此封装响应）。
// TASK-20260605-05：从 admin_controller.h 上移至基座头，供各域子 controller
// 共用，打破 Facade ↔ 域 controller 的头文件循环依赖。
struct AdminResult {
    int status = 200;
    nlohmann::json body;
    ErrorCode error_code = ErrorCode::InternalError;
    bool is_error = false;

    static AdminResult ok(nlohmann::json body, int status = 200) {
        return {status, std::move(body), ErrorCode::InternalError, false};
    }
    static AdminResult error(ErrorCode code, const std::string& msg = "") {
        nlohmann::json err;
        err["error"]["code"] = toAegisCode(code);
        err["error"]["type"] = toErrorType(code);
        err["error"]["message"] = msg.empty() ? toDefaultMessage(code) : msg;
        return {toHttpStatus(code), std::move(err), code, true};
    }
};

// TASK-20260605-05 — admin Controller 拆分共享基座（creative D1=C / D2=C）。
//
// 各业务域子 controller（AdminIamController / AdminGovernanceController / ...）
// 以及过渡期 Facade `AdminController` 统一继承本基类，复用跨租户隔离核心
// (effectiveTenantId)、审计 (auditAction / auditCrossTenantAction) 与 ID/时间戳
// 工具。**逐字提取自原 admin_controller.cpp，行为零变化**（SR-1/3/4 共用
// effectiveTenantId，必须单点共享避免拆分后各域复制走样）。
class AdminControllerBase {
protected:
    AdminControllerBase(PersistentStore* store, AuditLogger* audit)
        : store_(store), audit_(audit) {}

    // 跨租户隔离核心（SR-1/3/4 共用）：SuperAdmin 可指定任意 tenant，
    // 其它角色强制为本租户。
    std::string effectiveTenantId(const AuthContext& ctx,
                                  const std::string& requested) const;

    // 审计（同租户操作 / 跨租户操作）。audit_ 为 null 时静默跳过。
    void auditAction(const AuthContext& ctx, const std::string& action,
                     const std::string& detail);
    void auditCrossTenantAction(const AuthContext& ctx,
                                const std::string& target_tenant_id,
                                const std::string& action,
                                const std::string& detail);

    static std::string nowTimestamp();
    static std::string generateId();

    // 共享依赖（非拥有指针 / 由派生类 ctor 注入）。
    PersistentStore* store_ = nullptr;
    AuditLogger* audit_ = nullptr;
};

} // namespace aegisgate
