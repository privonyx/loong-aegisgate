import { useEffect, useState, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { Plus, Eye, CheckCircle2, X, Loader2 } from 'lucide-react';
import DataTable, { type Column } from '../components/DataTable';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { RuleSet } from '../types';

const PAGE_SIZE = 50;

export default function Rules() {
  const { t } = useTranslation('rules');
  const [data, setData] = useState<RuleSet[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [showForm, setShowForm] = useState(false);
  const [viewing, setViewing] = useState<RuleSet | null>(null);
  const [activating, setActivating] = useState<RuleSet | null>(null);
  const { toast } = useToast();

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listRuleSets('', PAGE_SIZE, page * PAGE_SIZE);
      setData(res.data);
      setTotal(res.total ?? res.count);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [page, toast, t]);

  useEffect(() => { load(); }, [load]);

  const handleActivate = async () => {
    if (!activating) return;
    try {
      await api.activateRuleSet(activating.version, activating.tenant_id);
      toast('success', t('toast.activated', { version: activating.version }));
      setActivating(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.activateFailed'));
    }
  };

  const columns: Column<RuleSet>[] = [
    { key: 'version', header: t('col.version'), render: (r) => `v${r.version}` },
    {
      key: 'is_active', header: t('col.status'),
      render: (r) => (
        <span className={`px-2 py-0.5 rounded text-xs font-medium ${
          r.is_active ? 'bg-success/10 text-success' : 'bg-muted/20 text-muted'
        }`}>{r.is_active ? t('status.active') : t('status.history')}</span>
      ),
    },
    {
      key: 'rules_count', header: t('col.rulesCount'),
      render: (r) => {
        try { const p = JSON.parse(r.rules_json); return Array.isArray(p) ? p.length : '—'; }
        catch { return '—'; }
      },
    },
    { key: 'created_at', header: t('col.createdAt'), render: (r) => new Date(r.created_at).toLocaleString() },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <button
          onClick={() => setShowForm(true)}
          className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors"
        >
          <Plus size={16} /> {t('newVersion')}
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
            <button title={t('viewVersion', { version: row.version })} onClick={() => setViewing(row)} className="p-1.5 rounded text-muted hover:text-primary hover:bg-primary/10"><Eye size={14} /></button>
            {!row.is_active && (
              <button title={t('activateVersion', { version: row.version })} onClick={() => setActivating(row)} className="p-1.5 rounded text-muted hover:text-success hover:bg-success/10"><CheckCircle2 size={14} /></button>
            )}
          </div>
        )}
      />

      {showForm && <RuleSetForm onClose={() => setShowForm(false)} onSaved={() => { setShowForm(false); load(); }} />}

      {viewing && <RulesViewer ruleSet={viewing} onClose={() => setViewing(null)} />}

      <ConfirmDialog
        open={!!activating}
        title={t('activateDialog.title')}
        message={t('activateDialog.message', { version: activating?.version })}
        confirmLabel={t('activate')}
        confirmTestId="confirm-activate"
        onConfirm={handleActivate}
        onCancel={() => setActivating(null)}
      />
    </div>
  );
}

function RulesViewer({ ruleSet, onClose }: { ruleSet: RuleSet; onClose: () => void }) {
  const { t } = useTranslation('rules');
  let pretty = ruleSet.rules_json;
  try { pretty = JSON.stringify(JSON.parse(ruleSet.rules_json), null, 2); } catch { /* keep raw */ }
  return (
    <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div className="bg-card border border-border rounded-lg w-full max-w-2xl p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{t('viewer.title', { version: ruleSet.version })}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>
        <pre className="bg-bg/50 border border-border rounded-md p-3 text-xs overflow-auto max-h-96 font-mono">{pretty}</pre>
      </div>
    </div>
  );
}

function RuleSetForm({ onClose, onSaved }: { onClose: () => void; onSaved: () => void }) {
  const { t } = useTranslation('rules');
  const [rulesText, setRulesText] = useState('[\n  \n]');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const { toast } = useToast();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    let parsed: unknown;
    try {
      parsed = JSON.parse(rulesText);
    } catch {
      setError(t('form.invalidJson'));
      return;
    }
    setSaving(true);
    try {
      const body = Array.isArray(parsed) ? { rules: parsed } : { rules_json: rulesText };
      await api.createRuleSet(body);
      toast('success', t('toast.created'));
      onSaved();
    } catch (e) {
      setError(e instanceof Error ? e.message : t('toast.saveFailed'));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div className="bg-card border border-border rounded-lg w-full max-w-2xl p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{t('form.title')}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>
        <form onSubmit={handleSubmit} className="space-y-3">
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.rulesLabel')}</label>
            <textarea
              aria-label={t('form.rulesAria')}
              value={rulesText}
              onChange={e => setRulesText(e.target.value)}
              rows={12}
              className="w-full font-mono text-xs"
            />
          </div>
          {error && <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>}
          <div className="flex justify-end gap-3 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('common:actions.cancel')}</button>
            <button type="submit" data-testid="submit-ruleset" disabled={saving} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors">
              {saving && <Loader2 size={14} className="animate-spin" />}
              {t('form.submit')}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
