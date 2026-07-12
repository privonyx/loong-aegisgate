// Phase 11.5 (TASK-20260518-02 E5.2) — Autonomy approval workflow client.
//
// Mirrors the AdminController endpoints added in E5.1:
//   GET    /admin/api/autonomy/proposals                 -> listProposals
//   POST   /admin/api/autonomy/proposals/{id}/approve    -> approve
//   POST   /admin/api/autonomy/proposals/{id}/reject     -> reject (reason)
//   DELETE /admin/api/autonomy/proposals/{id}            -> rollback
//   GET    /admin/api/autonomy/report                    -> report
//
// Types are kept loose-but-meaningful: payload + decision_trace are nested
// JSON blobs whose schema is owned by the producer (CostOptimizer for now).
// Page code reads known fields with `.value(...)` style fallbacks.

import { adminFetch } from './request';

// TASK-20260602-01 Epic 4 — fetch wrapper 抽取到 ./request adminFetch，
// 与 client.ts 共用，消除 DRY 违反。
const BASE = '/admin/api/autonomy';

export type ApprovalState =
  | 'PROPOSED'
  | 'APPROVED'
  | 'APPLIED'
  | 'REJECTED'
  | 'ROLLED_BACK';

export type AutonomySource =
  | 'CostOptimizer'
  | 'AutoRecovery'
  | 'BanditRouter'
  | 'AdaptiveGuard'
  | 'Workflow';

export interface ApprovalProposal {
  id: string;
  source: AutonomySource;
  subject: string;
  state: ApprovalState;
  proposer_user_id: string;
  proposed_at_ms: number;
  reviewer_user_id: string;
  reviewed_at_ms: number;
  reject_reason: string;
  payload: Record<string, unknown>;
  decision_trace: Record<string, unknown>;
  payload_sha256: string;
}

export interface ProposalListResponse {
  data: ApprovalProposal[];
  limit: number;
  offset: number;
  total?: number;
}

export interface AutonomyReport {
  totals: Record<ApprovalState, number>;
  by_source: Partial<Record<AutonomySource, Partial<Record<ApprovalState, number>>>>;
  estimated_savings_24h_usd: number;
  sample_size: number;
}

export interface ApproveResponse {
  id: string;
  state: ApprovalState;
  reviewer_user_id: string;
}

export interface RejectResponse {
  id: string;
  state: ApprovalState;
  reviewer_user_id: string;
  reject_reason: string;
}

export interface RollbackResponse {
  id: string;
  state: ApprovalState;
}

function request<T>(path: string, options?: RequestInit): Promise<T> {
  return adminFetch<T>(BASE, path, options);
}

export interface ListFilters {
  state?: ApprovalState | '';
  source?: AutonomySource | '';
  limit?: number;
  offset?: number;
}

export const autonomyApi = {
  listProposals: (filters: ListFilters = {}) => {
    const params = new URLSearchParams();
    if (filters.state)  params.set('state', filters.state);
    if (filters.source) params.set('source', filters.source);
    params.set('limit',  String(filters.limit  ?? 100));
    params.set('offset', String(filters.offset ?? 0));
    return request<ProposalListResponse>(`/proposals?${params.toString()}`);
  },

  approve: (id: string) =>
    request<ApproveResponse>(`/proposals/${encodeURIComponent(id)}/approve`, {
      method: 'POST',
      body: JSON.stringify({}),
    }),

  reject: (id: string, reason: string) =>
    request<RejectResponse>(`/proposals/${encodeURIComponent(id)}/reject`, {
      method: 'POST',
      body: JSON.stringify({ reason }),
    }),

  rollback: (id: string) =>
    request<RollbackResponse>(`/proposals/${encodeURIComponent(id)}`, {
      method: 'DELETE',
    }),

  report: () => request<AutonomyReport>('/report'),
};
