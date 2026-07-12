// TASK-20260614-03 阶段 5 — i18n 语言切换 E2E
//
// 覆盖整条链路（不止单一组件）：
//   1. 切换器切到 English → 顶栏（common:layout）+ 侧栏导航（nav）同时变英文
//   2. 切换会持久化到 localStorage（STORAGE_KEY）
//   3. 营销区组件 follow-locale：同一 i18n 语言下渲染对应语言文案（A2 纯净分离，无对端残留）
//   4. 切回中文恢复中文文案
//
// 依赖全局 setup.ts：每个用例前 i18n 复位 zh-CN。

import { render, screen, fireEvent, waitFor, within } from '@testing-library/react';
import { describe, it, expect, vi, afterEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo, CaseStudyHeadline } from '../types';
import Layout from '../components/Layout';
import HeroCaseStudy from '../components/HeroCaseStudy';
import i18n from '../i18n';
import { STORAGE_KEY } from '../i18n';

const superUser: UserInfo = { user_id: 'alice', tenant_id: 't1', role: 'super_admin' };

function renderLayout() {
  const auth = {
    user: superUser,
    loading: false,
    error: null as string | null,
    login: vi.fn().mockResolvedValue(undefined),
    logout: vi.fn().mockResolvedValue(undefined),
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

const heroFixture: CaseStudyHeadline = {
  scope: 'global',
  timestamp: '2026-05-28T12:00:00Z',
  aggregator_since: '2026-05-01T00:00:00Z',
  saved_vs_baseline: { actual_cost: 0.06, baseline_cost: 1.5, cost_saved: 1.44, savings_percent: 96 },
  cache_hit_by_type: { total_hit_rate: 0.5, hit_exact: 7, hit_semantic: 3, hit_conversation: 2, miss: 12 },
  quality_reason: { current_ema: 0.823, slope: 0.012, reason_factuality: 5, reason_refusal: 2, reason_latency_degraded: 1 },
};

describe('i18n 语言切换 E2E (TASK-20260614-03 阶段 5)', () => {
  afterEach(() => {
    i18n.changeLanguage('zh-CN');
    try { localStorage.removeItem(STORAGE_KEY); } catch { /* noop */ }
  });

  it('切到 English → 顶栏 + 导航同步英文，并持久化到 localStorage；切回中文恢复', async () => {
    renderLayout();
    // 初始中文（common:layout + nav 两个 namespace）
    expect(screen.getByText('管理面板')).toBeInTheDocument();
    expect(screen.getByText('仪表盘')).toBeInTheDocument();
    expect(screen.getByText('概览')).toBeInTheDocument();

    fireEvent.change(screen.getByTestId('lang-switcher'), { target: { value: 'en-US' } });

    await waitFor(() => expect(screen.getByText('Admin Panel')).toBeInTheDocument());
    expect(screen.getByText('Dashboard')).toBeInTheDocument();
    expect(screen.getByText('Overview')).toBeInTheDocument();
    // 中文文案已消失（无双语残留）
    expect(screen.queryByText('管理面板')).not.toBeInTheDocument();
    // 持久化
    expect(localStorage.getItem(STORAGE_KEY)).toBe('en-US');

    // 切回中文
    fireEvent.change(screen.getByTestId('lang-switcher'), { target: { value: 'zh-CN' } });
    await waitFor(() => expect(screen.getByText('管理面板')).toBeInTheDocument());
    expect(localStorage.getItem(STORAGE_KEY)).toBe('zh-CN');
  });

  it('营销区 follow-locale：英文下渲染英文文案，中文下渲染中文文案（A2）', async () => {
    // 中文语境
    const { unmount } = render(<MemoryRouter><HeroCaseStudy data={heroFixture} /></MemoryRouter>);
    expect(screen.getByText('案例数据')).toBeInTheDocument();
    expect(screen.getByText('相比基线节省')).toBeInTheDocument();
    expect(screen.getByText('缓存命中')).toBeInTheDocument();
    unmount();

    // 切英文重渲染
    await i18n.changeLanguage('en-US');
    render(<MemoryRouter><HeroCaseStudy data={heroFixture} /></MemoryRouter>);
    const heroes = screen.getAllByText('Case Study Numbers');
    expect(heroes.length).toBeGreaterThanOrEqual(1);
    expect(screen.getByText('saved vs baseline')).toBeInTheDocument();
    expect(screen.getByText('cache hit')).toBeInTheDocument();
    // 中文不应残留
    expect(screen.queryByText('相比基线节省')).not.toBeInTheDocument();
  });

  it('英文空数据态：hero 引导文案为纯英文', async () => {
    await i18n.changeLanguage('en-US');
    render(<MemoryRouter><HeroCaseStudy data={null} /></MemoryRouter>);
    const section = screen.getByText('Case Study Numbers').closest('section')!;
    expect(within(section).getByText(/No data yet/)).toBeInTheDocument();
    expect(within(section).getByText(/Join ADOPTERS\.md/)).toBeInTheDocument();
  });
});
