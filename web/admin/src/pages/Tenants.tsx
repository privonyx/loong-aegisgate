import { useEffect, useState, useCallback } from 'react';
import { Plus, Pencil, Trash2, X, Loader2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import DataTable, { type Column } from '../components/DataTable';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { Tenant } from '../types';

const PAGE_SIZE = 20;

export default function Tenants() {
  const { t } = useTranslation('tenants');
  const [data, setData] = useState<Tenant[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [showForm, setShowForm] = useState(false);
  const [editing, setEditing] = useState<Tenant | null>(null);
  const [deleting, setDeleting] = useState<Tenant | null>(null);
  const { toast } = useToast();

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listTenants(PAGE_SIZE, page * PAGE_SIZE);
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
      await api.deleteTenant(deleting.id);
      toast('success', t('toast.deleted'));
      setDeleting(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.deleteFailed'));
    }
  };

  const columns: Column<Tenant>[] = [
    { key: 'name', header: t('table.name') },
    {
      key: 'status', header: t('table.status'),
      render: (r) => (
        <span className={`px-2 py-0.5 rounded text-xs font-medium ${
          r.status === 'active' ? 'bg-success/10 text-success' : 'bg-muted/20 text-muted'
        }`}>{r.status}</span>
      ),
    },
    {
      key: 'model_whitelist', header: t('table.modelWhitelist'),
      render: (r) => r.model_whitelist?.length ? r.model_whitelist.join(', ') : '—',
    },
    {
      key: 'daily_cost_limit', header: t('table.dailyLimit'),
      render: (r) => r.daily_cost_limit >= 0 ? `$${r.daily_cost_limit}` : t('table.unlimited'),
    },
    {
      key: 'rate_limit_tokens', header: t('table.rateLimit'),
      render: (r) => r.rate_limit_tokens >= 0 ? `${r.rate_limit_tokens} tok/s` : t('table.unlimited'),
    },
    { key: 'created_at', header: t('table.createdAt'), render: (r) => new Date(r.created_at).toLocaleString() },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <button
          onClick={() => { setEditing(null); setShowForm(true); }}
          className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors"
        >
          <Plus size={16} /> {t('new')}
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
            <button onClick={() => { setEditing(row); setShowForm(true); }} className="p-1.5 rounded text-muted hover:text-primary hover:bg-primary/10"><Pencil size={14} /></button>
            <button onClick={() => setDeleting(row)} className="p-1.5 rounded text-muted hover:text-danger hover:bg-danger/10"><Trash2 size={14} /></button>
          </div>
        )}
      />

      {showForm && <TenantForm tenant={editing} onClose={() => setShowForm(false)} onSaved={() => { setShowForm(false); load(); }} />}

      <ConfirmDialog
        open={!!deleting}
        title={t('delete.title')}
        message={t('delete.message', { name: deleting?.name })}
        confirmLabel={t('common:actions.delete')}
        danger
        onConfirm={handleDelete}
        onCancel={() => setDeleting(null)}
      />
    </div>
  );
}

function TenantForm({ tenant, onClose, onSaved }: { tenant: Tenant | null; onClose: () => void; onSaved: () => void }) {
  const { t } = useTranslation('tenants');
  const [name, setName] = useState(tenant?.name ?? '');
  const [status, setStatus] = useState(tenant?.status ?? 'active');
  const [modelWhitelist, setModelWhitelist] = useState(tenant?.model_whitelist?.join(', ') ?? '');
  const [dailyCostLimit, setDailyCostLimit] = useState(String(tenant?.daily_cost_limit ?? -1));
  const [monthlyCostLimit, setMonthlyCostLimit] = useState(String(tenant?.monthly_cost_limit ?? -1));
  const [rateLimitTokens, setRateLimitTokens] = useState(String(tenant?.rate_limit_tokens ?? -1));
  const [rateLimitRefill, setRateLimitRefill] = useState(String(tenant?.rate_limit_refill ?? -1));
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const { toast } = useToast();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    const body: Partial<Tenant> = {
      name,
      status,
      model_whitelist: modelWhitelist.split(',').map(s => s.trim()).filter(Boolean),
      daily_cost_limit: Number(dailyCostLimit),
      monthly_cost_limit: Number(monthlyCostLimit),
      rate_limit_tokens: Number(rateLimitTokens),
      rate_limit_refill: Number(rateLimitRefill),
    };
    try {
      if (tenant) {
        await api.updateTenant(tenant.id, body);
        toast('success', t('toast.updated'));
      } else {
        await api.createTenant(body);
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
      <div className="bg-card border border-border rounded-lg w-full max-w-lg p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{tenant ? t('edit') : t('new')}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>

        <form onSubmit={handleSubmit} className="space-y-3">
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.name')}</label>
            <input value={name} onChange={e => setName(e.target.value)} className="w-full" required />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.status')}</label>
              <select value={status} onChange={e => setStatus(e.target.value)} className="w-full">
                <option value="active">active</option>
                <option value="suspended">suspended</option>
              </select>
            </div>
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.modelWhitelist')}</label>
              <input value={modelWhitelist} onChange={e => setModelWhitelist(e.target.value)} className="w-full" />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.dailyCostLimit')}</label>
              <input type="number" step="any" value={dailyCostLimit} onChange={e => setDailyCostLimit(e.target.value)} className="w-full" />
            </div>
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.monthlyCostLimit')}</label>
              <input type="number" step="any" value={monthlyCostLimit} onChange={e => setMonthlyCostLimit(e.target.value)} className="w-full" />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.rateLimitTokens')}</label>
              <input type="number" value={rateLimitTokens} onChange={e => setRateLimitTokens(e.target.value)} className="w-full" />
            </div>
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.rateLimitRefill')}</label>
              <input type="number" step="any" value={rateLimitRefill} onChange={e => setRateLimitRefill(e.target.value)} className="w-full" />
            </div>
          </div>

          {error && <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>}

          <div className="flex justify-end gap-3 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('common:actions.cancel')}</button>
            <button type="submit" disabled={saving} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors">
              {saving && <Loader2 size={14} className="animate-spin" />}
              {tenant ? t('common:actions.save') : t('form.create')}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
