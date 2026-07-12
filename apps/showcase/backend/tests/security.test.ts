/**
 * 安全需求守门测试（SR-1/2/3/4），含静态扫描式 mutation 防御。
 * 对应 plan §2 SR 4 元组表。
 */

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { createAegisClient } from '../src/aegis/client';
import { runStep } from '../src/runtime/chatLoop';
import { comicPlugin } from '../src/scenarios/comic/plugin';
import { MockAegisClient, meta } from './helpers/mockClient';

function walk(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) out.push(...walk(full));
    else if (full.endsWith('.ts')) out.push(full);
  }
  return out;
}

const SRC_FILES = walk('src');

describe('SR-1：密钥不入源码 / 产物 / 版本控制', () => {
  it('全部源码无硬编码的真实密钥形态（sk-... / 长 token）', () => {
    const keyLike = /\b(sk-[a-zA-Z0-9]{20,}|AKIA[0-9A-Z]{16})\b/;
    const offenders = SRC_FILES.filter((f) => keyLike.test(readFileSync(f, 'utf8')));
    expect(offenders).toEqual([]);
  });

  it('.env.example 中 API Key 仅为占位符', () => {
    const env = readFileSync('../.env.example', 'utf8');
    const line = env.split('\n').find((l) => l.startsWith('AEGISGATE_API_KEY=')) ?? '';
    expect(line.toLowerCase()).toMatch(/replace|your|placeholder/);
  });

  it('客户端把 apiKey 仅放 Authorization 头，结果不含 key（mutation：回传 key 应 FAIL）', async () => {
    let captured: Record<string, string> = {};
    const fetchImpl = (async (_url: string | URL, init?: RequestInit) => {
      captured = (init?.headers as Record<string, string>) ?? {};
      return new Response(JSON.stringify({ model: 'm', choices: [{ message: { content: 'ok' } }] }), { status: 200 });
    }) as unknown as typeof fetch;
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k-super-secret', fetchImpl });
    const res = await client.chat({ model: 'm', messages: [] });
    expect(captured.authorization).toBe('Bearer k-super-secret');
    expect(JSON.stringify(res)).not.toContain('k-super-secret');
  });
});

describe('SR-3：工具 handler 无 fs 写 / 命令执行 / 任意网络副作用（静态扫描）', () => {
  const toolFiles = ['src/scenarios/comic/tools.ts', 'src/scenarios/ecommerce/tools.ts'];
  it('工具源码不引入 child_process / fs 写 / 任意 fetch', () => {
    for (const file of toolFiles) {
      const src = readFileSync(file, 'utf8');
      expect(src).not.toMatch(/child_process/);
      expect(src).not.toMatch(/from\s+['"]node:fs['"]/);
      expect(src).not.toMatch(/fs\.(write|append|unlink|rm)/);
      expect(src).not.toMatch(/\bfetch\s*\(/);
      expect(src).not.toMatch(/exec(Sync)?\s*\(/);
    }
  });
});

describe('SR-4：护栏拦截不回显被拦截原文', () => {
  it('guard 步骤输出含拦截原因但不含原始敏感文本', async () => {
    const plugin = comicPlugin({ primary: 'gpt-4o-mini' });
    const step = plugin.uiSteps.find((s) => s.id === 'compliance')!;
    const secret = '机密原文XYZ-血肉模糊';
    const client = new MockAegisClient({
      guardResult: { guardrail: { blocked: true, reason: '露骨暴力越界' }, meta: meta() },
    });
    const result = await runStep({ plugin, step, inputs: { text: secret }, client });
    expect(result.output).toContain('露骨暴力越界');
    expect(result.output).not.toContain(secret);
    expect(JSON.stringify(result.events)).not.toContain(secret);
  });
});
