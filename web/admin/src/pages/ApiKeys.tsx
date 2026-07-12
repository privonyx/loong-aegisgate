import { useEffect, useState, useCallback } from 'react';
import { Plus, RotateCw, Ban, Copy, Check, X, Loader2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import DataTable, { type Column } from '../components/DataTable';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { ApiKey } from '../types';

const PAGE_SIZE = 20;

export default function ApiKeysPage() {
  const [data, setData] = useState<ApiKey[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [showCreate, setShowCreate] = useState(false);
  const [revoking, setRevoking] = useState<ApiKey | null>(null);
  const [rotating, setRotating] = useState<ApiKey | null>(null);
  const [newKey, setNewKey] = useState<string | null>(null);
  const [filterTenant, setFilterTenant] = useState('');
  const { toast } = useToast();
  const { t } = useTranslation('apikeys');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listApiKeys(filterTenant, PAGE_SIZE, page * PAGE_SIZE);
      setData(res.data);
      setTotal(res.total ?? res.count);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [page, filterTenant, toast, t]);

  useEffect(() => { load(); }, [load]);

  const handleRevoke = async () => {
    if (!revoking) return;
    try {
      await api.revokeApiKey(revoking.id);
      toast('success', t('toast.revoked'));
      setRevoking(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.revokeFailed'));
    }
  };

  const handleRotate = async () => {
    if (!rotating) return;
    try {
      const result = await api.rotateApiKey(rotating.id);
      setNewKey(result.key ?? null);
      toast('success', t('toast.rotated'));
      setRotating(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.rotateFailed'));
    }
  };

  const columns: Column<ApiKey>[] = [
    { key: 'name', header: t('col.name'), render: (r) => r.name || '—' },
    { key: 'key_prefix', header: t('col.prefix'), render: (r) => <span className="font-mono text-xs">{r.key_prefix}...</span> },
    { key: 'tenant_id', header: t('col.tenant'), render: (r) => <span className="font-mono text-xs">{r.tenant_id.slice(0, 8)}...</span> },
    { key: 'role', header: t('col.role'), render: (r) => <span className="px-2 py-0.5 rounded text-xs font-medium bg-primary/10 text-primary">{r.role}</span> },
    {
      key: 'status', header: t('col.status'),
      render: (r) => (
        <span className={`px-2 py-0.5 rounded text-xs font-medium ${
          r.status === 'active' ? 'bg-success/10 text-success' : 'bg-danger/10 text-danger'
        }`}>{r.status}</span>
      ),
    },
    { key: 'created_at', header: t('col.createdAt'), render: (r) => new Date(r.created_at).toLocaleString() },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <div className="flex items-center gap-3">
          <input placeholder={t('filterPlaceholder')} value={filterTenant} onChange={e => { setFilterTenant(e.target.value); setPage(0); }} className="text-sm w-48" />
          <button onClick={() => setShowCreate(true)} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors">
            <Plus size={16} /> {t('createKey')}
          </button>
        </div>
      </div>

      <DataTable
        columns={columns}
        data={data}
        total={total}
        page={page}
        pageSize={PAGE_SIZE}
        onPageChange={setPage}
        loading={loading}
        actions={(row) => row.status === 'active' ? (
          <div className="flex items-center gap-1 justify-end">
            <button onClick={() => setRotating(row)} className="p-1.5 rounded text-muted hover:text-warning hover:bg-warning/10" title={t('action.rotate')}><RotateCw size={14} /></button>
            <button onClick={() => setRevoking(row)} className="p-1.5 rounded text-muted hover:text-danger hover:bg-danger/10" title={t('action.revoke')}><Ban size={14} /></button>
          </div>
        ) : null}
      />

      {showCreate && <CreateKeyForm onClose={() => setShowCreate(false)} onCreated={(key) => { setShowCreate(false); setNewKey(key); load(); }} />}

      {newKey && <KeyRevealDialog keyValue={newKey} onClose={() => setNewKey(null)} />}

      <ConfirmDialog open={!!revoking} title={t('revokeTitle')} message={t('revokeConfirm', { name: revoking?.name || revoking?.key_prefix })} confirmLabel={t('action.revoke')} danger onConfirm={handleRevoke} onCancel={() => setRevoking(null)} />
      <ConfirmDialog open={!!rotating} title={t('rotateTitle')} message={t('rotateConfirm', { name: rotating?.name || rotating?.key_prefix })} confirmLabel={t('action.rotate')} onConfirm={handleRotate} onCancel={() => setRotating(null)} />
    </div>
  );
}

function CreateKeyForm({ onClose, onCreated }: { onClose: () => void; onCreated: (key: string) => void }) {
  const [userId, setUserId] = useState('');
  const [name, setName] = useState('');
  const [role, setRole] = useState('viewer');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const { t } = useTranslation('apikeys');

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    try {
      const result = await api.createApiKey({ user_id: userId, name, role });
      onCreated(result.key ?? '');
    } catch (e) {
      setError(e instanceof Error ? e.message : t('toast.createFailed'));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div className="bg-card border border-border rounded-lg w-full max-w-md p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{t('createTitle')}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>
        <form onSubmit={handleSubmit} className="space-y-3">
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.userId')}</label>
            <input value={userId} onChange={e => setUserId(e.target.value)} className="w-full" required />
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.name')}</label>
            <input value={name} onChange={e => setName(e.target.value)} className="w-full" />
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.role')}</label>
            <select value={role} onChange={e => setRole(e.target.value)} className="w-full">
              <option value="viewer">viewer</option>
              <option value="tenant_admin">tenant_admin</option>
              <option value="super_admin">super_admin</option>
            </select>
          </div>
          {error && <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>}
          <div className="flex justify-end gap-3 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('common:actions.cancel')}</button>
            <button type="submit" disabled={saving} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors">
              {saving && <Loader2 size={14} className="animate-spin" />} {t('create')}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}

function KeyRevealDialog({ keyValue, onClose }: { keyValue: string; onClose: () => void }) {
  const [copied, setCopied] = useState(false);
  const { t } = useTranslation('apikeys');

  const handleCopy = () => {
    navigator.clipboard.writeText(keyValue);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div className="bg-card border border-border rounded-lg w-full max-w-md p-6" onClick={e => e.stopPropagation()}>
        <h2 className="font-semibold mb-2">{t('revealTitle')}</h2>
        <p className="text-sm text-warning mb-4">{t('revealHint')}</p>
        <div className="flex items-center gap-2 bg-bg rounded-md p-3 border border-border">
          <code className="text-sm font-mono flex-1 break-all select-all">{keyValue}</code>
          <button onClick={handleCopy} className="shrink-0 p-1.5 rounded hover:bg-card text-muted hover:text-fg">
            {copied ? <Check size={16} className="text-success" /> : <Copy size={16} />}
          </button>
        </div>
        <div className="flex justify-end mt-4">
          <button onClick={onClose} className="px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors">{t('saved')}</button>
        </div>
      </div>
    </div>
  );
}
