// TASK-20260605-01 Epic C — FinOps 路由 RoleGuard 守卫（SR-1）。
// 验证前端守卫角色与后端 autonomy requireRole(TenantAdmin) 对齐：
// viewer 访问 /finops 被 role-denied 拦截（FinOps 页因此不挂载、不发 autonomy 请求）。
import { render, screen } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach } from 'vitest';

vi.mock('../api/client', () => ({
  api: {
    me: vi.fn(),
    login: vi.fn(),
    logout: vi.fn(),
  },
}));

import { api } from '../api/client';
import App from '../App';

describe('FinOps 路由 RoleGuard (SR-1)', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    window.history.pushState({}, '', '/admin/finops');
  });

  it('viewer 访问 /finops → role-denied（需 tenant_admin+）', async () => {
    vi.mocked(api.me).mockResolvedValue({ user_id: 'v', tenant_id: 't1', role: 'viewer' });
    render(<App />);
    expect(await screen.findByTestId('role-denied')).toBeInTheDocument();
  });
});
