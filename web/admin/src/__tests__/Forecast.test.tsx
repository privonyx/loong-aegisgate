// TASK-20260603-01 Epic 4 — Usage Predict (Forecast) 页测试。
// recharts 在 happy-dom 下 ResponsiveContainer 尺寸为 0，故只断言 KPI 文本 + API 调用，
// 不断言图表内部（与既有 Savings.test 范式一致）。
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ToastProvider } from '../components/Toast';
import type { UsagePrediction, BudgetPrediction } from '../types';

vi.mock('../api/client', () => ({
  api: {
    predictUsage: vi.fn(),
    predictBudget: vi.fn(),
  },
}));

import { api } from '../api/client';
import Forecast from '../pages/Forecast';

const usage: UsagePrediction = {
  daily_trend: 1.25,
  r_squared: 0.87,
  historical: [
    { date: '2026-06-01', total_cost: 10, request_count: 100 },
    { date: '2026-06-02', total_cost: 12, request_count: 110 },
  ],
  predicted: [
    { date: '2026-06-03', total_cost: 13 },
    { date: '2026-06-04', total_cost: 14 },
  ],
};

const budget: BudgetPrediction = {
  budget: 1000,
  budget_exhaustion_date: '2026-09-01',
  daily_trend: 1.25,
  r_squared: 0.87,
};

describe('Forecast 页', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(api.predictUsage).mockResolvedValue(usage);
    vi.mocked(api.predictBudget).mockResolvedValue(budget);
  });

  it('加载时调用 predictUsage + predictBudget 并渲染 KPI', async () => {
    render(<ToastProvider><Forecast /></ToastProvider>);
    await waitFor(() => expect(api.predictUsage).toHaveBeenCalled());
    expect(api.predictBudget).toHaveBeenCalled();
    expect(await screen.findByText('0.87')).toBeInTheDocument();        // r_squared
    expect(screen.getByText('2026-09-01')).toBeInTheDocument();          // budget_exhaustion_date
  });

  it('history 天数选择器钳制上限 ≤ 90', async () => {
    render(<ToastProvider><Forecast /></ToastProvider>);
    await screen.findByText('0.87');
    const sel = screen.getByLabelText(/历史天数/) as HTMLSelectElement;
    const opts = Array.from(sel.options).map(o => Number(o.value));
    expect(Math.max(...opts)).toBeLessThanOrEqual(90);
  });

  it('forecast 天数选择器钳制上限 ≤ 30', async () => {
    render(<ToastProvider><Forecast /></ToastProvider>);
    await screen.findByText('0.87');
    const sel = screen.getByLabelText(/预测天数/) as HTMLSelectElement;
    const opts = Array.from(sel.options).map(o => Number(o.value));
    expect(Math.max(...opts)).toBeLessThanOrEqual(30);
  });

  it('改变历史天数触发重新预测', async () => {
    const user = userEvent.setup();
    render(<ToastProvider><Forecast /></ToastProvider>);
    await screen.findByText('0.87');
    vi.mocked(api.predictUsage).mockClear();
    await user.selectOptions(screen.getByLabelText(/历史天数/), '60');
    await waitFor(() => expect(api.predictUsage).toHaveBeenCalledWith('', 60, expect.any(Number)));
  });
});
