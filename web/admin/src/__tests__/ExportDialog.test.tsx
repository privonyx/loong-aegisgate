// TASK-20260605-01 Epic B — 合规导出对话框测试（SR-3：仅复用既有 export 端点）。
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ToastProvider } from '../components/Toast';
import ExportDialog from '../components/ExportDialog';

vi.mock('../lib/download', () => ({ downloadReport: vi.fn() }));
import { downloadReport } from '../lib/download';

function setup(exportFn: ReturnType<typeof vi.fn>) {
  return render(
    <ToastProvider>
      <ExportDialog
        open
        title="导出审计日志"
        tenantId="t1"
        filenameBase="audit-report"
        exportFn={exportFn as never}
        onClose={() => {}}
      />
    </ToastProvider>
  );
}

describe('ExportDialog', () => {
  beforeEach(() => vi.clearAllMocks());

  it('选日期+格式后导出：以当前 tenant 调既有 exportFn 并触发下载', async () => {
    const exportFn = vi.fn().mockResolvedValue({ format: 'csv', data: 'a,b\n1,2' });
    const user = userEvent.setup();
    setup(exportFn);
    fireEvent.change(screen.getByLabelText('起始日期'), { target: { value: '2026-01-01' } });
    fireEvent.change(screen.getByLabelText('结束日期'), { target: { value: '2026-12-31' } });
    await user.click(screen.getByTestId('confirm-export'));
    await waitFor(() => expect(exportFn).toHaveBeenCalledWith('2026-01-01', '2026-12-31', 't1', 'csv'));
    expect(downloadReport).toHaveBeenCalledWith({ format: 'csv', data: 'a,b\n1,2' }, 'audit-report');
  });

  it('切换 JSON 格式透传给 exportFn', async () => {
    const exportFn = vi.fn().mockResolvedValue({ format: 'json', data: [{ x: 1 }] });
    const user = userEvent.setup();
    setup(exportFn);
    fireEvent.change(screen.getByLabelText('导出格式'), { target: { value: 'json' } });
    await user.click(screen.getByTestId('confirm-export'));
    await waitFor(() => expect(exportFn).toHaveBeenCalledWith('', '', 't1', 'json'));
  });

  it('导出失败显示错误 toast 且不触发下载', async () => {
    const exportFn = vi.fn().mockRejectedValue(new Error('boom'));
    const user = userEvent.setup();
    setup(exportFn);
    await user.click(screen.getByTestId('confirm-export'));
    expect(await screen.findByText(/boom/)).toBeInTheDocument();
    expect(downloadReport).not.toHaveBeenCalled();
  });
});
