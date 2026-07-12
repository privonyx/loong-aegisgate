// TASK-20260603-01 Epic 5 — MFA 账户安全页 + SR-2（secret/recovery 一次性展示）测试。
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ToastProvider } from '../components/Toast';
import type { MfaSetupResult } from '../types';

vi.mock('../api/client', () => ({
  api: {
    mfaSetup: vi.fn(),
    mfaVerify: vi.fn(),
    mfaDisable: vi.fn(),
  },
}));

import { api } from '../api/client';
import AccountSecurity from '../pages/AccountSecurity';

const setupResult: MfaSetupResult = {
  secret: 'JBSWY3DPEHPK3PXP',
  qr_uri: 'otpauth://totp/AegisGate:alice?secret=JBSWY3DPEHPK3PXP&issuer=AegisGate',
  recovery_codes: ['rec-aaa-111', 'rec-bbb-222'],
};

function renderPage() {
  return render(<ToastProvider><AccountSecurity /></ToastProvider>);
}

describe('AccountSecurity (MFA) 页', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(api.mfaSetup).mockResolvedValue(setupResult);
    vi.mocked(api.mfaVerify).mockResolvedValue({ verified: true, mfa_enabled: true });
    vi.mocked(api.mfaDisable).mockResolvedValue({ disabled: true });
  });

  it('点击启用 MFA 调用 mfaSetup 并展示 secret + 恢复码 + 保存提示', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: /启用 MFA/ }));
    await waitFor(() => expect(api.mfaSetup).toHaveBeenCalled());
    expect(await screen.findByText('JBSWY3DPEHPK3PXP')).toBeInTheDocument();
    expect(screen.getByText('rec-aaa-111')).toBeInTheDocument();
    expect(screen.getByText(/立即保存/)).toBeInTheDocument();
    expect(screen.getByText(/otpauth:\/\//)).toBeInTheDocument();
  });

  it('验证码提交调用 mfaVerify', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: /启用 MFA/ }));
    await screen.findByText('JBSWY3DPEHPK3PXP');
    await user.type(screen.getByLabelText('验证码'), '123456');
    await user.click(screen.getByTestId('verify-mfa'));
    await waitFor(() => expect(api.mfaVerify).toHaveBeenCalledWith('123456'));
  });

  // SR-2：verify 成功后 secret + recovery 码必须从 DOM 移除（不持久展示）。
  it('SR-2：验证成功后 secret 与恢复码不再可见', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: /启用 MFA/ }));
    await screen.findByText('JBSWY3DPEHPK3PXP');
    await user.type(screen.getByLabelText('验证码'), '123456');
    await user.click(screen.getByTestId('verify-mfa'));
    await waitFor(() => expect(screen.queryByText('JBSWY3DPEHPK3PXP')).not.toBeInTheDocument());
    expect(screen.queryByText('rec-aaa-111')).not.toBeInTheDocument();
    expect(screen.getAllByText(/MFA 已启用/).length).toBeGreaterThanOrEqual(1);
  });

  it('禁用 MFA 调用 mfaDisable', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.type(screen.getByLabelText('禁用验证码'), '654321');
    await user.click(screen.getByTestId('disable-mfa'));
    await waitFor(() => expect(api.mfaDisable).toHaveBeenCalledWith('654321'));
  });
});
