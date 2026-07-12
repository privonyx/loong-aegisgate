import { describe, expect, it } from 'vitest';
import { ObservabilityStore } from '../src/observability/store';
import type { ValueEvent } from '../src/runtime/chatLoop';

function event(over: Partial<ValueEvent> = {}): ValueEvent {
  return {
    ts: Date.now(),
    scenarioId: 'comic',
    stepId: 'outline',
    engine: 'creation',
    model: 'gpt-4o-mini',
    tokensSaved: 0,
    cacheHit: false,
    latencyMs: 10,
    guardrailBlocked: false,
    ...over,
  };
}

describe('ObservabilityStore', () => {
  it('聚合省量 / 缓存命中 / 护栏拦截 / 按引擎拆分', () => {
    const store = new ObservabilityStore();
    store.record(event({ tokensSaved: 100, cacheHit: false, engine: 'creation' }));
    store.record(event({ tokensSaved: 250, cacheHit: true, engine: 'creation' }));
    store.record(event({ engine: 'compliance', guardrailBlocked: true, model: 'gpt-4o' }));

    const s = store.summary();
    expect(s.totalRequests).toBe(3);
    expect(s.totalTokensSaved).toBe(350);
    expect(s.cacheHits).toBe(1);
    expect(s.cacheHitRate).toBeCloseTo(1 / 3);
    expect(s.guardrailBlocks).toBe(1);
    expect(s.byEngine.creation.requests).toBe(2);
    expect(s.byEngine.creation.tokensSaved).toBe(350);
    expect(s.byEngine.compliance.guardrailBlocks).toBe(1);
    expect(s.modelsUsed['gpt-4o-mini']).toBe(2);
    expect(s.modelsUsed['gpt-4o']).toBe(1);
  });

  it('空状态返回零值且不除零', () => {
    const s = new ObservabilityStore().summary();
    expect(s.totalRequests).toBe(0);
    expect(s.cacheHitRate).toBe(0);
    expect(s.avgLatencyMs).toBe(0);
  });

  it('log 倒序返回且受 limit 约束', () => {
    const store = new ObservabilityStore();
    for (let i = 0; i < 5; i += 1) store.record(event({ stepId: `s${i}` }));
    const log = store.log(2);
    expect(log).toHaveLength(2);
    expect(log[0].stepId).toBe('s4'); // 最新在前
  });
});
