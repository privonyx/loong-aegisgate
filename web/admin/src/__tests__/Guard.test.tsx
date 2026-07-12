// TASK-20260703-02 — Adaptive Guard 页交互 + 安全需求测试。
// 覆盖：
//   - 初始态 / 解释 200 渲染 7 字段 + 严格 A（反馈面板仅在解释成功时出现）
//   - 解释 404 → 友好空态且**不**出现反馈面板（严格 A）
//   - 解释 503 → not-wired banner
//   - SR-1：反馈 body reviewer_user_id / reviewer_role 由登录态派生，页面无可编辑控件
//   - SR-4/D5：feedback 429 → rateLimited toast；409 → anomalyBlocked toast（不伪装 accepted）
//   - D4：promote 走 ConfirmDialog 二次确认 + 成功 toast 含 FinOps 引导
//   - promote 503 → not-wired banner
//   - SR-2：viewer 经 RoleGuard → role-denied（UI 守卫）
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { MemoryRouter } from 'react-router-dom';
import { ToastProvider } from '../components/Toast';
import { AuthContext } from '../hooks/useAuth';
import RoleGuard from '../components/RoleGuard';
import type { UserInfo } from '../types';
import * as guardModule from '../api/guard';
import { ApiError } from '../api/request';
import Guard from '../pages/Guard';
import zhGuard from '../locales/zh-CN/guard.json';
import enGuard from '../locales/en-US/guard.json';

const explanation = {
  trigger_layer: 'L3',
  trigger_rule_id: 'rule-injection-01',
  model_version: 'guard-v2',
  threshold: 0.6,
  matched_pattern: 'ignore ***',
  confidence: 0.92,
  explanation_text: 'Blocked: prompt injection pattern detected.',
};

function authValue(user: UserInfo | null) {
  return {
    user,
    loading: false,
    error: null,
    login: vi.fn(),
    logout: vi.fn(),
    refresh: vi.fn(),
  };
}

const superAdmin: UserInfo = { user_id: 'u-1', tenant_id: 't1', role: 'super_admin' } as UserInfo;

function renderGuard(user: UserInfo | null = superAdmin) {
  return render(
    <AuthContext.Provider value={authValue(user)}>
      <ToastProvider>
        <MemoryRouter initialEntries={['/guard']}>
          <Guard />
        </MemoryRouter>
      </ToastProvider>
    </AuthContext.Provider>,
  );
}

async function lookup(id: string) {
  const u = userEvent.setup();
  await u.type(screen.getByTestId('guard-search-input'), id);
  await u.click(screen.getByTestId('guard-search-button'));
  return u;
}

describe('Guard page', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  it('初始态：渲染查询框，无解释卡片/反馈面板', () => {
    renderGuard();
    expect(screen.getByTestId('guard-search-input')).toBeInTheDocument();
    expect(screen.queryByTestId('guard-explanation-card')).not.toBeInTheDocument();
    expect(screen.queryByTestId('guard-feedback-panel')).not.toBeInTheDocument();
  });

  it('解释 200：渲染 7 字段 + 反馈面板出现（严格 A）', async () => {
    vi.spyOn(guardModule.guardApi, 'getExplanation').mockResolvedValue(explanation);
    renderGuard();
    await lookup('req-1');
    const card = await screen.findByTestId('guard-explanation-card');
    expect(card).toHaveTextContent('L3');
    expect(card).toHaveTextContent('rule-injection-01');
    expect(card).toHaveTextContent('guard-v2');
    expect(card).toHaveTextContent('ignore ***');
    expect(card).toHaveTextContent('prompt injection');
    expect(screen.getByTestId('guard-feedback-panel')).toBeInTheDocument();
  });

  it('解释 404：友好空态，不出现反馈面板（严格 A）', async () => {
    vi.spyOn(guardModule.guardApi, 'getExplanation').mockRejectedValue(
      new ApiError('not found', 404, 'AEGIS-6003'),
    );
    renderGuard();
    await lookup('req-404');
    expect(await screen.findByTestId('guard-explanation-empty')).toBeInTheDocument();
    expect(screen.queryByTestId('guard-feedback-panel')).not.toBeInTheDocument();
    expect(screen.queryByTestId('guard-explanation-card')).not.toBeInTheDocument();
  });

  it('解释 503：显示未启用 banner', async () => {
    vi.spyOn(guardModule.guardApi, 'getExplanation').mockRejectedValue(
      new ApiError('unavailable', 503, 'AEGIS-6008'),
    );
    renderGuard();
    await lookup('req-503');
    expect(await screen.findByTestId('guard-not-wired-banner')).toBeInTheDocument();
  });

  it('SR-1：反馈 body reviewer 字段派生自登录态，页面无 reviewer 输入控件', async () => {
    vi.spyOn(guardModule.guardApi, 'getExplanation').mockResolvedValue(explanation);
    const submitSpy = vi
      .spyOn(guardModule.guardApi, 'submitFeedback')
      .mockResolvedValue({ accepted: true, request_id: 'req-1' });
    renderGuard(superAdmin);
    const u = await lookup('req-1');
    await screen.findByTestId('guard-feedback-panel');

    // 页面不得存在 reviewer_role / reviewer_user_id 的可编辑输入控件
    expect(screen.queryByTestId('guard-reviewer-role-input')).not.toBeInTheDocument();
    expect(screen.queryByTestId('guard-reviewer-user-input')).not.toBeInTheDocument();

    await u.click(screen.getByTestId('guard-label-false_positive'));
    await u.click(screen.getByTestId('guard-feedback-submit'));

    await waitFor(() => expect(submitSpy).toHaveBeenCalled());
    const body = submitSpy.mock.calls[0][0];
    expect(body.request_id).toBe('req-1');
    expect(body.label).toBe('false_positive');
    expect(body.reviewer_user_id).toBe('u-1');
    expect(body.reviewer_role).toBe('security_admin'); // mapReviewerRole(super_admin)
  });

  it('SR-4/D5：反馈 429 → 频繁提示 toast（不伪装成功）', async () => {
    vi.spyOn(guardModule.guardApi, 'getExplanation').mockResolvedValue(explanation);
    vi.spyOn(guardModule.guardApi, 'submitFeedback').mockRejectedValue(
      new ApiError('rate', 429, 'AEGIS-1005'),
    );
    renderGuard();
    const u = await lookup('req-1');
    await screen.findByTestId('guard-feedback-panel');
    await u.click(screen.getByTestId('guard-label-confirmed_block'));
    await u.click(screen.getByTestId('guard-feedback-submit'));
    expect(await screen.findByText(/操作过于频繁/)).toBeInTheDocument();
    // 不得出现成功文案
    expect(screen.queryByText('反馈已提交')).not.toBeInTheDocument();
  });

  it('SR-4/D5：反馈 409 → 异常拦截 toast（不伪装成功）', async () => {
    vi.spyOn(guardModule.guardApi, 'getExplanation').mockResolvedValue(explanation);
    vi.spyOn(guardModule.guardApi, 'submitFeedback').mockRejectedValue(
      new ApiError('flagged', 409, 'AEGIS-5001'),
    );
    renderGuard();
    const u = await lookup('req-1');
    await screen.findByTestId('guard-feedback-panel');
    await u.click(screen.getByTestId('guard-label-false_negative'));
    await u.click(screen.getByTestId('guard-feedback-submit'));
    expect(await screen.findByText(/异常检测/)).toBeInTheDocument();
    expect(screen.queryByText('反馈已提交')).not.toBeInTheDocument();
  });

  it('D4：晋升走 ConfirmDialog 二次确认，成功 toast 含 FinOps 引导', async () => {
    const promoteSpy = vi
      .spyOn(guardModule.guardApi, 'promoteModel')
      .mockResolvedValue({ proposal_id: 'p-1' });
    renderGuard();
    const u = userEvent.setup();
    await u.click(screen.getByTestId('guard-promote-toggle'));
    await u.type(screen.getByTestId('guard-promote-model-id'), 'm-1');
    await u.type(screen.getByTestId('guard-promote-version'), '3');
    await u.click(screen.getByTestId('guard-promote-submit'));

    // 二次确认对话框出现，promote 尚未调用（SR-4）
    expect(screen.getByTestId('guard-promote-confirm')).toBeInTheDocument();
    expect(promoteSpy).not.toHaveBeenCalled();

    await u.click(screen.getByTestId('guard-promote-confirm'));
    await waitFor(() => expect(promoteSpy).toHaveBeenCalled());
    const body = promoteSpy.mock.calls[0][0];
    expect(body.model_id).toBe('m-1');
    expect(body.version).toBe('3');
    expect(body.action).toBe('promote');
    // D4：成功后引导跳 FinOps 审批（持久 CTA + toast 二者其一即可，此处锚 CTA）
    const cta = await screen.findByTestId('guard-goto-finops');
    expect(cta).toHaveAttribute('href', '/finops');
  });

  it('promote 503 → 未启用 banner', async () => {
    vi.spyOn(guardModule.guardApi, 'promoteModel').mockRejectedValue(
      new ApiError('unavailable', 503, 'AEGIS-6008'),
    );
    renderGuard();
    const u = userEvent.setup();
    await u.click(screen.getByTestId('guard-promote-toggle'));
    await u.type(screen.getByTestId('guard-promote-model-id'), 'm-1');
    await u.type(screen.getByTestId('guard-promote-version'), '3');
    await u.click(screen.getByTestId('guard-promote-submit'));
    await u.click(screen.getByTestId('guard-promote-confirm'));
    expect(await screen.findByTestId('guard-not-wired-banner')).toBeInTheDocument();
  });

  it('SR-2：viewer 经 RoleGuard → role-denied', () => {
    const viewer: UserInfo = { user_id: 'v', tenant_id: 't1', role: 'viewer' } as UserInfo;
    render(
      <AuthContext.Provider value={authValue(viewer)}>
        <ToastProvider>
          <MemoryRouter initialEntries={['/guard']}>
            <RoleGuard role="tenant_admin"><Guard /></RoleGuard>
          </MemoryRouter>
        </ToastProvider>
      </AuthContext.Provider>,
    );
    expect(screen.getByTestId('role-denied')).toBeInTheDocument();
    expect(screen.queryByTestId('guard-search-input')).not.toBeInTheDocument();
  });
});

// Epic 4.1 — guard namespace 双语 key 完整对齐（防漏译）。
describe('guard i18n key parity', () => {
  function keyPaths(obj: Record<string, unknown>, prefix = ''): string[] {
    return Object.entries(obj).flatMap(([k, v]) => {
      const path = prefix ? `${prefix}.${k}` : k;
      return v && typeof v === 'object'
        ? keyPaths(v as Record<string, unknown>, path)
        : [path];
    });
  }

  it('zh-CN 与 en-US 的 guard key 集合一致', () => {
    const zh = keyPaths(zhGuard).sort();
    const en = keyPaths(enGuard).sort();
    expect(zh).toEqual(en);
  });
});
