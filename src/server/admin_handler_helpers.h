#pragma once
#include "auth/auth_models.h"
#include "auth/authorization.h"
#include "server/admin_controller.h"   // AdminResult
#include <aegisgate/error_codes.h>

namespace aegisgate::admin {

// TASK-20260602-01 Epic 5 — RBAC handler decorator (spec §2 D3).
//
// 集中 admin handler 顶部样板 `if (!requireRole) return InsufficientPermissions;`
// 减少漏检风险 + 提升一致性。本任务局部应用到 ≥ 6 高频 read-only handler 作为
// 模式验证；全量替换 ~40 处 handler 留作 v2 (TASK-W-ControllerSplit) 一并完成。
//
// 使用：
//   AdminResult AdminController::caseStudyHeadline(const AuthContext& ctx) {
//       return admin::withRbac(ctx, Role::Viewer, [&]() -> AdminResult {
//           ... handler body ...
//           return AdminResult::ok(body);
//       });
//   }
//
// 注：lambda 必须显式声明返回 AdminResult；某些 handler 内部分支会构造 error
// 结果，编译器对 auto deduction 不够稳定。保持显式签名以避免奇怪错误。
template <typename Fn>
AdminResult withRbac(const AuthContext& ctx, Role min_role, Fn&& fn) {
    if (!auth::requireRole(ctx, min_role)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    return fn();
}

} // namespace aegisgate::admin
