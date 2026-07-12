// TASK-20260617-01 Epic 1 — 缓存命中率刷新同步（Bug1 / spec §3 D-bug1=B）。
//
// 背景：super_admin 下 KPI「缓存命中率」(summary.cache_hit_rate / metrics WS 帧)
//   与 Hero「缓存命中」(caseStudy.cache_hit_by_type.total_hit_rate / case_study WS 帧)
//   来自两路独立 WS 帧、刷新不同步 → 同一时刻两数字不一致。
// 修复：handleWsMessage 收到 metrics 帧时同步 patch caseStudy.total_hit_rate。

import { render, screen, act } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo, CaseStudyHeadline, WsMessage } from '../types';

// 捕获传给 useWebSocket 的 onMessage，便于测试中手动派发 WS 帧。
const wsHolder = vi.hoisted(() => ({
  onMessage: undefined as ((m: WsMessage) => void) | undefined,
}));

vi.mock('../hooks/useWebSocket', () => ({
  useWebSocket: (opts?: { onMessage?: (m: WsMessage) => void }) => {
    wsHolder.onMessage = opts?.onMessage;
    return { connected: true };
  },
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

const caseStudyFixture: CaseStudyHeadline = {
  scope: 'global',
  timestamp: '2026-06-17T00:00:00Z',
  aggregator_since: '2026-05-01T00:00:00Z',
  saved_vs_baseline: { actual_cost: 0.06, baseline_cost: 1.5, cost_saved: 1.44, savings_percent: 96 },
  cache_hit_by_type: { total_hit_rate: 0.5, hit_exact: 7, hit_semantic: 3, hit_conversation: 2, miss: 12 },
  quality_reason: { current_ema: 0.823, slope: 0.012, reason_factuality: 5, reason_refusal: 2, reason_latency_degraded: 1 },
};

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
    cache_hit_rate: 0.5,
    cost_saved_30d: 0.174,
    aggregator_since: '2026-05-01T00:00:00Z',
  });
  vi.spyOn(clientModule.api, 'queryAudits').mockResolvedValue({ data: [], count: 0 });
  vi.spyOn(clientModule.api, 'savingsSummary').mockResolvedValue(null as never);
  vi.spyOn(clientModule.api, 'securityEvents').mockResolvedValue(null as never);
  vi.spyOn(clientModule.api, 'caseStudyHeadline').mockResolvedValue(caseStudyFixture as never);
});

afterEach(() => {
  vi.runOnlyPendingTimers();
  vi.useRealTimers();
  vi.clearAllMocks();
  wsHolder.onMessage = undefined;
});

const flush = async () => { await act(async () => { await vi.advanceTimersByTimeAsync(0); }); };

const dispatchMetrics = async (cacheHitRate: number | null) => {
  await act(async () => {
    wsHolder.onMessage?.({
      type: 'metrics',
      data: {
        total_requests: 200,
        active_tenants: 2,
        total_cost_records: 60,
        cache_hit_rate: cacheHitRate,
      },
    } as WsMessage);
    await vi.advanceTimersByTimeAsync(0);
  });
};

describe('Dashboard 缓存命中率同步 (TASK-20260617-01 Bug1)', () => {
  it('super_admin：metrics 帧后 Hero 命中率与 KPI 同步刷新为 66.0%', async () => {
    renderDashboard('super_admin');
    await flush();
    // 初始：KPI + Hero 两处均 50.0%
    expect(screen.getAllByText('50.0%')).toHaveLength(2);

    await dispatchMetrics(0.66);

    // 修复后两处头部命中率都应跟随 metrics 帧刷新为 66.0%（同步）
    expect(screen.getAllByText('66.0%')).toHaveLength(2);
  });

  it('super_admin：metrics.cache_hit_rate=null 时不覆盖 Hero 既有命中率', async () => {
    renderDashboard('super_admin');
    await flush();

    await dispatchMetrics(null);

    // KPI 变 N/A，Hero 维持 50.0%（null 不应写入 caseStudy）
    expect(screen.getByText('50.0%')).toBeInTheDocument();
  });
});
