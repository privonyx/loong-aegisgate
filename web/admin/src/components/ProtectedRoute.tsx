import { Navigate } from 'react-router-dom';
import { useAuth } from '../hooks/useAuth';

export default function ProtectedRoute({ children }: { children: React.ReactNode }) {
  const { user, loading } = useAuth();

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="animate-spin rounded-full h-8 w-8 border-2 border-primary border-t-transparent" />
      </div>
    );
  }

  if (!user) return <Navigate to="/login" replace />;
  // TASK-20260604-01 P0-F：SSO 登录后 MFA 未验证 → 跳挑战页（不放行业务路由）。
  if (user.mfa_pending) return <Navigate to="/mfa-challenge" replace />;

  return <>{children}</>;
}
