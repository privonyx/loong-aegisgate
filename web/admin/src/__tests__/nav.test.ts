// TASK-20260605-04 Epic A — 分组导航数据结构 + 角色过滤纯函数。
// 覆盖:
//   1. super_admin 看到 4 组全部 12 项
//   2. SR-3-A：viewer 看不到 SSO/预测/FinOps（角色门槛项）
//   3. SR-3-B（D4）：组内项被角色过滤全空 → 整组被丢弃（空组隐藏）
//   4. NAV_GROUPS 覆盖既有 12 侧栏路由（防漏配/重复）
import { describe, it, expect } from 'vitest';
import { NAV_GROUPS, filterNavGroups, type NavGroup } from '../lib/nav';

const flatten = (groups: NavGroup[]) => groups.flatMap(g => g.items.map(it => it.to));

describe('nav 分组数据结构', () => {
  it('NAV_GROUPS 为 4 组（overview / access / finance / governance）', () => {
    expect(NAV_GROUPS.map(g => g.labelKey)).toEqual([
      'groups.overview', 'groups.access', 'groups.finance', 'groups.governance',
    ]);
  });

  it('覆盖既有 13 侧栏路由 / 无重复', () => {
    const routes = flatten(NAV_GROUPS).sort();
    expect(routes).toEqual([
      '/', '/audits', '/costs', '/finops', '/forecast', '/guard', '/keys',
      '/rules', '/savings', '/sso', '/templates', '/tenants', '/users',
    ]);
    // 无重复
    expect(new Set(routes).size).toBe(routes.length);
  });
});

describe('filterNavGroups（SR-3 角色过滤）', () => {
  it('super_admin 看到 4 组全部 13 项', () => {
    const groups = filterNavGroups(NAV_GROUPS, 'super_admin');
    expect(groups).toHaveLength(4);
    expect(flatten(groups)).toHaveLength(13);
  });

  it('SR-3-A：viewer 看不到 SSO/预测/FinOps/护栏诊断', () => {
    const routes = flatten(filterNavGroups(NAV_GROUPS, 'viewer'));
    expect(routes).not.toContain('/sso');
    expect(routes).not.toContain('/forecast');
    expect(routes).not.toContain('/finops');
    expect(routes).not.toContain('/guard');
    // 但无门槛项仍在
    expect(routes).toContain('/');
    expect(routes).toContain('/costs');
    expect(routes).toContain('/tenants');
  });

  it('SR-2：tenant_admin 看到护栏诊断 /guard', () => {
    const routes = flatten(filterNavGroups(NAV_GROUPS, 'tenant_admin'));
    expect(routes).toContain('/guard');
  });

  it('SR-2：developer 看不到护栏诊断 /guard', () => {
    const routes = flatten(filterNavGroups(NAV_GROUPS, 'developer'));
    expect(routes).not.toContain('/guard');
  });

  it('SR-3-A：tenant_admin 看到预测+FinOps 但看不到 SSO', () => {
    const routes = flatten(filterNavGroups(NAV_GROUPS, 'tenant_admin'));
    expect(routes).toContain('/forecast');
    expect(routes).toContain('/finops');
    expect(routes).not.toContain('/sso');
  });

  it('user role 为 null/undefined 时仅保留无门槛项', () => {
    const routes = flatten(filterNavGroups(NAV_GROUPS, null));
    expect(routes).not.toContain('/sso');
    expect(routes).not.toContain('/finops');
    expect(routes).toContain('/');
  });

  it('SR-3-B（D4）：组内项被角色过滤全空 → 整组丢弃（空组隐藏）', () => {
    const synthetic: NavGroup[] = [
      { id: 'overview', labelKey: 'groups.overview', items: [{ to: '/', icon: NAV_GROUPS[0].items[0].icon, labelKey: 'items.dashboard' }] },
      { id: 'super-only', labelKey: 'groups.superOnly', items: [
        { to: '/x', icon: NAV_GROUPS[0].items[0].icon, labelKey: 'items.x', minRole: 'super_admin' },
      ] },
    ];
    const groups = filterNavGroups(synthetic, 'viewer');
    expect(groups.map(g => g.id)).toEqual(['overview']);
    expect(groups.find(g => g.id === 'super-only')).toBeUndefined();
  });
});
