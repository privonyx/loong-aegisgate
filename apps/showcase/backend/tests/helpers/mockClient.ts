/**
 * 可注入的 AegisGate 客户端 mock 接缝（real_only 决策下的单测兜底）。
 * 用脚本化的响应队列驱动 Function-Calling 多轮循环与护栏分支。
 */

import type {
  AegisChatClient,
  ChatRequest,
  ChatResponse,
  GatewayMeta,
  GuardrailCheckResult,
  GuardrailDecision,
} from '../../src/aegis/types';

export function meta(overrides: Partial<GatewayMeta> = {}): GatewayMeta {
  return {
    model: 'mock-model',
    tokensSaved: 0,
    cacheHit: false,
    latencyMs: 5,
    ...overrides,
  };
}

export class MockAegisClient implements AegisChatClient {
  readonly chatCalls: ChatRequest[] = [];
  readonly guardCalls: { text: string; model: string }[] = [];
  private readonly chatQueue: ChatResponse[];
  private guardResult: GuardrailCheckResult;

  constructor(opts: { chatResponses?: ChatResponse[]; guardResult?: GuardrailCheckResult } = {}) {
    this.chatQueue = [...(opts.chatResponses ?? [])];
    this.guardResult = opts.guardResult ?? {
      guardrail: { blocked: false },
      meta: meta(),
    };
  }

  setGuardResult(result: GuardrailCheckResult): void {
    this.guardResult = result;
  }

  async chat(req: ChatRequest): Promise<ChatResponse> {
    this.chatCalls.push(req);
    const next = this.chatQueue.shift();
    if (!next) {
      return { content: '（默认应答）', meta: meta(), guardrail: { blocked: false } };
    }
    return next;
  }

  async checkGuardrail(text: string, model: string): Promise<GuardrailCheckResult> {
    this.guardCalls.push({ text, model });
    return this.guardResult;
  }
}

export function guardrail(blocked: boolean, reason?: string, code?: string): GuardrailDecision {
  return { blocked, reason, code };
}
