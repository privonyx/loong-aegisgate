import type { ReactNode } from 'react';
import { ShieldX } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../hooks/useAuth';
import { hasRole, type Role } from '../lib/roles';

// SR-3：SuperAdmin-only（或其它最小角色）页面的 UI 守卫。
// 角色不足时渲染「无权限」面板，避免无意义请求 + 引导用户；
// 但后端 RBAC（requireRole → 403）仍是唯一真防线。
export default function RoleGuard({ role, children }: { role: Role; children: ReactNode }) {
  const { user } = useAuth();
  const { t } = useTranslation();
  if (!hasRole(user?.role, role)) {
    return (
      <div className="flex flex-col items-center justify-center h-64 text-center gap-3" data-testid="role-denied">
        <ShieldX size={40} className="text-danger" />
        <p className="text-sm text-muted">{t('roleGuard.denied')}</p>
      </div>
    );
  }
  return <>{children}</>;
}
