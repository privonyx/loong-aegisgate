import { useEffect, useState, useCallback } from 'react';
import { Plus, Pencil, Trash2, X, Loader2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import DataTable, { type Column } from '../components/DataTable';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { SsoProvider } from '../types';

const PAGE_SIZE = 50;

export default function Sso() {
  const [data, setData] = useState<SsoProvider[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [showForm, setShowForm] = useState(false);
  const [editing, setEditing] = useState<SsoProvider | null>(null);
  const [deleting, setDeleting] = useState<SsoProvider | null>(null);
  const { toast } = useToast();
  const { t } = useTranslation('sso');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listSsoProviders(PAGE_SIZE, page * PAGE_SIZE);
      setData(res.data);
      setTotal(res.total ?? res.count);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [page, toast, t]);

  useEffect(() => { load(); }, [load]);

  const handleDelete = async () => {
    if (!deleting) return;
    try {
      await api.deleteSsoProvider(deleting.id);
      toast('success', t('toast.deleted'));
      setDeleting(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.deleteFailed'));
    }
  };

  const columns: Column<SsoProvider>[] = [
    { key: 'name', header: t('col.name') },
    { key: 'issuer_url', header: 'Issuer URL', render: (r) => <span className="text-muted text-xs">{r.issuer_url}</span> },
    {
      key: 'has_client_secret', header: t('col.secret'),
      render: (r) => (
        <span className={`px-2 py-0.5 rounded text-xs font-medium ${
          r.has_client_secret ? 'bg-success/10 text-success' : 'bg-warning/10 text-warning'
        }`}>{r.has_client_secret ? t('secretConfigured') : t('secretMissing')}</span>
      ),
    },
    {
      key: 'enabled', header: t('col.status'),
      render: (r) => (
        <span className={`px-2 py-0.5 rounded text-xs font-medium ${
          r.enabled ? 'bg-success/10 text-success' : 'bg-muted/20 text-muted'
        }`}>{r.enabled ? t('enabled') : t('disabled')}</span>
      ),
    },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <button
          onClick={() => { setEditing(null); setShowForm(true); }}
          className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors"
        >
          <Plus size={16} /> {t('newConfig')}
        </button>
      </div>

      <DataTable
        columns={columns}
        data={data}
        total={total}
        page={page}
        pageSize={PAGE_SIZE}
        onPageChange={setPage}
        loading={loading}
        actions={(row) => (
          <div className="flex items-center gap-1 justify-end">
            <button title={t('common:actions.edit')} onClick={() => { setEditing(row); setShowForm(true); }} className="p-1.5 rounded text-muted hover:text-primary hover:bg-primary/10"><Pencil size={14} /></button>
            <button title={t('common:actions.delete')} onClick={() => setDeleting(row)} className="p-1.5 rounded text-muted hover:text-danger hover:bg-danger/10"><Trash2 size={14} /></button>
          </div>
        )}
      />

      {showForm && <SsoForm provider={editing} onClose={() => setShowForm(false)} onSaved={() => { setShowForm(false); load(); }} />}

      <ConfirmDialog
        open={!!deleting}
        title={t('deleteTitle')}
        message={t('deleteConfirm', { name: deleting?.name ?? '' })}
        confirmLabel={t('common:actions.delete')}
        confirmTestId="confirm-delete"
        danger
        onConfirm={handleDelete}
        onCancel={() => setDeleting(null)}
      />
    </div>
  );
}

function SsoForm({ provider, onClose, onSaved }: { provider: SsoProvider | null; onClose: () => void; onSaved: () => void }) {
  const [name, setName] = useState(provider?.name ?? '');
  const [issuerUrl, setIssuerUrl] = useState(provider?.issuer_url ?? '');
  const [clientId, setClientId] = useState(provider?.client_id ?? '');
  // SR-1：client_secret 永远以空值起始（即使编辑态），后端绝不回显 secret 明文。
  const [clientSecret, setClientSecret] = useState('');
  const [redirectUri, setRedirectUri] = useState(provider?.redirect_uri ?? '');
  const [scopes, setScopes] = useState(provider?.scopes?.join(', ') ?? 'openid, profile, email');
  const [defaultRole, setDefaultRole] = useState(provider?.default_role ?? 'viewer');
  const [jit, setJit] = useState(provider?.jit_provisioning ?? false);
  const [enabled, setEnabled] = useState(provider?.enabled ?? true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const { toast } = useToast();
  const { t } = useTranslation('sso');

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    const body: Partial<SsoProvider> & { client_secret?: string } = {
      name,
      issuer_url: issuerUrl,
      client_id: clientId,
      redirect_uri: redirectUri,
      scopes: scopes.split(',').map(s => s.trim()).filter(Boolean),
      default_role: defaultRole,
      jit_provisioning: jit,
      enabled,
    };
    // SR-1：仅在用户填写了新 secret 时才提交 client_secret（空 = 保持不变）。
    if (clientSecret) body.client_secret = clientSecret;
    try {
      if (provider) {
        await api.updateSsoProvider(provider.id, body);
        toast('success', t('toast.updated'));
      } else {
        await api.createSsoProvider(body);
        toast('success', t('toast.created'));
      }
      onSaved();
    } catch (e) {
      setError(e instanceof Error ? e.message : t('toast.saveFailed'));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div className="bg-card border border-border rounded-lg w-full max-w-lg p-6 max-h-[90vh] overflow-auto" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{provider ? t('editConfig') : t('newConfig')}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>

        <form onSubmit={handleSubmit} className="space-y-3">
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.name')} *</label>
            <input aria-label={t('form.name')} value={name} onChange={e => setName(e.target.value)} className="w-full" required />
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">Issuer URL *</label>
            <input aria-label="Issuer URL" value={issuerUrl} onChange={e => setIssuerUrl(e.target.value)} className="w-full" required />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">Client ID</label>
              <input aria-label="Client ID" value={clientId} onChange={e => setClientId(e.target.value)} className="w-full" />
            </div>
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.clientSecret')}</label>
              <input
                aria-label={t('form.clientSecret')}
                type="password"
                value={clientSecret}
                onChange={e => setClientSecret(e.target.value)}
                placeholder={provider ? t('form.secretPlaceholder') : ''}
                className="w-full"
                autoComplete="new-password"
              />
            </div>
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">Redirect URI</label>
            <input aria-label="Redirect URI" value={redirectUri} onChange={e => setRedirectUri(e.target.value)} className="w-full" />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.scopes')}</label>
              <input aria-label="Scopes" value={scopes} onChange={e => setScopes(e.target.value)} className="w-full" />
            </div>
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.defaultRole')}</label>
              <input aria-label={t('form.defaultRole')} value={defaultRole} onChange={e => setDefaultRole(e.target.value)} className="w-full" />
            </div>
          </div>
          <div className="flex items-center gap-6">
            <label className="flex items-center gap-2 text-sm text-muted">
              <input type="checkbox" checked={jit} onChange={e => setJit(e.target.checked)} />
              {t('form.jit')}
            </label>
            <label className="flex items-center gap-2 text-sm text-muted">
              <input type="checkbox" checked={enabled} onChange={e => setEnabled(e.target.checked)} />
              {t('form.enabledLabel')}
            </label>
          </div>

          {error && <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>}

          <div className="flex justify-end gap-3 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('common:actions.cancel')}</button>
            <button type="submit" data-testid="submit-sso" disabled={saving} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors">
              {saving && <Loader2 size={14} className="animate-spin" />}
              {provider ? t('common:actions.save') : t('create')}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
