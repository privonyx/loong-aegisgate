// TASK-20260605-01 — 合规导出对话框（D2=C：from/to 日期 + 格式选择）。
// 复用既有受 RBAC 保护的 export* 端点（SR-3：前端无新暴露面，后端 TenantAdmin + 跨租户 403）。
import { useState } from 'react';
import { Download, X, Loader2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useToast } from './Toast';
import { downloadReport } from '../lib/download';
import type { ExportReport } from '../types';

interface ExportDialogProps {
  open: boolean;
  title: string;
  // 当前列表的租户过滤（SuperAdmin 可跨租户；非 super 跨租户后端返回 403）。
  tenantId: string;
  filenameBase: string;
  exportFn: (from: string, to: string, tenantId: string, format: 'csv' | 'json') => Promise<ExportReport>;
  onClose: () => void;
}

export default function ExportDialog({
  open, title, tenantId, filenameBase, exportFn, onClose,
}: ExportDialogProps) {
  const [from, setFrom] = useState('');
  const [to, setTo] = useState('');
  const [format, setFormat] = useState<'csv' | 'json'>('csv');
  const [busy, setBusy] = useState(false);
  const { toast } = useToast();
  const { t } = useTranslation();

  if (!open) return null;

  const handleExport = async () => {
    setBusy(true);
    try {
      const report = await exportFn(from, to, tenantId, format);
      downloadReport(report, filenameBase);
      toast('success', t('export.started'));
      onClose();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('export.failed'));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div
        className="bg-card border border-border rounded-lg w-full max-w-md p-6"
        onClick={e => e.stopPropagation()}
        data-testid="export-dialog"
      >
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{title}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>

        <div className="space-y-3">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-sm text-muted mb-1">{t('export.fromDate')}</label>
              <input type="date" aria-label={t('export.fromDate')} value={from} onChange={e => setFrom(e.target.value)} className="w-full text-sm" />
            </div>
            <div>
              <label className="block text-sm text-muted mb-1">{t('export.toDate')}</label>
              <input type="date" aria-label={t('export.toDate')} value={to} onChange={e => setTo(e.target.value)} className="w-full text-sm" />
            </div>
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">{t('export.format')}</label>
            <select aria-label={t('export.formatLabel')} value={format} onChange={e => setFormat(e.target.value as 'csv' | 'json')} className="w-full text-sm">
              <option value="csv">CSV</option>
              <option value="json">JSON</option>
            </select>
          </div>
          <p className="text-xs text-muted">{t('export.hint')}</p>
        </div>

        <div className="flex justify-end gap-3 pt-5">
          <button onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('actions.cancel')}</button>
          <button
            onClick={handleExport}
            data-testid="confirm-export"
            disabled={busy}
            className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors"
          >
            {busy ? <Loader2 size={14} className="animate-spin" /> : <Download size={14} />}
            {t('actions.export')}
          </button>
        </div>
      </div>
    </div>
  );
}
