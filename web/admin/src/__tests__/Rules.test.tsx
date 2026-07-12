// TASK-20260603-01 Epic 2 — Rule Set 页测试（版本列表 + 新建版本 + 激活）。
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ToastProvider } from '../components/Toast';
import type { RuleSet } from '../types';

vi.mock('../api/client', () => ({
  api: {
    listRuleSets: vi.fn(),
    createRuleSet: vi.fn(),
    activateRuleSet: vi.fn(),
  },
}));

import { api } from '../api/client';
import Rules from '../pages/Rules';

const v2: RuleSet = { version: 2, tenant_id: 'tn-1', created_at: '2026-06-02T00:00:00Z', is_active: true, rules_json: '[{"id":"r1"}]' };
const v1: RuleSet = { version: 1, tenant_id: 'tn-1', created_at: '2026-06-01T00:00:00Z', is_active: false, rules_json: '[]' };

function renderPage() {
  return render(<ToastProvider><Rules /></ToastProvider>);
}

describe('Rules 页', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(api.listRuleSets).mockResolvedValue({ data: [v2, v1], count: 2 });
    vi.mocked(api.createRuleSet).mockResolvedValue(v2);
    vi.mocked(api.activateRuleSet).mockResolvedValue({ activated: true, version: 1 });
  });

  it('渲染规则版本列表', async () => {
    renderPage();
    expect(await screen.findByText('v2')).toBeInTheDocument();
    expect(screen.getByText('v1')).toBeInTheDocument();
    expect(api.listRuleSets).toHaveBeenCalled();
  });

  it('使用后端 total 字段并按 offset 翻页（P0-E residual）', async () => {
    vi.mocked(api.listRuleSets).mockResolvedValue({ data: [v2, v1], count: 2, total: 120 });
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('v2');
    // 首次加载 offset=0。
    expect(api.listRuleSets).toHaveBeenCalledWith('', 50, 0);
    // total=120 > pageSize=50 → 分页器显示真实总数。
    expect(screen.getByText(/共 120 条/)).toBeInTheDocument();
    // 点下一页 → offset=50（DataTable 的 next 是文档内最后一个按钮）。
    const buttons = screen.getAllByRole('button');
    await user.click(buttons[buttons.length - 1]);
    await waitFor(() => expect(api.listRuleSets).toHaveBeenCalledWith('', 50, 50));
  });

  it('新建版本：合法 JSON 提交调用 createRuleSet', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('v2');
    await user.click(screen.getByRole('button', { name: /新建版本/ }));
    fireEvent.change(screen.getByLabelText(/规则 JSON/), { target: { value: '[{"id":"x"}]' } });
    await user.click(screen.getByTestId('submit-ruleset'));
    await waitFor(() => expect(api.createRuleSet).toHaveBeenCalled());
  });

  it('新建版本：非法 JSON 阻断且不调用 createRuleSet', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('v2');
    await user.click(screen.getByRole('button', { name: /新建版本/ }));
    fireEvent.change(screen.getByLabelText(/规则 JSON/), { target: { value: 'not-json' } });
    await user.click(screen.getByTestId('submit-ruleset'));
    expect(await screen.findByText(/JSON 格式无效/)).toBeInTheDocument();
    expect(api.createRuleSet).not.toHaveBeenCalled();
  });

  it('激活非激活版本调用 activateRuleSet', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('v1');
    await user.click(screen.getByTitle('激活版本 1'));
    await user.click(screen.getByTestId('confirm-activate'));
    await waitFor(() => expect(api.activateRuleSet).toHaveBeenCalledWith(1, 'tn-1'));
  });
});
