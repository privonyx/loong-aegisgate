/**
 * AegisGate 真实客户端（OpenAI 兼容 + 网关价值元数据捕获）。
 *
 * SDK 的 ChatResult 不暴露 HTTP 响应头，而 X-AegisGate-Tokens-Saved /
 * 缓存命中 / 应答模型这些「价值信号」恰恰在响应头里——因此这里用 fetch
 * 直连网关，自行抽取响应头（plan 已记此约束）。fetchImpl 可注入便于单测。
 *
 * SR-1：apiKey 仅在 Authorization 头使用，绝不写入响应体 / 日志 / 错误信息。
 * SR-4：护栏拦截只回传 reason / code，绝不回显被拦截原文。
 */

import type {
  AegisChatClient,
  ChatRequest,
  ChatResponse,
  GatewayMeta,
  GuardrailCheckResult,
  GuardrailDecision,
  StreamDelta,
  ToolCall,
} from './types';

export class AegisGateError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly code?: string
  ) {
    super(message);
    this.name = 'AegisGateError';
  }
}

export interface AegisClientConfig {
  baseUrl: string;
  apiKey: string;
  /** 注入 fetch 实现（单测用）；默认全局 fetch。 */
  fetchImpl?: typeof fetch;
  timeoutMs?: number;
}

const HEADER_TOKENS_SAVED = 'x-aegisgate-tokens-saved';
const HEADER_CACHE = 'x-aegisgate-cache';
const HEADER_CACHE_HIT = 'x-aegisgate-cache-hit';
const HEADER_MODEL = 'x-aegisgate-model';

const GUARDRAIL_KEYWORDS = ['guardrail', 'security', 'content', 'policy', 'blocked', 'injection'];

function parseIntHeader(value: string | null): number {
  if (!value) return 0;
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) ? n : 0;
}

function isCacheHit(headers: Headers): boolean {
  const cache = headers.get(HEADER_CACHE);
  if (cache && cache.toLowerCase() === 'hit') return true;
  const cacheHit = headers.get(HEADER_CACHE_HIT);
  return cacheHit ? cacheHit.toLowerCase() === 'true' : false;
}

function extractMeta(headers: Headers, bodyModel: string | undefined, latencyMs: number, usage?: unknown): GatewayMeta {
  const model = headers.get(HEADER_MODEL) ?? bodyModel ?? 'unknown';
  return {
    model,
    tokensSaved: parseIntHeader(headers.get(HEADER_TOKENS_SAVED)),
    cacheHit: isCacheHit(headers),
    latencyMs,
    usage: usage as GatewayMeta['usage'],
  };
}

/** 判断错误响应是否为护栏拦截（而非真正的网关错误）。 */
function detectGuardrail(status: number, body: unknown): GuardrailDecision | null {
  if (status === 403) {
    const reason = extractErrorMessage(body) ?? '内容触发安全护栏';
    return { blocked: true, reason, code: extractErrorCode(body) ?? 'guardrail_blocked' };
  }
  const code = extractErrorCode(body)?.toLowerCase() ?? '';
  const message = extractErrorMessage(body)?.toLowerCase() ?? '';
  if (GUARDRAIL_KEYWORDS.some((kw) => code.includes(kw) || message.includes(kw))) {
    return { blocked: true, reason: extractErrorMessage(body) ?? '内容触发安全护栏', code: extractErrorCode(body) ?? 'guardrail_blocked' };
  }
  return null;
}

function extractErrorMessage(body: unknown): string | undefined {
  if (body && typeof body === 'object') {
    const err = (body as Record<string, unknown>).error;
    if (typeof err === 'string') return err;
    if (err && typeof err === 'object' && typeof (err as Record<string, unknown>).message === 'string') {
      return (err as Record<string, string>).message;
    }
    if (typeof (body as Record<string, unknown>).message === 'string') {
      return (body as Record<string, string>).message;
    }
  }
  return undefined;
}

function extractErrorCode(body: unknown): string | undefined {
  if (body && typeof body === 'object') {
    const err = (body as Record<string, unknown>).error;
    if (err && typeof err === 'object') {
      const code = (err as Record<string, unknown>).code ?? (err as Record<string, unknown>).type;
      if (typeof code === 'string') return code;
    }
  }
  return undefined;
}

interface OpenAIChoice {
  message?: { content?: string | null; tool_calls?: ToolCall[] };
}
interface OpenAIResponse {
  model?: string;
  choices?: OpenAIChoice[];
  usage?: unknown;
}

export function createAegisClient(config: AegisClientConfig): AegisChatClient {
  const fetchImpl = config.fetchImpl ?? fetch;
  const baseUrl = config.baseUrl.replace(/\/+$/, '');
  const url = `${baseUrl}/v1/chat/completions`;

  function buildPayload(req: ChatRequest, stream: boolean): Record<string, unknown> {
    const payload: Record<string, unknown> = {
      model: req.model,
      messages: req.messages,
    };
    if (req.tools && req.tools.length > 0) payload.tools = req.tools;
    if (req.temperature !== undefined) payload.temperature = req.temperature;
    if (req.maxTokens !== undefined) payload.max_tokens = req.maxTokens;
    if (stream) payload.stream = true;
    return payload;
  }

  function requestInit(payload: Record<string, unknown>): RequestInit {
    return {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        // SR-1：apiKey 仅在此 Authorization 头使用。
        authorization: `Bearer ${config.apiKey}`,
      },
      body: JSON.stringify(payload),
      signal: config.timeoutMs ? AbortSignal.timeout(config.timeoutMs) : undefined,
    };
  }

  async function postChat(req: ChatRequest): Promise<ChatResponse> {
    const started = Date.now();
    const res = await fetchImpl(url, requestInit(buildPayload(req, false)));
    const latencyMs = Date.now() - started;

    const rawText = await res.text();
    let body: unknown = undefined;
    try {
      body = rawText ? JSON.parse(rawText) : undefined;
    } catch {
      body = undefined;
    }

    if (!res.ok) {
      const guardrail = detectGuardrail(res.status, body);
      if (guardrail) {
        // 护栏拦截：作为正常结果返回（不抛错），让上层做价值可视化。
        return {
          content: null,
          meta: extractMeta(res.headers, undefined, latencyMs),
          guardrail,
        };
      }
      // SR-1/SR-4：错误信息只含状态与网关消息，绝不含 apiKey / 原始请求体。
      throw new AegisGateError(
        extractErrorMessage(body) ?? `AegisGate 请求失败（HTTP ${res.status}）`,
        res.status,
        extractErrorCode(body)
      );
    }

    const parsed = (body ?? {}) as OpenAIResponse;
    const choice = parsed.choices?.[0]?.message;
    return {
      content: choice?.content ?? null,
      toolCalls: choice?.tool_calls,
      meta: extractMeta(res.headers, parsed.model, latencyMs, parsed.usage),
      guardrail: { blocked: false },
    };
  }

  async function postChatStream(
    req: ChatRequest,
    onDelta: (delta: StreamDelta) => void
  ): Promise<ChatResponse> {
    const started = Date.now();
    const res = await fetchImpl(url, requestInit(buildPayload(req, true)));

    // 非 2xx：与非流式一致处理（护栏拦截作为正常结果，其余抛错）。
    if (!res.ok) {
      const rawText = await res.text();
      let body: unknown;
      try {
        body = rawText ? JSON.parse(rawText) : undefined;
      } catch {
        body = undefined;
      }
      const guardrail = detectGuardrail(res.status, body);
      if (guardrail) {
        return { content: null, meta: extractMeta(res.headers, undefined, Date.now() - started), guardrail };
      }
      throw new AegisGateError(
        extractErrorMessage(body) ?? `AegisGate 请求失败（HTTP ${res.status}）`,
        res.status,
        extractErrorCode(body)
      );
    }

    if (!res.body) {
      throw new AegisGateError('AegisGate 流式响应无响应体', 502);
    }

    const reader = (res.body as ReadableStream<Uint8Array>).getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    let content = '';
    const toolAcc = new Map<number, { id?: string; type?: string; name?: string; args: string }>();
    let metaTokensSaved = 0;
    let metaModel: string | undefined;
    let metaUsage: unknown;
    let guardrail: GuardrailDecision | undefined;
    let errored: AegisGateError | undefined;

    const processLine = (line: string): void => {
      const trimmed = line.trim();
      if (!trimmed.startsWith('data:')) return;
      const data = trimmed.slice(5).trim();
      if (data === '' || data === '[DONE]') return;
      let j: Record<string, unknown>;
      try {
        j = JSON.parse(data) as Record<string, unknown>;
      } catch {
        return;
      }

      // 错误/护栏事件（流式下网关以 HTTP 200 + SSE error 帧返回）。
      if (j.error !== undefined) {
        const g = detectGuardrail(200, j);
        if (g) {
          guardrail = g;
        } else {
          errored = new AegisGateError(
            extractErrorMessage(j) ?? 'AegisGate 流式错误',
            500,
            extractErrorCode(j)
          );
        }
        return;
      }

      // 网关价值元数据事件：{"aegisgate":{"tokens_saved":..,"usage":..}}
      const aegis = j.aegisgate as Record<string, unknown> | undefined;
      if (aegis) {
        if (typeof aegis.tokens_saved === 'number') metaTokensSaved = aegis.tokens_saved;
        if (aegis.usage !== undefined) metaUsage = aegis.usage;
        return;
      }

      if (typeof j.model === 'string') metaModel = j.model;
      const choices = j.choices as Array<Record<string, unknown>> | undefined;
      const choice = choices?.[0];
      if (!choice) return;
      const delta = (choice.delta ?? {}) as Record<string, unknown>;

      if (typeof delta.content === 'string' && delta.content.length > 0) {
        content += delta.content;
        onDelta({ content: delta.content });
      }

      const toolDeltas = delta.tool_calls as Array<Record<string, unknown>> | undefined;
      if (Array.isArray(toolDeltas)) {
        for (const tc of toolDeltas) {
          const idx = typeof tc.index === 'number' ? tc.index : 0;
          const cur = toolAcc.get(idx) ?? { args: '' };
          if (typeof tc.id === 'string') cur.id = tc.id;
          if (typeof tc.type === 'string') cur.type = tc.type;
          const fn = tc.function as Record<string, unknown> | undefined;
          if (fn) {
            if (typeof fn.name === 'string') cur.name = fn.name;
            if (typeof fn.arguments === 'string') cur.args += fn.arguments;
          }
          toolAcc.set(idx, cur);
        }
      }
    };

    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      let nl: number;
      while ((nl = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, nl);
        buffer = buffer.slice(nl + 1);
        processLine(line);
      }
    }
    if (buffer) processLine(buffer);

    if (errored) throw errored;

    const toolCalls: ToolCall[] | undefined =
      toolAcc.size > 0
        ? [...toolAcc.entries()]
            .sort((a, b) => a[0] - b[0])
            .map(([, v]) => ({
              id: v.id ?? '',
              type: 'function' as const,
              function: { name: v.name ?? '', arguments: v.args },
            }))
        : undefined;

    const latencyMs = Date.now() - started;
    return {
      content: content.length > 0 ? content : null,
      toolCalls,
      meta: {
        model: metaModel ?? res.headers.get(HEADER_MODEL) ?? 'unknown',
        tokensSaved: metaTokensSaved || parseIntHeader(res.headers.get(HEADER_TOKENS_SAVED)),
        cacheHit: isCacheHit(res.headers),
        latencyMs,
        usage: metaUsage as GatewayMeta['usage'],
      },
      guardrail: guardrail ?? { blocked: false },
    };
  }

  return {
    chat: postChat,
    chatStream: postChatStream,
    async checkGuardrail(text: string, model: string): Promise<GuardrailCheckResult> {
      const response = await postChat({
        model,
        messages: [{ role: 'user', content: text }],
        maxTokens: 16,
      });
      return {
        guardrail: response.guardrail ?? { blocked: false },
        meta: response.meta,
      };
    },
  };
}
