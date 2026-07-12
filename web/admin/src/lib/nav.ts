// TASK-20260605-04 — 后台导航分组数据结构 + 角色过滤纯函数。
// 设计：docs/specs/2026-06-05-admin-nav-consolidation-design.md §4/§5.1
// 创意：memory-bank/creative/creative-admin-nav-consolidation.md
//
// 把原 Layout.tsx 内联的扁平 NAV_ITEMS 重构为按业务域分组的 NAV_GROUPS，
// 并抽出 filterNavGroups 纯函数（item 级角色过滤 + 空组隐藏），便于隔离单测。
// ⚠️ SR-3：UI 守卫仅为体验层；后端 requireRole → 403 才是真防线。
import {
  LayoutDashboard, Building2, Users, KeyRound,
  ScrollText, DollarSign, PiggyBank, ShieldCheck, FileText, ShieldAlert, Lock, TrendingUp,
  Stethoscope,
  type LucideIcon,
} from 'lucide-react';
import { hasRole, type Role } from './roles';

// labelKey 为 nav namespace 内的 i18n key（渲染层 t(labelKey)），
// 数据层不再持有译文（TASK-20260614-03 i18n）。
export type NavItem = { to: string; icon: LucideIcon; labelKey: string; minRole?: Role };
export type NavGroup = { id: string; labelKey: string; items: NavItem[] };

export const NAV_GROUPS: NavGroup[] = [
  {
    id: 'overview',
    labelKey: 'groups.overview',
    items: [
      { to: '/', icon: LayoutDashboard, labelKey: 'items.dashboard' },
    ],
  },
  {
    id: 'access',
    labelKey: 'groups.access',
    items: [
      { to: '/tenants', icon: Building2, labelKey: 'items.tenants' },
      { to: '/users', icon: Users, labelKey: 'items.users' },
      { to: '/keys', icon: KeyRound, labelKey: 'items.keys' },
      { to: '/sso', icon: Lock, labelKey: 'items.sso', minRole: 'super_admin' },
    ],
  },
  {
    id: 'finance',
    labelKey: 'groups.finance',
    items: [
      { to: '/costs', icon: DollarSign, labelKey: 'items.costs' },
      { to: '/savings', icon: PiggyBank, labelKey: 'items.savings' },
      { to: '/forecast', icon: TrendingUp, labelKey: 'items.forecast', minRole: 'tenant_admin' },
      { to: '/finops', icon: ShieldCheck, labelKey: 'items.finops', minRole: 'tenant_admin' },
    ],
  },
  {
    id: 'governance',
    labelKey: 'groups.governance',
    items: [
      { to: '/rules', icon: ShieldAlert, labelKey: 'items.rules' },
      { to: '/guard', icon: Stethoscope, labelKey: 'items.guard', minRole: 'tenant_admin' },
      { to: '/templates', icon: FileText, labelKey: 'items.templates' },
      { to: '/audits', icon: ScrollText, labelKey: 'items.audits' },
    ],
  },
];

// 按角色过滤组内项（SR-3）；丢弃过滤后为空的分组（D4：不留空分组标题）。
export function filterNavGroups(groups: NavGroup[], role: string | undefined | null): NavGroup[] {
  return groups
    .map(g => ({ ...g, items: g.items.filter(it => !it.minRole || hasRole(role, it.minRole)) }))
    .filter(g => g.items.length > 0);
}
