import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo } from '../types';
import Login from '../pages/Login';

function mockAuth(overrides: Partial<ReturnType<typeof createMockAuth>> = {}) {
  return { ...createMockAuth(), ...overrides };
}

function createMockAuth() {
  return {
    user: null as UserInfo | null,
    loading: false,
    error: null as string | null,
    login: vi.fn().mockResolvedValue(undefined),
    logout: vi.fn().mockResolvedValue(undefined),
    refresh: vi.fn().mockResolvedValue(undefined),
  };
}

function renderLogin(auth = mockAuth()) {
  return render(
    <AuthContext.Provider value={auth}>
      <MemoryRouter initialEntries={['/login']}>
        <Login />
      </MemoryRouter>
    </AuthContext.Provider>,
  );
}

describe('Login', () => {
  it('renders the login form', () => {
    renderLogin();
    expect(screen.getByText('AegisGate')).toBeInTheDocument();
    expect(screen.getByPlaceholderText('请输入 API Key')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '登 录' })).toBeInTheDocument();
  });

  it('disables button when input is empty', () => {
    renderLogin();
    expect(screen.getByRole('button', { name: '登 录' })).toBeDisabled();
  });

  it('enables button when API key is entered', async () => {
    const user = userEvent.setup();
    renderLogin();
    await user.type(screen.getByPlaceholderText('请输入 API Key'), 'test-key');
    expect(screen.getByRole('button', { name: '登 录' })).toBeEnabled();
  });

  it('calls login on form submit', async () => {
    const user = userEvent.setup();
    const auth = mockAuth();
    renderLogin(auth);
    await user.type(screen.getByPlaceholderText('请输入 API Key'), 'my-api-key');
    await user.click(screen.getByRole('button', { name: '登 录' }));
    expect(auth.login).toHaveBeenCalledWith('my-api-key');
  });

  // TASK-20260604-01 P0-F：SSO 登录入口。
  it('renders the SSO login button', () => {
    renderLogin();
    expect(screen.getByRole('button', { name: '使用 SSO 登录' })).toBeInTheDocument();
  });

  it('shows error message on login failure', async () => {
    const user = userEvent.setup();
    const auth = mockAuth({
      login: vi.fn().mockRejectedValue(new Error('Invalid API key')),
    });
    renderLogin(auth);
    await user.type(screen.getByPlaceholderText('请输入 API Key'), 'bad-key');
    await user.click(screen.getByRole('button', { name: '登 录' }));
    expect(await screen.findByText('Invalid API key')).toBeInTheDocument();
  });
});
