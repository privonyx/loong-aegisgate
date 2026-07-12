import { useState, type FormEvent } from 'react';
import { useNavigate } from 'react-router-dom';
import { KeyRound, Shield, Loader2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../hooks/useAuth';

export default function Login() {
  const [apiKey, setApiKey] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const { login } = useAuth();
  const { t } = useTranslation('auth');
  const navigate = useNavigate();

  const handleSubmit = async (e: FormEvent) => {
    e.preventDefault();
    if (!apiKey.trim()) return;
    setLoading(true);
    setError('');
    try {
      await login(apiKey.trim());
      navigate('/', { replace: true });
    } catch (err) {
      setError(err instanceof Error ? err.message : t('login.failed'));
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="flex items-center justify-center h-full bg-bg">
      <div className="w-full max-w-sm">
        <div className="flex flex-col items-center mb-8">
          <div className="p-3 rounded-xl bg-primary/10 text-primary mb-4">
            <Shield size={32} />
          </div>
          <h1 className="text-2xl font-bold tracking-tight">AegisGate</h1>
          <p className="text-sm text-muted mt-1">{t('login.subtitle')}</p>
        </div>

        <form onSubmit={handleSubmit} className="bg-card border border-border rounded-lg p-6 space-y-4">
          <div>
            <label className="block text-sm text-muted mb-1.5">{t('login.apiKeyLabel')}</label>
            <div className="relative">
              <KeyRound size={16} className="absolute left-3 top-1/2 -translate-y-1/2 text-muted" />
              <input
                type="password"
                value={apiKey}
                onChange={e => setApiKey(e.target.value)}
                placeholder={t('login.apiKeyPlaceholder')}
                className="w-full pl-10"
                autoFocus
                disabled={loading}
              />
            </div>
          </div>

          {error && (
            <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>
          )}

          <button
            type="submit"
            disabled={loading || !apiKey.trim()}
            className="w-full flex items-center justify-center gap-2 py-2.5 rounded-md bg-primary text-white font-medium text-sm hover:bg-primary/90 disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
          >
            {loading ? <Loader2 size={16} className="animate-spin" /> : null}
            {loading ? t('login.submitting') : t('login.submit')}
          </button>

          {/* TASK-20260604-01 P0-F：SSO 全页重定向（非 fetch），后端发起 OIDC 授权流。 */}
          <div className="flex items-center gap-3 pt-1">
            <span className="h-px flex-1 bg-border" />
            <span className="text-xs text-muted">{t('login.or')}</span>
            <span className="h-px flex-1 bg-border" />
          </div>
          <button
            type="button"
            onClick={() => { window.location.href = '/admin/auth/sso/login'; }}
            disabled={loading}
            className="w-full flex items-center justify-center gap-2 py-2.5 rounded-md border border-border bg-card text-fg font-medium text-sm hover:bg-bg/50 disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
          >
            {t('login.sso')}
          </button>
        </form>
      </div>
    </div>
  );
}
