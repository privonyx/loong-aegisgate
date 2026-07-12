// TASK-20260527-02 Epic 4 — Dashboard Row 4 (Case Study Numbers) tests.
//
// 覆盖：
//   1. 初始渲染时获取 caseStudyHeadline + 渲染 3 头条卡片
//   2. WS case_study 消息合并到 caseStudy state（实时刷新 Row 4）

import { render, screen, waitFor, act } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { CaseStudyHeadline, WsCaseStudy, UserInfo } from '../types';

// Mock useWebSocket: 只暴露 onMessage 回调以便测试触发。
const wsCallbacks: Array<(msg: unknown) => void> = [];
vi.mock('../hooks/useWebSocket', () => ({
  useWebSocket: ({ onMessage }: { onMessage: (msg: unknown) => void }) => {
    wsCallbacks.push(onMessage);
    return { isConnected: true, send: vi.fn() };
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

// Dashboard 依赖 useAuth；super_admin 走 WS 路径（不触发 P0-G HTTP 轮询）。
function renderDashboard(role = 'super_admin') {
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

const headlineFixture: CaseStudyHeadline = {
  scope: 'global',
  timestamp: '2026-05-27T10:00:00Z',
  saved_vs_baseline: {
    actual_cost: 0.06,
    baseline_cost: 1.50,
    cost_saved: 1.44,
    savings_percent: 96.0,
  },
  cache_hit_by_type: {
    total_hit_rate: 0.5,
    hit_exact: 7,
    hit_semantic: 3,
    hit_conversation: 2,
    miss: 12,
  },
  quality_reason: {
    current_ema: 0.823,
    slope: 0.012,
    reason_factuality: 5,
    reason_refusal: 2,
    reason_latency_degraded: 1,
  },
};

beforeEach(() => {
  wsCallbacks.length = 0;
  vi.spyOn(clientModule.api, 'dashboardSummary').mockResolvedValue({
    total_requests: 100,
    active_tenants: 1,
    total_cost: 0.5,
    total_cost_records: 50,
    cache_hit_rate: 0.42,
    cost_saved_30d: 0.174,
    aggregator_since: '2026-05-01T00:00:00Z',
  });
  vi.spyOn(clientModule.api, 'queryAudits').mockResolvedValue({
    data: [],
    count: 0,
  });
  vi.spyOn(clientModule.api, 'savingsSummary').mockResolvedValue(null as never);
  vi.spyOn(clientModule.api, 'securityEvents').mockResolvedValue(null as never);
  vi.spyOn(clientModule.api, 'caseStudyHeadline').mockResolvedValue(headlineFixture);
});

describe('Dashboard Row 0 Hero — Case Study Numbers (TASK-20260528-01 migration)', () => {
  it('初始渲染调用 caseStudyHeadline 并展示 3 头条卡片', async () => {
    renderDashboard();

    // SR4 字面量 #1（hero header 标题，zh-CN：案例数据）
    expect(await screen.findByText('案例数据')).toBeInTheDocument();
    // hero 三大区都应渲染（相比基线节省 / 缓存命中 / 质量 EMA）
    expect(screen.getByText('相比基线节省')).toBeInTheDocument();
    expect(screen.getByText('缓存命中')).toBeInTheDocument();
    expect(screen.getByText('质量 (EMA)')).toBeInTheDocument();
    expect(screen.getByText('0.823')).toBeInTheDocument();

    // SR4 字面量 #2 "已为你拦截" 在 CacheInterceptStat 子组件
    expect(screen.getByText(/已为你拦截/)).toBeInTheDocument();

    // hit type 三档计数（exact 7 / semantic 3 / conversation 2，由 fixture 驱动）
    // 注：A2 本地化后拦截叙事改为 "精确 7 · 语义 3 · 会话 2" 格式
    expect(screen.getByText(/语义/)).toBeInTheDocument();
    expect(screen.getByText(/会话/)).toBeInTheDocument();

    // quality reason 三档计数仍渲染于 hero 第 3 列底部
    expect(screen.getByText(/事实性/)).toBeInTheDocument();
    expect(screen.getByText(/拒答/)).toBeInTheDocument();
    expect(screen.getByText(/延迟/)).toBeInTheDocument();

    expect(clientModule.api.caseStudyHeadline).toHaveBeenCalled();
  });

  // TASK-20260602-01 Epic 1 regression — WS nested envelope schema.
  // 后端 admin_ws_controller.cpp 推送 metrics/audit/case_study 均用
  // { type, data: { ... } } nested 结构。本测试断言 Dashboard 的
  // handleWsMessage 能正确接收 metrics nested envelope（不再因为 cast 错位
  // 导致 KPI 实时更新失效）。
  // fixture 数字选取 ≥ 4 位 (A-NEW4 跨组件区分度 ≥ 2 倍)，避免与 case_study
  // hero 内 1-3 位数字（如 50.0%、823 EMA 派生 .823）的字符歧义。
  it('TASK-20260602-01 Epic 1: 收到 metrics nested envelope 后更新 KPI', async () => {
    renderDashboard();
    await screen.findByText('案例数据');
    await waitFor(() => expect(wsCallbacks.length).toBeGreaterThan(0));

    // 后端 nested envelope 格式（与 admin_ws_controller.cpp buildMetricsSnapshot 一致）
    const metricsMsg = {
      type: 'metrics' as const,
      data: {
        total_requests: 12345,           // 不与 fixture cache exact=7 等 1-2 位数字冲突
        active_tenants: 8888,             // 不与 cache semantic=3 / conversation=2 冲突
        total_cost_records: 999,          // WS handler 不消费此字段（保留 KPI 既有值）
        cache_hit_rate: 0.7777,           // 渲染 "77.8%" 不与 fixture hero "50.0%" 冲突
      },
    };
    act(() => {
      wsCallbacks.forEach(cb => cb(metricsMsg));
    });

    // Row 1 KPI 卡：总请求数 (toLocaleString → "12,345") / 活跃租户 ("8888") /
    // 缓存命中率 ("77.8%") — 应反映 WS 推送的最新值。
    // TASK-20260617-01 Bug1：metrics 帧现在同步 patch Hero「缓存命中」总命中率，
    //   故 "77.8%" 应同时出现在 KPI 卡与 Hero 卡（两处刷新同步）。
    await waitFor(() => {
      expect(screen.getByText('12,345')).toBeInTheDocument();
      expect(screen.getByText('8888')).toBeInTheDocument();
      expect(screen.getAllByText('77.8%')).toHaveLength(2);
    });
  });

  it('收到 WS case_study 消息后合并 state 并刷新 Row 0 hero', async () => {
    renderDashboard();
    await screen.findByText('案例数据');

    const updated: WsCaseStudy = {
      type: 'case_study',
      data: {
        scope: 'global',
        saved_vs_baseline: {
          actual_cost: 0.10,
          baseline_cost: 2.00,
          cost_saved: 1.90,
          savings_percent: 95.0,
        },
        cache_hit_by_type: {
          total_hit_rate: 0.55,
          hit_exact: 10,
          hit_semantic: 4,
          hit_conversation: 3,
          miss: 14,
        },
        quality_reason: {
          current_ema: 0.91,
          slope: 0.05,
          reason_factuality: 6,
          reason_refusal: 2,
          reason_latency_degraded: 0,
        },
      },
    };

    await waitFor(() => expect(wsCallbacks.length).toBeGreaterThan(0));
    act(() => {
      wsCallbacks.forEach(cb => cb(updated));
    });

    await waitFor(() => {
      expect(screen.getByText('$1.90')).toBeInTheDocument();
      expect(screen.getByText('55.0%')).toBeInTheDocument();
      expect(screen.getByText('0.910')).toBeInTheDocument();
    });
  });
});
