import { render, screen } from '@testing-library/react';
import { MemoryRouter, Routes, Route } from 'react-router-dom';
import { AuthContext } from '../hooks/useAuth';
import type { UserInfo } from '../types';
import ProtectedRoute from '../components/ProtectedRoute';

function createMockAuth(user: UserInfo | null, loading = false) {
  return {
    user,
    loading,
    error: null as string | null,
    login: vi.fn().mockResolvedValue(undefined),
    logout: vi.fn().mockResolvedValue(undefined),
    refresh: vi.fn().mockResolvedValue(undefined),
  };
}

function renderWithAuth(user: UserInfo | null, loading = false) {
  return render(
    <AuthContext.Provider value={createMockAuth(user, loading)}>
      <MemoryRouter initialEntries={['/']}>
        <Routes>
          <Route
            path="/"
            element={
              <ProtectedRoute>
                <div data-testid="protected-content">Secret</div>
              </ProtectedRoute>
            }
          />
          <Route path="/login" element={<div data-testid="login-page">Login</div>} />
          <Route path="/mfa-challenge" element={<div data-testid="mfa-challenge-page">MFA</div>} />
        </Routes>
      </MemoryRouter>
    </AuthContext.Provider>,
  );
}

describe('ProtectedRoute', () => {
  it('shows spinner while loading', () => {
    renderWithAuth(null, true);
    expect(screen.queryByTestId('protected-content')).not.toBeInTheDocument();
    expect(screen.queryByTestId('login-page')).not.toBeInTheDocument();
  });

  it('redirects to /login when not authenticated', () => {
    renderWithAuth(null);
    expect(screen.getByTestId('login-page')).toBeInTheDocument();
    expect(screen.queryByTestId('protected-content')).not.toBeInTheDocument();
  });

  it('renders children when authenticated', () => {
    const user: UserInfo = { user_id: 'u1', tenant_id: 't1', role: 'admin' };
    renderWithAuth(user);
    expect(screen.getByTestId('protected-content')).toBeInTheDocument();
    expect(screen.getByText('Secret')).toBeInTheDocument();
  });

  // TASK-20260604-01 P0-F：MFA 待验证态 → 跳挑战页而非放行业务路由。
  it('redirects to /mfa-challenge when mfa is pending', () => {
    const user: UserInfo = { user_id: 'u1', tenant_id: 't1', role: 'admin', mfa_pending: true };
    renderWithAuth(user);
    expect(screen.getByTestId('mfa-challenge-page')).toBeInTheDocument();
    expect(screen.queryByTestId('protected-content')).not.toBeInTheDocument();
  });
});
