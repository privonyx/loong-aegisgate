/**
 * AegisGate AI Gateway 客户端
 * 支持超时、指数退避 + jitter 重试、429/5xx 自动重试、Retry-After 尊重、追踪头注入
 */

import {
  AegisGateAPIError,
  AegisGateAuthenticationError,
  AegisGateConnectionError,
  AegisGateRateLimitError,
  AegisGateTimeoutError,
} from './errors.js';
import type {
  AegisGateClientConfig,
  ChatCompletionRequest,
  ChatCompletionResponse,
  ChatCompletionChunk,
  ChatMessage,
  ChatOptions,
  ChatResult,
  HealthResponse,
  ModelListResponse,
} from './types.js';

const DEFAULT_BASE_URL = 'http://localhost:8080';
const DEFAULT_TIMEOUT = 60_000;
const DEFAULT_MAX_RETRIES = 2;
const DEFAULT_RETRY_DELAY = 1000;
const DEFAULT_RETRYABLE_STATUS_CODES = new Set([429, 500, 502, 503, 504]);

function backoffDelay(attempt: number, baseDelay: number, jitter: boolean): number {
  let delay = baseDelay * Math.pow(2, attempt);
  if (jitter) {
    delay = delay * (0.5 + Math.random() * 0.5);
  }
  return delay;
}

function parseRetryAfter(headers: Headers): number | null {
  const value = headers.get('retry-after');
  if (value === null) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed * 1000 : null;
}

export class AegisGateClient {
  private readonly apiKey: string;
  private readonly baseUrl: string;
  private readonly timeout: number;
  private readonly maxRetries: number;
  private readonly retryDelay: number;
  private readonly retryJitter: boolean;
  private readonly retryOnStatus: Set<number>;
  private readonly traceId: string | undefined;
  private readonly traceHeaders: Record<string, string>;
  private readonly defaultHeaders: Record<string, string>;

  constructor(config: AegisGateClientConfig) {
    this.apiKey = config.apiKey;
    this.baseUrl = (config.baseUrl ?? DEFAULT_BASE_URL).replace(/\/$/, '');
    this.timeout = config.timeout ?? DEFAULT_TIMEOUT;
    this.maxRetries = config.maxRetries ?? DEFAULT_MAX_RETRIES;
    this.retryDelay = config.retryDelay ?? DEFAULT_RETRY_DELAY;
    this.retryJitter = config.retryJitter ?? true;
    this.retryOnStatus = config.retryOnStatus
      ? new Set(config.retryOnStatus)
      : DEFAULT_RETRYABLE_STATUS_CODES;
    this.traceId = config.traceId;
    this.traceHeaders = config.traceHeaders ?? {};
    this.defaultHeaders = config.defaultHeaders ?? {};
  }

  async chat(userMessage: string, options: ChatOptions = {}): Promise<ChatResult> {
    const messages: ChatMessage[] = [];
    if (options.systemPrompt) {
      messages.push({ role: 'system', content: options.systemPrompt });
    }
    messages.push({ role: 'user', content: userMessage });

    const response = await this.chatCompletions({
      model: options.model ?? 'gpt-4o',
      messages,
      temperature: options.temperature,
      max_tokens: options.maxTokens,
      stream: false,
    });

    const choice = response.choices[0];
    if (!choice) {
      throw new Error('Chat completion 返回空 choices');
    }

    return {
      content: choice.message.content,
      role: 'assistant',
      finishReason: choice.finish_reason,
      usage: response.usage,
      raw: response,
    };
  }

  async chatCompletions(request: ChatCompletionRequest): Promise<ChatCompletionResponse> {
    const body = { ...request, stream: false };
    const response = await this.request<ChatCompletionResponse>('/v1/chat/completions', {
      method: 'POST',
      body: JSON.stringify(body),
    });
    return response;
  }

  async *chatCompletionsStream(
    request: ChatCompletionRequest
  ): AsyncGenerator<ChatCompletionChunk, void, undefined> {
    const body = { ...request, stream: true };
    const response = await this.fetchWithTimeout(`${this.baseUrl}/v1/chat/completions`, {
      method: 'POST',
      headers: this.authHeaders(),
      body: JSON.stringify(body),
    });

    if (!response.ok) {
      await this.throwApiError(response);
    }

    const reader = response.body?.getReader();
    if (!reader) {
      throw new AegisGateConnectionError('响应体不可读');
    }

    const decoder = new TextDecoder();
    let buffer = '';

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() ?? '';

        for (const line of lines) {
          if (line.startsWith('data: ')) {
            const data = line.slice(6);
            if (data === '[DONE]') return;
            try {
              const chunk = JSON.parse(data) as ChatCompletionChunk;
              yield chunk;
            } catch {
              // skip unparseable lines
            }
          }
        }
      }
    } finally {
      reader.releaseLock();
    }
  }

  async listModels(): Promise<ModelListResponse> {
    return this.request<ModelListResponse>('/v1/models', { method: 'GET' });
  }

  async health(): Promise<HealthResponse> {
    const response = await this.fetchWithTimeout(`${this.baseUrl}/health`, {
      method: 'GET',
      headers: { 'Content-Type': 'application/json' },
    });

    if (!response.ok) {
      throw new Error(`Health check 失败: ${response.status} ${response.statusText}`);
    }

    return (await response.json()) as HealthResponse;
  }

  async metrics(): Promise<string> {
    const response = await this.requestRaw('/metrics', { method: 'GET' });
    return response.text();
  }

  async reloadConfig(): Promise<void> {
    await this.requestRaw('/admin/reload', { method: 'POST' });
  }

  private authHeaders(): Record<string, string> {
    const headers: Record<string, string> = {
      Authorization: `Bearer ${this.apiKey}`,
      'Content-Type': 'application/json',
      ...this.defaultHeaders,
      ...this.traceHeaders,
    };
    if (this.traceId && !headers['X-Trace-Id']) {
      headers['X-Trace-Id'] = this.traceId;
    }
    return headers;
  }

  private async request<T>(path: string, init: RequestInit): Promise<T> {
    const response = await this.requestRaw(path, init);
    const json = (await response.json()) as T;
    return json;
  }

  private async requestRaw(path: string, init: RequestInit): Promise<Response> {
    const url = `${this.baseUrl}${path}`;
    let lastError: Error | null = null;

    for (let attempt = 0; attempt <= this.maxRetries; attempt++) {
      try {
        const response = await this.fetchWithTimeout(url, {
          ...init,
          headers: {
            ...this.authHeaders(),
            ...(init.headers as Record<string, string>),
          },
        });

        if (!response.ok) {
          const retryAfterMs = parseRetryAfter(response.headers);

          if (this.retryOnStatus.has(response.status) && attempt < this.maxRetries) {
            const delay = retryAfterMs ?? backoffDelay(attempt, this.retryDelay, this.retryJitter);
            await this.sleep(delay);
            continue;
          }

          await this.throwApiError(response);
        }

        return response;
      } catch (err) {
        lastError = err instanceof Error ? err : new Error(String(err));

        if (err instanceof AegisGateAuthenticationError) throw err;
        if (
          err instanceof AegisGateAPIError &&
          err.statusCode !== undefined &&
          err.statusCode >= 400 &&
          err.statusCode < 500 &&
          err.statusCode !== 429
        ) {
          throw err;
        }

        if (attempt === this.maxRetries) {
          throw lastError;
        }

        await this.sleep(backoffDelay(attempt, this.retryDelay, this.retryJitter));
      }
    }

    throw lastError ?? new AegisGateConnectionError('请求失败');
  }

  private async fetchWithTimeout(
    url: string,
    init: RequestInit
  ): Promise<Response> {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), this.timeout);

    try {
      const response = await fetch(url, {
        ...init,
        signal: controller.signal,
      });
      return response;
    } catch (err) {
      if (err instanceof Error && err.name === 'AbortError') {
        throw new AegisGateTimeoutError(`请求在 ${this.timeout}ms 后超时`);
      }
      if (err instanceof TypeError && err.message.includes('fetch')) {
        throw new AegisGateConnectionError('网络请求失败', err);
      }
      throw err;
    } finally {
      clearTimeout(timeoutId);
    }
  }

  private async throwApiError(response: Response): Promise<never> {
    const body = await response.text();
    let parsed: { error?: { message?: string; code?: string } } | null = null;
    try {
      parsed = JSON.parse(body) as { error?: { message?: string; code?: string } };
    } catch {
      // skip parse failures
    }

    const message = parsed?.error?.message ?? `API 错误: ${response.status} ${response.statusText}`;
    const errorCode = parsed?.error?.code;

    if (response.status === 401) {
      throw new AegisGateAuthenticationError(message, { responseBody: body });
    }
    if (response.status === 429) {
      const retryAfter = parseRetryAfter(response.headers);
      const err = new AegisGateRateLimitError(message, { responseBody: body });
      (err as AegisGateRateLimitError & { retryAfter: number | null }).retryAfter = retryAfter;
      throw err;
    }

    throw new AegisGateAPIError(message, {
      statusCode: response.status,
      errorCode,
      responseBody: body,
    });
  }

  private sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }
}
