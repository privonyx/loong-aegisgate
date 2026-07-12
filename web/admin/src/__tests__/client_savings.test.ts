// @vitest-environment node
//
// Savings Summary client API 测试（TASK-20260510-01 Epic 3.3）。
// 参考 client.test.ts 双向缺陷注入模式：测试 query string 拼接、tenant_id 注入、
// 401 重定向行为；不依赖 happy-dom。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { api } from '../api/client';

interface FakeLocation {
  href: string;
  pathname: string;
}

describe('api.savingsSummary', () => {
  let fetchMock: ReturnType<typeof vi.fn>;
  let fakeLocation: FakeLocation;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    fakeLocation = { href: '', pathname: '/admin/savings' };
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

  const emptyResp = {
    from: '', to: '', aggregator_since: null,
    total_cost_saved: 0, total_cost_actual: 0, roi_percent: 0,
    total_tokens_saved: 0, total_cache_hits: 0, fallback_pricing_count: 0,
    by_type: [], by_model: [], time_series: [],
    top_tenants: [], routing_recommendations: [],
  };

  it('无参数时不附加 query string', async () => {
    fetchMock.mockResolvedValue(mockJson(200, emptyResp));
    await api.savingsSummary();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/savings/summary',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('from + to 拼接到 query string', async () => {
    fetchMock.mockResolvedValue(mockJson(200, emptyResp));
    await api.savingsSummary('2026-05-01T00:00:00Z', '2026-05-10T23:59:59Z');
    const call = fetchMock.mock.calls[0][0] as string;
    expect(call.startsWith('/admin/api/savings/summary?')).toBe(true);
    expect(call).toContain('from=2026-05-01T00%3A00%3A00Z');
    expect(call).toContain('to=2026-05-10T23%3A59%3A59Z');
  });

  it('tenant_id 仅在非空时注入', async () => {
    fetchMock.mockResolvedValue(mockJson(200, emptyResp));
    await api.savingsSummary('', '', 'tenant-A');
    const call = fetchMock.mock.calls[0][0] as string;
    expect(call).toContain('tenant_id=tenant-A');
    expect(call).not.toContain('from=');
    expect(call).not.toContain('to=');
  });

  it('返回的 SavingsSummary 字段透传', async () => {
    const fixture = {
      ...emptyResp,
      total_cost_saved: 12.34,
      total_tokens_saved: 5678,
      fallback_pricing_count: 2,
      by_type: [
        { type: 'cache_hit', cost_saved: 10.0, tokens_saved: 5000, event_count: 50 },
        { type: 'compression', cost_saved: 2.34, tokens_saved: 678, event_count: 12 },
      ],
      top_tenants: [
        { tenant_id: 'tenant-A', cost_saved: 12.34, tokens_saved: 5678, event_count: 62 },
      ],
    };
    fetchMock.mockResolvedValue(mockJson(200, fixture));
    const result = await api.savingsSummary();
    expect(result.total_cost_saved).toBe(12.34);
    expect(result.by_type).toHaveLength(2);
    expect(result.top_tenants[0].tenant_id).toBe('tenant-A');
    expect(result.fallback_pricing_count).toBe(2);
  });

  it('401 时跳转到 /admin/login（保持 SPA basename 一致）', async () => {
    fetchMock.mockResolvedValue(mockJson(401, {}));
    await expect(api.savingsSummary()).rejects.toThrow('Unauthorized');
    expect(fakeLocation.href).toBe('/admin/login');
  });

  it('400 时透传后端 error.message', async () => {
    fetchMock.mockResolvedValue(mockJson(400, {
      error: {
        code: 'AEGIS-5001',
        type: 'invalid_request_error',
        message: 'time_range too large (max 365 days)',
      },
    }));
    await expect(api.savingsSummary('2020-01-01T00:00:00Z', '2026-12-31T23:59:59Z'))
      .rejects.toThrow('time_range too large (max 365 days)');
  });
});

describe('api.securityEvents', () => {
  let fetchMock: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    vi.stubGlobal('window', { location: { href: '', pathname: '/admin/dashboard' } });
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

  it('GET /admin/api/security/events', async () => {
    fetchMock.mockResolvedValue(mockJson(200, {
      timestamp: '2026-05-10T09:00:00Z',
      scope: 'global',
      guardrail_blocks_total: 0,
      preprocessor_normalized_total: 5,
      rate_limited_total: 1,
    }));
    const r = await api.securityEvents();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/security/events',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
    expect(r.scope).toBe('global');
    expect(r.preprocessor_normalized_total).toBe(5);
  });
});
