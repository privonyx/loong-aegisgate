import { useEffect, useState, useCallback, useMemo } from 'react';
import {
  Activity, Building2, DollarSign, Zap, ScrollText, PiggyBank,
} from 'lucide-react';
// TASK-20260528-01: Row 0 Hero (replaces Row 4 Case Study Numbers block)
import HeroCaseStudy from '../components/HeroCaseStudy';
import {
  LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer,
  PieChart, Pie, Cell, BarChart, Bar,
} from 'recharts';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import type { TFunction } from 'i18next';
import StatCard from '../components/StatCard';
import { api } from '../api/client';
import { useWebSocket } from '../hooks/useWebSocket';
import { useAuth } from '../hooks/useAuth';
import type {
  DashboardSummary, WsMessage, AuditRecord,
  SavingsSummary, SecurityEventSummary,
  CaseStudyHeadline,
} from '../types';

const CHART_COLORS = [
  'hsl(210,100%,60%)', 'hsl(142,71%,45%)', 'hsl(38,92%,50%)',
  'hsl(190,95%,50%)', 'hsl(0,84%,60%)',
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

// 24h 请求趋势：基于审计事件按小时聚合（替代 Math.random mock）。
function buildRequestTrend(audits: AuditRecord[]): { hour: string; requests: number }[] {
  const now = new Date();
  const buckets = new Map<number, number>();
  for (let i = 0; i < 24; ++i) buckets.set(i, 0);
  audits.forEach(a => {
    const t = new Date(a.timestamp);
    const diffH = Math.floor((now.getTime() - t.getTime()) / 3_600_000);
    if (diffH < 0 || diffH >= 24) return;
    const slot = (now.getHours() - diffH + 24) % 24;
    buckets.set(slot, (buckets.get(slot) ?? 0) + 1);
  });
  return Array.from(buckets.entries())
    .sort(([a], [b]) => a - b)
    .map(([h, n]) => ({ hour: `${String(h).padStart(2, '0')}:00`, requests: n }));
}

// 模型成本分布：从 SavingsSummary.by_model.cost_saved 推导（替代硬编码）。
// 当全部为 0 时返回空数组（UI 显示"暂无数据"）。
function buildModelDistribution(savings: SavingsSummary | null) {
  if (!savings || savings.by_model.length === 0) return [];
  return savings.by_model
    .map(m => ({ name: m.model, value: m.cost_saved }))
    .filter(m => m.value > 0)
    .sort((a, b) => b.value - a.value)
    .slice(0, 5);
}

// 安全事件统计：来自 SecurityEventSummary。SuperAdmin 看 raw counts；
// 非 SuperAdmin 看 severity 分级映射为虚拟 count（none=0/low=1/medium=10/high=100）。
function buildSecurityChart(events: SecurityEventSummary | null, t: TFunction) {
  if (!events) return [];
  if (events.scope === 'global') {
    return [
      { type: t('security.injection'), count: events.guardrail_blocks_total ?? 0 },
      { type: t('security.normalized'), count: events.preprocessor_normalized_total ?? 0 },
      { type: t('security.rateLimit'), count: events.rate_limited_total ?? 0 },
    ];
  }
  const sev = (s?: 'none' | 'low' | 'medium' | 'high') =>
    s === 'high' ? 100 : s === 'medium' ? 10 : s === 'low' ? 1 : 0;
  return [
    { type: t('security.injection'), count: sev(events.guardrail_blocks_severity) },
    { type: t('security.rateLimit'), count: sev(events.rate_limited_severity) },
  ];
}

export default function Dashboard() {
  const { t } = useTranslation('dashboard');
  const [summary, setSummary] = useState<DashboardSummary | null>(null);
  const [savings, setSavings] = useState<SavingsSummary | null>(null);
  const [security, setSecurity] = useState<SecurityEventSummary | null>(null);
  const [caseStudy, setCaseStudy] = useState<CaseStudyHeadline | null>(null);
  const [recentAudits, setRecentAudits] = useState<AuditRecord[]>([]);
  const [loading, setLoading] = useState(true);
  const navigate = useNavigate();
  const { user } = useAuth();
  // TASK-20260604-01 P0-G / D3=C：super_admin 走 WS 全局 metrics（SR-NEW1 约束保持）；
  // 非 super 角色不接收 WS 全局 metrics → 改用 HTTP 轮询保证实时性。
  const isSuper = user?.role === 'super_admin';

  const loadData = useCallback(async (initial: boolean) => {
    if (initial) setLoading(true);
    const to = new Date().toISOString();
    const from = new Date(Date.now() - 30 * 24 * 3600 * 1000).toISOString();
    try {
      const [s, a, sv, ev, cs] = await Promise.all([
        api.dashboardSummary(),
        api.queryAudits('', 100, 0),
        api.savingsSummary(from, to, '').catch(() => null),
        api.securityEvents().catch(() => null),
        api.caseStudyHeadline().catch(() => null),
      ]);
      setSummary(s);
      setRecentAudits(a.data);
      setSavings(sv);
      setSecurity(ev);
      setCaseStudy(cs);
    } catch {
      // 静默：保留既有数据，下一轮重试。
    } finally {
      if (initial) setLoading(false);
    }
  }, []);

  useEffect(() => {
    void loadData(true);
  }, [loadData]);

  // P0-G：非 super 角色每 15s HTTP 轮询刷新（WS 不推全局 metrics）。
  useEffect(() => {
    if (isSuper) return;
    const id = setInterval(() => { void loadData(false); }, 15000);
    return () => clearInterval(id);
  }, [isSuper, loadData]);

  const handleWsMessage = useCallback((msg: WsMessage) => {
    if (msg.type === 'metrics') {
      // TASK-20260602-01 Epic 1: 适配后端 nested data envelope（D1=B）。
      // 仅合并 WS 推送的字段，total_cost / cost_saved_30d / aggregator_since
      // 不在 metrics 推送范围 → 保留既有 state 值不被覆盖为 undefined。
      setSummary(prev => prev ? {
        ...prev,
        total_requests: msg.data.total_requests,
        active_tenants: msg.data.active_tenants,
        cache_hit_rate: msg.data.cache_hit_rate,
      } : prev);
      // TASK-20260617-01 Bug1 (spec §3 D-bug1=B)：KPI「缓存命中率」与 Hero「缓存命中」
      // 是同一指标的两处展示。metrics 帧（高频）与 case_study 帧（~30s）异步到达会让
      // 两处数字刷新不同步 → 收到 metrics 帧时同步把 caseStudy 的 total_hit_rate 跟上，
      // 使两处头部命中率帧级一致。cache_hit_rate 为 null 时不覆盖既有值。
      if (msg.data.cache_hit_rate != null) {
        const liveHitRate = msg.data.cache_hit_rate;
        setCaseStudy(prev => prev ? {
          ...prev,
          cache_hit_by_type: { ...prev.cache_hit_by_type, total_hit_rate: liveHitRate },
        } : prev);
      }
    } else if (msg.type === 'audit') {
      // TASK-20260602-01 Epic 1: 后端字段 stage → 前端 AuditRecord.stage_name 映射。
      const a: AuditRecord = {
        request_id: msg.data.request_id,
        timestamp: msg.data.timestamp,
        tenant_id: msg.data.tenant_id,
        action: msg.data.action,
        stage_name: msg.data.stage,
        detail: msg.data.detail,
      };
      setRecentAudits(prev => [a, ...prev].slice(0, 100));
    } else if (msg.type === 'case_study') {
      // TASK-20260527-02 — 30s WS push of Case Study Numbers.
      // Merge into existing headline so we keep timestamp/tenant_id metadata
      // even if the WS payload doesn't include them.
      setCaseStudy(prev => ({
        scope: msg.data.scope,
        tenant_id: msg.data.tenant_id ?? prev?.tenant_id,
        timestamp: new Date().toISOString(),
        saved_vs_baseline: msg.data.saved_vs_baseline,
        cache_hit_by_type: msg.data.cache_hit_by_type,
        quality_reason: msg.data.quality_reason,
      }));
    }
  }, []);

  useWebSocket({ onMessage: handleWsMessage });

  const requestTrend = useMemo(() => buildRequestTrend(recentAudits), [recentAudits]);
  const costByModel = useMemo(() => buildModelDistribution(savings), [savings]);
  const securityChart = useMemo(() => buildSecurityChart(security, t), [security, t]);

  if (loading || !summary) {
    return (
      <div className="space-y-6 animate-pulse">
        <div className="grid grid-cols-5 gap-4">
          {Array.from({ length: 5 }).map((_, i) => (
            <div key={i} className="h-28 bg-card border border-border rounded-lg" />
          ))}
        </div>
        <div className="grid grid-cols-2 gap-4">
          <div className="h-72 bg-card border border-border rounded-lg" />
          <div className="h-72 bg-card border border-border rounded-lg" />
        </div>
      </div>
    );
  }

  const costSaved = summary.cost_saved_30d ?? 0;
  const cacheHitDisplay = summary.cache_hit_rate != null
    ? `${(summary.cache_hit_rate * 100).toFixed(1)}%`
    : 'N/A';

  return (
    <div className="space-y-6">
      {/* TASK-20260604-01 P0-G：数据刷新方式标注（super=WS 实时 / 非 super=15s 轮询）。 */}
      <div className="flex justify-end">
        <span className="text-xs text-muted" data-testid="refresh-mode">
          {isSuper ? t('refreshMode.realtime') : t('refreshMode.polling')}
        </span>
      </div>

      {/* Row 0 — Hero Case Study Numbers (TASK-20260528-01 D2=A: promoted from Row 4) */}
      <HeroCaseStudy data={caseStudy} />

      {/* Row 1: 5 KPI 卡片（新增"已节省（近30天）"，可点击跳到 Savings 详情页） */}
      <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-5 gap-4">
        <StatCard title={t('kpi.totalRequests')} value={summary.total_requests.toLocaleString()} icon={Activity} />
        <StatCard title={t('kpi.activeTenants')} value={summary.active_tenants} icon={Building2} accent="text-success" />
        <StatCard title={t('kpi.totalCost')} value={`$${summary.total_cost.toFixed(2)}`} icon={DollarSign} accent="text-warning" />
        <StatCard title={t('kpi.cacheHitRate')} value={cacheHitDisplay} icon={Zap} accent="text-accent" />
        <button
          type="button"
          onClick={() => navigate('/savings')}
          className="text-left rounded-lg focus:outline-none focus:ring-2 focus:ring-success"
          aria-label={t('kpi.savedDetailAria')}
        >
          <StatCard
            title={t('kpi.savedRecent')}
            value={`$${costSaved.toFixed(2)}`}
            icon={PiggyBank}
            accent="text-success"
          />
        </button>
      </div>

      {/* Row 2: 24h 请求趋势（来自审计事件聚合，非 mock） + 最近审计 */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4">{t('charts.requestTrend')}</h3>
          <ResponsiveContainer width="100%" height={240}>
            <LineChart data={requestTrend}>
              <XAxis dataKey="hour" stroke="var(--muted)" fontSize={11} tickLine={false} />
              <YAxis stroke="var(--muted)" fontSize={11} tickLine={false} axisLine={false} />
              <Tooltip {...tooltipStyle} />
              <Line type="monotone" dataKey="requests" stroke="var(--primary)" strokeWidth={2} dot={false} />
            </LineChart>
          </ResponsiveContainer>
        </div>

        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4">
            <span className="flex items-center gap-2">
              <ScrollText size={14} /> {t('charts.recentAudits')}
            </span>
          </h3>
          <div className="space-y-2 max-h-[240px] overflow-y-auto">
            {recentAudits.length === 0 ? (
              <p className="text-sm text-muted text-center py-8">{t('charts.noAudits')}</p>
            ) : (
              recentAudits.slice(0, 10).map((a, i) => (
                <div
                  key={`${a.request_id}-${i}`}
                  className="flex items-start gap-3 text-xs p-2 rounded bg-bg/50 animate-[fadeIn_0.3s_ease-in]"
                >
                  <span className="text-muted shrink-0 w-16">{new Date(a.timestamp).toLocaleTimeString()}</span>
                  <span className="text-primary font-mono truncate">{a.action}</span>
                  <span className="text-muted truncate flex-1">{a.detail}</span>
                </div>
              ))
            )}
          </div>
        </div>
      </div>

      {/* Row 3: 模型节省分布（真数据，按 by_model.cost_saved） + 安全事件统计（real metrics） */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-4">{t('charts.modelSavings')}</h3>
          {costByModel.length === 0 ? (
            <p className="text-sm text-muted text-center py-16">{t('charts.noSavings')}</p>
          ) : (
            <ResponsiveContainer width="100%" height={240}>
              <PieChart margin={{ top: 10, right: 30, bottom: 10, left: 30 }}>
                <Pie
                  data={costByModel} dataKey="value" nameKey="name"
                  cx="50%" cy="50%" outerRadius={65}
                  label={({ name, percent }: { name?: string; percent?: number }) =>
                    `${name ?? ''} ${((percent ?? 0) * 100).toFixed(0)}%`
                  }
                  fontSize={11} labelLine={{ stroke: 'var(--muted)' }} fill="var(--fg)"
                >
                  {costByModel.map((_, i) => (
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
          <h3 className="text-sm font-medium text-muted mb-4">
            {t('charts.securityEvents')}{security?.scope === 'tenant' ? t('charts.securitySeverity') : ''}
          </h3>
          {securityChart.length === 0 ? (
            <p className="text-sm text-muted text-center py-16">{t('charts.noSecurity')}</p>
          ) : (
            <ResponsiveContainer width="100%" height={200}>
              <BarChart data={securityChart}>
                <XAxis dataKey="type" stroke="var(--muted)" fontSize={11} tickLine={false} />
                <YAxis stroke="var(--muted)" fontSize={11} tickLine={false} axisLine={false} />
                <Tooltip {...tooltipStyle} cursor={{ fill: 'var(--fg)', opacity: 0.06 }} />
                <Bar dataKey="count" fill="var(--danger)" radius={[4, 4, 0, 0]} />
              </BarChart>
            </ResponsiveContainer>
          )}
        </div>
      </div>

    </div>
  );
}
