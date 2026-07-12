import { useEffect, useState, useCallback, useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import { Download } from 'lucide-react';
import { BarChart, Bar, XAxis, YAxis, Tooltip, ResponsiveContainer, PieChart, Pie, Cell } from 'recharts';
import DataTable, { type Column } from '../components/DataTable';
import ExportDialog from '../components/ExportDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { CostRecord } from '../types';

const PAGE_SIZE = 50;
const COLORS = ['hsl(210,100%,60%)', 'hsl(142,71%,45%)', 'hsl(38,92%,50%)', 'hsl(190,95%,50%)', 'hsl(0,84%,60%)', 'hsl(280,70%,60%)'];

export default function CostsPage() {
  const [data, setData] = useState<CostRecord[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [filterTenant, setFilterTenant] = useState('');
  const [filterModel, setFilterModel] = useState('');
  const [showExport, setShowExport] = useState(false);
  const { toast } = useToast();
  const { t } = useTranslation('costs');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.queryCosts(filterTenant, filterModel, PAGE_SIZE, page * PAGE_SIZE);
      setData(res.data);
      setTotal(res.total ?? res.count);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [page, filterTenant, filterModel, toast, t]);

  useEffect(() => { load(); }, [load]);

  const costByModel = useMemo(() => {
    const map = new Map<string, number>();
    data.forEach(c => map.set(c.model, (map.get(c.model) ?? 0) + c.total_cost));
    return Array.from(map.entries()).map(([name, value]) => ({ name, value: +value.toFixed(4) }));
  }, [data]);

  const tokensByDate = useMemo(() => {
    const map = new Map<string, { input: number; output: number }>();
    data.forEach(c => {
      const date = c.timestamp.slice(0, 10);
      const existing = map.get(date) ?? { input: 0, output: 0 };
      existing.input += c.input_tokens;
      existing.output += c.output_tokens;
      map.set(date, existing);
    });
    return Array.from(map.entries())
      .map(([date, v]) => ({ date, ...v }))
      .sort((a, b) => a.date.localeCompare(b.date));
  }, [data]);

  const columns: Column<CostRecord>[] = [
    { key: 'timestamp', header: t('columns.timestamp'), render: (r) => new Date(r.timestamp).toLocaleString(), className: 'whitespace-nowrap' },
    { key: 'request_id', header: t('columns.requestId'), render: (r) => <span className="font-mono text-xs">{r.request_id.slice(0, 12)}...</span> },
    { key: 'tenant_id', header: t('columns.tenant'), render: (r) => <span className="font-mono text-xs">{r.tenant_id.slice(0, 8)}...</span> },
    { key: 'model', header: t('columns.model'), render: (r) => <span className="text-primary">{r.model}</span> },
    { key: 'total_cost', header: t('columns.cost'), render: (r) => `$${r.total_cost.toFixed(4)}` },
    { key: 'input_tokens', header: t('columns.inputTokens'), render: (r) => r.input_tokens.toLocaleString() },
    { key: 'output_tokens', header: t('columns.outputTokens'), render: (r) => r.output_tokens.toLocaleString() },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <div className="flex items-center gap-3">
          <input placeholder={t('filter.tenant')} value={filterTenant} onChange={e => { setFilterTenant(e.target.value); setPage(0); }} className="text-sm w-48" />
          <input placeholder={t('filter.model')} value={filterModel} onChange={e => { setFilterModel(e.target.value); setPage(0); }} className="text-sm w-36" />
          <button
            onClick={() => setShowExport(true)}
            className="flex items-center gap-1.5 px-3 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors"
          >
            <Download size={14} /> {t('common:actions.export')}
          </button>
        </div>
      </div>

      {/* Charts row */}
      {data.length > 0 && (
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          <div className="bg-card border border-border rounded-lg p-5">
            <h3 className="text-sm font-medium text-muted mb-4">{t('chart.modelCostDistribution')}</h3>
            <ResponsiveContainer width="100%" height={200}>
              <PieChart>
                <Pie data={costByModel} dataKey="value" nameKey="name" cx="50%" cy="50%" outerRadius={75}
                  label={({ name, percent }) => `${name} ${(percent * 100).toFixed(0)}%`} fontSize={11}>
                  {costByModel.map((_, i) => <Cell key={i} fill={COLORS[i % COLORS.length]} />)}
                </Pie>
                <Tooltip contentStyle={{ background: 'var(--card)', border: '1px solid var(--border-clr)', borderRadius: 8, fontSize: 12 }} />
              </PieChart>
            </ResponsiveContainer>
          </div>
          <div className="bg-card border border-border rounded-lg p-5">
            <h3 className="text-sm font-medium text-muted mb-4">{t('chart.tokenUsageTrend')}</h3>
            <ResponsiveContainer width="100%" height={200}>
              <BarChart data={tokensByDate}>
                <XAxis dataKey="date" stroke="var(--muted)" fontSize={11} tickLine={false} />
                <YAxis stroke="var(--muted)" fontSize={11} tickLine={false} axisLine={false} />
                <Tooltip contentStyle={{ background: 'var(--card)', border: '1px solid var(--border-clr)', borderRadius: 8, fontSize: 12 }} />
                <Bar dataKey="input" name={t('chart.input')} stackId="a" fill="hsl(210,100%,60%)" radius={[0, 0, 0, 0]} />
                <Bar dataKey="output" name={t('chart.output')} stackId="a" fill="hsl(190,95%,50%)" radius={[4, 4, 0, 0]} />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </div>
      )}

      <DataTable
        columns={columns}
        data={data}
        total={total}
        page={page}
        pageSize={PAGE_SIZE}
        onPageChange={setPage}
        loading={loading}
      />

      <ExportDialog
        open={showExport}
        title={t('exportDialogTitle')}
        tenantId={filterTenant}
        filenameBase="cost-report"
        exportFn={api.exportCostReport}
        onClose={() => setShowExport(false)}
      />
    </div>
  );
}
