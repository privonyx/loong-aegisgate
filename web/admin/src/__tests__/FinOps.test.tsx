// Phase 11.5 (TASK-20260518-02 E5.3) — FinOps page vitest suite.
//
// 6 happy-dom tests pinned by the plan §D Task 5.3 checklist:
//   1. renders proposal list (cards + KPI)
//   2. filters by state (re-fires API call with state=PROPOSED)
//   3. approve calls API with correct args + closes drawer
//   4. reject requires non-empty reason (modal validation)
//   5. rollback shows confirmation modal and only calls API on confirm
//   6. decision_trace JSON pretty-printed inside the detail drawer
//
// All vitest spies replace the autonomyApi module so no real fetch fires.

import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { ToastProvider } from '../components/Toast';
import * as autonomyModule from '../api/autonomy';
import type {
  ApprovalProposal,
  AutonomyReport,
} from '../api/autonomy';
import FinOps from '../pages/FinOps';

const proposed: ApprovalProposal = {
  id: 'prop-001',
  source: 'CostOptimizer',
  subject: 'tenant-A: gpt-4o → gpt-4o-mini',
  state: 'PROPOSED',
  proposer_user_id: 'system',
  proposed_at_ms: 1_700_000_000_000,
  reviewer_user_id: '',
  reviewed_at_ms: 0,
  reject_reason: '',
  payload: {
    action: 'override_quality_tier',
    tenant_id: 'tenant-A',
    current_model: 'gpt-4o',
    recommended_model: 'gpt-4o-mini',
    from_quality_tier: 'premium',
    to_quality_tier: 'standard',
    estimated_savings_usd_24h: 14.5,
    affected_requests_per_hour: 500,
  },
  decision_trace: {
    source_id: 'cost-optimizer-v2',
    algorithm_name: 'savings-ranker',
    input_hash_sha256: 'a'.repeat(64),
    proposed_at_ms: 1_700_000_000_000,
  },
  payload_sha256: 'sha-fake-001',
};

const applied: ApprovalProposal = {
  ...proposed,
  id: 'prop-002',
  subject: 'tenant-B: claude-3-opus → claude-3-haiku',
  state: 'APPLIED',
  reviewer_user_id: 'u-admin',
  reviewed_at_ms: 1_700_000_100_000,
  payload: { ...proposed.payload, estimated_savings_usd_24h: 9.0 },
};

const report: AutonomyReport = {
  totals: {
    PROPOSED: 1,
    APPROVED: 0,
    APPLIED: 1,
    REJECTED: 0,
    ROLLED_BACK: 0,
  },
  by_source: { CostOptimizer: { PROPOSED: 1, APPLIED: 1 } },
  estimated_savings_24h_usd: 9.0,
  sample_size: 2,
};

function renderFinOps() {
  return render(
    <ToastProvider>
      <MemoryRouter initialEntries={['/finops']}>
        <FinOps />
      </MemoryRouter>
    </ToastProvider>,
  );
}

describe('FinOps page', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
    vi.spyOn(autonomyModule.autonomyApi, 'listProposals').mockResolvedValue({
      data: [proposed, applied],
      limit: 20,
      offset: 0,
      total: 2,
    });
    vi.spyOn(autonomyModule.autonomyApi, 'report').mockResolvedValue(report);
    vi.spyOn(autonomyModule.autonomyApi, 'approve').mockResolvedValue({
      id: proposed.id,
      state: 'APPROVED',
      reviewer_user_id: 'u-admin',
    });
    vi.spyOn(autonomyModule.autonomyApi, 'reject').mockResolvedValue({
      id: proposed.id,
      state: 'REJECTED',
      reviewer_user_id: 'u-admin',
      reject_reason: 'too risky',
    });
    vi.spyOn(autonomyModule.autonomyApi, 'rollback').mockResolvedValue({
      id: applied.id,
      state: 'ROLLED_BACK',
    });
  });

  it('1. 渲染提案列表 + KPI', async () => {
    renderFinOps();
    expect(await screen.findByTestId('finops-list')).toBeInTheDocument();
    expect(screen.getByTestId(`proposal-${proposed.id}`)).toBeInTheDocument();
    expect(screen.getByTestId(`proposal-${applied.id}`)).toBeInTheDocument();
    // KPI labels visible (some words may also appear in select <option>,
    // so we assert via getAllByText for robustness).
    expect(screen.getAllByText('待审批').length).toBeGreaterThan(0);
    expect(screen.getAllByText('累计已应用').length).toBeGreaterThan(0);
    expect(screen.getAllByText('估算 24h 节省').length).toBeGreaterThan(0);
    // Result count line shows 2 items
    expect(screen.getByTestId('finops-result-count')).toHaveTextContent('2');
  });

  it('2. state 过滤器变更时重新拉取列表', async () => {
    const u = userEvent.setup();
    const spy = autonomyModule.autonomyApi.listProposals as ReturnType<
      typeof vi.fn
    >;
    renderFinOps();
    await screen.findByTestId('finops-list');
    spy.mockClear();
    const select = screen.getByLabelText('state-filter') as HTMLSelectElement;
    await u.selectOptions(select, 'PROPOSED');
    await waitFor(() => expect(spy).toHaveBeenCalled());
    const args = spy.mock.calls[0][0];
    expect(args.state).toBe('PROPOSED');
  });

  it('3. 批准按钮调用 approve 并通过确认 modal', async () => {
    const u = userEvent.setup();
    const approveSpy = autonomyModule.autonomyApi.approve as ReturnType<
      typeof vi.fn
    >;
    renderFinOps();
    await screen.findByTestId('finops-list');

    await u.click(screen.getByTestId(`proposal-${proposed.id}`));
    await u.click(screen.getByTestId('action-approve'));
    expect(screen.getByTestId('finops-confirm-modal')).toBeInTheDocument();

    await u.click(screen.getByTestId('finops-confirm-go'));
    await waitFor(() =>
      expect(approveSpy).toHaveBeenCalledWith(proposed.id),
    );
  });

  it('4. 拒绝必须填写原因（空原因不应调用 API）', async () => {
    const u = userEvent.setup();
    const rejectSpy = autonomyModule.autonomyApi.reject as ReturnType<
      typeof vi.fn
    >;
    renderFinOps();
    await screen.findByTestId('finops-list');

    await u.click(screen.getByTestId(`proposal-${proposed.id}`));
    await u.click(screen.getByTestId('action-reject'));
    expect(screen.getByTestId('reject-reason-input')).toBeInTheDocument();

    // 直接点确认 — 原因为空，reject 不应被调用
    await u.click(screen.getByTestId('finops-confirm-go'));
    await new Promise(r => setTimeout(r, 50));
    expect(rejectSpy).not.toHaveBeenCalled();

    // 填入原因后再确认应触发 reject
    await u.type(
      screen.getByTestId('reject-reason-input'),
      'too risky',
    );
    await u.click(screen.getByTestId('finops-confirm-go'));
    await waitFor(() =>
      expect(rejectSpy).toHaveBeenCalledWith(proposed.id, 'too risky'),
    );
  });

  it('5. 回滚操作必须通过二次确认 modal', async () => {
    const u = userEvent.setup();
    const rollbackSpy = autonomyModule.autonomyApi.rollback as ReturnType<
      typeof vi.fn
    >;
    renderFinOps();
    await screen.findByTestId('finops-list');

    await u.click(screen.getByTestId(`proposal-${applied.id}`));
    await u.click(screen.getByTestId('action-rollback'));
    // 仅打开 modal，rollback 还没被调用
    expect(screen.getByTestId('finops-confirm-modal')).toBeInTheDocument();
    expect(rollbackSpy).not.toHaveBeenCalled();

    await u.click(screen.getByTestId('finops-confirm-go'));
    await waitFor(() =>
      expect(rollbackSpy).toHaveBeenCalledWith(applied.id),
    );
  });

  // TASK-20260605-02 P1：后端 total 驱动真分页（共 N 条 = total，非当前页条数）。
  it('7. total > PAGE_SIZE 时翻页，下一页以 offset=20 重新拉取', async () => {
    const u = userEvent.setup();
    const spy = autonomyModule.autonomyApi.listProposals as ReturnType<
      typeof vi.fn
    >;
    spy.mockResolvedValue({
      data: [proposed, applied],
      limit: 20,
      offset: 0,
      total: 45,
    });
    renderFinOps();
    await screen.findByTestId('finops-list');

    // 共 45 条（total），即使当前页只有 2 条。
    expect(screen.getByTestId('finops-result-count')).toHaveTextContent('45');
    expect(screen.getByTestId('finops-page-indicator')).toHaveTextContent('1 / 3');

    spy.mockClear();
    await u.click(screen.getByTestId('finops-next-page'));
    await waitFor(() => expect(spy).toHaveBeenCalled());
    expect(spy.mock.calls[0][0]).toMatchObject({ limit: 20, offset: 20 });
  });

  it('6. 详情抽屉以美化 JSON 展示 decision_trace', async () => {
    const u = userEvent.setup();
    renderFinOps();
    await screen.findByTestId('finops-list');

    await u.click(screen.getByTestId(`proposal-${proposed.id}`));
    const trace = await screen.findByTestId('proposal-decision-trace');
    expect(trace.textContent).toContain('source_id');
    expect(trace.textContent).toContain('cost-optimizer-v2');
    expect(trace.textContent).toContain('algorithm_name');
    // 美化输出至少有缩进空格
    expect(trace.textContent).toMatch(/\n\s{2,}/);

    const payload = screen.getByTestId('proposal-payload');
    expect(payload.textContent).toContain('estimated_savings_usd_24h');
    expect(payload.textContent).toContain('14.5');
  });
});
