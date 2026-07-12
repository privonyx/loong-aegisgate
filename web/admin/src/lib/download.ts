// TASK-20260605-01 — 合规导出下载 helper。
// 把后端 ExportReport（csv 字符串 / json 对象）落地为浏览器文件下载。
// 纯前端，不发起任何额外请求（导出数据已由 client 的 export* 方法取回）。
import type { ExportReport } from '../types';

export function downloadReport(report: ExportReport, filenameBase: string): void {
  const isCsv = report.format === 'csv';
  const content = isCsv
    ? String(report.data ?? '')
    : JSON.stringify(report.data ?? null, null, 2);
  const mime = isCsv ? 'text/csv;charset=utf-8' : 'application/json';
  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `${filenameBase}.${report.format}`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}
