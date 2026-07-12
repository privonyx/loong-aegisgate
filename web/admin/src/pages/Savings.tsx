import { useEffect, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  PiggyBank, TrendingUp, Database, Wand2, Network, Info, AlertTriangle, Layers,
} from 'lucide-react';
import {
  LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer,
  PieChart, Pie, Cell, BarChart, Bar, Legend,
} from 'recharts';
import StatCard from '../components/StatCard';
import { api } from '../api/client';
import type { SavingsSummary } from '../types';
import { useAuth } from '../hooks/useAuth';

const CHART_COLORS = [
  'hsl(142,71%,45%)', 'hsl(210,100%,60%)', 'hsl(38,92%,50%)',
  'hsl(190,95%,50%)', 'hsl(280,80%,60%)',
];

const tooltipStyle = {
  contentStyle: {
    background: 'var(--card)', border: '1px solid var(--border-clr)',
    borderRadius: 8, fontSize: 12, color: 'var(--fg)',
  },
  labelStyle: { color: 'var(--fg)' },
  itemStyle: { color: 'var(--fg)' },
  cursor: { fill: 'var(--fg)', opacity: 0.06 },
};

type WindowKey = '24h' | '7d' | '30d' | 'all';
const WINDOWS: { key: WindowKey; labelKey: string; hours: number | null }[] = [
  { key: '24h', labelKey: 'windows.24h', hours: 24 },
  { key: '7d', labelKey: 'windows.7d', hours: 24 * 7 },
  { key: '30d', labelKey: 'windows.30d', hours: 24 * 30 },
  { key: 'all', labelKey: 'windows.all', hours: null },
];

function windowToRange(w: WindowKey): { from: string; to: string } {
  const cfg = WINDOWS.find(x => x.key === w)!;
  const to = new Date().toISOString();
  if (cfg.hours == null) return { from: '', to: '' };  // 全部 → 后端 since aggregator
  const from = new Date(Date.now() - cfg.hours * 3600 * 1000).toISOString();
  return { from, to };
}

// Phase 6.1 (TASK-20260513-01 Epic 5.2) — heuristic modality classifier.
// Until the backend exposes CostTracker.modality through getSavingsSummary
// the page derives modality from the model name. This is intentionally a
// preview view: a tooltip warns operators that the precise breakdown will
// land once the API surface is extended (planned for the Epic 5.1c follow-up).
type ModalityKey =
  | 'embedding' | 'image_gen' | 'audio_transcribe' | 'audio_speech'
  | 'moderation' | 'chat';

function inferModalityFromModel(model: string): ModalityKey {
  const m = model.toLowerCase();
  if (m.includes('embed')) return 'embedding';
  if (m.includes('whisper') || m.includes('transcrib')) return 'audio_transcribe';
  if (m.includes('tts') || m.includes('speech')) return 'audio_speech';
  if (m.includes('dall') || m.includes('image') || m.includes('sd')) return 'image_gen';
  if (m.includes('moderation')) return 'moderation';
  return 'chat';
}

export default function Savings() {
  const { t } = useTranslation('savings');
  const { user } = useAuth();
  const [windowKey, setWindowKey] = useState<WindowKey>('7d');
  const [data, setData] = useState<SavingsSummary | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [showAlgo, setShowAlgo] = useState(false);

  useEffect(() => {
    setLoading(true);
    setError(null);
    const { from, to } = windowToRange(windowKey);
    api.savingsSummary(from, to, '')
      .then(setData)
      .catch(e => setError(e instanceof Error ? e.message : t('toast.loadFailed')))
      .finally(() => setLoading(false));
  }, [windowKey]);

  const isSuper = user?.role === 'SuperAdmin' || user?.role === 'super_admin';

  const pieData = useMemo(() => {
    if (!data) return [];
    return data.by_type
      .filter(row => row.cost_saved > 0)
      .map(row => ({
        name: t(`typeLabels.${row.type}`, { defaultValue: row.type }),
        value: row.cost_saved,
      }));
  }, [data, t]);

  const modelBars = useMemo(() => {
    if (!data) return [];
    return [...data.by_model]
      .sort((a, b) => b.cost_saved - a.cost_saved)
      .slice(0, 8)
      .map(m => ({
        model: m.model.length > 20 ? m.model.slice(0, 18) + '...' : m.model,
        saved: Number(m.cost_saved.toFixed(4)),
        requests: m.request_count,
      }));
  }, [data]);

  // Epic 5.2 — fold by_model into a coarse modality breakdown. Models that
  // contribute zero saved cost are dropped so the chart stays meaningful.
  const modalityBreakdown = useMemo(() => {
    if (!data) return [] as { modality: ModalityKey; label: string; cost_saved: number; tokens_saved: number; request_count: number; }[];
    const acc: Partial<Record<ModalityKey, { cost_saved: number; tokens_saved: number; request_count: number }>> = {};
    for (const m of data.by_model) {
      const k = inferModalityFromModel(m.model);
      const slot = acc[k] ?? { cost_saved: 0, tokens_saved: 0, request_count: 0 };
      slot.cost_saved += m.cost_saved;
      slot.tokens_saved += m.tokens_saved;
      slot.request_count += m.request_count;
      acc[k] = slot;
    }
    return (Object.entries(acc) as [ModalityKey, { cost_saved: number; tokens_saved: number; request_count: number }][])
      .filter(([, v]) => v.cost_saved > 0 || v.request_count > 0)
      .sort((a, b) => b[1].cost_saved - a[1].cost_saved)
      .map(([k, v]) => ({
        modality: k,
        label: t(`modalityLabels.${k}`),
        cost_saved: Number(v.cost_saved.toFixed(4)),
        tokens_saved: v.tokens_saved,
        request_count: v.request_count,
      }));
  }, [data, t]);

  if (loading) {
    return (
      <div className="space-y-6 animate-pulse">
        <div className="grid grid-cols-4 gap-4">
          {Array.from({ length: 4 }).map((_, i) => (
            <div key={i} className="h-28 bg-card border border-border rounded-lg" />
          ))}
        </div>
        <div className="h-72 bg-card border border-border rounded-lg" />
      </div>
    );
  }

  if (error) {
    return (
      <div className="bg-danger/10 border border-danger/30 text-danger rounded-lg p-4 flex items-start gap-3">
        <AlertTriangle size={18} className="shrink-0 mt-0.5" />
        <div>
          <p className="font-medium">{t('error.title')}</p>
          <p className="text-sm opacity-80 mt-1">{error}</p>
        </div>
      </div>
    );
  }

  if (!data) return null;

  return (
    <div className="space-y-6">
      {/* 时间窗口切换 */}
      <div className="flex items-center justify-between flex-wrap gap-3">
        <h2 className="text-xl font-semibold flex items-center gap-2">
          <PiggyBank size={22} className="text-success" />
          {t('title')}
        </h2>
        <div className="flex items-center gap-2">
          {WINDOWS.map(w => (
            <button
              key={w.key}
              onClick={() => setWindowKey(w.key)}
              className={`px-3 py-1.5 text-sm rounded-md transition-colors ${
                windowKey === w.key
                  ? 'bg-primary text-white'
                  : 'bg-card border border-border text-muted hover:text-fg'
              }`}
            >
              {t(w.labelKey)}
            </button>
          ))}
          <button
            onClick={() => setShowAlgo(s => !s)}
            className="ml-2 px-3 py-1.5 text-sm rounded-md bg-card border border-border text-muted hover:text-fg flex items-center gap-1.5"
            title={t('algoButton.title')}
          >
            <Info size={14} />
            {t('algoButton.label')}
          </button>
        </div>
      </div>

      {/* aggregator_since 提示（透明度 SR-NEW1） */}
      {data.aggregator_since && (
        <p className="text-xs text-muted flex items-center gap-1.5">
          <Info size={12} />
          {t('aggregator.since')}<code className="font-mono">{new Date(data.aggregator_since).toLocaleString()}</code>
          {t('aggregator.window')}{new Date(data.from).toLocaleString()}{t('aggregator.separator')}{new Date(data.to).toLocaleString()}{t('aggregator.end')}
        </p>
      )}

      {/* 算法说明面板（透明度 SR-NEW1 / TR1） */}
      {showAlgo && (
        <div className="bg-card border border-border rounded-lg p-5 space-y-3 text-sm">
          <h3 className="font-medium flex items-center gap-2">
            <Info size={16} className="text-primary" />
            {t('algo.title')}
          </h3>
          <div className="space-y-2 text-muted">
            <p>
              <strong className="text-fg">{t('algo.cacheHit.term')}</strong>{t('algo.cacheHit.body1')}
              <code className="font-mono mx-1">TokenEstimator</code>{t('algo.cacheHit.body2')}
            </p>
            <p>
              <strong className="text-fg">{t('algo.compression.term')}</strong>{t('algo.compression.body')}
            </p>
            <p>
              <strong className="text-fg">{t('algo.routing.term')}</strong>{t('algo.routing.body')}
            </p>
            {data.fallback_pricing_count > 0 && (
              <p className="text-warning flex items-start gap-2 mt-3">
                <AlertTriangle size={14} className="shrink-0 mt-0.5" />
                <span>
                  {t('algo.fallback.lead')}<strong>{data.fallback_pricing_count}</strong>{t('algo.fallback.mid')}
                  <code className="font-mono mx-1">CostTracker.loadPricing()</code>{t('algo.fallback.tail')}
                </span>
              </p>
            )}
          </div>
        </div>
      )}

      {/* 顶部 4 KPI */}
      <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-4 gap-4">
        <StatCard
          title={t('kpi.costSaved')}
          value={`$${data.total_cost_saved.toFixed(2)}`}
          icon={PiggyBank}
          accent="text-success"
        />
        <StatCard
          title={t('kpi.costActual')}
          value={`$${data.total_cost_actual.toFixed(2)}`}
          icon={TrendingUp}
        />
        <StatCard
          title="ROI"
          value={`${data.roi_percent.toFixed(1)}%`}
          icon={TrendingUp}
          accent="text-accent"
        />
        <StatCard
          title={t('kpi.cacheHits')}
          value={data.total_cache_hits.toLocaleString()}
          icon={Database}
          accent="text-primary"
        />
      </div>

      {/* 节省来源拆分 (Pie) + 时间趋势 (Line) */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4 flex items-center gap-2">
            <Wand2 size={14} /> {t('sourceSplit.title')}
          </h3>
          {pieData.length === 0 ? (
            <p className="text-sm text-muted text-center py-16">{t('sourceSplit.empty')}</p>
          ) : (
            <ResponsiveContainer width="100%" height={260}>
              <PieChart margin={{ top: 10, right: 30, bottom: 10, left: 30 }}>
                <Pie
                  data={pieData} dataKey="value" nameKey="name"
                  cx="50%" cy="50%" outerRadius={75}
                  label={({ name, percent }: { name?: string; percent?: number }) =>
                    `${name ?? ''} ${((percent ?? 0) * 100).toFixed(0)}%`
                  }
                  fontSize={11} labelLine={{ stroke: 'var(--muted)' }} fill="var(--fg)"
                >
                  {pieData.map((_, i) => (
                    <Cell key={i} fill={CHART_COLORS[i % CHART_COLORS.length]} />
                  ))}
                </Pie>
                <Tooltip
                  {...tooltipStyle}
                  formatter={(v: number) => `$${v.toFixed(4)}`}
                />
              </PieChart>
            </ResponsiveContainer>
          )}
        </div>

        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4 flex items-center gap-2">
            <TrendingUp size={14} /> {t('trend.title')}
          </h3>
          {data.time_series.length === 0 ? (
            <p className="text-sm text-muted text-center py-16">{t('trend.empty')}</p>
          ) : (
            <ResponsiveContainer width="100%" height={260}>
              <LineChart data={data.time_series}>
                <XAxis dataKey="date" stroke="var(--muted)" fontSize={11} tickLine={false} />
                <YAxis stroke="var(--muted)" fontSize={11} tickLine={false} axisLine={false} />
                <Tooltip
                  {...tooltipStyle}
                  formatter={(v: number) => `$${v.toFixed(4)}`}
                />
                <Line type="monotone" dataKey="cost_saved" stroke="var(--success)"
                      strokeWidth={2} dot={false} name={t('trend.lineName')} />
              </LineChart>
            </ResponsiveContainer>
          )}
        </div>
      </div>

      {/* 模型对比 (Bar) + 路由建议 (Table) */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4">{t('modelCompare.title')}</h3>
          {modelBars.length === 0 ? (
            <p className="text-sm text-muted text-center py-16">{t('modelCompare.empty')}</p>
          ) : (
            <ResponsiveContainer width="100%" height={260}>
              <BarChart data={modelBars}>
                <XAxis dataKey="model" stroke="var(--muted)" fontSize={11}
                       tickLine={false} angle={-15} textAnchor="end" height={60} />
                <YAxis stroke="var(--muted)" fontSize={11} tickLine={false} axisLine={false} />
                <Tooltip
                  {...tooltipStyle}
                  formatter={(v: number) => `$${v.toFixed(4)}`}
                />
                <Legend wrapperStyle={{ fontSize: 11 }} />
                <Bar dataKey="saved" fill="var(--success)" radius={[4, 4, 0, 0]} name={t('modelCompare.barName')} />
              </BarChart>
            </ResponsiveContainer>
          )}
        </div>

        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4 flex items-center gap-2">
            <Network size={14} /> {t('routing.title')}
          </h3>
          {data.routing_recommendations.length === 0 ? (
            <p className="text-sm text-muted text-center py-16">{t('routing.empty')}</p>
          ) : (
            <table className="w-full text-sm">
              <thead className="text-xs text-muted">
                <tr className="border-b border-border">
                  <th className="text-left py-2 font-normal">{t('routing.route')}</th>
                  <th className="text-right py-2 font-normal">{t('routing.potential')}</th>
                  <th className="text-right py-2 font-normal">{t('routing.events')}</th>
                </tr>
              </thead>
              <tbody>
                {data.routing_recommendations.slice(0, 10).map((r, i) => (
                  <tr key={i} className="border-b border-border/50">
                    <td className="py-2 font-mono text-xs">{r.route}</td>
                    <td className="py-2 text-right text-success">${r.potential_savings.toFixed(4)}</td>
                    <td className="py-2 text-right text-muted">{r.event_count}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </div>

      {/* 按模态归因（Phase 6.1 预览，TASK-20260513-01 Epic 5.2） */}
      <div className="bg-card border border-border rounded-lg p-5">
        <h3 className="text-sm font-medium text-muted mb-1 flex items-center gap-2">
          <Layers size={14} className="text-accent" />
          {t('modality.title')}
        </h3>
        <p className="text-xs text-muted mb-4 flex items-start gap-1.5">
          <Info size={12} className="shrink-0 mt-0.5" />
          {t('modality.hint')} <code className="font-mono mx-1">CostTracker.modality</code>{' '}
          {t('modality.hintTail')}
        </p>
        {modalityBreakdown.length === 0 ? (
          <p className="text-sm text-muted text-center py-12" data-testid="modality-empty">
            {t('modality.empty')}
          </p>
        ) : (
          <table className="w-full text-sm" data-testid="modality-breakdown-table">
            <thead className="text-xs text-muted">
              <tr className="border-b border-border">
                <th className="text-left py-2 font-normal">{t('modality.modality')}</th>
                <th className="text-right py-2 font-normal">{t('modality.costSaved')}</th>
                <th className="text-right py-2 font-normal">{t('modality.tokensSaved')}</th>
                <th className="text-right py-2 font-normal">{t('modality.requests')}</th>
              </tr>
            </thead>
            <tbody>
              {modalityBreakdown.map(row => (
                <tr key={row.modality} className="border-b border-border/50">
                  <td className="py-2">{row.label}</td>
                  <td className="py-2 text-right text-success">${row.cost_saved.toFixed(4)}</td>
                  <td className="py-2 text-right">{row.tokens_saved.toLocaleString()}</td>
                  <td className="py-2 text-right text-muted">{row.request_count}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      {/* Top10 排行（仅 SuperAdmin） */}
      {isSuper && data.top_tenants.length > 0 && (
        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4">
            {t('topTenants.title', { count: Math.min(data.top_tenants.length, 10) })}
          </h3>
          <table className="w-full text-sm">
            <thead className="text-xs text-muted">
              <tr className="border-b border-border">
                <th className="text-left py-2 font-normal w-12">#</th>
                <th className="text-left py-2 font-normal">{t('topTenants.tenantId')}</th>
                <th className="text-right py-2 font-normal">{t('topTenants.costSaved')}</th>
                <th className="text-right py-2 font-normal">{t('topTenants.tokensSaved')}</th>
                <th className="text-right py-2 font-normal">{t('topTenants.events')}</th>
              </tr>
            </thead>
            <tbody>
              {data.top_tenants.map((t, i) => (
                <tr key={t.tenant_id} className="border-b border-border/50">
                  <td className="py-2 text-muted">{i + 1}</td>
                  <td className="py-2 font-mono text-xs">{t.tenant_id}</td>
                  <td className="py-2 text-right text-success">${t.cost_saved.toFixed(4)}</td>
                  <td className="py-2 text-right">{t.tokens_saved.toLocaleString()}</td>
                  <td className="py-2 text-right text-muted">{t.event_count}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
