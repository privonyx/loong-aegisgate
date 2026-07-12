import { useEffect, useRef, type ReactNode } from 'react';
import { AlertTriangle, X } from 'lucide-react';
import { useTranslation } from 'react-i18next';

interface ConfirmDialogProps {
  open: boolean;
  title: string;
  message: string;
  confirmLabel?: string;
  onConfirm: () => void;
  onCancel: () => void;
  danger?: boolean;
  // TASK-20260602-01 Epic 7.1 — optional content slot 用于在 message 与按钮之间
  // 渲染额外控件（如 FinOps reject 操作的拒绝原因 textarea）。
  children?: ReactNode;
  // TASK-20260602-01 Epic 7.1 — optional data-testid 钩子，方便 caller 保留
  // 自有测试断言（如 FinOps.test.tsx 现有 'finops-confirm-modal' 测试）。
  testId?: string;
  confirmTestId?: string;
}

export default function ConfirmDialog({
  open, title, message, confirmLabel, onConfirm, onCancel, danger,
  children, testId, confirmTestId,
}: ConfirmDialogProps) {
  const { t } = useTranslation();
  const dialogRef = useRef<HTMLDialogElement>(null);

  useEffect(() => {
    if (open) dialogRef.current?.showModal();
    else dialogRef.current?.close();
  }, [open]);

  if (!open) return null;

  return (
    <dialog
      ref={dialogRef}
      className="bg-card border border-border rounded-lg p-0 max-w-md w-full backdrop:bg-black/50"
      onCancel={onCancel}
      data-testid={testId}
    >
      <div className="p-6">
        <div className="flex items-start gap-4">
          <div className={`p-2 rounded-lg ${danger ? 'bg-danger/10 text-danger' : 'bg-warning/10 text-warning'}`}>
            <AlertTriangle size={20} />
          </div>
          <div className="flex-1">
            <h3 className="font-semibold text-fg">{title}</h3>
            <p className="text-sm text-muted mt-2">{message}</p>
          </div>
          <button onClick={onCancel} className="text-muted hover:text-fg">
            <X size={18} />
          </button>
        </div>

        {children && <div className="mt-4">{children}</div>}

        <div className="flex justify-end gap-3 mt-6">
          <button
            onClick={onCancel}
            className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg hover:bg-bg transition-colors"
          >
            {t('actions.cancel')}
          </button>
          <button
            onClick={onConfirm}
            data-testid={confirmTestId}
            className={`px-4 py-2 text-sm rounded-md text-white transition-colors ${
              danger ? 'bg-danger hover:bg-danger/80' : 'bg-primary hover:bg-primary/80'
            }`}
          >
            {confirmLabel ?? t('actions.confirm')}
          </button>
        </div>
      </div>
    </dialog>
  );
}
