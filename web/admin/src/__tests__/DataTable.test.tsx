// TASK-20260602-01 Epic 6 — DataTable unit tests.
//
// 覆盖:
//   1. loading=true 显示 skeleton（无表头/表行）
//   2. data=[] 显示"暂无数据"占位行
//   3. 渲染 columns 表头 + 各行 cell
//   4. total > pageSize 时显示分页 + click 翻页触发 onPageChange

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, it, expect, vi } from 'vitest';
import DataTable, { type Column } from '../components/DataTable';

interface Row {
  id: string;
  name: string;
}

const cols: Column<Row>[] = [
  { key: 'id', header: 'ID' },
  { key: 'name', header: '名称' },
];

describe('DataTable', () => {
  it('loading=true 显示 skeleton，不显示表头', () => {
    render(
      <DataTable<Row>
        columns={cols} data={[]} total={0} page={0} pageSize={10}
        onPageChange={vi.fn()} loading
      />,
    );
    expect(screen.queryByText('ID')).not.toBeInTheDocument();
    expect(screen.queryByText('名称')).not.toBeInTheDocument();
  });

  it('空数据显示占位行 + 不分页', () => {
    const onPageChange = vi.fn();
    render(
      <DataTable<Row>
        columns={cols} data={[]} total={0} page={0} pageSize={10}
        onPageChange={onPageChange}
      />,
    );
    expect(screen.getByText('ID')).toBeInTheDocument();
    expect(screen.getByText('暂无数据')).toBeInTheDocument();
    // 不应显示分页（totalPages=1）
    expect(screen.queryByText(/共/)).not.toBeInTheDocument();
  });

  it('渲染 columns + 各行 cell', () => {
    const rows: Row[] = [
      { id: 'r1', name: 'Alice' },
      { id: 'r2', name: 'Bob' },
    ];
    render(
      <DataTable<Row>
        columns={cols} data={rows} total={2} page={0} pageSize={10}
        onPageChange={vi.fn()}
      />,
    );
    expect(screen.getByText('ID')).toBeInTheDocument();
    expect(screen.getByText('Alice')).toBeInTheDocument();
    expect(screen.getByText('Bob')).toBeInTheDocument();
    expect(screen.getByText('r1')).toBeInTheDocument();
  });

  it('total > pageSize 显示分页 + click 下一页触发 onPageChange', () => {
    const rows: Row[] = Array.from({ length: 10 }, (_, i) => ({
      id: `r${i}`, name: `n${i}`,
    }));
    const onPageChange = vi.fn();
    render(
      <DataTable<Row>
        columns={cols} data={rows} total={50} page={0} pageSize={10}
        onPageChange={onPageChange}
      />,
    );
    // 显示页码 "1 / 5"
    expect(screen.getByText('1 / 5')).toBeInTheDocument();
    expect(screen.getByText(/共\s*50\s*条/)).toBeInTheDocument();
    // 找到非禁用的"下一页"按钮（lucide ChevronRight icon button）
    const buttons = screen.getAllByRole('button');
    const enabledButtons = buttons.filter(b => !b.hasAttribute('disabled'));
    // 第一页只有 next 按钮可用
    expect(enabledButtons.length).toBe(1);
    fireEvent.click(enabledButtons[0]!);
    expect(onPageChange).toHaveBeenCalledWith(1);
  });
});
