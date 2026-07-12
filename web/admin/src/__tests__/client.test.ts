// @vitest-environment node
//
// SR2: 测试 401 跳转必须落到 /admin/login（与 BrowserRouter basename 一致），
// 不能跳转到根路径 /login。
//
// 使用 node 环境 + 自己 stub window，避开当前 jsdom@29 + html-encoding-sniffer@6
// 在 node 20 下的 ERR_REQUIRE_ESM 兼容性 bug（与本任务无关，预留独立 issue）。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { api } from '../api/client';

interface FakeLocation {
  href: string;
  pathname: string;
}

describe('api client', () => {
  let fetchMock: ReturnType<typeof vi.fn>;
  let fakeLocation: FakeLocation;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);

    fakeLocation = { href: '', pathname: '/admin/dashboard' };
    vi.stubGlobal('window', { location: fakeLocation });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  function mockJson(status: number, body: unknown) {
    return {
      ok: status >= 200 && status < 300,
      status,
      statusText: status === 200 ? 'OK' : 'Error',
      json: () => Promise.resolve(body),
    };
  }

  it('使用 /admin/api 作为 BASE 前缀', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { id: 'u1', tenant_id: 't1', role: 'admin' }));
    await api.me();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/me',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('login 调用 /admin/api/auth/login', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { id: 'u1', tenant_id: 't1', role: 'admin' }));
    await api.login('sk_test');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/auth/login',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ api_key: 'sk_test' }),
      }),
    );
  });

  it('logout 调用 /admin/api/auth/logout', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { logged_out: true }));
    await api.logout();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/auth/logout',
      expect.objectContaining({ method: 'POST' }),
    );
  });

  it('401 响应跳转到 /admin/login（不是根路径 /login）', async () => {
    fetchMock.mockResolvedValue(mockJson(401, {}));
    await expect(api.me()).rejects.toThrow('Unauthorized');
    expect(fakeLocation.href).toBe('/admin/login');
  });

  it('401 在登录端点上不重定向（避免循环）', async () => {
    fetchMock.mockResolvedValue(mockJson(401, {}));
    await expect(api.login('bad')).rejects.toThrow('Unauthorized');
    expect(fakeLocation.href).toBe('');
  });

  it('401 在 /admin/login 页面上不重定向（避免循环）', async () => {
    fakeLocation.pathname = '/admin/login';
    fetchMock.mockResolvedValue(mockJson(401, {}));
    await expect(api.me()).rejects.toThrow('Unauthorized');
    expect(fakeLocation.href).toBe('');
  });

  it('listTenants 拼接查询参数到 /admin/api/tenants', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { items: [], total: 0 }));
    await api.listTenants(50, 10);
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/tenants?limit=50&offset=10',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('dashboardSummary 调用 /admin/api/dashboard/summary', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { tenants: 1, users: 1, api_keys: 1 }));
    await api.dashboardSummary();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/dashboard/summary',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  // TASK-20260605-02 P0：成本导出 format=json 必须透传到后端 URL（后端补 json 分支）。
  it('exportCostReport 透传 format=json 到 /admin/api/export/cost', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { format: 'json', data: [] }));
    await api.exportCostReport('2026-01-01', '2026-12-31', 't1', 'json');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/export/cost?format=json&from=2026-01-01&to=2026-12-31&tenant_id=t1',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });
});
