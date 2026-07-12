// @vitest-environment node
//
// TASK-20260603-01 — Feature Gap 5 域 api 方法契约测试。
// SR-4：全部新方法必须经 request()（adminFetch 封装），断言 credentials:same-origin
// 透传 + path/method/body 与后端契约（spec §2）一致。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { api } from '../api/client';

describe('feature-gap api client', () => {
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

  // --- Prompt Templates ---
  it('listPromptTemplates 拼接 query 到 /admin/api/templates', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { data: [], count: 0 }));
    await api.listPromptTemplates('t1', 20, 40);
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/templates?tenant_id=t1&limit=20&offset=40',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('createPromptTemplate POST /admin/api/templates 带 body', async () => {
    fetchMock.mockResolvedValue(mockJson(200, {}));
    await api.createPromptTemplate({ name: 'n', content: 'c', weight: 50, is_active: true });
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/templates',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ name: 'n', content: 'c', weight: 50, is_active: true }),
      }),
    );
  });

  it('updatePromptTemplate PUT /admin/api/templates/{id}', async () => {
    fetchMock.mockResolvedValue(mockJson(200, {}));
    await api.updatePromptTemplate('id1', { weight: 70 });
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/templates/id1',
      expect.objectContaining({ method: 'PUT', body: JSON.stringify({ weight: 70 }) }),
    );
  });

  it('deletePromptTemplate DELETE /admin/api/templates/{id}', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { deleted: true, id: 'id1' }));
    await api.deletePromptTemplate('id1');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/templates/id1',
      expect.objectContaining({ method: 'DELETE' }),
    );
  });

  // --- Rule Sets ---
  it('listRuleSets 拼接 query 到 /admin/api/rules', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { data: [], count: 0 }));
    await api.listRuleSets('t1', 50);
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/rules?tenant_id=t1&limit=50&offset=0',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('createRuleSet POST /admin/api/rules 带 rules', async () => {
    fetchMock.mockResolvedValue(mockJson(200, {}));
    await api.createRuleSet({ rules: [{ a: 1 }] });
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/rules',
      expect.objectContaining({ method: 'POST', body: JSON.stringify({ rules: [{ a: 1 }] }) }),
    );
  });

  it('activateRuleSet POST /admin/api/rules/activate 带 version', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { activated: true, version: 3 }));
    await api.activateRuleSet(3, 't1');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/rules/activate',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ version: 3, tenant_id: 't1' }),
      }),
    );
  });

  // --- SSO Providers ---
  it('listSsoProviders GET /admin/api/sso/providers', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { data: [], count: 0 }));
    await api.listSsoProviders();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/sso/providers?limit=100&offset=0',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('createSsoProvider POST 带 client_secret（write-only）', async () => {
    fetchMock.mockResolvedValue(mockJson(200, {}));
    await api.createSsoProvider({ name: 'okta', client_secret: 'shh' });
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/sso/providers',
      expect.objectContaining({ method: 'POST' }),
    );
  });

  it('deleteSsoProvider DELETE /admin/api/sso/providers/{id}', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { deleted: true }));
    await api.deleteSsoProvider('p1');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/sso/providers/p1',
      expect.objectContaining({ method: 'DELETE' }),
    );
  });

  // --- Predict ---
  it('predictUsage 拼接 history/forecast 到 /admin/api/predict/usage', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { daily_trend: 0, r_squared: 0, historical: [], predicted: [] }));
    await api.predictUsage('t1', 30, 14);
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/predict/usage?tenant_id=t1&history_days=30&forecast_days=14',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  it('predictBudget 拼接 budget 到 /admin/api/predict/budget', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { budget: 1000, budget_exhaustion_date: '', daily_trend: 0, r_squared: 0 }));
    await api.predictBudget('t1', 1000, 30);
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/predict/budget?tenant_id=t1&budget=1000&history_days=30',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  // --- MFA ---
  it('mfaSetup POST /admin/api/mfa/setup', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { secret: 's', qr_uri: 'otpauth://x', recovery_codes: ['a'] }));
    await api.mfaSetup();
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/mfa/setup',
      expect.objectContaining({ method: 'POST' }),
    );
  });

  it('mfaVerify POST /admin/api/mfa/verify 带 code', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { verified: true, mfa_enabled: true }));
    await api.mfaVerify('123456');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/mfa/verify',
      expect.objectContaining({ method: 'POST', body: JSON.stringify({ code: '123456' }) }),
    );
  });

  it('mfaDisable POST /admin/api/mfa/disable 带 code', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { disabled: true }));
    await api.mfaDisable('123456');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/mfa/disable',
      expect.objectContaining({ method: 'POST', body: JSON.stringify({ code: '123456' }) }),
    );
  });
});
