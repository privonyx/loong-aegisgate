// TASK-20260603-01 Epic 6 — 前端角色等级工具（SR-3 UI 守卫用）。
// ⚠️ UI 守卫仅为体验层；后端 requireRole / 403 才是真防线（纵深防御）。
// 角色字符串与后端 roleToString 对齐：viewer / developer / tenant_admin / super_admin。

export type Role = 'viewer' | 'developer' | 'tenant_admin' | 'super_admin';

export const ROLE_RANK: Record<Role, number> = {
  viewer: 0,
  developer: 1,
  tenant_admin: 2,
  super_admin: 3,
};

export function roleRank(role: string | undefined | null): number {
  if (!role) return -1;
  return ROLE_RANK[role as Role] ?? -1;
}

// userRole 是否满足 minRole 门槛。
export function hasRole(userRole: string | undefined | null, minRole: Role): boolean {
  return roleRank(userRole) >= ROLE_RANK[minRole];
}
