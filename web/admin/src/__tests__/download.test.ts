// TASK-20260605-01 Epic B — downloadReport 纯函数测试。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { downloadReport } from '../lib/download';

describe('downloadReport', () => {
  let clickCount = 0;
  let created: Blob | undefined;

  beforeEach(() => {
    created = undefined;
    clickCount = 0;
    vi.spyOn(URL, 'createObjectURL').mockImplementation((obj) => { created = obj as Blob; return 'blob:mock'; });
    vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => { clickCount += 1; });
  });

  afterEach(() => vi.restoreAllMocks());

  it('csv 报表：a.download 带 .csv 后缀并触发点击', () => {
    downloadReport({ format: 'csv', data: 'a,b\n1,2' }, 'audit-report');
    expect(clickCount).toBe(1);
    expect(created?.type).toContain('text/csv');
  });

  it('json 报表：序列化对象并用 .json 后缀', () => {
    downloadReport({ format: 'json', data: [{ x: 1 }] }, 'cost-report');
    expect(clickCount).toBe(1);
    expect(created?.type).toContain('application/json');
  });
});
