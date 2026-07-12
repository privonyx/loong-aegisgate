// TASK-20260604-01 P0-G / D3=C — 非 super 角色 Dashboard HTTP 轮询测试。
//
// 覆盖：
//   1. 非 super 角色每 15s 调用 dashboardSummary（HTTP 轮询补偿 WS 不推全局 metrics）
//   2. super 角色不启动轮询（走 WS）
//   3. 刷新方式 UI 标注随角色切换

import { render, screen, act } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo } from '../types';

vi.mock('../hooks/useWebSocket', () => ({
  useWebSocket: () => ({ isConnected: true, send: vi.fn() }),
}));

vi.mock('../api/client', () => ({
  api: {
    dashboardSummary: vi.fn(),
    queryAudits: vi.fn(),
    savingsSummary: vi.fn(),
    securityEvents: vi.fn(),
    caseStudyHeadline: vi.fn(),
  },
}));

import * as clientModule from '../api/client';
import Dashboard from '../pages/Dashboard';

function renderDashboard(role: string) {
  const user: UserInfo = { user_id: 'u1', tenant_id: 't1', role };
  const auth = {
    user,
    loading: false,
    error: null as string | null,
    login: vi.fn().mockResolvedValue(undefined),
    logout: vi.fn().mockResolvedValue(undefined),
    refresh: vi.fn().mockResolvedValue(undefined),
  };
  return render(
    <AuthContext.Provider value={auth}>
      <MemoryRouter>
        <Dashboard />
      </MemoryRouter>
    </AuthContext.Provider>,
  );
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.spyOn(clientModule.api, 'dashboardSummary').mockResolvedValue({
    total_requests: 100,
    active_tenants: 1,
    total_cost: 0.5,
    total_cost_records: 50,
    cache_hit_rate: 0.42,
    cost_saved_30d: 0.174,
    aggregator_since: '2026-05-01T00:00:00Z',
  });
  vi.spyOn(clientModule.api, 'queryAudits').mockResolvedValue({ data: [], count: 0 });
  vi.spyOn(clientModule.api, 'savingsSummary').mockResolvedValue(null as never);
  vi.spyOn(clientModule.api, 'securityEvents').mockResolvedValue(null as never);
  vi.spyOn(clientModule.api, 'caseStudyHeadline').mockResolvedValue(null as never);
});

afterEach(() => {
  vi.runOnlyPendingTimers();
  vi.useRealTimers();
  vi.clearAllMocks();
});

// fake timers 下 waitFor/findBy 失效（其内部依赖定时器）；改用 act + advanceTimersByTimeAsync
// 刷新微任务。flush(0) 仅推进微任务不触发 15s interval。
const flush = async () => { await act(async () => { await vi.advanceTimersByTimeAsync(0); }); };

describe('Dashboard P0-G HTTP polling (D3=C)', () => {
  it('非 super 角色每 15s 轮询 dashboardSummary', async () => {
    renderDashboard('tenant_admin');
    await flush();
    expect(clientModule.api.dashboardSummary).toHaveBeenCalledTimes(1);

    await act(async () => { await vi.advanceTimersByTimeAsync(15000); });
    expect(clientModule.api.dashboardSummary).toHaveBeenCalledTimes(2);

    await act(async () => { await vi.advanceTimersByTimeAsync(15000); });
    expect(clientModule.api.dashboardSummary).toHaveBeenCalledTimes(3);
  });

  it('super 角色不启动 HTTP 轮询（走 WS）', async () => {
    renderDashboard('super_admin');
    await flush();
    expect(clientModule.api.dashboardSummary).toHaveBeenCalledTimes(1);

    await act(async () => { await vi.advanceTimersByTimeAsync(30000); });
    expect(clientModule.api.dashboardSummary).toHaveBeenCalledTimes(1);
  });

  it('刷新方式标注随角色切换', async () => {
    renderDashboard('tenant_admin');
    await flush();
    expect(screen.getByTestId('refresh-mode')).toHaveTextContent('每 15 秒自动刷新');
  });
});
