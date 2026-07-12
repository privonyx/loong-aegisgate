/**
 * AegisGate 交互契约（OpenAI 兼容子集 + 网关价值元数据）。
 *
 * 这些类型描述 Demo 后端与 AegisGate 之间的请求/响应形状，以及从网关
 * 响应头中抽取的「价值元数据」（缓存省量 / 路由模型 / 护栏决策）。
 */

import type { ToolSpec } from '../scenarios/types';

export type ChatRole = 'system' | 'user' | 'assistant' | 'tool';

export interface ToolCall {
  id: string;
  type: 'function';
  function: { name: string; arguments: string };
}

export interface ChatMessage {
  role: ChatRole;
  content: string | null;
  tool_calls?: ToolCall[];
  tool_call_id?: string;
  name?: string;
}

export interface ChatRequest {
  model: string;
  messages: ChatMessage[];
  tools?: ToolSpec[];
  temperature?: number;
  maxTokens?: number;
}

export interface TokenUsage {
  prompt_tokens: number;
  completion_tokens: number;
  total_tokens: number;
}

/** 从 AegisGate 响应（体 + 头）抽取的网关价值元数据。 */
export interface GatewayMeta {
  /** 实际应答模型（智能路由可见性）。 */
  model: string;
  /** 经语义缓存 / Token 优化省下的 token 数（X-AegisGate-Tokens-Saved）。 */
  tokensSaved: number;
  /** 本次是否命中语义缓存。 */
  cacheHit: boolean;
  /** 端到端延迟（毫秒）。 */
  latencyMs: number;
  usage?: TokenUsage;
}

/** 护栏决策（注入/越界/合规拦截）。 */
export interface GuardrailDecision {
  blocked: boolean;
  reason?: string;
  code?: string;
}

export interface ChatResponse {
  content: string | null;
  toolCalls?: ToolCall[];
  meta: GatewayMeta;
  guardrail?: GuardrailDecision;
}

export interface GuardrailCheckResult {
  guardrail: GuardrailDecision;
  meta: GatewayMeta;
}

/** 流式增量片段（目前只透传文本内容；工具调用 delta 在客户端内部累积）。 */
export interface StreamDelta {
  content: string;
}

/**
 * AegisGate 客户端接口（运行时依赖此抽象，便于单测注入 mock）。
 * 真实实现见 `aegis/client.ts`（阶段 2）。
 */
export interface AegisChatClient {
  /** 发起一次（可能带工具的）对话补全，经 AegisGate。 */
  chat(req: ChatRequest): Promise<ChatResponse>;
  /**
   * 流式发起对话补全（stream:true），每收到文本增量即回调 onDelta，
   * 同时在内部累积内容与工具调用，最终返回与 chat() 等价的完整 ChatResponse。
   * 可选方法：未实现时上层回退到非流式 chat()。
   */
  chatStream?(
    req: ChatRequest,
    onDelta: (delta: StreamDelta) => void
  ): Promise<ChatResponse>;
  /**
   * 把候选文本送入网关护栏做合规预审，返回护栏决策 + 价值元数据。
   * 用于「合规引擎」步骤，显式演示出入站护栏。
   */
  checkGuardrail(text: string, model: string): Promise<GuardrailCheckResult>;
}
