/** 前端类型（镜像后端契约的公开子集）。 */

export type ScenarioEngine = 'creation' | 'compliance';
export type UiStepKind = 'generate' | 'tool' | 'guard';
export type UiInputType = 'text' | 'number' | 'textarea' | 'select';

export interface UiInputSpec {
  name: string;
  label: string;
  type: UiInputType;
  required?: boolean;
  placeholder?: string;
  options?: { value: string; label: string }[];
  default?: string | number;
}

export interface UiStep {
  id: string;
  label: string;
  description?: string;
  engine: ScenarioEngine;
  kind: UiStepKind;
  inputs?: UiInputSpec[];
}

export interface ScenarioSummary {
  id: string;
  meta: { name: string; description: string; icon: string; accentColor: string };
  uiSteps: UiStep[];
  model: { primary: string; fallback?: string };
}

export interface ValueEvent {
  ts: number;
  scenarioId: string;
  stepId: string;
  engine: ScenarioEngine;
  model: string;
  tokensSaved: number;
  cacheHit: boolean;
  latencyMs: number;
  guardrailBlocked: boolean;
  guardrailReason?: string;
}

export interface ToolInvocation {
  name: string;
  args: Record<string, unknown>;
  ok: boolean;
  error?: string;
}

export interface RunStepResult {
  output: string;
  events: ValueEvent[];
  toolInvocations: ToolInvocation[];
  guardrail?: { blocked: boolean; reason?: string; code?: string };
  rounds: number;
}

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
