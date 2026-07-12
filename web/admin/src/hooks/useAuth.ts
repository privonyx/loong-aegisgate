import { useState, useEffect, useCallback, createContext, useContext } from 'react';
import type { UserInfo } from '../types';
import { api } from '../api/client';

interface AuthState {
  user: UserInfo | null;
  loading: boolean;
  error: string | null;
  login: (apiKey: string) => Promise<void>;
  logout: () => Promise<void>;
  // TASK-20260604-01 P0-F：MFA 挑战完成后重新拉取 /me 刷新 mfa_pending 态。
  refresh: () => Promise<void>;
}

const AuthContext = createContext<AuthState | null>(null);

export function useAuthProvider(): AuthState {
  const [user, setUser] = useState<UserInfo | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    api.me()
      .then(setUser)
      .catch(() => setUser(null))
      .finally(() => setLoading(false));
  }, []);

  const login = useCallback(async (apiKey: string) => {
    setError(null);
    setLoading(true);
    try {
      const info = await api.login(apiKey);
      setUser(info);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Login failed');
      throw e;
    } finally {
      setLoading(false);
    }
  }, []);

  const logout = useCallback(async () => {
    try {
      await api.logout();
    } finally {
      setUser(null);
    }
  }, []);

  const refresh = useCallback(async () => {
    try {
      const info = await api.me();
      setUser(info);
    } catch {
      setUser(null);
    }
  }, []);

  return { user, loading, error, login, logout, refresh };
}

export { AuthContext };

export function useAuth(): AuthState {
  const ctx = useContext(AuthContext);
  if (!ctx) throw new Error('useAuth must be used within AuthProvider');
  return ctx;
}
