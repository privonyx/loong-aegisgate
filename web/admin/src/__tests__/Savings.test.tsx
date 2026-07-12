import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { SavingsSummary, UserInfo } from '../types';
import * as clientModule from '../api/client';
import Savings from '../pages/Savings';

const fixture: SavingsSummary = {
  from: '2026-05-03T00:00:00Z',
  to: '2026-05-10T00:00:00Z',
  aggregator_since: '2026-05-01T08:00:00Z',
  total_cost_saved: 12.34,
  total_cost_actual: 100.0,
  roi_percent: 11.0,
  total_tokens_saved: 12345,
  total_cache_hits: 50,
  fallback_pricing_count: 2,
  by_type: [
    { type: 'cache_hit', cost_saved: 8.0, tokens_saved: 8000, event_count: 50 },
    { type: 'compression', cost_saved: 4.34, tokens_saved: 4345, event_count: 100 },
  ],
  by_model: [
    { model: 'gpt-4', cost_saved: 10.0, tokens_saved: 10000, request_count: 30 },
    { model: 'gpt-3.5', cost_saved: 2.34, tokens_saved: 2345, request_count: 120 },
  ],
  time_series: [
    { date: '2026-05-09', cost_saved: 5.0, tokens_saved: 5000, requests: 25 },
    { date: '2026-05-10', cost_saved: 7.34, tokens_saved: 7345, requests: 25 },
  ],
  top_tenants: [
    { tenant_id: 'tenant-A', cost_saved: 12.0, tokens_saved: 12000, event_count: 140 },
    { tenant_id: 'tenant-B', cost_saved: 0.34, tokens_saved: 345, event_count: 10 },
  ],
  routing_recommendations: [
    { route: 'gpt-4->gpt-3.5', potential_savings: 0.5, event_count: 3 },
  ],
};

function renderSavings(role: string = 'TenantAdmin', auth_overrides = {}) {
  const user: UserInfo = { user_id: 'u1', tenant_id: 'tenant-A', role };
  const auth = {
    user,
    loading: false,
    error: null,
    login: vi.fn(),
    logout: vi.fn(),
    refresh: vi.fn(),
    ...auth_overrides,
  };
  return render(
    <AuthContext.Provider value={auth}>
      <MemoryRouter initialEntries={['/savings']}>
        <Savings />
      </MemoryRouter>
    </AuthContext.Provider>,
  );
}

describe('Savings page', () => {
  beforeEach(() => {
    vi.spyOn(clientModule.api, 'savingsSummary').mockResolvedValue(fixture);
  });

  it('渲染 4 张顶部 KPI 卡（含已节省金额、ROI、缓存命中次数）', async () => {
    renderSavings();
    expect(await screen.findByText('已节省金额')).toBeInTheDocument();
    expect(screen.getByText('$12.34')).toBeInTheDocument();
    expect(screen.getByText('ROI')).toBeInTheDocument();
    expect(screen.getByText('11.0%')).toBeInTheDocument();
    expect(screen.getByText('缓存命中次数')).toBeInTheDocument();
    expect(screen.getByText('50')).toBeInTheDocument();
  });

  it('默认调用 savingsSummary 时 from/to 为近 7 天 ISO 范围', async () => {
    const spy = vi.spyOn(clientModule.api, 'savingsSummary');
    renderSavings();
    await waitFor(() => expect(spy).toHaveBeenCalled());
    const [from, to, tenant] = spy.mock.calls[0];
    expect(typeof from).toBe('string');
    expect(typeof to).toBe('string');
    expect(from).toMatch(/^\d{4}-\d{2}-\d{2}T/);
    expect(tenant).toBe('');
  });

  it('点击近 30 天按钮触发新的 API 调用（不同时间窗口）', async () => {
    const u = userEvent.setup();
    const spy = vi.spyOn(clientModule.api, 'savingsSummary');
    renderSavings();
    await screen.findByText('已节省金额');
    spy.mockClear();
    await u.click(screen.getByRole('button', { name: '近 30 天' }));
    await waitFor(() => expect(spy).toHaveBeenCalled());
  });

  it('点击算法说明按钮展开面板', async () => {
    const u = userEvent.setup();
    renderSavings();
    await screen.findByText('已节省金额');
    expect(screen.queryByText(/节省金额计算方法/)).not.toBeInTheDocument();
    await u.click(screen.getByRole('button', { name: /算法说明/ }));
    expect(screen.getByText(/节省金额计算方法/)).toBeInTheDocument();
    // 透明度警示：fallback_pricing_count > 0 时显示降级提示
    expect(screen.getByText(/降级估算/)).toBeInTheDocument();
  });

  it('SuperAdmin 看到 Top10 排行（SR1 验证：非 SuperAdmin 看不到）', async () => {
    const { unmount } = renderSavings('SuperAdmin');
    expect(await screen.findByText(/节省最多租户/)).toBeInTheDocument();
    expect(screen.getByText('tenant-A')).toBeInTheDocument();
    unmount();

    renderSavings('TenantAdmin');
    await screen.findByText('已节省金额');
    expect(screen.queryByText(/节省最多租户/)).not.toBeInTheDocument();
  });

  it('展示 routing_recommendations 表格', async () => {
    renderSavings();
    expect(await screen.findByText('gpt-4->gpt-3.5')).toBeInTheDocument();
  });

  // Epic 5.2 — modality breakdown preview card.
  it('按模态归因卡片渲染并启发式归类', async () => {
    renderSavings();
    expect(await screen.findByText('按模态归因（预览）')).toBeInTheDocument();
    expect(screen.getByTestId('modality-breakdown-table')).toBeInTheDocument();
    // gpt-4 / gpt-3.5 are both classified as chat (no embed/whisper/etc keyword).
    expect(screen.getByText('对话 (Chat / Completion)')).toBeInTheDocument();
  });

  it('按模态归因 - embedding 模型独立归类', async () => {
    const customFixture: SavingsSummary = {
      ...fixture,
      by_model: [
        { model: 'text-embedding-3-large', cost_saved: 5.0, tokens_saved: 5000, request_count: 100 },
        { model: 'whisper-1', cost_saved: 2.0, tokens_saved: 0, request_count: 20 },
        { model: 'gpt-4', cost_saved: 8.0, tokens_saved: 8000, request_count: 30 },
      ],
    };
    vi.spyOn(clientModule.api, 'savingsSummary').mockResolvedValue(customFixture);
    renderSavings();
    expect(await screen.findByText(/向量嵌入/)).toBeInTheDocument();
    expect(screen.getByText(/语音转写/)).toBeInTheDocument();
    expect(screen.getByText(/对话/)).toBeInTheDocument();
  });
});
