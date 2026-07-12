/** 后端 BFF API 客户端（前端从不直接接触 AegisGate Key —— SR-1）。 */

import type { ObservabilitySummary, RunStepResult, ScenarioSummary, ValueEvent } from './types';

const BASE = '/api';

async function handle<T>(res: Response): Promise<T> {
  if (!res.ok) {
    let message = `请求失败（HTTP ${res.status}）`;
    try {
      const body = (await res.json()) as { error?: string };
      if (body?.error) message = body.error;
    } catch {
      /* 忽略解析失败 */
    }
    throw new Error(message);
  }
  return (await res.json()) as T;
}

export async function getScenarios(): Promise<ScenarioSummary[]> {
  const data = await handle<{ scenarios: ScenarioSummary[] }>(await fetch(`${BASE}/scenarios`));
  return data.scenarios;
}

export async function runStep(
  scenarioId: string,
  stepId: string,
  inputs: Record<string, unknown>
): Promise<RunStepResult> {
  const res = await fetch(`${BASE}/scenarios/${scenarioId}/run`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ stepId, inputs }),
  });
  return handle<RunStepResult>(res);
}

export async function getSummary(): Promise<ObservabilitySummary> {
  return handle<ObservabilitySummary>(await fetch(`${BASE}/observability/summary`));
}

export interface RunStreamHandlers {
  /** 文本增量（逐字流式输出）。 */
  onToken?: (text: string) => void;
  /** 价值事件（缓存省量 / 路由模型 / 护栏决策），用于实时刷新价值面板。 */
  onValue?: (event: ValueEvent) => void;
  /** 步骤完成，返回完整结果。 */
  onDone?: (result: RunStepResult) => void;
  /** 出错（含护栏拦截以外的错误）。 */
  onError?: (message: string) => void;
}

/**
 * 流式运行步骤（SSE）：经后端 /run/stream，逐帧消费 token / value / done / error。
 * 后端再经 AegisGate stream:true 与上游真流式对接，实现逐字输出。
 */
export async function runStepStream(
  scenarioId: string,
  stepId: string,
  inputs: Record<string, unknown>,
  handlers: RunStreamHandlers
): Promise<void> {
  const res = await fetch(`${BASE}/scenarios/${scenarioId}/run/stream`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ stepId, inputs }),
  });

  if (!res.ok || !res.body) {
    let message = `请求失败（HTTP ${res.status}）`;
    try {
      const body = (await res.json()) as { error?: string };
      if (body?.error) message = body.error;
    } catch {
      /* 忽略 */
    }
    handlers.onError?.(message);
    return;
  }

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffer = '';

  const dispatch = (block: string): void => {
    let event = 'message';
    const dataLines: string[] = [];
    for (const line of block.split('\n')) {
      if (line.startsWith('event:')) event = line.slice(6).trim();
      else if (line.startsWith('data:')) dataLines.push(line.slice(5).trim());
    }
    if (dataLines.length === 0) return;
    let payload: unknown;
    try {
      payload = JSON.parse(dataLines.join('\n'));
    } catch {
      return;
    }
    switch (event) {
      case 'token':
        handlers.onToken?.((payload as { text: string }).text);
        break;
      case 'value':
        handlers.onValue?.(payload as ValueEvent);
        break;
      case 'done':
        handlers.onDone?.(payload as RunStepResult);
        break;
      case 'error':
        handlers.onError?.((payload as { error: string }).error);
        break;
      default:
        break;
    }
  };

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value, { stream: true });
    let sep: number;
    while ((sep = buffer.indexOf('\n\n')) >= 0) {
      const block = buffer.slice(0, sep);
      buffer = buffer.slice(sep + 2);
      if (block.trim()) dispatch(block);
    }
  }
  if (buffer.trim()) dispatch(buffer);
}
