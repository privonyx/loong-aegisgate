import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import type { AddressInfo } from 'node:net';
import type { Server } from 'node:http';
import { createApp } from '../src/app';
import { ObservabilityStore } from '../src/observability/store';
import { ScenarioRegistry } from '../src/runtime/registry';
import { MockAegisClient, meta } from './helpers/mockClient';
import { makeTestPlugin } from './helpers/fixtures';

let server: Server;
let base: string;
let store: ObservabilityStore;

beforeAll(async () => {
  const registry = new ScenarioRegistry();
  registry.register(makeTestPlugin());
  store = new ObservabilityStore();
  const client = new MockAegisClient({
    chatResponses: [{ content: '生成结果', meta: meta({ tokensSaved: 42, cacheHit: true, model: 'gpt-4o-mini' }), guardrail: { blocked: false } }],
  });
  const app = createApp({ registry, client, store, model: { primary: 'gpt-4o-mini', fallback: 'gpt-4o' } });
  await new Promise<void>((resolve) => {
    server = app.listen(0, () => resolve());
  });
  const addr = server.address() as AddressInfo;
  base = `http://127.0.0.1:${addr.port}`;
});

afterAll(() => {
  server?.close();
});

describe('GET /api/health', () => {
  it('返回服务状态与模型名（不含密钥）', async () => {
    const res = await fetch(`${base}/api/health`);
    const body = (await res.json()) as Record<string, any>;
    expect(res.status).toBe(200);
    expect(body.ok).toBe(true);
    expect(body.scenarios).toBe(1);
    expect(body.model.primary).toBe('gpt-4o-mini');
    expect(JSON.stringify(body)).not.toContain('apiKey');
  });
});

describe('GET /api/scenarios', () => {
  it('返回脱敏摘要列表（不含 handler/dataset）', async () => {
    const res = await fetch(`${base}/api/scenarios`);
    const body = (await res.json()) as Record<string, any>;
    expect(body.scenarios).toHaveLength(1);
    expect(body.scenarios[0].id).toBe('test');
    expect(body.scenarios[0].tools).toBeUndefined();
    expect(body.scenarios[0].dataset).toBeUndefined();
  });

  it('未知场景返回 404', async () => {
    const res = await fetch(`${base}/api/scenarios/nope`);
    expect(res.status).toBe(404);
  });
});

describe('POST /api/scenarios/:id/run', () => {
  it('运行 generate 步骤并向观测 store 记录事件', async () => {
    const res = await fetch(`${base}/api/scenarios/test/run`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ stepId: 'gen', inputs: { topic: '机甲' } }),
    });
    const body = (await res.json()) as Record<string, any>;
    expect(res.status).toBe(200);
    expect(body.output).toBe('生成结果');
    // 观测聚合应已记录该事件
    const summaryRes = await fetch(`${base}/api/observability/summary`);
    const summary = (await summaryRes.json()) as Record<string, any>;
    expect(summary.totalRequests).toBeGreaterThanOrEqual(1);
    expect(summary.totalTokensSaved).toBeGreaterThanOrEqual(42);
  });

  it('请求体缺 stepId → 400', async () => {
    const res = await fetch(`${base}/api/scenarios/test/run`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ inputs: {} }),
    });
    expect(res.status).toBe(400);
  });

  it('未知步骤 → 400', async () => {
    const res = await fetch(`${base}/api/scenarios/test/run`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ stepId: 'no-such-step', inputs: {} }),
    });
    expect(res.status).toBe(400);
  });

  it('未知场景 → 404', async () => {
    const res = await fetch(`${base}/api/scenarios/nope/run`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ stepId: 'gen', inputs: {} }),
    });
    expect(res.status).toBe(404);
  });
});

describe('GET /api/observability/log', () => {
  it('返回事件日志', async () => {
    const res = await fetch(`${base}/api/observability/log?limit=10`);
    const body = (await res.json()) as Record<string, any>;
    expect(Array.isArray(body.entries)).toBe(true);
  });
});
