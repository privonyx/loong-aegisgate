// TASK-20260602-01 Epic 6 — Layout sidebar + topbar unit tests.
// TASK-20260603-01 Epic 6 — 扩展：nav role 过滤（SR-3）+ 账户安全入口。
//
// 覆盖:
//   1. 渲染基础 nav 链接（仪表盘 / 租户 / 用户 / 密钥 / 审计 / 成本 / 省钱 / FinOps 审批）
//      + 新增 Feature Gap 链接（模板 / 护栏规则）
//   2. 用户已登录时显示 user_id + role
//   3. 退出按钮 click → 调用 useAuth.logout
//   4. SR-3：SSO 配置仅 super_admin 可见；预测需 tenant_admin+
//   5. 顶栏「账户安全」入口指向 /account/security

import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { describe, it, expect, vi, afterEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo } from '../types';
import Layout from '../components/Layout';
import i18n from '../i18n';

function renderLayout(user: UserInfo | null, logoutMock = vi.fn().mockResolvedValue(undefined)) {
  const auth = {
    user,
    loading: false,
    error: null as string | null,
    login: vi.fn().mockResolvedValue(undefined),
    logout: logoutMock,
    refresh: vi.fn().mockResolvedValue(undefined),
  };
  return render(
    <AuthContext.Provider value={auth}>
      <MemoryRouter initialEntries={['/']}>
        <Layout>
          <div data-testid="page-body">Hello</div>
        </Layout>
      </MemoryRouter>
    </AuthContext.Provider>,
  );
}

describe('Layout', () => {
  afterEach(() => {
    i18n.changeLanguage('zh-CN');
  });

  // TASK-20260614-03 — 语言切换：选 en-US → nav 文案变英文。
  it('语言切换器切到 English 后 nav 文案变英文', async () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'super_admin' });
    expect(screen.getByText('仪表盘')).toBeInTheDocument();
    const switcher = screen.getByTestId('lang-switcher');
    fireEvent.change(switcher, { target: { value: 'en-US' } });
    await waitFor(() => expect(screen.getByText('Dashboard')).toBeInTheDocument());
    expect(screen.getByText('Overview')).toBeInTheDocument();
  });

  it('TASK-20260605-04：渲染 4 个分类标题（概览/租户与访问/成本与财务/策略与治理）', () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'super_admin' });
    expect(screen.getByText('概览')).toBeInTheDocument();
    expect(screen.getByText('租户与访问')).toBeInTheDocument();
    expect(screen.getByText('成本与财务')).toBeInTheDocument();
    expect(screen.getByText('策略与治理')).toBeInTheDocument();
  });

  it('渲染基础 + Feature Gap nav 链接', () => {
    const user: UserInfo = { user_id: 'alice', tenant_id: 't1', role: 'tenant_admin' };
    renderLayout(user);
    // 与 NAV_ITEMS 表对齐
    expect(screen.getByText('仪表盘')).toBeInTheDocument();
    expect(screen.getByText('租户')).toBeInTheDocument();
    expect(screen.getByText('用户')).toBeInTheDocument();
    expect(screen.getByText('密钥')).toBeInTheDocument();
    expect(screen.getByText('审计')).toBeInTheDocument();
    expect(screen.getByText('成本')).toBeInTheDocument();
    expect(screen.getByText('省钱')).toBeInTheDocument();
    expect(screen.getByText('FinOps 审批')).toBeInTheDocument();
    expect(screen.getByText('模板')).toBeInTheDocument();
    expect(screen.getByText('护栏规则')).toBeInTheDocument();
  });

  it('SR-3：SSO 配置仅 super_admin 可见', () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'tenant_admin' });
    expect(screen.queryByText('SSO 配置')).not.toBeInTheDocument();
  });

  it('SR-3：super_admin 可见 SSO 配置 + 预测', () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'super_admin' });
    expect(screen.getByText('SSO 配置')).toBeInTheDocument();
    expect(screen.getByText('预测')).toBeInTheDocument();
  });

  it('SR-3：viewer 看不到预测（需 tenant_admin+）', () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'viewer' });
    expect(screen.queryByText('预测')).not.toBeInTheDocument();
  });

  it('SR-1：viewer 看不到 FinOps 审批（需 tenant_admin+ / 与后端 RBAC 对齐）', () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'viewer' });
    expect(screen.queryByText('FinOps 审批')).not.toBeInTheDocument();
  });

  it('顶栏「账户安全」入口指向 /account/security', () => {
    renderLayout({ user_id: 'a', tenant_id: 't1', role: 'viewer' });
    const link = screen.getByTitle('账户安全 (MFA)');
    expect(link.getAttribute('href')).toContain('/account/security');
  });

  it('显示用户 user_id + role', () => {
    const user: UserInfo = { user_id: 'alice', tenant_id: 't1', role: 'super_admin' };
    renderLayout(user);
    expect(screen.getByText(/alice/)).toBeInTheDocument();
    expect(screen.getByText(/super_admin/)).toBeInTheDocument();
  });

  it('退出按钮 click 调用 useAuth.logout', async () => {
    const logoutMock = vi.fn().mockResolvedValue(undefined);
    const user: UserInfo = { user_id: 'alice', tenant_id: 't1', role: 'viewer' };
    renderLayout(user, logoutMock);
    const logoutBtn = screen.getByTitle('退出');
    fireEvent.click(logoutBtn);
    await waitFor(() => expect(logoutMock).toHaveBeenCalledTimes(1));
  });

  it('子内容（children）渲染在 main 区域', () => {
    const user: UserInfo = { user_id: 'alice', tenant_id: 't1', role: 'viewer' };
    renderLayout(user);
    expect(screen.getByTestId('page-body')).toBeInTheDocument();
    expect(screen.getByText('Hello')).toBeInTheDocument();
  });
});
