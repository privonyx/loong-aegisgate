import { describe, expect, it } from 'vitest';
import { interpolate, runStep } from '../src/runtime/chatLoop';
import type { ChatResponse } from '../src/aegis/types';
import { MockAegisClient, meta } from './helpers/mockClient';
import { makeTestPlugin } from './helpers/fixtures';

const plugin = makeTestPlugin();
const genStep = plugin.uiSteps[0];
const toolStep = plugin.uiSteps[1];
const guardStep = plugin.uiSteps[2];

describe('interpolate', () => {
  it('插值 {{var}} 模板', () => {
    expect(interpolate('主题：{{topic}}', { topic: '机甲' })).toBe('主题：机甲');
  });
  it('缺失变量替换为空串', () => {
    expect(interpolate('{{a}}-{{b}}', { a: 'x' })).toBe('x-');
  });
});

describe('runStep — generate（创作引擎）', () => {
  it('无工具调用时直接返回模型内容', async () => {
    const client = new MockAegisClient({
      chatResponses: [{ content: '生成的剧本', meta: meta({ model: 'gpt-4o-mini', tokensSaved: 12, cacheHit: true }), guardrail: { blocked: false } }],
    });
    const result = await runStep({ plugin, step: genStep, inputs: { topic: '机甲' }, client });
    expect(result.output).toBe('生成的剧本');
    expect(result.rounds).toBe(1);
    expect(result.events).toHaveLength(1);
    expect(result.events[0].tokensSaved).toBe(12);
    expect(result.events[0].cacheHit).toBe(true);
    expect(result.events[0].model).toBe('gpt-4o-mini');
    // 用户提示由模板插值而来
    expect(client.chatCalls[0].messages[1].content).toBe('主题：机甲');
  });

  it('多轮 Function-Calling：执行工具并回灌后再得最终结果', async () => {
    const withToolCall: ChatResponse = {
      content: null,
      toolCalls: [{ id: 'c1', type: 'function', function: { name: 'getFacts', arguments: '{"topic":"x"}' } }],
      meta: meta(),
      guardrail: { blocked: false },
    };
    const final: ChatResponse = { content: '基于事实的剧情', meta: meta({ tokensSaved: 3 }), guardrail: { blocked: false } };
    const client = new MockAegisClient({ chatResponses: [withToolCall, final] });
    const result = await runStep({ plugin, step: genStep, inputs: { topic: 'x' }, client });
    expect(result.rounds).toBe(2);
    expect(result.output).toBe('基于事实的剧情');
    expect(result.toolInvocations).toHaveLength(1);
    expect(result.toolInvocations[0]).toMatchObject({ name: 'getFacts', ok: true });
    expect(result.events).toHaveLength(2);
    // 第二轮请求应包含 tool 回灌消息
    const secondReqMessages = client.chatCalls[1].messages;
    expect(secondReqMessages.some((m) => m.role === 'tool' && m.name === 'getFacts')).toBe(true);
  });

  it('护栏拦截 → 终止并返回拦截输出', async () => {
    const blocked: ChatResponse = {
      content: null,
      meta: meta(),
      guardrail: { blocked: true, reason: '检测到注入', code: 'guardrail_blocked' },
    };
    const client = new MockAegisClient({ chatResponses: [blocked] });
    const result = await runStep({ plugin, step: genStep, inputs: { topic: 'x' }, client });
    expect(result.guardrail?.blocked).toBe(true);
    expect(result.output).toContain('已被护栏拦截');
    expect(result.events[0].guardrailBlocked).toBe(true);
  });

  it('达到最大轮次上限优雅终止', async () => {
    const loopCall: ChatResponse = {
      content: null,
      toolCalls: [{ id: 'c', type: 'function', function: { name: 'getFacts', arguments: '{}' } }],
      meta: meta(),
      guardrail: { blocked: false },
    };
    const client = new MockAegisClient({ chatResponses: [loopCall, loopCall, loopCall] });
    const result = await runStep({ plugin, step: genStep, inputs: { topic: 'x' }, client, maxRounds: 2 });
    expect(result.rounds).toBe(2);
    expect(result.output).toContain('最大工具调用轮次');
  });
});

describe('runStep — tool（直接工具调用）', () => {
  it('直接调用绑定工具 handler，不经模型', async () => {
    const client = new MockAegisClient();
    const result = await runStep({ plugin, step: toolStep, inputs: { topic: '能源' }, client });
    expect(client.chatCalls).toHaveLength(0);
    expect(result.toolInvocations[0]).toMatchObject({ name: 'getFacts', ok: true });
    expect(result.output).toContain('能源');
  });
});

describe('runStep — guard（合规引擎）', () => {
  it('通过护栏 → 返回通过提示', async () => {
    const client = new MockAegisClient({ guardResult: { guardrail: { blocked: false }, meta: meta() } });
    const result = await runStep({ plugin, step: guardStep, inputs: { text: '正常剧情' }, client });
    expect(client.guardCalls[0].text).toBe('正常剧情');
    expect(result.guardrail?.blocked).toBe(false);
    expect(result.output).toContain('合规预审通过');
  });

  it('护栏拦截 → 返回拦截原因 + 价值事件标记', async () => {
    const client = new MockAegisClient({
      guardResult: { guardrail: { blocked: true, reason: '暴力内容越界' }, meta: meta({ latencyMs: 8 }) },
    });
    const result = await runStep({ plugin, step: guardStep, inputs: { text: '血腥场面' }, client });
    expect(result.guardrail?.blocked).toBe(true);
    expect(result.output).toContain('暴力内容越界');
    expect(result.events[0].guardrailBlocked).toBe(true);
    expect(result.events[0].engine).toBe('compliance');
  });
});
