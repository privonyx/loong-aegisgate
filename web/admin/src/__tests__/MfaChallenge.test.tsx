// TASK-20260604-01 P0-F — MFA 登录挑战页测试（verify + recovery + 跳转）。
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { MemoryRouter, Routes, Route } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo } from '../types';
import MfaChallenge from '../pages/MfaChallenge';

const mfaVerify = vi.fn();
const mfaRecovery = vi.fn();

vi.mock('../api/client', () => ({
  api: {
    mfaVerify: (code: string) => mfaVerify(code),
    mfaRecovery: (code: string) => mfaRecovery(code),
  },
}));

function mockAuth(user: UserInfo | null, refresh = vi.fn().mockResolvedValue(undefined)) {
  return {
    user,
    loading: false,
    error: null as string | null,
    login: vi.fn().mockResolvedValue(undefined),
    logout: vi.fn().mockResolvedValue(undefined),
    refresh,
  };
}

function renderChallenge(auth = mockAuth({ user_id: 'u1', tenant_id: 't1', role: 'admin', mfa_pending: true })) {
  return render(
    <AuthContext.Provider value={auth}>
      <MemoryRouter initialEntries={['/mfa-challenge']}>
        <Routes>
          <Route path="/mfa-challenge" element={<MfaChallenge />} />
          <Route path="/" element={<div data-testid="home">Home</div>} />
          <Route path="/login" element={<div data-testid="login">Login</div>} />
        </Routes>
      </MemoryRouter>
    </AuthContext.Provider>,
  );
}

describe('MfaChallenge', () => {
  beforeEach(() => {
    mfaVerify.mockReset().mockResolvedValue({ verified: true });
    mfaRecovery.mockReset().mockResolvedValue({ verified: true, remaining_codes: 4 });
  });

  it('renders the verify form when mfa is pending', () => {
    renderChallenge();
    expect(screen.getByText('多因素验证')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '验 证' })).toBeInTheDocument();
  });

  it('calls mfaVerify and navigates home on success', async () => {
    const user = userEvent.setup();
    const refresh = vi.fn().mockResolvedValue(undefined);
    renderChallenge(mockAuth({ user_id: 'u1', tenant_id: 't1', role: 'admin', mfa_pending: true }, refresh));
    await user.type(screen.getByPlaceholderText('000000'), '123456');
    await user.click(screen.getByRole('button', { name: '验 证' }));
    expect(mfaVerify).toHaveBeenCalledWith('123456');
    expect(refresh).toHaveBeenCalled();
    expect(await screen.findByTestId('home')).toBeInTheDocument();
  });

  it('switches to recovery mode and calls mfaRecovery', async () => {
    const user = userEvent.setup();
    renderChallenge();
    await user.click(screen.getByRole('button', { name: '使用恢复码' }));
    await user.type(screen.getByPlaceholderText('请输入恢复码'), 'recovery-xyz');
    await user.click(screen.getByRole('button', { name: '验 证' }));
    expect(mfaRecovery).toHaveBeenCalledWith('recovery-xyz');
  });

  it('shows error message on verify failure', async () => {
    const user = userEvent.setup();
    mfaVerify.mockRejectedValue(new Error('Invalid code'));
    renderChallenge();
    await user.type(screen.getByPlaceholderText('000000'), '000000');
    await user.click(screen.getByRole('button', { name: '验 证' }));
    expect(await screen.findByText('Invalid code')).toBeInTheDocument();
  });

  it('redirects to /login when no session', () => {
    renderChallenge(mockAuth(null));
    expect(screen.getByTestId('login')).toBeInTheDocument();
  });

  it('redirects home when mfa no longer pending', () => {
    renderChallenge(mockAuth({ user_id: 'u1', tenant_id: 't1', role: 'admin', mfa_pending: false }));
    expect(screen.getByTestId('home')).toBeInTheDocument();
  });
});
