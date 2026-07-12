import { describe, expect, it } from 'vitest';
import { ScenarioRegistry } from '../src/runtime/registry';
import { makeTestPlugin } from './helpers/fixtures';

describe('ScenarioRegistry', () => {
  it('注册后可按 id 查询', () => {
    const reg = new ScenarioRegistry();
    const plugin = makeTestPlugin();
    reg.register(plugin);
    expect(reg.get('test')).toBe(plugin);
    expect(reg.has('test')).toBe(true);
    expect(reg.require('test')).toBe(plugin);
  });

  it('重复注册同 id 抛错', () => {
    const reg = new ScenarioRegistry();
    reg.register(makeTestPlugin());
    expect(() => reg.register(makeTestPlugin())).toThrow(/重复注册/);
  });

  it('require 未知 id 抛错', () => {
    const reg = new ScenarioRegistry();
    expect(() => reg.require('nope')).toThrow(/未知场景/);
    expect(reg.get('nope')).toBeUndefined();
    expect(reg.has('nope')).toBe(false);
  });

  it('summaries 不含 handler / dataset 内部细节', () => {
    const reg = new ScenarioRegistry();
    reg.register(makeTestPlugin());
    const summaries = reg.summaries();
    expect(summaries).toHaveLength(1);
    const s = summaries[0];
    expect(s.id).toBe('test');
    expect(s.meta.name).toBe('测试场景');
    expect(s.uiSteps).toHaveLength(3);
    // 摘要不应泄露 tools handler / dataset
    const raw = s as unknown as Record<string, unknown>;
    expect(raw.tools).toBeUndefined();
    expect(raw.dataset).toBeUndefined();
  });
});
