// @vitest-environment node
//
// TASK-20260614-03 — adminFetch 错误本地化测试。
// 后端稳定 error.code（AEGIS-xxxx）→ 前端 errors namespace 本地化；
// 未知 code 回退后端 message。

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { adminFetch, ApiError } from '../api/request';
import i18n from '../i18n';

describe('adminFetch error localization (TASK-20260614-03)', () => {
  let fetchMock: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    vi.stubGlobal('window', { location: { href: '', pathname: '/admin/x' } });
    i18n.changeLanguage('zh-CN');
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  function mockResp(status: number, body: unknown) {
    return {
      ok: status >= 200 && status < 300,
      status,
      statusText: 'Error',
      json: () => Promise.resolve(body),
    };
  }

  it('通用默认 message（等于 code 默认值）按 zh-CN 本地化', async () => {
    // 后端未覆盖 message → 输出 toDefaultMessage(code) 英文默认值。
    fetchMock.mockResolvedValue(
      mockResp(403, { error: { code: 'AEGIS-1002', message: 'Insufficient permissions' } }),
    );
    await expect(adminFetch('', '/x')).rejects.toThrow('权限不足');
  });

  it('en-US 下同一 code 显示英文默认文案', async () => {
    i18n.changeLanguage('en-US');
    fetchMock.mockResolvedValue(
      mockResp(403, { error: { code: 'AEGIS-1002', message: 'Insufficient permissions' } }),
    );
    await expect(adminFetch('', '/x')).rejects.toThrow('Insufficient permissions');
  });

  it('后端携带具体细节的 message 原样透传（不被 code 覆盖）', async () => {
    // message ≠ 通用默认 → 含动态细节，保留（与 client_savings 契约一致）。
    fetchMock.mockResolvedValue(
      mockResp(400, { error: { code: 'AEGIS-5001', message: 'time_range too large (max 365 days)' } }),
    );
    await expect(adminFetch('', '/x')).rejects.toThrow('time_range too large (max 365 days)');
  });

  it('未知 code 回退到后端 message', async () => {
    fetchMock.mockResolvedValue(
      mockResp(400, { error: { code: 'AEGIS-9999', message: 'weird backend error' } }),
    );
    await expect(adminFetch('', '/x')).rejects.toThrow('weird backend error');
  });

  it('无 error 包络时回退 statusText', async () => {
    fetchMock.mockResolvedValue({
      ok: false,
      status: 500,
      statusText: 'Internal Error',
      json: () => Promise.reject(new Error('no json')),
    });
    await expect(adminFetch('', '/x')).rejects.toThrow('Internal Error');
  });

  // TASK-20260703-02 — 抛出的错误须携带 HTTP status + 后端 code，
  // 供调用方按状态分级处理（Guard 页 D5：404 空态 / 503 banner / 429 提示）。
  it('非 2xx 抛 ApiError 且携带 status 与 code', async () => {
    fetchMock.mockResolvedValue(
      mockResp(404, { error: { code: 'AEGIS-6003', message: 'Approval proposal not found' } }),
    );
    const err = (await adminFetch('', '/x').catch(e => e)) as ApiError;
    expect(err).toBeInstanceOf(ApiError);
    expect(err.status).toBe(404);
    expect(err.code).toBe('AEGIS-6003');
  });

  it('无 error 包络的 ApiError 仍带 status（code 为 undefined）', async () => {
    fetchMock.mockResolvedValue({
      ok: false,
      status: 503,
      statusText: 'Unavailable',
      json: () => Promise.reject(new Error('no json')),
    });
    const err = (await adminFetch('', '/x').catch(e => e)) as ApiError;
    expect(err).toBeInstanceOf(ApiError);
    expect(err.status).toBe(503);
  });
});
