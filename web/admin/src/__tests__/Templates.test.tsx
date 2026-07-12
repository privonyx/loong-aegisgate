// TASK-20260603-01 Epic 1 — Prompt Template CRUD 页测试。
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ToastProvider } from '../components/Toast';
import type { PromptTemplate } from '../types';

vi.mock('../api/client', () => ({
  api: {
    listPromptTemplates: vi.fn(),
    createPromptTemplate: vi.fn(),
    updatePromptTemplate: vi.fn(),
    deletePromptTemplate: vi.fn(),
  },
}));

import { api } from '../api/client';
import Templates from '../pages/Templates';

const t1: PromptTemplate = {
  id: 'tpl-1', tenant_id: 'tn-1', name: '欢迎语', content: 'Hello {{name}}',
  version: 2, weight: 80, is_active: true,
  created_at: '2026-06-01T00:00:00Z', updated_at: '2026-06-02T00:00:00Z',
};

function renderPage() {
  return render(<ToastProvider><Templates /></ToastProvider>);
}

describe('Templates 页', () => {
  beforeEach(() => {
    vi.mocked(api.listPromptTemplates).mockResolvedValue({ data: [t1], count: 1 });
    vi.mocked(api.createPromptTemplate).mockResolvedValue(t1);
    vi.mocked(api.updatePromptTemplate).mockResolvedValue(t1);
    vi.mocked(api.deletePromptTemplate).mockResolvedValue({ deleted: true, id: 'tpl-1' });
  });

  it('加载并渲染模板列表', async () => {
    renderPage();
    expect(await screen.findByText('欢迎语')).toBeInTheDocument();
    expect(api.listPromptTemplates).toHaveBeenCalled();
  });

  it('点击新建打开表单并提交创建', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('欢迎语');
    await user.click(screen.getByRole('button', { name: /新建模板/ }));
    await user.type(screen.getByLabelText(/名称/), '新模板');
    await user.type(screen.getByLabelText(/内容/), '正文内容');
    await user.click(screen.getByRole('button', { name: /^创建$/ }));
    await waitFor(() => expect(api.createPromptTemplate).toHaveBeenCalled());
    const body = vi.mocked(api.createPromptTemplate).mock.calls[0][0];
    expect(body.name).toBe('新模板');
    expect(body.content).toBe('正文内容');
  });

  it('删除确认后调用 deletePromptTemplate', async () => {
    const user = userEvent.setup();
    renderPage();
    await screen.findByText('欢迎语');
    await user.click(screen.getByTitle('删除'));
    await user.click(screen.getByTestId('confirm-delete'));
    await waitFor(() => expect(api.deletePromptTemplate).toHaveBeenCalledWith('tpl-1'));
  });
});
