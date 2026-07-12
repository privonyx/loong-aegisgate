// TASK-20260603-01 Epic 3 — SSO Provider CRUD 页 + SR-1（secret 不回显）测试。
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ToastProvider } from '../components/Toast';
import type { SsoProvider } from '../types';

vi.mock('../api/client', () => ({
  api: {
    listSsoProviders: vi.fn(),
    createSsoProvider: vi.fn(),
    updateSsoProvider: vi.fn(),
    deleteSsoProvider: vi.fn(),
  },
}));

import { api } from '../api/client';
import Sso from '../pages/Sso';

const p1: SsoProvider = {
  id: 'p1', tenant_id: 'tn-1', name: 'Okta', issuer_url: 'https://okta.example.com',
  client_id: 'cid-123', has_client_secret: true, redirect_uri: 'https://app/callback',
  scopes: ['openid', 'email'], jit_provisioning: true, default_role: 'viewer',
  enabled: true, created_at: '2026-06-01T00:00:00Z', updated_at: '2026-06-02T00:00:00Z',
};

function renderPage() {
  return render(<ToastProvider><Sso /></ToastProvider>);
}

describe('SSO Provider 页', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(api.listSsoProviders).mockResolvedValue({ data: [p1], count: 1, total: 1 });
    vi.mocked(api.createSsoProvider).mockResolvedValue(p1);
    vi.mocked(api.updateSsoProvider).mockResolvedValue(p1);
    vi.mocked(api.deleteSsoProvider).mockResolvedValue({ deleted: true });
  });

  it('渲染 provider 列表 + 密钥状态徽章（不显示明文 secret）', async () => {
    renderPage();
    expect(await screen.findByText('Okta')).toBeInTheDocument();
    expect(screen.getByText('已配置')).toBeInTheDocument();
  });

  // TASK-20260605-02 P1：首屏按 PAGE_SIZE 真分页拉取（limit=50, offset=0）。
  it('初次加载以 limit=50/offset=0 真分页拉取', async () => {
    renderPage();
    await screen.findByText('Okta');
    expect(api.listSsoProviders).toHaveBeenCalledWith(50, 0);
  });

  // TASK-20260605-02 P1：total > PAGE_SIZE 时翻页，下一页用 offset=50 再拉取。
  it('翻页时以 offset=50 重新拉取（total 驱动分页）', async () => {
    const user = userEvent.setup();
    vi.mocked(api.listSsoProviders).mockResolvedValue({ data: [p1], count: 1, total: 120 });
    renderPage();
    await screen.findByText('Okta');
    const pageIndicator = screen.getByText('1 / 3');
    const nextBtn = pageIndicator.nextElementSibling as HTMLElement;
    await user.click(nextBtn);
    await waitFor(() => expect(api.listSsoProviders).toHaveBeenCalledWith(50, 50));
  });

  it('新建 provider：填写 secret 提交时带 client_secret', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('Okta');
    await user.click(screen.getByRole('button', { name: /新建 SSO/ }));
    await user.type(screen.getByLabelText('名称'), 'Auth0');
    await user.type(screen.getByLabelText('Issuer URL'), 'https://auth0.example.com');
    await user.type(screen.getByLabelText('客户端密钥'), 'super-secret');
    await user.click(screen.getByTestId('submit-sso'));
    await waitFor(() => expect(api.createSsoProvider).toHaveBeenCalled());
    const body = vi.mocked(api.createSsoProvider).mock.calls[0][0];
    expect(body.client_secret).toBe('super-secret');
  });

  // SR-1：编辑态 secret 输入框初值必须为空 + placeholder 留空提示；不回显明文 secret。
  it('SR-1：编辑态 client_secret 输入框为空且 placeholder 含「留空」', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('Okta');
    await user.click(screen.getByTitle('编辑'));
    const secretInput = screen.getByLabelText('客户端密钥') as HTMLInputElement;
    expect(secretInput.value).toBe('');
    expect(secretInput.getAttribute('placeholder') ?? '').toMatch(/留空/);
  });

  // SR-1：编辑时未填 secret → 提交体不含 client_secret（保持不变）。
  it('SR-1：编辑未改 secret 时提交体不含 client_secret', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('Okta');
    await user.click(screen.getByTitle('编辑'));
    await user.click(screen.getByTestId('submit-sso'));
    await waitFor(() => expect(api.updateSsoProvider).toHaveBeenCalled());
    const body = vi.mocked(api.updateSsoProvider).mock.calls[0][1];
    expect(body.client_secret).toBeUndefined();
  });
});
