import { useState } from 'react';
import { ShieldCheck, Loader2, Copy, AlertTriangle, KeyRound } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { MfaSetupResult } from '../types';

type MfaStage = 'idle' | 'setup' | 'enabled';

export default function AccountSecurity() {
  const [stage, setStage] = useState<MfaStage>('idle');
  // SR-2：setupData（含明文 secret + recovery_codes）仅在 'setup' 阶段驻留组件内存，
  // verify 成功后立即清空；绝不写入 localStorage / sessionStorage。
  const [setupData, setSetupData] = useState<MfaSetupResult | null>(null);
  const [code, setCode] = useState('');
  const [disableCode, setDisableCode] = useState('');
  const [busy, setBusy] = useState(false);
  const { toast } = useToast();
  const { t } = useTranslation('account');

  const handleSetup = async () => {
    setBusy(true);
    try {
      const res = await api.mfaSetup();
      setSetupData(res);
      setStage('setup');
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.setupFailed'));
    } finally {
      setBusy(false);
    }
  };

  const handleVerify = async () => {
    setBusy(true);
    try {
      await api.mfaVerify(code);
      // SR-2：验证成功 → 清空内存中的明文 secret / recovery 码。
      setSetupData(null);
      setCode('');
      setStage('enabled');
      toast('success', t('toast.enabled'));
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.verifyInvalid'));
    } finally {
      setBusy(false);
    }
  };

  const handleDisable = async () => {
    setBusy(true);
    try {
      await api.mfaDisable(disableCode);
      setDisableCode('');
      setStage('idle');
      toast('success', t('toast.disabled'));
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.disableFailed'));
    } finally {
      setBusy(false);
    }
  };

  const copy = (text: string) => {
    navigator.clipboard?.writeText(text).then(
      () => toast('success', t('toast.copied')),
      () => toast('error', t('toast.copyFailed')),
    );
  };

  return (
    <div className="space-y-4 max-w-2xl">
      <h1 className="text-xl font-semibold flex items-center gap-2"><ShieldCheck size={20} /> {t('title')}</h1>

      {stage === 'idle' && (
        <div className="bg-card border border-border rounded-lg p-6 space-y-4">
          <p className="text-sm text-muted">{t('idle.desc')}</p>
          <button
            onClick={handleSetup}
            disabled={busy}
            className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors"
          >
            {busy ? <Loader2 size={16} className="animate-spin" /> : <KeyRound size={16} />} {t('idle.enable')}
          </button>
        </div>
      )}

      {stage === 'setup' && setupData && (
        <div className="bg-card border border-border rounded-lg p-6 space-y-4">
          <div className="flex items-start gap-3 bg-warning/10 text-warning rounded-md p-3 text-sm">
            <AlertTriangle size={18} className="shrink-0 mt-0.5" />
            <span>{t('setup.warning')}</span>
          </div>

          <div>
            <p className="text-sm text-muted mb-1">{t('setup.secretLabel')}</p>
            <div className="flex items-center gap-2">
              <code className="bg-bg/50 border border-border rounded px-3 py-1.5 font-mono text-sm">{setupData.secret}</code>
              <button onClick={() => copy(setupData.secret)} className="p-1.5 rounded text-muted hover:text-primary hover:bg-primary/10"><Copy size={14} /></button>
            </div>
          </div>

          <div>
            <p className="text-sm text-muted mb-1">{t('setup.uriLabel')}</p>
            <div className="flex items-center gap-2">
              <code className="bg-bg/50 border border-border rounded px-3 py-1.5 font-mono text-xs break-all flex-1">{setupData.qr_uri}</code>
              <button onClick={() => copy(setupData.qr_uri)} className="p-1.5 rounded text-muted hover:text-primary hover:bg-primary/10 shrink-0"><Copy size={14} /></button>
            </div>
          </div>

          <div>
            <p className="text-sm text-muted mb-1">{t('setup.recoveryLabel')}</p>
            <ul className="grid grid-cols-2 gap-1.5">
              {setupData.recovery_codes.map(rc => (
                <li key={rc} className="bg-bg/50 border border-border rounded px-2 py-1 font-mono text-xs">{rc}</li>
              ))}
            </ul>
          </div>

          <div className="pt-2 border-t border-border">
            <label className="block text-sm text-muted mb-1">{t('setup.codeLabel')}</label>
            <div className="flex items-center gap-2">
              <input aria-label={t('setup.codeAria')} value={code} onChange={e => setCode(e.target.value)} className="w-40" inputMode="numeric" />
              <button
                data-testid="verify-mfa"
                onClick={handleVerify}
                disabled={busy || !code}
                className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors"
              >
                {busy && <Loader2 size={14} className="animate-spin" />} {t('setup.verifyEnable')}
              </button>
            </div>
          </div>
        </div>
      )}

      {stage === 'enabled' && (
        <div className="bg-card border border-border rounded-lg p-6">
          <div className="flex items-center gap-2 text-success text-sm font-medium">
            <ShieldCheck size={18} /> {t('enabled.label')}
          </div>
        </div>
      )}

      {/* 禁用区：idle（已启用用户）/ enabled 状态均可禁用，需当前验证码。 */}
      {stage !== 'setup' && (
        <div className="bg-card border border-border rounded-lg p-6 space-y-3">
          <p className="text-sm text-muted">{t('disable.desc')}</p>
          <div className="flex items-center gap-2">
            <input aria-label={t('disable.codeAria')} value={disableCode} onChange={e => setDisableCode(e.target.value)} className="w-40" inputMode="numeric" />
            <button
              data-testid="disable-mfa"
              onClick={handleDisable}
              disabled={busy || !disableCode}
              className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md border border-danger text-danger hover:bg-danger/10 disabled:opacity-50 transition-colors"
            >
              {busy && <Loader2 size={14} className="animate-spin" />} {t('disable.button')}
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
