/**
 * Express 应用装配（依赖注入，便于单测）。
 * 路由层薄：参数校验（SR-2）+ 委托运行时 + 聚合观测；不持有任何密钥。
 */

import cors from 'cors';
import express, { type Express, type NextFunction, type Request, type Response } from 'express';
import { z } from 'zod';
import type { AegisChatClient } from './aegis/types';
import { runStep } from './runtime/chatLoop';
import type { ScenarioRegistry } from './runtime/registry';
import { ObservabilityStore } from './observability/store';
import type { UiStep } from './scenarios/types';

export interface AppDeps {
  registry: ScenarioRegistry;
  client: AegisChatClient;
  store: ObservabilityStore;
  corsOrigin?: string;
  model: { primary: string; fallback?: string };
}

const runBodySchema = z.object({
  stepId: z.string().min(1),
  inputs: z.record(z.string(), z.unknown()).default({}),
});

export function createApp(deps: AppDeps): Express {
  const app = express();
  app.use(express.json({ limit: '1mb' }));
  app.use(cors({ origin: deps.corsOrigin ?? true }));

  app.get('/api/health', (_req, res) => {
    res.json({
      ok: true,
      service: 'aegisgate-showcase-backend',
      scenarios: deps.registry.list().length,
      model: deps.model, // 仅模型名，不含任何密钥（SR-1）
    });
  });

  app.get('/api/scenarios', (_req, res) => {
    res.json({ scenarios: deps.registry.summaries() });
  });

  app.get('/api/scenarios/:id', (req, res) => {
    const plugin = deps.registry.get(req.params.id);
    if (!plugin) {
      res.status(404).json({ error: `未知场景: ${req.params.id}` });
      return;
    }
    res.json(deps.registry.summaries().find((s) => s.id === plugin.id));
  });

  app.post('/api/scenarios/:id/run', async (req, res, next) => {
    try {
      const plugin = deps.registry.get(req.params.id);
      if (!plugin) {
        res.status(404).json({ error: `未知场景: ${req.params.id}` });
        return;
      }
      const parsed = runBodySchema.safeParse(req.body);
      if (!parsed.success) {
        res.status(400).json({ error: '请求参数无效', details: parsed.error.issues });
        return;
      }
      const step: UiStep | undefined = plugin.uiSteps.find((s) => s.id === parsed.data.stepId);
      if (!step) {
        res.status(400).json({ error: `场景 ${plugin.id} 无此步骤: ${parsed.data.stepId}` });
        return;
      }

      const result = await runStep({
        plugin,
        step,
        inputs: parsed.data.inputs,
        client: deps.client,
        onEvent: (event) => deps.store.record(event),
      });
      res.json(result);
    } catch (err) {
      next(err);
    }
  });

  app.post('/api/scenarios/:id/run/stream', async (req, res) => {
    const plugin = deps.registry.get(req.params.id);
    if (!plugin) {
      res.status(404).json({ error: `未知场景: ${req.params.id}` });
      return;
    }
    const parsed = runBodySchema.safeParse(req.body);
    if (!parsed.success) {
      res.status(400).json({ error: '请求参数无效', details: parsed.error.issues });
      return;
    }
    const step: UiStep | undefined = plugin.uiSteps.find((s) => s.id === parsed.data.stepId);
    if (!step) {
      res.status(400).json({ error: `场景 ${plugin.id} 无此步骤: ${parsed.data.stepId}` });
      return;
    }

    res.status(200);
    res.setHeader('Content-Type', 'text/event-stream; charset=utf-8');
    res.setHeader('Cache-Control', 'no-cache, no-transform');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('X-Accel-Buffering', 'no');
    res.flushHeaders?.();

    const send = (event: string, data: unknown): void => {
      res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
    };

    try {
      const result = await runStep({
        plugin,
        step,
        inputs: parsed.data.inputs,
        client: deps.client,
        onEvent: (event) => {
          deps.store.record(event);
          send('value', event);
        },
        onToken: (text) => send('token', { text }),
      });
      send('done', result);
    } catch (err) {
      // SR-1/SR-4：仅回传状态与消息，绝不含密钥 / 原始请求。
      const message = err instanceof Error ? err.message : '内部错误';
      send('error', { error: message });
    } finally {
      res.end();
    }
  });

  app.get('/api/observability/summary', (_req, res) => {
    res.json(deps.store.summary());
  });

  app.get('/api/observability/log', (req, res) => {
    const limit = Number.parseInt(String(req.query.limit ?? '50'), 10);
    res.json({ entries: deps.store.log(Number.isFinite(limit) ? limit : 50) });
  });

  // 兜底错误处理：SR-1/SR-4 — 绝不回显密钥 / 被拦截原文 / 内部栈细节。
  app.use((err: unknown, _req: Request, res: Response, _next: NextFunction) => {
    const message = err instanceof Error ? err.message : '内部错误';
    res.status(500).json({ error: message });
  });

  return app;
}
