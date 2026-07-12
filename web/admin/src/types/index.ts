export interface UserInfo {
  user_id: string;
  tenant_id: string;
  role: string;
  // TASK-20260604-01 P0-F：SSO 登录后 MFA 待验证态，驱动 SPA 跳 /mfa-challenge。
  mfa_pending?: boolean;
}

export interface Tenant {
  id: string;
  name: string;
  status: string;
  model_whitelist: string[];
  daily_cost_limit: number;
  monthly_cost_limit: number;
  rate_limit_tokens: number;
  rate_limit_refill: number;
  created_at: string;
  updated_at: string;
}

export interface User {
  id: string;
  tenant_id: string;
  username: string;
  display_name: string;
  role: string;
  status: string;
  created_at: string;
  updated_at: string;
}

export interface ApiKey {
  id: string;
  user_id: string;
  tenant_id: string;
  name: string;
  key_prefix: string;
  role: string;
  status: string;
  expires_at: string;
  last_used_at: string;
  created_at: string;
  updated_at: string;
  key?: string;
  rotated_from?: string;
}

export interface AuditRecord {
  request_id: string;
  timestamp: string;
  tenant_id: string;
  action: string;
  stage_name: string;
  detail: string;
}

export interface CostRecord {
  request_id: string;
  tenant_id: string;
  model: string;
  total_cost: number;
  input_tokens: number;
  output_tokens: number;
  timestamp: string;
}

export interface DashboardSummary {
  total_requests: number;
  active_tenants: number;
  total_cost: number;
  total_cost_records: number;
  cache_hit_rate: number | null;
  cache_hit_count?: number;
  cache_miss_count?: number;
  cost_saved_30d: number;
  aggregator_since: string | null;
}

// Savings Dashboard 类型（TASK-20260510-01）。
// 与 backend AdminController.getSavingsSummary 输出字段对齐；后端缺失任何字段时
// 视为零值（前端用 null-safe 取值）。

export interface SavingsByType {
  type: 'cache_hit' | 'compression' | 'routing_potential';
  cost_saved: number;
  tokens_saved: number;
  event_count: number;
}

export interface SavingsByModel {
  model: string;
  cost_saved: number;
  tokens_saved: number;
  request_count: number;
}

export interface SavingsTimePoint {
  date: string;        // YYYY-MM-DD（UTC）
  cost_saved: number;
  tokens_saved: number;
  requests: number;
}

export interface SavingsTopTenant {
  tenant_id: string;
  cost_saved: number;
  tokens_saved: number;
  event_count: number;
}

export interface RoutingRecommendation {
  route: string;             // "current_model->recommended_model"
  potential_savings: number;
  event_count: number;
}

export interface SavingsSummary {
  from: string;              // ISO 8601 (UTC)
  to: string;
  aggregator_since: string | null;
  total_cost_saved: number;
  total_cost_actual: number;
  roi_percent: number;
  total_tokens_saved: number;
  total_cache_hits: number;
  fallback_pricing_count: number;
  by_type: SavingsByType[];
  by_model: SavingsByModel[];
  time_series: SavingsTimePoint[];
  top_tenants: SavingsTopTenant[];   // 仅 SuperAdmin 非空
  routing_recommendations: RoutingRecommendation[];
}

export interface SecurityEventSummary {
  timestamp: string;
  scope: 'global' | 'tenant';
  // SuperAdmin 视角（global）
  guardrail_blocks_total?: number;
  preprocessor_normalized_total?: number;
  rate_limited_total?: number;
  cache_hits_total?: number;
  cache_queries_total?: number;
  // 非 SuperAdmin 视角（tenant，severity 分级）
  guardrail_blocks_severity?: 'none' | 'low' | 'medium' | 'high';
  rate_limited_severity?: 'none' | 'low' | 'medium' | 'high';
}

// TASK-20260527-02 — Case Study Numbers (MVP-5 prep) types.
// 与 backend AdminController::caseStudyHeadline JSON 输出对齐（spec §3.3）。
// 后端缺失任何字段时视为 0（Dashboard 用 null-safe 取值）。

export interface CaseStudySavedVsBaseline {
  actual_cost: number;
  baseline_cost: number;
  cost_saved: number;
  savings_percent: number;
}

export interface CaseStudyCacheHitByType {
  total_hit_rate: number;
  hit_exact: number;
  hit_semantic: number;
  hit_conversation: number;
  miss: number;
}

export interface CaseStudyQualityReason {
  current_ema: number;
  slope: number;
  reason_factuality: number;
  reason_refusal: number;
  reason_latency_degraded: number;
}

export interface CaseStudyHeadline {
  scope: 'global' | 'tenant';
  tenant_id?: string;
  timestamp: string;
  // TASK-20260528-01 D8=B: aggregator since-uptime ISO 8601 timestamp;
  // null when backend has no aggregator yet ("since startup" display fallback).
  aggregator_since?: string | null;
  saved_vs_baseline: CaseStudySavedVsBaseline;
  cache_hit_by_type: CaseStudyCacheHitByType;
  quality_reason: CaseStudyQualityReason;
}

// ============================================================================
// TASK-20260603-01 — Feature Gap 补齐：5 孤立域类型
// 与 backend admin_controller.cpp JSON 输出 1:1 对齐（spec §2 grep 实测锁定）。
// ============================================================================

// Prompt Template（admin_controller.cpp:403-512）
export interface PromptTemplate {
  id: string;
  tenant_id: string;
  name: string;
  content: string;
  version: number;
  weight: number;
  is_active: boolean;
  created_at: string;
  updated_at?: string;
}

// Rule Set（admin_controller.cpp:516-608 / 仅 list/active/create/activate，无 U/D）
export interface RuleSet {
  version: number;
  tenant_id: string;
  created_at: string;
  is_active: boolean;
  rules_json: string;   // 字符串化 JSON
}

// SSO Provider（admin_controller.cpp:1034-1306）
// ⚠️ SR-1：后端只回 has_client_secret 布尔，绝不回显 client_secret 明文；
// 本类型刻意不含 client_secret 读字段（write-only 仅出现在表单提交体）。
export interface SsoProvider {
  id: string;
  tenant_id: string;
  name: string;
  issuer_url: string;
  client_id: string;
  has_client_secret: boolean;
  redirect_uri: string;
  scopes: string[];
  jit_provisioning: boolean;
  default_role: string;
  enabled: boolean;
  created_at: string;
  updated_at: string;
  claim_mapping?: Record<string, unknown>;
  group_role_mapping?: Record<string, unknown>;
}

// Usage Predict（admin_controller.cpp:1323-1376）
export interface UsageHistoryPoint {
  date: string;
  total_cost: number;
  request_count: number;
}

export interface UsagePredictedPoint {
  date: string;
  total_cost: number;
}

export interface UsagePrediction {
  daily_trend: number;
  r_squared: number;
  historical: UsageHistoryPoint[];
  predicted: UsagePredictedPoint[];
}

export interface BudgetPrediction {
  budget: number;
  budget_exhaustion_date: string;   // ISO 8601 或空串（无法预测时）
  daily_trend: number;
  r_squared: number;
}

// MFA（admin_controller.cpp:1380-1515）
// ⚠️ SR-2：setup 一次性返回明文 secret + recovery_codes，前端仅在 setup 阶段
// 展示并提示立即保存，不得持久化（localStorage/sessionStorage 禁写）。
export interface MfaSetupResult {
  secret: string;
  qr_uri: string;           // otpauth://...
  recovery_codes: string[];
}

export interface MfaVerifyResult {
  verified: boolean;
  mfa_enabled: boolean;
}

export interface ListResponse<T> {
  data: T[];
  count: number;        // 当前页条数
  total?: number;       // TASK-20260604-01 P0-E：全量计数，供翻页（旧后端可能缺省）
}

// TASK-20260604-01 P0-C — 合规导出响应。csv → data 为字符串；json → 对象/数组。
export interface ExportReport {
  format: 'csv' | 'json';
  data: string | unknown;
}

export interface ApiError {
  error: {
    code: string;
    type: string;
    message: string;
  };
}

export type Theme = 'dark' | 'light';

// TASK-20260602-01 Epic 1 — WS envelope schema 统一为 nested `data` 格式（D1=B）。
// 后端 admin_ws_controller.cpp 自始就用 `{ type, data: { ... } }` 嵌套结构推送
// metrics + audit + case_study；前端原 flat 定义（无 `data`）导致 cast 错位 →
// Dashboard KPI 实时更新失效 + Audits liveMode 失效（P0 真 bug）。
// 修复策略：前端类型与 handler 全面适配 nested envelope，与 WsCaseStudy 一致。
export interface WsMetrics {
  type: 'metrics';
  data: {
    total_requests: number;
    active_tenants: number;
    total_cost_records: number;        // 后端实际字段名（非 total_cost）
    cache_hit_rate: number | null;
  };
}

export interface WsAuditEvent {
  type: 'audit';
  data: {
    request_id: string;
    timestamp: string;
    tenant_id: string;
    action: string;
    stage: string;                     // 后端实际字段名（前端 AuditRecord 字段为 stage_name，handler 内映射）
    detail: string;
  };
}

// TASK-20260527-02 — case_study WS payload (30s throttle, see admin_ws_controller).
// data 字段 mirrors /admin/api/case-study/headline schema 的 data 部分。
export interface WsCaseStudy {
  type: 'case_study';
  data: {
    scope: 'global' | 'tenant';
    tenant_id?: string;
    saved_vs_baseline: CaseStudySavedVsBaseline;
    cache_hit_by_type: CaseStudyCacheHitByType;
    quality_reason: CaseStudyQualityReason;
  };
}

export type WsMessage = WsMetrics | WsAuditEvent | WsCaseStudy;

// TASK-20260703-02 — Adaptive Guard 管理面类型（对齐 /admin/api/guard/* 契约）。
export type GuardFeedbackLabel =
  | 'false_positive'
  | 'false_negative'
  | 'confirmed_block'
  | 'confirmed_allow';

// GET /admin/api/guard/explanation/{id} → GuardExplanation.toJson()（guard_explanation.h）
export interface GuardExplanation {
  trigger_layer: string;
  trigger_rule_id: string;
  model_version: string;
  threshold: number;
  matched_pattern: string;   // 已 PII-mask
  confidence: number;
  explanation_text: string;  // 已 PII-mask
}

export interface GuardFeedbackResult {
  accepted: boolean;
  request_id: string;
}

export interface GuardPromoteResult {
  proposal_id: string;
}
