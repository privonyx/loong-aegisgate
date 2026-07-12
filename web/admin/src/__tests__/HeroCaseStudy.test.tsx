// TASK-20260528-01 Epic 3 — HeroCaseStudy + 子组件 tests
//
// 覆盖（spec §6.1）：
//   1. data=null → 引导文案 + CTA 链接
//   2. global scope 渲染（含 4 SR4 字面量）
//   3. tenant scope 显示 tenant_id
//   4. aggregator_since=null → 自启动以来文案
//   5. aggregator_since 有效 → 显示 since 锚定
//   6. SR4 字面量 "Case Study Numbers" / "已为你拦截" / "相当于" 全部出现

import { render, screen } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import HeroCaseStudy from '../components/HeroCaseStudy';
import type { CaseStudyHeadline } from '../types';

const fixture: CaseStudyHeadline = {
  scope: 'global',
  timestamp: '2026-05-28T12:00:00Z',
  aggregator_since: '2026-05-01T00:00:00Z',
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

function renderWithRouter(ui: React.ReactElement) {
  return render(<MemoryRouter>{ui}</MemoryRouter>);
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('HeroCaseStudy (TASK-20260528-01 SR4)', () => {
  it('data=null → 引导文案 + CTA 双语', () => {
    renderWithRouter(<HeroCaseStudy data={null} />);
    expect(screen.getByText('案例数据')).toBeInTheDocument();
    // 应有"暂无数据"中文引导
    expect(screen.getByText(/暂无数据/)).toBeInTheDocument();
    // CTA 链接到 ADOPTERS（可能多处出现）
    expect(screen.getAllByText(/ADOPTERS/).length).toBeGreaterThanOrEqual(1);
  });

  it('global scope 渲染含 SR4 字面量（Case Study Numbers / 已为你拦截 / 相当于）', async () => {
    renderWithRouter(<HeroCaseStudy data={fixture} />);
    vi.advanceTimersByTime(1500);

    // SR4 字面量 #1 — hero 标题（zh-CN：案例数据）
    expect(screen.getByText('案例数据')).toBeInTheDocument();
    // SR4 字面量 #2 — 已为你拦截（在 CacheInterceptStat 子组件）
    expect(screen.getByText(/已为你拦截/)).toBeInTheDocument();
    // intercepted = 7+3+2 = 12（counting-up 完成后 + 总分母 12+12=24 / "12" 数字本身可能多处）
    expect(screen.getAllByText(/12/).length).toBeGreaterThanOrEqual(1);
    // SR4 字面量 #3 "相当于" 在 cost_saved < $5 (1.44/5=0 杯) 时不渲染（防"0 杯咖啡"）
    // 故由后续独立用例验证 "相当于" 出现
  });

  it('cost_saved ≥ $5 触发 "相当于" 类比文案', () => {
    const big = { ...fixture, saved_vs_baseline: { ...fixture.saved_vs_baseline, cost_saved: 75 } };
    renderWithRouter(<HeroCaseStudy data={big} />);
    vi.advanceTimersByTime(1500);
    // $75 → 5 次午餐
    expect(screen.getByText(/相当于/)).toBeInTheDocument();
  });

  it('tenant scope 显示 tenant_id', () => {
    renderWithRouter(<HeroCaseStudy data={{ ...fixture, scope: 'tenant', tenant_id: 't-42' }} />);
    expect(screen.getByText(/t-42/)).toBeInTheDocument();
  });

  it('aggregator_since=null → "自启动以来"文案', () => {
    renderWithRouter(<HeroCaseStudy data={{ ...fixture, aggregator_since: null }} />);
    expect(screen.getByText(/自启动以来/)).toBeInTheDocument();
  });

  it('aggregator_since 有效 → 显示 since 锚定（ISO 日期片段）', () => {
    renderWithRouter(<HeroCaseStudy data={fixture} />);
    // 至少包含日期片段（2026-05-01 局部）
    expect(screen.getByText(/2026-05-01/)).toBeInTheDocument();
  });

  it('CTA footer 含 "aegisctl estimate" 引用（SR4 字面量 #4）', () => {
    // SR4 字面量 #4 锁定在 hero footer CTA 区域，与 spec/plan/impl/docs 4 方共享
    renderWithRouter(<HeroCaseStudy data={fixture} />);
    expect(screen.getByText(/aegisctl estimate/)).toBeInTheDocument();
  });

  // TASK-20260617-01 Bug2 — 三卡头部图标须 shrink-0，防 flex 行内被标签挤压/遮挡。
  it.each([
    ['相比基线节省'],
    ['缓存命中'],
    ['质量 (EMA)'],
  ])('卡片头部图标 "%s" 含 shrink-0 防遮挡', (label) => {
    renderWithRouter(<HeroCaseStudy data={fixture} />);
    const header = screen.getByText(label).parentElement;
    const icon = header?.querySelector('svg');
    expect(icon).not.toBeNull();
    expect(icon).toHaveClass('shrink-0');
  });
});
