import { describe, expect, it } from 'vitest';
import { comicPlugin } from '../src/scenarios/comic/plugin';
import { comicWorldBible, type WorldBible } from '../src/scenarios/comic/data';
import type { ScenarioContext } from '../src/scenarios/types';
import { runStep } from '../src/runtime/chatLoop';
import type { ChatResponse } from '../src/aegis/types';
import { MockAegisClient, meta } from './helpers/mockClient';

const model = { primary: 'gpt-4o-mini', fallback: 'gpt-4o' };
const plugin = comicPlugin(model);

function ctx(): ScenarioContext {
  return {
    scenarioId: 'comic',
    dataset: comicWorldBible,
    checkGuardrail: async () => ({ blocked: false }),
  };
}

function tool(name: string) {
  const t = plugin.tools.find((x) => x.spec.function.name === name);
  if (!t) throw new Error(`missing tool ${name}`);
  return t;
}

describe('comic 插件结构', () => {
  it('暴露双引擎步骤（创作 generate + 合规 guard）', () => {
    expect(plugin.id).toBe('comic');
    const engines = new Set(plugin.uiSteps.map((s) => s.engine));
    expect(engines.has('creation')).toBe(true);
    expect(engines.has('compliance')).toBe(true);
    const compliance = plugin.uiSteps.find((s) => s.engine === 'compliance');
    expect(compliance?.kind).toBe('guard');
    expect(plugin.guardrail.ruleFile).toBe('comic.yaml');
    expect(plugin.model).toEqual(model);
  });
});

describe('SR-3：comic 工具 handler 纯函数（只读 dataset，无副作用）', () => {
  it('getWorldBible 返回世界观且不改 dataset', async () => {
    const before = JSON.stringify(comicWorldBible);
    const res = await tool('getWorldBible').handler({}, ctx());
    expect(res.ok).toBe(true);
    expect((res.data as WorldBible).ipName).toBe('星轨彼端');
    expect(JSON.stringify(comicWorldBible)).toBe(before); // dataset 未被篡改
  });

  it('listCharacters 支持按名筛选', async () => {
    const all = await tool('listCharacters').handler({}, ctx());
    expect((all.data as { characters: unknown[] }).characters.length).toBeGreaterThanOrEqual(4);
    const filtered = await tool('listCharacters').handler({ name: '凌墟' }, ctx());
    expect((filtered.data as { characters: { name: string }[] }).characters).toHaveLength(1);
    expect((filtered.data as { characters: { name: string }[] }).characters[0].name).toBe('凌墟');
  });

  it('getEpisode 命中与未命中', async () => {
    const hit = await tool('getEpisode').handler({ episodeId: 'ep1' }, ctx());
    expect(hit.ok).toBe(true);
    const miss = await tool('getEpisode').handler({ episodeId: 'ep999' }, ctx());
    expect(miss.ok).toBe(false);
  });
});

describe('创作引擎：分集大纲生成（generate + Function-Calling）', () => {
  it('模型调用 getWorldBible 后产出大纲，并抽取省钱价值事件', async () => {
    const withTool: ChatResponse = {
      content: null,
      toolCalls: [{ id: 't1', type: 'function', function: { name: 'getWorldBible', arguments: '{}' } }],
      meta: meta({ model: 'gpt-4o-mini' }),
      guardrail: { blocked: false },
    };
    const final: ChatResponse = {
      content: '第1集……第2集……第3集（钩子）',
      meta: meta({ model: 'gpt-4o-mini', tokensSaved: 320, cacheHit: true }),
      guardrail: { blocked: false },
    };
    const client = new MockAegisClient({ chatResponses: [withTool, final] });
    const step = plugin.uiSteps.find((s) => s.id === 'outline')!;
    const result = await runStep({ plugin, step, inputs: { theme: '第一条线索', episodes: 3 }, client });

    expect(result.output).toContain('第3集');
    expect(result.toolInvocations.some((t) => t.name === 'getWorldBible' && t.ok)).toBe(true);
    const saved = result.events.reduce((sum, e) => sum + e.tokensSaved, 0);
    expect(saved).toBe(320);
    expect(result.events.some((e) => e.cacheHit)).toBe(true);
  });
});

describe('合规引擎：越界内容触发护栏拦截', () => {
  it('越界文本 → 拦截 + compliance 引擎价值事件', async () => {
    const client = new MockAegisClient({
      guardResult: { guardrail: { blocked: true, reason: '露骨暴力描写越界', code: 'comic-block-graphic-violence' }, meta: meta() },
    });
    const step = plugin.uiSteps.find((s) => s.id === 'compliance')!;
    const result = await runStep({ plugin, step, inputs: { text: '血肉模糊的虐杀场面' }, client });
    expect(result.guardrail?.blocked).toBe(true);
    expect(result.output).toContain('露骨暴力描写越界');
    expect(result.events[0].engine).toBe('compliance');
    expect(result.events[0].guardrailBlocked).toBe(true);
  });

  it('正常文本 → 合规通过', async () => {
    const client = new MockAegisClient({ guardResult: { guardrail: { blocked: false }, meta: meta() } });
    const step = plugin.uiSteps.find((s) => s.id === 'compliance')!;
    const result = await runStep({ plugin, step, inputs: { text: '凌墟驾驶机甲穿越星轨' }, client });
    expect(result.guardrail?.blocked).toBe(false);
    expect(result.output).toContain('合规预审通过');
  });
});
