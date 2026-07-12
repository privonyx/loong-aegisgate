import { useEffect, useState, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { Radio, Download } from 'lucide-react';
import DataTable, { type Column } from '../components/DataTable';
import ExportDialog from '../components/ExportDialog';
import { useToast } from '../components/Toast';
import { useWebSocket } from '../hooks/useWebSocket';
import { api } from '../api/client';
import type { AuditRecord, WsMessage } from '../types';

const PAGE_SIZE = 50;

export default function AuditsPage() {
  const [data, setData] = useState<AuditRecord[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [filterTenant, setFilterTenant] = useState('');
  const [liveMode, setLiveMode] = useState(false);
  const [showExport, setShowExport] = useState(false);
  const { toast } = useToast();
  const { t } = useTranslation('audits');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.queryAudits(filterTenant, PAGE_SIZE, page * PAGE_SIZE);
      setData(res.data);
      setTotal(res.total ?? res.count);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [page, filterTenant, toast, t]);

  useEffect(() => { if (!liveMode) load(); }, [load, liveMode]);

  const handleWsMessage = useCallback((msg: WsMessage) => {
    if (msg.type === 'audit' && liveMode) {
      // TASK-20260602-01 Epic 1: 适配后端 nested data envelope（D1=B）。
      // 后端字段名 stage → 前端 AuditRecord.stage_name 映射。
      const a: AuditRecord = {
        request_id: msg.data.request_id,
        timestamp: msg.data.timestamp,
        tenant_id: msg.data.tenant_id,
        action: msg.data.action,
        stage_name: msg.data.stage,
        detail: msg.data.detail,
      };
      setData(prev => [a, ...prev].slice(0, 200));
      setTotal(prev => prev + 1);
    }
  }, [liveMode]);

  useWebSocket({ onMessage: handleWsMessage, enabled: liveMode });

  const columns: Column<AuditRecord>[] = [
    { key: 'timestamp', header: t('columns.timestamp'), render: (r) => new Date(r.timestamp).toLocaleString(), className: 'whitespace-nowrap' },
    { key: 'request_id', header: t('columns.requestId'), render: (r) => <span className="font-mono text-xs">{r.request_id.slice(0, 12)}...</span> },
    { key: 'tenant_id', header: t('columns.tenant'), render: (r) => <span className="font-mono text-xs">{r.tenant_id.slice(0, 8)}...</span> },
    { key: 'action', header: t('common:table.actions'), render: (r) => <span className="text-primary font-medium">{r.action}</span> },
    { key: 'stage_name', header: t('columns.stage') },
    { key: 'detail', header: t('columns.detail'), render: (r) => <span className="truncate max-w-xs block">{r.detail}</span> },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <div className="flex items-center gap-3">
          <input placeholder={t('filter.tenant')} value={filterTenant} onChange={e => { setFilterTenant(e.target.value); setPage(0); }} className="text-sm w-48" />
          <button
            onClick={() => setShowExport(true)}
            className="flex items-center gap-1.5 px-3 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors"
          >
            <Download size={14} /> {t('common:actions.export')}
          </button>
          <button
            onClick={() => setLiveMode(!liveMode)}
            className={`flex items-center gap-1.5 px-3 py-2 text-sm rounded-md border transition-colors ${
              liveMode ? 'border-success text-success bg-success/10' : 'border-border text-muted hover:text-fg'
            }`}
          >
            <Radio size={14} className={liveMode ? 'animate-pulse' : ''} />
            {liveMode ? t('mode.live') : t('mode.history')}
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
        loading={loading && !liveMode}
      />

      <ExportDialog
        open={showExport}
        title={t('exportDialogTitle')}
        tenantId={filterTenant}
        filenameBase="audit-report"
        exportFn={api.exportAuditReport}
        onClose={() => setShowExport(false)}
      />
    </div>
  );
}
