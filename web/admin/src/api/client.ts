import type {
  UserInfo, Tenant, User, ApiKey,
  AuditRecord, CostRecord, DashboardSummary, ListResponse,
  SavingsSummary, SecurityEventSummary,
  CaseStudyHeadline,
  PromptTemplate, RuleSet, SsoProvider,
  UsagePrediction, BudgetPrediction,
  MfaSetupResult, MfaVerifyResult,
  ExportReport,
} from '../types';
import { adminFetch } from './request';

// Admin API 统一在 /admin/api/ namespace 下；SPA 客户端路由位于 /admin/<page>。
// 401 未认证时跳转到 SPA 登录页（保留 /admin/ 前缀，与 BrowserRouter basename 一致）。
// TASK-20260602-01 Epic 4 — fetch wrapper 抽取到 ./request adminFetch，
// 与 autonomy.ts 共用，消除 DRY 违反。
const BASE = '/admin/api';

function request<T>(path: string, options?: RequestInit): Promise<T> {
  return adminFetch<T>(BASE, path, options);
}

export const api = {
  login: (apiKey: string) =>
    request<UserInfo>('/auth/login', {
      method: 'POST',
      body: JSON.stringify({ api_key: apiKey }),
    }),

  logout: () => request<{ logged_out: boolean }>('/auth/logout', { method: 'POST' }),

  me: () => request<UserInfo>('/me'),

  // Tenants
  createTenant: (body: Partial<Tenant>) =>
    request<Tenant>('/tenants', { method: 'POST', body: JSON.stringify(body) }),
  listTenants: (limit = 100, offset = 0) =>
    request<ListResponse<Tenant>>(`/tenants?limit=${limit}&offset=${offset}`),
  getTenant: (id: string) => request<Tenant>(`/tenants/${id}`),
  updateTenant: (id: string, body: Partial<Tenant>) =>
    request<Tenant>(`/tenants/${id}`, { method: 'PUT', body: JSON.stringify(body) }),
  deleteTenant: (id: string) =>
    request<{ deleted: boolean }>(`/tenants/${id}`, { method: 'DELETE' }),

  // Users
  createUser: (body: Partial<User> & { tenant_id?: string }) =>
    request<User>('/users', { method: 'POST', body: JSON.stringify(body) }),
  listUsers: (tenantId = '', limit = 100, offset = 0) =>
    request<ListResponse<User>>(`/users?tenant_id=${tenantId}&limit=${limit}&offset=${offset}`),
  getUser: (id: string) => request<User>(`/users/${id}`),
  updateUser: (id: string, body: Partial<User>) =>
    request<User>(`/users/${id}`, { method: 'PUT', body: JSON.stringify(body) }),
  deleteUser: (id: string) =>
    request<{ deleted: boolean }>(`/users/${id}`, { method: 'DELETE' }),

  // API Keys
  createApiKey: (body: { user_id: string; name?: string; role?: string; expires_at?: string }) =>
    request<ApiKey>('/keys', { method: 'POST', body: JSON.stringify(body) }),
  listApiKeys: (tenantId = '', limit = 100, offset = 0) =>
    request<ListResponse<ApiKey>>(`/keys?tenant_id=${tenantId}&limit=${limit}&offset=${offset}`),
  revokeApiKey: (id: string) =>
    request<{ revoked: boolean }>(`/keys/${id}/revoke`, { method: 'POST' }),
  rotateApiKey: (id: string) =>
    request<ApiKey>(`/keys/${id}/rotate`, { method: 'POST' }),

  // Audit & Costs
  queryAudits: (tenantId = '', limit = 100, offset = 0) =>
    request<ListResponse<AuditRecord>>(`/audits?tenant_id=${tenantId}&limit=${limit}&offset=${offset}`),
  queryCosts: (tenantId = '', model = '', limit = 100, offset = 0) =>
    request<ListResponse<CostRecord>>(`/costs?tenant_id=${tenantId}&model=${model}&limit=${limit}&offset=${offset}`),

  // Compliance export (TASK-20260604-01 P0-C)。
  // 后端 RBAC：最低 TenantAdmin；非 SuperAdmin 跨租户 → 403（SR-4）。
  // 返回 { format, data }；csv → data 为字符串，json → data 为对象/数组。
  exportAuditReport: (from = '', to = '', tenantId = '', format: 'csv' | 'json' = 'csv') => {
    const params = new URLSearchParams({ format });
    if (from) params.set('from', from);
    if (to) params.set('to', to);
    if (tenantId) params.set('tenant_id', tenantId);
    return request<ExportReport>(`/export/audit?${params.toString()}`);
  },
  exportCostReport: (from = '', to = '', tenantId = '', format: 'csv' | 'json' = 'csv') => {
    const params = new URLSearchParams({ format });
    if (from) params.set('from', from);
    if (to) params.set('to', to);
    if (tenantId) params.set('tenant_id', tenantId);
    return request<ExportReport>(`/export/cost?${params.toString()}`);
  },

  // Dashboard
  dashboardSummary: () => request<DashboardSummary>('/dashboard/summary'),

  // Savings Dashboard (TASK-20260510-01)。
  // from/to 必须是 ISO 8601 (UTC)；为空时后端取近 7 天。
  // tenantId 仅 SuperAdmin 可跨租户；非 SuperAdmin 显式跨 tenant 会得到 403。
  savingsSummary: (from = '', to = '', tenantId = '') => {
    const params = new URLSearchParams();
    if (from) params.set('from', from);
    if (to) params.set('to', to);
    if (tenantId) params.set('tenant_id', tenantId);
    const qs = params.toString();
    return request<SavingsSummary>(`/savings/summary${qs ? '?' + qs : ''}`);
  },

  // Security events 计数（替代 Dashboard 原 mock 数据）
  securityEvents: () => request<SecurityEventSummary>('/security/events'),

  // Case Study Numbers (TASK-20260527-02 / MVP-5 prep).
  // 返回 3 头条数字（saved_vs_baseline / cache_hit_by_type / quality_reason）。
  // SR1 RBAC 由后端 effectiveTenantId 强制：SuperAdmin → 全局；其他 → 本租户。
  caseStudyHeadline: () =>
    request<CaseStudyHeadline>('/case-study/headline'),

  // ==========================================================================
  // TASK-20260603-01 — Feature Gap 补齐：5 孤立域。
  // SR-4：全部经 request()（adminFetch 封装）；契约见 spec §2。
  // ==========================================================================

  // Prompt Templates
  listPromptTemplates: (tenantId = '', limit = 100, offset = 0) =>
    request<ListResponse<PromptTemplate>>(`/templates?tenant_id=${tenantId}&limit=${limit}&offset=${offset}`),
  getPromptTemplate: (id: string) => request<PromptTemplate>(`/templates/${id}`),
  createPromptTemplate: (body: Partial<PromptTemplate>) =>
    request<PromptTemplate>('/templates', { method: 'POST', body: JSON.stringify(body) }),
  updatePromptTemplate: (id: string, body: Partial<PromptTemplate>) =>
    request<PromptTemplate>(`/templates/${id}`, { method: 'PUT', body: JSON.stringify(body) }),
  deletePromptTemplate: (id: string) =>
    request<{ deleted: boolean; id: string }>(`/templates/${id}`, { method: 'DELETE' }),

  // Rule Sets（无 update/delete；create=新版本 / activate=切换激活版本）
  listRuleSets: (tenantId = '', limit = 100, offset = 0) =>
    request<ListResponse<RuleSet>>(`/rules?tenant_id=${tenantId}&limit=${limit}&offset=${offset}`),
  getActiveRuleSet: (tenantId = '') =>
    request<RuleSet>(`/rules/active${tenantId ? `?tenant_id=${tenantId}` : ''}`),
  createRuleSet: (body: { rules?: unknown[]; rules_json?: string; tenant_id?: string }) =>
    request<RuleSet>('/rules', { method: 'POST', body: JSON.stringify(body) }),
  activateRuleSet: (version: number, tenantId = '') =>
    request<{ activated: boolean; version: number }>('/rules/activate', {
      method: 'POST',
      body: JSON.stringify({ version, ...(tenantId ? { tenant_id: tenantId } : {}) }),
    }),

  // SSO Providers（SR-1：client_secret 仅出现在写入 body，响应仅 has_client_secret）
  listSsoProviders: (limit = 100, offset = 0) =>
    request<ListResponse<SsoProvider>>(`/sso/providers?limit=${limit}&offset=${offset}`),
  getSsoProvider: (id: string) => request<SsoProvider>(`/sso/providers/${id}`),
  createSsoProvider: (body: Partial<SsoProvider> & { client_secret?: string }) =>
    request<SsoProvider>('/sso/providers', { method: 'POST', body: JSON.stringify(body) }),
  updateSsoProvider: (id: string, body: Partial<SsoProvider> & { client_secret?: string }) =>
    request<SsoProvider>(`/sso/providers/${id}`, { method: 'PUT', body: JSON.stringify(body) }),
  deleteSsoProvider: (id: string) =>
    request<{ deleted: boolean }>(`/sso/providers/${id}`, { method: 'DELETE' }),

  // Usage Predict（只读 / 仅 SuperAdmin / TenantAdmin）
  predictUsage: (tenantId = '', historyDays = 30, forecastDays = 14) =>
    request<UsagePrediction>(`/predict/usage?tenant_id=${tenantId}&history_days=${historyDays}&forecast_days=${forecastDays}`),
  predictBudget: (tenantId = '', budget = 1000, historyDays = 30) =>
    request<BudgetPrediction>(`/predict/budget?tenant_id=${tenantId}&budget=${budget}&history_days=${historyDays}`),

  // MFA（SR-2：setup 一次性返回 secret/recovery_codes，前端不持久化）
  mfaSetup: () => request<MfaSetupResult>('/mfa/setup', { method: 'POST' }),
  mfaVerify: (code: string) =>
    request<MfaVerifyResult>('/mfa/verify', { method: 'POST', body: JSON.stringify({ code }) }),
  mfaDisable: (code: string) =>
    request<{ disabled: boolean }>('/mfa/disable', { method: 'POST', body: JSON.stringify({ code }) }),
  mfaRecovery: (code: string) =>
    request<{ verified: boolean; remaining_codes: number }>('/mfa/recovery', {
      method: 'POST',
      body: JSON.stringify({ code }),
    }),
};
