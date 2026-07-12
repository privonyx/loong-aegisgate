import { useEffect, useState, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { Plus, Pencil, Trash2, X, Loader2 } from 'lucide-react';
import DataTable, { type Column } from '../components/DataTable';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { PromptTemplate } from '../types';

const PAGE_SIZE = 20;

export default function Templates() {
  const { t } = useTranslation('templates');
  const [data, setData] = useState<PromptTemplate[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [showForm, setShowForm] = useState(false);
  const [editing, setEditing] = useState<PromptTemplate | null>(null);
  const [deleting, setDeleting] = useState<PromptTemplate | null>(null);
  const { toast } = useToast();

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listPromptTemplates('', PAGE_SIZE, page * PAGE_SIZE);
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
      await api.deletePromptTemplate(deleting.id);
      toast('success', t('toast.deleted'));
      setDeleting(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.deleteFailed'));
    }
  };

  const columns: Column<PromptTemplate>[] = [
    { key: 'name', header: t('col.name') },
    { key: 'version', header: t('col.version'), render: (r) => `v${r.version}` },
    { key: 'weight', header: t('col.weight') },
    {
      key: 'is_active', header: t('col.status'),
      render: (r) => (
        <span className={`px-2 py-0.5 rounded text-xs font-medium ${
          r.is_active ? 'bg-success/10 text-success' : 'bg-muted/20 text-muted'
        }`}>{r.is_active ? t('status.active') : t('status.inactive')}</span>
      ),
    },
    {
      key: 'content', header: t('col.contentPreview'),
      render: (r) => <span className="text-muted">{r.content.length > 40 ? r.content.slice(0, 40) + '…' : r.content}</span>,
    },
    { key: 'created_at', header: t('col.createdAt'), render: (r) => new Date(r.created_at).toLocaleString() },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <button
          onClick={() => { setEditing(null); setShowForm(true); }}
          className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors"
        >
          <Plus size={16} /> {t('newTemplate')}
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

      {showForm && <TemplateForm template={editing} onClose={() => setShowForm(false)} onSaved={() => { setShowForm(false); load(); }} />}

      <ConfirmDialog
        open={!!deleting}
        title={t('deleteDialog.title')}
        message={t('deleteDialog.message', { name: deleting?.name })}
        confirmLabel={t('common:actions.delete')}
        confirmTestId="confirm-delete"
        danger
        onConfirm={handleDelete}
        onCancel={() => setDeleting(null)}
      />
    </div>
  );
}

function TemplateForm({ template, onClose, onSaved }: { template: PromptTemplate | null; onClose: () => void; onSaved: () => void }) {
  const { t } = useTranslation('templates');
  const [name, setName] = useState(template?.name ?? '');
  const [content, setContent] = useState(template?.content ?? '');
  const [weight, setWeight] = useState(String(template?.weight ?? 100));
  const [isActive, setIsActive] = useState(template?.is_active ?? true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const { toast } = useToast();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    try {
      if (template) {
        await api.updatePromptTemplate(template.id, { name, content, weight: Number(weight), is_active: isActive });
        toast('success', t('toast.updated'));
      } else {
        await api.createPromptTemplate({ name, content, weight: Number(weight), is_active: isActive });
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
          <h2 className="font-semibold">{template ? t('form.editTitle') : t('form.createTitle')}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>

        <form onSubmit={handleSubmit} className="space-y-3">
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.nameLabel')}</label>
            <input aria-label={t('form.nameAria')} value={name} onChange={e => setName(e.target.value)} className="w-full" required />
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.contentLabel')}</label>
            <textarea aria-label={t('form.contentAria')} value={content} onChange={e => setContent(e.target.value)} rows={5} className="w-full font-mono text-xs" required />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">{t('form.weightLabel')}</label>
              <input aria-label={t('form.weightAria')} type="number" value={weight} onChange={e => setWeight(e.target.value)} className="w-full" />
            </div>
            <div className="flex items-end">
              <label className="flex items-center gap-2 text-sm text-muted">
                <input type="checkbox" checked={isActive} onChange={e => setIsActive(e.target.checked)} />
                {t('form.activeLabel')}
              </label>
            </div>
          </div>

          {error && <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>}

          <div className="flex justify-end gap-3 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('common:actions.cancel')}</button>
            <button type="submit" disabled={saving} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors">
              {saving && <Loader2 size={14} className="animate-spin" />}
              {template ? t('common:actions.save') : t('form.submitCreate')}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
