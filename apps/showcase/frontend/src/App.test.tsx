import { describe, expect, it, vi, beforeEach } from 'vitest';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { App } from './App';
import type { ObservabilitySummary, RunStepResult, ScenarioSummary } from './types';

vi.mock('./api', () => ({
  getScenarios: vi.fn(),
  getSummary: vi.fn(),
  runStepStream: vi.fn(),
}));

import { getScenarios, getSummary, runStepStream } from './api';

const scenarios: ScenarioSummary[] = [
  {
    id: 'comic',
    meta: { name: 'AI 漫剧创作台', description: '漫剧场景', icon: 'x', accentColor: '#6d5dfc' },
    model: { primary: 'gpt-4o-mini', fallback: 'gpt-4o' },
    uiSteps: [
      { id: 'outline', label: '生成分集大纲', engine: 'creation', kind: 'generate', inputs: [{ name: 'theme', label: '主题', type: 'text', default: '线索' }] },
    ],
  },
  {
    id: 'ecommerce',
    meta: { name: '电商导购助手', description: '电商场景', icon: 'y', accentColor: '#10b981' },
    model: { primary: 'gpt-4o-mini' },
    uiSteps: [],
  },
];

const emptySummary: ObservabilitySummary = {
  totalRequests: 0,
  totalTokensSaved: 0,
  cacheHits: 0,
  cacheHitRate: 0,
  guardrailBlocks: 0,
  avgLatencyMs: 0,
  byEngine: { creation: { requests: 0, tokensSaved: 0, cacheHits: 0, guardrailBlocks: 0 }, compliance: { requests: 0, tokensSaved: 0, cacheHits: 0, guardrailBlocks: 0 } },
  modelsUsed: {},
};

beforeEach(() => {
  vi.mocked(getScenarios).mockResolvedValue(scenarios);
  vi.mocked(getSummary).mockResolvedValue(emptySummary);
  vi.mocked(runStepStream).mockReset();
});

describe('App', () => {
  it('加载并渲染场景标签，默认选中第一个', async () => {
    render(<App />);
    await waitFor(() => expect(screen.getAllByText('AI 漫剧创作台').length).toBeGreaterThan(0));
    expect(screen.getByText('电商导购助手')).toBeInTheDocument();
    // 引导台展示第一个场景步骤
    expect(screen.getByText('生成分集大纲')).toBeInTheDocument();
  });

  it('流式运行步骤：逐字 token 后展示最终输出', async () => {
    const result: RunStepResult = {
      output: '第1集 第2集 第3集',
      events: [{ ts: 1, scenarioId: 'comic', stepId: 'outline', engine: 'creation', model: 'gpt-4o-mini', tokensSaved: 100, cacheHit: true, latencyMs: 50, guardrailBlocked: false }],
      toolInvocations: [{ name: 'getWorldBible', args: {}, ok: true }],
      rounds: 2,
    };
    vi.mocked(runStepStream).mockImplementation(async (_sid, _step, _inputs, handlers) => {
      handlers.onValue?.(result.events[0]);
      handlers.onToken?.('第1集 ');
      handlers.onToken?.('第2集 ');
      handlers.onDone?.(result);
    });

    render(<App />);
    await waitFor(() => expect(screen.getByText('生成分集大纲')).toBeInTheDocument());
    fireEvent.click(screen.getByRole('button', { name: '运行此步骤' }));
    await waitFor(() => expect(screen.getByText('第1集 第2集 第3集')).toBeInTheDocument());
    expect(runStepStream).toHaveBeenCalledWith(
      'comic',
      'outline',
      { theme: '线索' },
      expect.objectContaining({ onToken: expect.any(Function), onDone: expect.any(Function) })
    );
  });

  it('后端加载失败显示错误横幅', async () => {
    vi.mocked(getScenarios).mockRejectedValue(new Error('连接被拒绝'));
    render(<App />);
    await waitFor(() => expect(screen.getByText(/无法连接后端/)).toBeInTheDocument());
  });
});
