// @vitest-environment node
//
// TASK-20260703-02 — Adaptive Guard api 客户端契约测试。
// SR-1：submitFeedback 的 reviewer 身份字段由调用方派生（mapReviewerRole），
// 契约测试锁定 URL/method/body + adminFetch（credentials:same-origin）透传。
// 端点前缀 /admin/api/guard（TASK-20260706-01：迁入 admin session cookie
// 的 Path=/admin 作用域，修复 /v1/guard 因 cookie path 不匹配导致的 401）。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { guardApi, mapReviewerRole } from '../api/guard';

describe('guard api client', () => {
  let fetchMock: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    vi.stubGlobal('window', { location: { href: '', pathname: '/admin/guard' } });
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

  // --- mapReviewerRole（SR-1 派生正确性，与后端 mapToReviewerRole 对齐）---
  it('mapReviewerRole 映射 super_admin → security_admin', () => {
    expect(mapReviewerRole('super_admin')).toBe('security_admin');
  });

  it('mapReviewerRole 映射 tenant_admin → platform_admin', () => {
    expect(mapReviewerRole('tenant_admin')).toBe('platform_admin');
  });

  it('mapReviewerRole 其余角色原样返回（后端将 403）', () => {
    expect(mapReviewerRole('viewer')).toBe('viewer');
    expect(mapReviewerRole('developer')).toBe('developer');
    expect(mapReviewerRole(undefined)).toBe('');
  });

  // --- getExplanation ---
  it('getExplanation GET /admin/api/guard/explanation/{id}（编码 id）', async () => {
    fetchMock.mockResolvedValue(mockJson(200, {
      trigger_layer: 'L3', trigger_rule_id: 'r1', model_version: 'v1',
      threshold: 0.5, matched_pattern: '***', confidence: 0.9, explanation_text: 'blocked',
    }));
    await guardApi.getExplanation('req 1/x');
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/guard/explanation/req%201%2Fx',
      expect.objectContaining({ credentials: 'same-origin' }),
    );
  });

  // --- submitFeedback ---
  it('submitFeedback POST /admin/api/guard/feedback 带派生 reviewer 字段', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { accepted: true, request_id: 'req-1' }));
    await guardApi.submitFeedback({
      request_id: 'req-1',
      label: 'false_positive',
      reviewer_user_id: 'u-1',
      reviewer_role: 'security_admin',
      comment: 'over-block',
    });
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/guard/feedback',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({
          request_id: 'req-1',
          label: 'false_positive',
          reviewer_user_id: 'u-1',
          reviewer_role: 'security_admin',
          comment: 'over-block',
        }),
      }),
    );
  });

  // --- promoteModel ---
  it('promoteModel POST /admin/api/guard/model/promote 带 action/model_id/version', async () => {
    fetchMock.mockResolvedValue(mockJson(200, { proposal_id: 'p-1' }));
    await guardApi.promoteModel({ action: 'promote', model_id: 'm-1', version: '3' });
    expect(fetchMock).toHaveBeenCalledWith(
      '/admin/api/guard/model/promote',
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ action: 'promote', model_id: 'm-1', version: '3' }),
      }),
    );
  });
});
