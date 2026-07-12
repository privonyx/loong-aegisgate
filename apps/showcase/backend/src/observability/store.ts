/**
 * 观测聚合（内存级）：收集运行时抛出的价值事件，供「双核心价值面板」读取。
 * 聚合两张杀手锏指标：缓存/Token 省钱（创作引擎）+ 护栏拦截（合规引擎）。
 */

import type { ScenarioEngine } from '../scenarios/types';
import type { ValueEvent } from '../runtime/chatLoop';

export interface EngineStats {
  requests: number;
  tokensSaved: number;
  cacheHits: number;
  guardrailBlocks: number;
}

export interface ObservabilitySummary {
  totalRequests: number;
  totalTokensSaved: number;
  cacheHits: number;
  cacheHitRate: number;
  guardrailBlocks: number;
  avgLatencyMs: number;
  byEngine: Record<ScenarioEngine, EngineStats>;
  modelsUsed: Record<string, number>;
}

const MAX_LOG = 200;

function emptyEngineStats(): EngineStats {
  return { requests: 0, tokensSaved: 0, cacheHits: 0, guardrailBlocks: 0 };
}

export class ObservabilityStore {
  private readonly events: ValueEvent[] = [];

  record(event: ValueEvent): void {
    this.events.push(event);
    if (this.events.length > MAX_LOG) {
      this.events.splice(0, this.events.length - MAX_LOG);
    }
  }

  summary(): ObservabilitySummary {
    const byEngine: Record<ScenarioEngine, EngineStats> = {
      creation: emptyEngineStats(),
      compliance: emptyEngineStats(),
    };
    const modelsUsed: Record<string, number> = {};
    let totalTokensSaved = 0;
    let cacheHits = 0;
    let guardrailBlocks = 0;
    let latencySum = 0;

    for (const e of this.events) {
      const eng = byEngine[e.engine];
      eng.requests += 1;
      eng.tokensSaved += e.tokensSaved;
      totalTokensSaved += e.tokensSaved;
      if (e.cacheHit) {
        cacheHits += 1;
        eng.cacheHits += 1;
      }
      if (e.guardrailBlocked) {
        guardrailBlocks += 1;
        eng.guardrailBlocks += 1;
      }
      latencySum += e.latencyMs;
      modelsUsed[e.model] = (modelsUsed[e.model] ?? 0) + 1;
    }

    const totalRequests = this.events.length;
    return {
      totalRequests,
      totalTokensSaved,
      cacheHits,
      cacheHitRate: totalRequests > 0 ? cacheHits / totalRequests : 0,
      guardrailBlocks,
      avgLatencyMs: totalRequests > 0 ? Math.round(latencySum / totalRequests) : 0,
      byEngine,
      modelsUsed,
    };
  }

  log(limit = 50): ValueEvent[] {
    return this.events.slice(-limit).reverse();
  }

  reset(): void {
    this.events.length = 0;
  }
}
