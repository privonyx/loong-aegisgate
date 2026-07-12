// Phase 11.5 (TASK-20260518-02 E5.2) — FinOps approval workflow page.
//
// Surfaces the AutonomyApprovalWorkflow proposals in three coordinated panels:
//   1. KPI strip — sample size, applied count, estimated 24h savings.
//   2. Proposal list (card grid) with state + source filters.
//   3. Detail drawer — payload + decision_trace + audit hint + action buttons
//      (approve / reject / rollback) with confirmation modals.
//
// All actions reuse the shared Toast / fetch error surfaces. Empty / loading
// / error states match the visual language used by Savings.tsx so the page
// feels like a natural sibling under /admin.

import { useCallback, useEffect, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  AlertTriangle,
  CheckCircle2,
  ClipboardList,
  Clock,
  Filter,
  PiggyBank,
  RefreshCcw,
  ShieldCheck,
  Undo2,
  XCircle,
} from 'lucide-react';
import StatCard from '../components/StatCard';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import {
  autonomyApi,
  type ApprovalProposal,
  type ApprovalState,
  type AutonomyReport,
  type AutonomySource,
} from '../api/autonomy';

const PAGE_SIZE = 20;

const STATE_FILTERS: { value: ApprovalState | ''; labelKey: string }[] = [
  { value: '',            labelKey: 'stateFilters.all' },
  { value: 'PROPOSED',    labelKey: 'stateFilters.proposed' },
  { value: 'APPROVED',    labelKey: 'stateFilters.approved' },
  { value: 'APPLIED',     labelKey: 'stateFilters.applied' },
  { value: 'REJECTED',    labelKey: 'stateFilters.rejected' },
  { value: 'ROLLED_BACK', labelKey: 'stateFilters.rolledBack' },
];

const SOURCE_FILTERS: { value: AutonomySource | ''; labelKey: string }[] = [
  { value: '',              labelKey: 'sourceFilters.all' },
  { value: 'CostOptimizer', labelKey: 'sourceFilters.costOptimizer' },
  { value: 'AutoRecovery',  labelKey: 'sourceFilters.autoRecovery' },
  { value: 'BanditRouter',  labelKey: 'sourceFilters.banditRouter' },
  { value: 'AdaptiveGuard', labelKey: 'sourceFilters.adaptiveGuard' },
  { value: 'Workflow',      labelKey: 'sourceFilters.workflow' },
];

const STATE_BADGE: Record<ApprovalState, string> = {
  PROPOSED:    'bg-primary/10 text-primary border-primary/30',
  APPROVED:    'bg-accent/10 text-accent border-accent/30',
  APPLIED:     'bg-success/10 text-success border-success/30',
  REJECTED:    'bg-danger/10 text-danger border-danger/30',
  ROLLED_BACK: 'bg-warning/10 text-warning border-warning/30',
};

function relative(ms: number): string {
  if (!ms) return '—';
  const dt = new Date(ms);
  return dt.toLocaleString();
}

function payloadField(
  p: ApprovalProposal,
  key: string,
  fallback = '—',
): string {
  const v = (p.payload ?? {})[key];
  if (v == null) return fallback;
  if (typeof v === 'number') return v.toString();
  return String(v);
}

function savingsOf(p: ApprovalProposal): number {
  const v = (p.payload ?? {}).estimated_savings_usd_24h;
  return typeof v === 'number' ? v : 0;
}

export default function FinOps() {
  const { t } = useTranslation('finops');
  const { toast } = useToast();
  const [proposals, setProposals] = useState<ApprovalProposal[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [report, setReport] = useState<AutonomyReport | null>(null);
  const [stateFilter, setStateFilter] = useState<ApprovalState | ''>('');
  const [sourceFilter, setSourceFilter] = useState<AutonomySource | ''>('');
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [selected, setSelected] = useState<ApprovalProposal | null>(null);
  const [confirm, setConfirm] = useState<
    | null
    | { kind: 'approve' | 'rollback'; id: string }
    | { kind: 'reject'; id: string; reason: string }
  >(null);
  const [refreshKey, setRefreshKey] = useState(0);

  const reload = useCallback(() => setRefreshKey(k => k + 1), []);

  useEffect(() => {
    let alive = true;
    setLoading(true);
    setError(null);

    Promise.all([
      autonomyApi.listProposals({
        state:  stateFilter || undefined,
        source: sourceFilter || undefined,
        limit: PAGE_SIZE,
        offset: page * PAGE_SIZE,
      }),
      autonomyApi.report().catch(() => null),
    ])
      .then(([list, rep]) => {
        if (!alive) return;
        setProposals(list.data);
        setTotal(list.total ?? list.data.length);
        setReport(rep);
      })
      .catch(e => {
        if (!alive) return;
        setError(e instanceof Error ? e.message : t('toast.loadFailed'));
      })
      .finally(() => alive && setLoading(false));

    return () => {
      alive = false;
    };
  }, [stateFilter, sourceFilter, page, refreshKey]);

  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));

  const topByCost = useMemo(() => {
    return [...proposals]
      .sort((a, b) => savingsOf(b) - savingsOf(a))
      .slice(0, 5);
  }, [proposals]);

  const handleApprove = async (id: string) => {
    try {
      await autonomyApi.approve(id);
      toast('success', t('toast.approved'));
      setSelected(null);
      setConfirm(null);
      reload();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.approveFailed'));
    }
  };

  const handleReject = async (id: string, reason: string) => {
    if (!reason.trim()) {
      toast('error', t('toast.rejectReasonRequired'));
      return;
    }
    try {
      await autonomyApi.reject(id, reason);
      toast('success', t('toast.rejected'));
      setSelected(null);
      setConfirm(null);
      reload();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.rejectFailed'));
    }
  };

  const handleRollback = async (id: string) => {
    try {
      await autonomyApi.rollback(id);
      toast('success', t('toast.rollbackStarted'));
      setSelected(null);
      setConfirm(null);
      reload();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.rollbackFailed'));
    }
  };

  if (loading) {
    return (
      <div className="space-y-6 animate-pulse">
        <div className="grid grid-cols-3 gap-4">
          {Array.from({ length: 3 }).map((_, i) => (
            <div
              key={i}
              className="h-28 bg-card border border-border rounded-lg"
            />
          ))}
        </div>
        <div className="h-72 bg-card border border-border rounded-lg" />
      </div>
    );
  }

  if (error) {
    return (
      <div
        className="bg-danger/10 border border-danger/30 text-danger rounded-lg p-4 flex items-start gap-3"
        data-testid="finops-error"
      >
        <AlertTriangle size={18} className="shrink-0 mt-0.5" />
        <div>
          <p className="font-medium">{t('error.title')}</p>
          <p className="text-sm opacity-80 mt-1">{error}</p>
          <button
            onClick={reload}
            className="mt-2 px-3 py-1.5 text-sm rounded-md border border-danger/40 hover:bg-danger/20"
          >
            {t('common:actions.retry')}
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className="space-y-6" data-testid="finops-page">
      {/* Header + 刷新按钮 */}
      <div className="flex items-center justify-between flex-wrap gap-3">
        <h2 className="text-xl font-semibold flex items-center gap-2">
          <ShieldCheck size={22} className="text-primary" />
          {t('title')}
        </h2>
        <button
          onClick={reload}
          className="px-3 py-1.5 text-sm rounded-md bg-card border border-border text-muted hover:text-fg flex items-center gap-1.5"
        >
          <RefreshCcw size={14} />
          {t('common:actions.refresh')}
        </button>
      </div>

      {/* KPI 概览 */}
      <div className="grid grid-cols-1 sm:grid-cols-3 gap-4">
        <StatCard
          title={t('kpi.proposed')}
          value={(report?.totals?.PROPOSED ?? 0).toLocaleString()}
          icon={Clock}
          accent="text-primary"
        />
        <StatCard
          title={t('kpi.applied')}
          value={(report?.totals?.APPLIED ?? 0).toLocaleString()}
          icon={CheckCircle2}
          accent="text-success"
        />
        <StatCard
          title={t('kpi.savings24h')}
          value={`$${(report?.estimated_savings_24h_usd ?? 0).toFixed(2)}`}
          icon={PiggyBank}
          accent="text-success"
        />
      </div>

      {/* 过滤器 */}
      <div
        className="flex flex-wrap items-center gap-3"
        data-testid="finops-filters"
      >
        <span className="text-sm text-muted flex items-center gap-1.5">
          <Filter size={14} />
          {t('filter.label')}
        </span>
        <select
          aria-label="state-filter"
          className="px-3 py-1.5 text-sm rounded-md bg-card border border-border"
          value={stateFilter}
          onChange={e => {
            setStateFilter(e.target.value as ApprovalState | '');
            setPage(0);
          }}
        >
          {STATE_FILTERS.map(opt => (
            <option key={opt.value} value={opt.value}>
              {t(opt.labelKey)}
            </option>
          ))}
        </select>
        <select
          aria-label="source-filter"
          className="px-3 py-1.5 text-sm rounded-md bg-card border border-border"
          value={sourceFilter}
          onChange={e => {
            setSourceFilter(e.target.value as AutonomySource | '');
            setPage(0);
          }}
        >
          {SOURCE_FILTERS.map(opt => (
            <option key={opt.value} value={opt.value}>
              {t(opt.labelKey)}
            </option>
          ))}
        </select>
        <span
          className="text-xs text-muted ml-auto"
          data-testid="finops-result-count"
        >
          {t('common:table.total', { count: total })}
        </span>
      </div>

      {/* Top 5 节省决策表 */}
      {topByCost.length > 0 && (
        <div className="bg-card border border-border rounded-lg p-5">
          <h3 className="text-sm font-medium text-muted mb-3">
            {t('topTable.title')}
          </h3>
          <table className="w-full text-sm">
            <thead className="text-xs text-muted">
              <tr className="border-b border-border">
                <th className="text-left py-2 font-normal">{t('topTable.subject')}</th>
                <th className="text-left py-2 font-normal">{t('topTable.state')}</th>
                <th className="text-right py-2 font-normal">{t('topTable.savings24h')}</th>
                <th className="text-right py-2 font-normal">{t('topTable.affectedRps')}</th>
              </tr>
            </thead>
            <tbody>
              {topByCost.map(p => (
                <tr
                  key={p.id}
                  className="border-b border-border/50 hover:bg-bg cursor-pointer"
                  onClick={() => setSelected(p)}
                >
                  <td className="py-2 font-mono text-xs">{p.subject}</td>
                  <td className="py-2">
                    <span
                      className={`px-2 py-0.5 text-xs rounded border ${STATE_BADGE[p.state]}`}
                    >
                      {p.state}
                    </span>
                  </td>
                  <td className="py-2 text-right text-success">
                    ${savingsOf(p).toFixed(2)}
                  </td>
                  <td className="py-2 text-right text-muted">
                    {payloadField(p, 'affected_requests_per_hour', '—')}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {/* 提案卡片网格 */}
      {proposals.length === 0 ? (
        <div
          className="bg-card border border-border rounded-lg p-10 text-center text-sm text-muted"
          data-testid="finops-empty"
        >
          <ClipboardList
            size={28}
            className="mx-auto text-muted/60 mb-2"
          />
          {t('empty')}
        </div>
      ) : (
        <div
          className="grid grid-cols-1 lg:grid-cols-2 gap-3"
          data-testid="finops-list"
        >
          {proposals.map(p => (
            <button
              key={p.id}
              type="button"
              onClick={() => setSelected(p)}
              className="bg-card border border-border rounded-lg p-4 text-left hover:border-primary/50 transition-colors"
              data-testid={`proposal-${p.id}`}
            >
              <div className="flex items-start justify-between gap-2 mb-2">
                <div>
                  <p className="text-sm font-medium">
                    {p.subject || t('card.noSubject')}
                  </p>
                  <p className="text-xs text-muted mt-0.5">
                    {p.source} · {relative(p.proposed_at_ms)}
                  </p>
                </div>
                <span
                  className={`px-2 py-0.5 text-xs rounded border ${STATE_BADGE[p.state]}`}
                >
                  {p.state}
                </span>
              </div>
              <div className="grid grid-cols-2 gap-2 text-xs text-muted">
                <div>
                  <span className="opacity-70">{t('card.recommendedModel')}</span>
                  <span className="text-fg font-mono">
                    {payloadField(p, 'recommended_model', '—')}
                  </span>
                </div>
                <div>
                  <span className="opacity-70">tier: </span>
                  <span className="text-fg font-mono">
                    {payloadField(p, 'from_quality_tier', '?')} →{' '}
                    {payloadField(p, 'to_quality_tier', '?')}
                  </span>
                </div>
                <div>
                  <span className="opacity-70">{t('card.savings')}</span>
                  <span className="text-success">
                    ${savingsOf(p).toFixed(2)}
                  </span>
                </div>
                <div>
                  <span className="opacity-70">{t('card.tenant')}</span>
                  <span className="text-fg font-mono">
                    {payloadField(p, 'tenant_id', '—')}
                  </span>
                </div>
              </div>
            </button>
          ))}
        </div>
      )}

      {/* 翻页控件 — TASK-20260605-02 P1：后端 total 驱动真分页 */}
      {totalPages > 1 && (
        <div
          className="flex items-center justify-center gap-3 text-sm text-muted"
          data-testid="finops-pagination"
        >
          <button
            type="button"
            onClick={() => setPage(p => Math.max(0, p - 1))}
            disabled={page <= 0}
            className="px-3 py-1.5 rounded-md bg-card border border-border hover:text-fg disabled:opacity-30 disabled:cursor-not-allowed"
            data-testid="finops-prev-page"
          >
            {t('pagination.prev')}
          </button>
          <span data-testid="finops-page-indicator">
            {page + 1} / {totalPages}
          </span>
          <button
            type="button"
            onClick={() => setPage(p => Math.min(totalPages - 1, p + 1))}
            disabled={page >= totalPages - 1}
            className="px-3 py-1.5 rounded-md bg-card border border-border hover:text-fg disabled:opacity-30 disabled:cursor-not-allowed"
            data-testid="finops-next-page"
          >
            {t('pagination.next')}
          </button>
        </div>
      )}

      {/* 详情抽屉 */}
      {selected && (
        <div
          className="fixed inset-0 bg-black/40 z-40 flex justify-end"
          onClick={() => setSelected(null)}
          data-testid="finops-drawer"
        >
          <div
            className="w-full max-w-xl bg-card border-l border-border h-full overflow-auto p-6 space-y-4"
            onClick={e => e.stopPropagation()}
          >
            <div className="flex items-start justify-between">
              <div>
                <h3 className="text-lg font-semibold">{selected.subject}</h3>
                <p className="text-xs text-muted mt-1 font-mono">
                  {selected.id}
                </p>
              </div>
              <button
                onClick={() => setSelected(null)}
                className="text-muted hover:text-fg"
              >
                <XCircle size={20} />
              </button>
            </div>

            <div className="grid grid-cols-2 gap-2 text-sm">
              <div>
                <p className="text-xs text-muted">{t('drawer.state')}</p>
                <p>
                  <span
                    className={`px-2 py-0.5 text-xs rounded border ${STATE_BADGE[selected.state]}`}
                  >
                    {selected.state}
                  </span>
                </p>
              </div>
              <div>
                <p className="text-xs text-muted">{t('drawer.source')}</p>
                <p className="font-mono text-xs">{selected.source}</p>
              </div>
              <div>
                <p className="text-xs text-muted">{t('drawer.proposedAt')}</p>
                <p>{relative(selected.proposed_at_ms)}</p>
              </div>
              <div>
                <p className="text-xs text-muted">{t('drawer.reviewer')}</p>
                <p className="font-mono text-xs">
                  {selected.reviewer_user_id || '—'}
                </p>
              </div>
            </div>

            {selected.reject_reason && (
              <div className="bg-danger/10 border border-danger/30 rounded-lg p-3 text-sm text-danger">
                <p className="font-medium mb-1">{t('drawer.rejectReason')}</p>
                <p className="opacity-90">{selected.reject_reason}</p>
              </div>
            )}

            <div>
              <p className="text-xs text-muted mb-1">Payload</p>
              <pre
                data-testid="proposal-payload"
                className="bg-bg border border-border rounded-md p-3 text-xs overflow-auto"
              >
                {JSON.stringify(selected.payload, null, 2)}
              </pre>
            </div>

            <div>
              <p className="text-xs text-muted mb-1">decision_trace</p>
              <pre
                data-testid="proposal-decision-trace"
                className="bg-bg border border-border rounded-md p-3 text-xs overflow-auto"
              >
                {JSON.stringify(selected.decision_trace, null, 2)}
              </pre>
              <p className="text-xs text-muted mt-1">
                payload_sha256: <code>{selected.payload_sha256 || '—'}</code>
              </p>
            </div>

            <div className="flex flex-wrap gap-2 pt-2 border-t border-border">
              {selected.state === 'PROPOSED' && (
                <>
                  <button
                    onClick={() =>
                      setConfirm({ kind: 'approve', id: selected.id })
                    }
                    className="px-3 py-1.5 text-sm rounded-md bg-success/15 text-success border border-success/30 hover:bg-success/25"
                    data-testid="action-approve"
                  >
                    {t('actions.approve')}
                  </button>
                  <button
                    onClick={() =>
                      setConfirm({
                        kind: 'reject',
                        id: selected.id,
                        reason: '',
                      })
                    }
                    className="px-3 py-1.5 text-sm rounded-md bg-danger/15 text-danger border border-danger/30 hover:bg-danger/25"
                    data-testid="action-reject"
                  >
                    {t('actions.reject')}
                  </button>
                </>
              )}
              {selected.state === 'APPLIED' && (
                <button
                  onClick={() =>
                    setConfirm({ kind: 'rollback', id: selected.id })
                  }
                  className="px-3 py-1.5 text-sm rounded-md bg-warning/15 text-warning border border-warning/30 hover:bg-warning/25 flex items-center gap-1.5"
                  data-testid="action-rollback"
                >
                  <Undo2 size={14} />
                  {t('actions.rollback')}
                </button>
              )}
            </div>
          </div>
        </div>
      )}

      {/* 二次确认 modal — TASK-20260602-01 Epic 7.1: 改用共享 ConfirmDialog
          组件（含 children slot 用于 reject 操作的 reason textarea）。原内联
          modal ~60 行已消除，保留 data-testid 钩子以兼容既有 FinOps 测试。 */}
      <ConfirmDialog
        open={!!confirm}
        danger={confirm?.kind === 'reject' || confirm?.kind === 'rollback'}
        testId="finops-confirm-modal"
        confirmTestId="finops-confirm-go"
        title={
          confirm?.kind === 'approve'  ? t('confirm.approveTitle') :
          confirm?.kind === 'reject'   ? t('confirm.rejectTitle') :
          confirm?.kind === 'rollback' ? t('confirm.rollbackTitle') : ''
        }
        message={
          confirm?.kind === 'rollback'
            ? t('confirm.rollbackMessage')
            : t('confirm.defaultMessage')
        }
        onCancel={() => setConfirm(null)}
        onConfirm={() => {
          if (!confirm) return;
          if (confirm.kind === 'approve') handleApprove(confirm.id);
          else if (confirm.kind === 'reject') handleReject(confirm.id, confirm.reason);
          else if (confirm.kind === 'rollback') handleRollback(confirm.id);
        }}
      >
        {confirm?.kind === 'reject' && (
          <textarea
            value={confirm.reason}
            onChange={e =>
              setConfirm({ ...confirm, reason: e.target.value })
            }
            placeholder={t('rejectReasonPlaceholder')}
            rows={3}
            aria-label="reject-reason"
            className="w-full bg-bg border border-border rounded-md p-2 text-sm"
            data-testid="reject-reason-input"
          />
        )}
      </ConfirmDialog>
    </div>
  );
}
