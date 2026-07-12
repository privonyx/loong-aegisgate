// TASK-20260603-01 Epic 6 — RoleGuard（SR-3 UI 守卫）测试。
import { render, screen } from '@testing-library/react';
import { describe, it, expect, vi } from 'vitest';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo } from '../types';
import RoleGuard from '../components/RoleGuard';

function renderGuard(role: string | null, required: 'tenant_admin' | 'super_admin') {
  const user: UserInfo | null = role ? { user_id: 'u1', tenant_id: 't1', role } : null;
  const auth = { user, loading: false, error: null, login: vi.fn(), logout: vi.fn(), refresh: vi.fn() };
  return render(
    <AuthContext.Provider value={auth}>
      <RoleGuard role={required}>
        <div data-testid="protected">机密内容</div>
      </RoleGuard>
    </AuthContext.Provider>,
  );
}

describe('RoleGuard (SR-3)', () => {
  it('super_admin 访问 super_admin 页 → 渲染内容', () => {
    renderGuard('super_admin', 'super_admin');
    expect(screen.getByTestId('protected')).toBeInTheDocument();
  });

  it('tenant_admin 访问 super_admin 页 → 拦截显示无权限', () => {
    renderGuard('tenant_admin', 'super_admin');
    expect(screen.queryByTestId('protected')).not.toBeInTheDocument();
    expect(screen.getByTestId('role-denied')).toBeInTheDocument();
  });

  it('viewer 访问 tenant_admin 页 → 拦截', () => {
    renderGuard('viewer', 'tenant_admin');
    expect(screen.queryByTestId('protected')).not.toBeInTheDocument();
    expect(screen.getByTestId('role-denied')).toBeInTheDocument();
  });

  it('tenant_admin 访问 tenant_admin 页 → 渲染内容', () => {
    renderGuard('tenant_admin', 'tenant_admin');
    expect(screen.getByTestId('protected')).toBeInTheDocument();
  });

  it('未登录（user=null）→ 拦截', () => {
    renderGuard(null, 'tenant_admin');
    expect(screen.getByTestId('role-denied')).toBeInTheDocument();
  });
});
