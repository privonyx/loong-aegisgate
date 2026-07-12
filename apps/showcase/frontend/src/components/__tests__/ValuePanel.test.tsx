import { describe, expect, it } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { ValuePanel } from '../ValuePanel';
import type { ObservabilitySummary, ValueEvent } from '../../types';

const summary: ObservabilitySummary = {
  totalRequests: 5,
  totalTokensSaved: 12800,
  cacheHits: 2,
  cacheHitRate: 0.4,
  guardrailBlocks: 1,
  avgLatencyMs: 320,
  byEngine: {
    creation: { requests: 4, tokensSaved: 12800, cacheHits: 2, guardrailBlocks: 0 },
    compliance: { requests: 1, tokensSaved: 0, cacheHits: 0, guardrailBlocks: 1 },
  },
  modelsUsed: { 'gpt-4o-mini': 4, 'gpt-4o': 1 },
};

const events: ValueEvent[] = [
  { ts: 1, scenarioId: 'comic', stepId: 'outline', engine: 'creation', model: 'gpt-4o-mini', tokensSaved: 320, cacheHit: true, latencyMs: 100, guardrailBlocked: false },
  { ts: 2, scenarioId: 'comic', stepId: 'compliance', engine: 'compliance', model: 'gpt-4o-mini', tokensSaved: 0, cacheHit: false, latencyMs: 80, guardrailBlocked: true, guardrailReason: '暴力越界' },
];

describe('ValuePanel 双核心仪表盘', () => {
  it('渲染省钱与护栏两张核心卡', () => {
    render(<ValuePanel summary={summary} recentEvents={events} />);
    expect(screen.getByTestId('tokens-saved')).toHaveTextContent('12,800');
    expect(screen.getByTestId('cache-rate')).toHaveTextContent('40%');
    expect(screen.getByTestId('guard-blocks')).toHaveTextContent('1');
  });

  it('展开明细显示模型分布与日志', () => {
    render(<ValuePanel summary={summary} recentEvents={events} />);
    fireEvent.click(screen.getByRole('button', { name: /展开明细/ }));
    expect(screen.getAllByText(/gpt-4o-mini/).length).toBeGreaterThan(0);
    expect(screen.getByText('🛡️ 拦截')).toBeInTheDocument();
  });

  it('空状态安全渲染（summary=null）', () => {
    render(<ValuePanel summary={null} recentEvents={[]} />);
    expect(screen.getByTestId('tokens-saved')).toHaveTextContent('0');
  });
});
