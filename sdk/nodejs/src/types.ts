/**
 * AegisGate SDK 类型定义
 * OpenAI 兼容的 API 接口类型
 */

/** 消息角色 */
export type MessageRole = 'system' | 'user' | 'assistant';

/** 单条消息 */
export interface ChatMessage {
  role: MessageRole;
  content: string;
}

/** Chat Completions 请求体 */
export interface ChatCompletionRequest {
  model: string;
  messages: ChatMessage[];
  temperature?: number;
  max_tokens?: number;
  stream?: boolean;
  /** 其他 OpenAI 兼容参数 */
  [key: string]: unknown;
}

/** 非流式响应的消息 */
export interface ChatCompletionMessage {
  role: 'assistant';
  content: string;
}

/** 流式响应的增量消息 */
export interface ChatCompletionChunkDelta {
  role?: 'assistant';
  content?: string;
}

/** 非流式 choice */
export interface ChatCompletionChoice {
  index: number;
  message: ChatCompletionMessage;
  finish_reason: 'stop' | 'length' | 'content_filter' | null;
}

/** 流式 choice */
export interface ChatCompletionChunkChoice {
  index: number;
  delta: ChatCompletionChunkDelta;
  finish_reason: 'stop' | 'length' | 'content_filter' | null;
}

/** Token 使用量 */
export interface Usage {
  prompt_tokens: number;
  completion_tokens: number;
  total_tokens: number;
}

/** Chat Completions 非流式响应 */
export interface ChatCompletionResponse {
  id: string;
  object: 'chat.completion';
  model: string;
  choices: ChatCompletionChoice[];
  usage?: Usage;
}

/** Chat Completions 流式响应块 */
export interface ChatCompletionChunk {
  id: string;
  object: 'chat.completion.chunk';
  model: string;
  choices: ChatCompletionChunkChoice[];
}

/** 模型信息 */
export interface Model {
  id: string;
  object: 'model';
  owned_by?: string;
}

/** 模型列表响应 */
export interface ModelListResponse {
  object: 'list';
  data: Model[];
}

/** 健康检查响应 */
export interface HealthResponse {
  status: string;
  version: string;
}

/** 客户端配置 */
export interface AegisGateClientConfig {
  /** API 密钥，用于认证 */
  apiKey: string;
  /** 网关基础 URL，默认 http://localhost:8080 */
  baseUrl?: string;
  /** 请求超时（毫秒），默认 60000 */
  timeout?: number;
  /** 重试次数，默认 2 */
  maxRetries?: number;
  /** 重试间隔（毫秒），默认 1000 */
  retryDelay?: number;
  /** 启用指数退避 jitter（默认 true） */
  retryJitter?: boolean;
  /** 可重试的 HTTP 状态码集合（默认 [429, 500, 502, 503, 504]） */
  retryOnStatus?: number[];
  /** 静态追踪 ID，自动注入 X-Trace-Id 请求头 */
  traceId?: string;
  /** 自定义追踪头（如 W3C traceparent / b3 等），每次请求都会带上 */
  traceHeaders?: Record<string, string>;
  /** 额外的自定义请求头 */
  defaultHeaders?: Record<string, string>;
}

/** Chat 便捷方法选项 */
export interface ChatOptions {
  model?: string;
  temperature?: number;
  maxTokens?: number;
  systemPrompt?: string;
}

/** Chat 便捷方法返回（非流式） */
export interface ChatResult {
  content: string;
  role: 'assistant';
  finishReason: 'stop' | 'length' | 'content_filter' | null;
  usage?: Usage;
  raw: ChatCompletionResponse;
}
