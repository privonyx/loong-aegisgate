#include "auth/authorization.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using namespace aegisgate::auth;

namespace {
AuthContext makeCtx(Role role, const std::string& tenant, bool rbac = true) {
    AuthContext ctx;
    ctx.role = role;
    ctx.tenant_id = tenant;
    ctx.is_rbac_enabled = rbac;
    return ctx;
}
}

// --- requireRole ---

TEST(RequireRoleTest, ViewerCanOnlyViewerLevel) {
    auto ctx = makeCtx(Role::Viewer, "t1");
    EXPECT_TRUE(requireRole(ctx, Role::Viewer));
    EXPECT_FALSE(requireRole(ctx, Role::Developer));
    EXPECT_FALSE(requireRole(ctx, Role::TenantAdmin));
    EXPECT_FALSE(requireRole(ctx, Role::SuperAdmin));
}

TEST(RequireRoleTest, DeveloperCanDeveloperAndBelow) {
    auto ctx = makeCtx(Role::Developer, "t1");
    EXPECT_TRUE(requireRole(ctx, Role::Viewer));
    EXPECT_TRUE(requireRole(ctx, Role::Developer));
    EXPECT_FALSE(requireRole(ctx, Role::TenantAdmin));
}

TEST(RequireRoleTest, TenantAdminCanTenantAdminAndBelow) {
    auto ctx = makeCtx(Role::TenantAdmin, "t1");
    EXPECT_TRUE(requireRole(ctx, Role::Viewer));
    EXPECT_TRUE(requireRole(ctx, Role::Developer));
    EXPECT_TRUE(requireRole(ctx, Role::TenantAdmin));
    EXPECT_FALSE(requireRole(ctx, Role::SuperAdmin));
}

TEST(RequireRoleTest, SuperAdminCanAll) {
    auto ctx = makeCtx(Role::SuperAdmin, "t1");
    EXPECT_TRUE(requireRole(ctx, Role::Viewer));
    EXPECT_TRUE(requireRole(ctx, Role::Developer));
    EXPECT_TRUE(requireRole(ctx, Role::TenantAdmin));
    EXPECT_TRUE(requireRole(ctx, Role::SuperAdmin));
}

TEST(RequireRoleTest, RbacDisabledAlwaysTrue) {
    auto ctx = makeCtx(Role::Viewer, "t1", false);
    EXPECT_TRUE(requireRole(ctx, Role::SuperAdmin));
}

// --- requireTenantAccess ---

TEST(RequireTenantAccessTest, SameTenantAllowed) {
    auto ctx = makeCtx(Role::Developer, "t1");
    EXPECT_TRUE(requireTenantAccess(ctx, "t1"));
}

TEST(RequireTenantAccessTest, CrossTenantDenied) {
    auto ctx = makeCtx(Role::TenantAdmin, "t1");
    EXPECT_FALSE(requireTenantAccess(ctx, "t2"));
}

TEST(RequireTenantAccessTest, SuperAdminCrossTenantAllowed) {
    auto ctx = makeCtx(Role::SuperAdmin, "t1");
    EXPECT_TRUE(requireTenantAccess(ctx, "t2"));
}

TEST(RequireTenantAccessTest, RbacDisabledAlwaysTrue) {
    auto ctx = makeCtx(Role::Viewer, "t1", false);
    EXPECT_TRUE(requireTenantAccess(ctx, "t2"));
}

// --- authorize (combo) ---

TEST(AuthorizeTest, TenantAdminSameTenantOk) {
    auto ctx = makeCtx(Role::TenantAdmin, "t1");
    EXPECT_TRUE(authorize(ctx, Role::TenantAdmin, "t1"));
}

TEST(AuthorizeTest, TenantAdminCrossTenantDenied) {
    auto ctx = makeCtx(Role::TenantAdmin, "t1");
    EXPECT_FALSE(authorize(ctx, Role::TenantAdmin, "t2"));
}

TEST(AuthorizeTest, DeveloperNeedsTenantAdminDenied) {
    auto ctx = makeCtx(Role::Developer, "t1");
    EXPECT_FALSE(authorize(ctx, Role::TenantAdmin, "t1"));
}

TEST(AuthorizeTest, SuperAdminCrossTenantAllowed) {
    auto ctx = makeCtx(Role::SuperAdmin, "t1");
    EXPECT_TRUE(authorize(ctx, Role::SuperAdmin, "t2"));
}

// --- canGrantRole (SR-1 vertical privilege escalation) ---

TEST(CanGrantRoleTest, ViewerMayOnlyGrantViewer) {
    auto ctx = makeCtx(Role::Viewer, "t1");
    EXPECT_TRUE(canGrantRole(ctx, Role::Viewer));
    EXPECT_FALSE(canGrantRole(ctx, Role::Developer));
    EXPECT_FALSE(canGrantRole(ctx, Role::TenantAdmin));
    EXPECT_FALSE(canGrantRole(ctx, Role::SuperAdmin));
}

TEST(CanGrantRoleTest, DeveloperMayGrantUpToDeveloper) {
    auto ctx = makeCtx(Role::Developer, "t1");
    EXPECT_TRUE(canGrantRole(ctx, Role::Viewer));
    EXPECT_TRUE(canGrantRole(ctx, Role::Developer));
    EXPECT_FALSE(canGrantRole(ctx, Role::TenantAdmin));
    EXPECT_FALSE(canGrantRole(ctx, Role::SuperAdmin));
}

TEST(CanGrantRoleTest, TenantAdminMayNotGrantSuperAdmin) {
    auto ctx = makeCtx(Role::TenantAdmin, "t1");
    EXPECT_TRUE(canGrantRole(ctx, Role::Viewer));
    EXPECT_TRUE(canGrantRole(ctx, Role::Developer));
    EXPECT_TRUE(canGrantRole(ctx, Role::TenantAdmin));
    EXPECT_FALSE(canGrantRole(ctx, Role::SuperAdmin));
}

TEST(CanGrantRoleTest, SuperAdminMayGrantAnyRole) {
    auto ctx = makeCtx(Role::SuperAdmin, "t1");
    EXPECT_TRUE(canGrantRole(ctx, Role::Viewer));
    EXPECT_TRUE(canGrantRole(ctx, Role::Developer));
    EXPECT_TRUE(canGrantRole(ctx, Role::TenantAdmin));
    EXPECT_TRUE(canGrantRole(ctx, Role::SuperAdmin));
}

TEST(CanGrantRoleTest, RbacDisabledAllowsAnyGrant) {
    auto ctx = makeCtx(Role::Viewer, "t1", /*rbac=*/false);
    EXPECT_TRUE(canGrantRole(ctx, Role::SuperAdmin));
    EXPECT_TRUE(canGrantRole(ctx, Role::TenantAdmin));
}
