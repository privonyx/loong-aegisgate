import { useState, type FormEvent } from 'react';
import { Navigate, useNavigate } from 'react-router-dom';
import { ShieldCheck, Loader2, KeyRound } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useAuth } from '../hooks/useAuth';
import { api } from '../api/client';

// TASK-20260604-01 P0-F：SSO 登录后 MFA 挑战页。后端 me 返回 mfa_pending=true 时，
// ProtectedRoute 将用户导向此页；验证成功后 refresh() 重新拉取 me 清除 pending 态。
export default function MfaChallenge() {
  const { user, loading, refresh } = useAuth();
  const [code, setCode] = useState('');
  const [useRecovery, setUseRecovery] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState('');
  const { t } = useTranslation('auth');
  const navigate = useNavigate();

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="animate-spin rounded-full h-8 w-8 border-2 border-primary border-t-transparent" />
      </div>
    );
  }

  // 无 session → 回登录页；已不再 pending（已验证 / 不需 MFA）→ 回首页。
  if (!user) return <Navigate to="/login" replace />;
  if (!user.mfa_pending) return <Navigate to="/" replace />;

  const handleSubmit = async (e: FormEvent) => {
    e.preventDefault();
    if (!code.trim()) return;
    setSubmitting(true);
    setError('');
    try {
      if (useRecovery) {
        await api.mfaRecovery(code.trim());
      } else {
        await api.mfaVerify(code.trim());
      }
      await refresh();
      navigate('/', { replace: true });
    } catch (err) {
      setError(err instanceof Error ? err.message : t('mfa.failed'));
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <div className="flex items-center justify-center h-full bg-bg">
      <div className="w-full max-w-sm">
        <div className="flex flex-col items-center mb-8">
          <div className="p-3 rounded-xl bg-primary/10 text-primary mb-4">
            <ShieldCheck size={32} />
          </div>
          <h1 className="text-2xl font-bold tracking-tight">{t('mfa.title')}</h1>
          <p className="text-sm text-muted mt-1">
            {useRecovery ? t('mfa.promptRecovery') : t('mfa.promptCode')}
          </p>
        </div>

        <form onSubmit={handleSubmit} className="bg-card border border-border rounded-lg p-6 space-y-4">
          <div>
            <label className="block text-sm text-muted mb-1.5">
              {useRecovery ? t('mfa.labelRecovery') : t('mfa.labelCode')}
            </label>
            <div className="relative">
              <KeyRound size={16} className="absolute left-3 top-1/2 -translate-y-1/2 text-muted" />
              <input
                type="text"
                inputMode={useRecovery ? 'text' : 'numeric'}
                value={code}
                onChange={e => setCode(e.target.value)}
                placeholder={useRecovery ? t('mfa.recoveryPlaceholder') : '000000'}
                className="w-full pl-10"
                autoFocus
                disabled={submitting}
              />
            </div>
          </div>

          {error && (
            <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>
          )}

          <button
            type="submit"
            disabled={submitting || !code.trim()}
            className="w-full flex items-center justify-center gap-2 py-2.5 rounded-md bg-primary text-white font-medium text-sm hover:bg-primary/90 disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
          >
            {submitting ? <Loader2 size={16} className="animate-spin" /> : null}
            {submitting ? t('mfa.submitting') : t('mfa.submit')}
          </button>

          <button
            type="button"
            onClick={() => { setUseRecovery(v => !v); setCode(''); setError(''); }}
            className="w-full text-xs text-muted hover:text-fg transition-colors"
          >
            {useRecovery ? t('mfa.useAuthenticator') : t('mfa.useRecovery')}
          </button>
        </form>
      </div>
    </div>
  );
}
