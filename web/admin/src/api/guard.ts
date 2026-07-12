// TASK-20260703-02 — Adaptive Guard admin client。
// 端点前缀 /admin/api/guard，走 admin session 鉴权（adminFetch same-origin cookie）。
// TASK-20260706-01：原前缀 /v1/guard 不在登录 cookie 的 Path=/admin 作用域内，
// 浏览器不会随请求发送 aegis_session → 恒 401。迁入 /admin/api/guard 使 cookie
// 作用域天然对齐（cookie 保持 Path=/admin 不放宽）。
import { adminFetch } from './request';
import type {
  GuardExplanation, GuardFeedbackLabel, GuardFeedbackResult, GuardPromoteResult,
} from '../types';

const BASE = '/admin/api/guard';
function request<T>(path: string, options?: RequestInit): Promise<T> {
  return adminFetch<T>(BASE, path, options);
}

// 与后端 mapToReviewerRole 对齐（admin_http_controller.cpp:30-36）；
// 前端派生仅为构造合法 body，真校验在后端（ctx.role + body role 双重白名单）。
export function mapReviewerRole(role: string | undefined | null): string {
  if (role === 'super_admin') return 'security_admin';
  if (role === 'tenant_admin') return 'platform_admin';
  return role ?? '';
}

export interface FeedbackInput {
  request_id: string;
  label: GuardFeedbackLabel;
  reviewer_user_id: string;   // 调用方传 useAuth().user.user_id
  reviewer_role: string;      // 调用方传 mapReviewerRole(user.role)
  trace_id?: string;
  comment?: string;
  original_text_redacted?: string;
}

export interface PromoteInput {
  action: string;
  model_id: string;
  version: string;
  [k: string]: unknown;
}

export const guardApi = {
  getExplanation: (requestId: string) =>
    request<GuardExplanation>(`/explanation/${encodeURIComponent(requestId)}`),
  submitFeedback: (body: FeedbackInput) =>
    request<GuardFeedbackResult>('/feedback', {
      method: 'POST', body: JSON.stringify(body),
    }),
  promoteModel: (body: PromoteInput) =>
    request<GuardPromoteResult>('/model/promote', {
      method: 'POST', body: JSON.stringify(body),
    }),
};
