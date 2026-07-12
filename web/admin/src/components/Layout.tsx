import { useState, Fragment, type ReactNode } from 'react';
import { NavLink, useNavigate } from 'react-router-dom';
import {
  PanelLeftClose, PanelLeft,
  Moon, Sun, LogOut, ChevronRight, ShieldCheck, Languages,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../hooks/useAuth';
import { useTheme } from '../hooks/useTheme';
import { useLanguage } from '../hooks/useLanguage';
import type { Lang } from '../i18n';
import { NAV_GROUPS, filterNavGroups } from '../lib/nav';

function useSidebarState() {
  const [collapsed, setCollapsed] = useState(() =>
    localStorage.getItem('sidebar-collapsed') === 'true'
  );
  const toggle = () => {
    const next = !collapsed;
    setCollapsed(next);
    localStorage.setItem('sidebar-collapsed', String(next));
  };
  return { collapsed, toggle };
}

export default function Layout({ children }: { children: ReactNode }) {
  const { user, logout } = useAuth();
  const { theme, toggle: toggleTheme } = useTheme();
  const { collapsed, toggle: toggleSidebar } = useSidebarState();
  const { t } = useTranslation();
  const { lang, setLang, languages } = useLanguage();
  const navigate = useNavigate();

  const handleLogout = async () => {
    await logout();
    navigate('/login');
  };

  return (
    <div className="flex h-full">
      {/* Sidebar */}
      <aside
        className="flex flex-col border-r border-border bg-sidebar shrink-0 transition-[width] duration-200"
        style={{ width: collapsed ? 64 : 240 }}
      >
        <div className="flex items-center h-14 px-4 border-b border-border">
          {!collapsed && (
            <span className="text-primary font-bold text-lg tracking-tight">AegisGate</span>
          )}
          {collapsed && (
            <span className="text-primary font-bold text-lg mx-auto">AG</span>
          )}
        </div>

        <nav className="flex-1 py-2 overflow-y-auto">
          {filterNavGroups(NAV_GROUPS, user?.role).map((group, groupIdx) => (
            <Fragment key={group.id}>
              {/* 分组分隔：展开态显示分类标题；折叠态显示分隔线（D2=a / D3=a）*/}
              {!collapsed ? (
                <div
                  className={`px-4 ${groupIdx === 0 ? 'pt-2' : 'pt-4'} pb-1 text-[11px] font-semibold uppercase tracking-wider text-muted/70 select-none`}
                >
                  {t(group.labelKey, { ns: 'nav' })}
                </div>
              ) : (
                groupIdx > 0 && <div className="my-2 mx-3 border-t border-border/60" />
              )}

              <div className="space-y-0.5">
                {group.items.map(({ to, icon: Icon, labelKey }) => {
                  const label = t(labelKey, { ns: 'nav' });
                  return (
                  <NavLink
                    key={to}
                    to={to}
                    end={to === '/'}
                    title={collapsed ? label : undefined}
                    className={({ isActive }) =>
                      `flex items-center gap-3 px-4 py-2.5 text-sm transition-colors
                       ${isActive ? 'text-primary bg-primary/10 border-r-2 border-primary' : 'text-muted hover:text-fg hover:bg-card'}`
                    }
                  >
                    <Icon size={18} />
                    {!collapsed && <span>{label}</span>}
                  </NavLink>
                  );
                })}
              </div>
            </Fragment>
          ))}
        </nav>

        <button
          onClick={toggleSidebar}
          className="flex items-center gap-3 px-4 py-3 text-sm text-muted hover:text-fg border-t border-border transition-colors"
        >
          {collapsed ? <PanelLeft size={18} /> : <PanelLeftClose size={18} />}
          {!collapsed && <span>{t('layout.collapse')}</span>}
        </button>
      </aside>

      {/* Main content */}
      <div className="flex-1 flex flex-col min-w-0">
        {/* Top bar */}
        <header className="flex items-center justify-between h-14 px-6 border-b border-border bg-card shrink-0">
          <div className="flex items-center gap-1.5 text-sm text-muted">
            <span>{t('layout.adminPanel')}</span>
            <ChevronRight size={14} />
          </div>

          <div className="flex items-center gap-3">
            <div className="flex items-center gap-1 text-muted" title={t('layout.language')}>
              <Languages size={16} />
              <select
                data-testid="lang-switcher"
                aria-label={t('layout.language')}
                value={lang}
                onChange={(e) => setLang(e.target.value as Lang)}
                className="bg-transparent text-sm text-muted hover:text-fg focus:outline-none cursor-pointer"
              >
                {languages.map((l) => (
                  <option key={l} value={l} className="bg-card text-fg">
                    {l === 'zh-CN' ? '中文' : 'English'}
                  </option>
                ))}
              </select>
            </div>

            <button
              onClick={toggleTheme}
              className="p-2 rounded-md text-muted hover:text-fg hover:bg-bg transition-colors"
              title={theme === 'dark' ? t('layout.toggleLight') : t('layout.toggleDark')}
            >
              {theme === 'dark' ? <Sun size={16} /> : <Moon size={16} />}
            </button>

            {user && (
              <span className="text-sm text-muted">
                {user.user_id} <span className="text-xs opacity-60">({user.role})</span>
              </span>
            )}

            <NavLink
              to="/account/security"
              className="p-2 rounded-md text-muted hover:text-fg hover:bg-bg transition-colors"
              title={t('layout.accountSecurity')}
            >
              <ShieldCheck size={16} />
            </NavLink>

            <button
              onClick={handleLogout}
              className="p-2 rounded-md text-muted hover:text-danger hover:bg-danger/10 transition-colors"
              title={t('layout.logout')}
            >
              <LogOut size={16} />
            </button>
          </div>
        </header>

        <main className="flex-1 overflow-auto p-6">
          {children}
        </main>
      </div>
    </div>
  );
}
