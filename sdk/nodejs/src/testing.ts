/**
 * 无网络依赖的 Mock 客户端，供单元测试使用。
 */

import { AegisGateAPIError, AegisGateSecurityError } from './errors.js';
import type {
  ChatCompletionChunk,
  ChatCompletionRequest,
  ChatCompletionResponse,
  ChatMessage,
  ChatOptions,
  ChatResult,
  HealthResponse,
  ModelListResponse,
  Usage,
} from './types.js';

function lastUserPrompt(messages: ChatMessage[]): string {
  for (let i = messages.length - 1; i >= 0; i--) {
    if (messages[i]!.role === 'user') {
      return messages[i]!.content;
    }
  }
  return '';
}

function fullPromptForUsage(messages: ChatMessage[]): string {
  return messages.map((m) => m.content).join('\n');
}

function estimateUsage(prompt: string, completion: string): Usage {
  const pt = Math.max(1, Math.floor(prompt.length / 4));
  const ct = Math.max(1, Math.floor(completion.length / 4));
  return {
    prompt_tokens: pt,
    completion_tokens: ct,
    total_tokens: pt + ct,
  };
}

let mockIdSeq = 1;
function nextCompletionId(): string {
  return `mock-chatcmpl-${mockIdSeq++}`;
}

/** 单次调用的记录项 */
export interface MockClientCallRecord {
  method: string;
  userMessage?: string;
  request?: ChatCompletionRequest;
  [key: string]: unknown;
}

export class MockAegisGateClient {
  private readonly chatMocks: Array<{ pattern: string; content: string; model: string }> = [];
  private readonly errorMocks: Array<{ pattern: string; aegisCode: string; message: string; statusCode: number }> =
    [];
  private readonly streamMocks: Array<{ pattern: string; chunks: string[]; model: string }> = [];
  private defaultContent: string | null = null;
  private defaultModel = 'mock-model';
  private readonly _calls: MockClientCallRecord[] = [];

  get calls(): MockClientCallRecord[] {
    return [...this._calls];
  }

  get callCount(): number {
    return this._calls.length;
  }

  mockChat(promptPattern: string, options: { content: string; model?: string }): void {
    this.chatMocks.push({
      pattern: promptPattern,
      content: options.content,
      model: options.model ?? 'mock-model',
    });
  }

  mockError(
    promptPattern: string,
    options: { aegisCode: string; message: string; statusCode?: number }
  ): void {
    this.errorMocks.push({
      pattern: promptPattern,
      aegisCode: options.aegisCode,
      message: options.message,
      statusCode: options.statusCode ?? 400,
    });
  }

  mockStream(promptPattern: string, options: { chunks: string[]; model?: string }): void {
    this.streamMocks.push({
      pattern: promptPattern,
      chunks: [...options.chunks],
      model: options.model ?? 'mock-model',
    });
  }

  setDefaultResponse(content: string, model?: string): void {
    this.defaultContent = content;
    this.defaultModel = model ?? 'mock-model';
  }

  reset(): void {
    this.chatMocks.length = 0;
    this.errorMocks.length = 0;
    this.streamMocks.length = 0;
    this.defaultContent = null;
    this.defaultModel = 'mock-model';
    this._calls.length = 0;
  }

  private firstMatch<T extends { pattern: string }>(prompt: string, list: T[]): T | undefined {
    return list.find((e) => e.pattern.length > 0 && prompt.includes(e.pattern));
  }

  private throwIfError(prompt: string): void {
    const hit = this.firstMatch(prompt, this.errorMocks);
    if (!hit) return;
    if (hit.statusCode === 403) {
      throw new AegisGateSecurityError(hit.message, {
        aegisCode: hit.aegisCode,
        errorCode: hit.aegisCode,
        responseBody: JSON.stringify({ code: hit.aegisCode, message: hit.message }),
      });
    }
    throw new AegisGateAPIError(hit.message, {
      statusCode: hit.statusCode,
      errorCode: hit.aegisCode,
      aegisCode: hit.aegisCode,
      responseBody: JSON.stringify({ code: hit.aegisCode, message: hit.message }),
    });
  }

  private makeCompletion(
    content: string,
    model: string,
    messages: ChatMessage[]
  ): ChatCompletionResponse {
    const usage = estimateUsage(fullPromptForUsage(messages), content);
    return {
      id: nextCompletionId(),
      object: 'chat.completion',
      model,
      choices: [
        {
          index: 0,
          message: { role: 'assistant', content },
          finish_reason: 'stop',
        },
      ],
      usage,
    };
  }

  private resolveNonStreamCompletion(request: ChatCompletionRequest): ChatCompletionResponse {
    const messages = request.messages;
    const prompt = lastUserPrompt(messages);
    this.throwIfError(prompt);

    const hit = this.firstMatch(prompt, this.chatMocks);
    if (hit) {
      return this.makeCompletion(hit.content, hit.model, messages);
    }

    if (this.defaultContent !== null) {
      return this.makeCompletion(this.defaultContent, this.defaultModel, messages);
    }

    throw new AegisGateAPIError(
      `未匹配的 prompt，请使用 mockChat / setDefaultResponse: ${JSON.stringify(prompt)}`,
      {
        statusCode: 404,
        errorCode: 'MOCK_UNMATCHED',
        aegisCode: 'MOCK_UNMATCHED',
      }
    );
  }

  async chat(userMessage: string, options: ChatOptions = {}): Promise<ChatResult> {
    this._calls.push({
      method: 'chat',
      userMessage,
      model: options.model,
      temperature: options.temperature,
      maxTokens: options.maxTokens,
      systemPrompt: options.systemPrompt,
    });

    const messages: ChatMessage[] = [];
    if (options.systemPrompt) {
      messages.push({ role: 'system', content: options.systemPrompt });
    }
    messages.push({ role: 'user', content: userMessage });

    const response = this.resolveNonStreamCompletion({
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
    this._calls.push({ method: 'chatCompletions', request: { ...request, stream: false } });
    return this.resolveNonStreamCompletion(request);
  }

  async *chatCompletionsStream(
    request: ChatCompletionRequest
  ): AsyncGenerator<ChatCompletionChunk, void, undefined> {
    this._calls.push({ method: 'chatCompletionsStream', request: { ...request, stream: true } });

    const messages = request.messages;
    const prompt = lastUserPrompt(messages);
    this.throwIfError(prompt);

    const sHit = this.firstMatch(prompt, this.streamMocks);
    let chunks: string[];
    let mdl: string;

    if (sHit) {
      chunks = sHit.chunks;
      mdl = sHit.model;
    } else {
      const cHit = this.firstMatch(prompt, this.chatMocks);
      if (cHit) {
        chunks = [cHit.content];
        mdl = cHit.model;
      } else if (this.defaultContent !== null) {
        chunks = [this.defaultContent];
        mdl = this.defaultModel;
      } else {
        throw new AegisGateAPIError(`未匹配的流式 prompt: ${JSON.stringify(prompt)}`, {
          statusCode: 404,
          errorCode: 'MOCK_UNMATCHED',
          aegisCode: 'MOCK_UNMATCHED',
        });
      }
    }

    const id = nextCompletionId();
    const n = chunks.length;
    for (let i = 0; i < n; i++) {
      const text = chunks[i]!;
      yield {
        id,
        object: 'chat.completion.chunk',
        model: mdl,
        choices: [
          {
            index: 0,
            delta: { content: text },
            finish_reason: i === n - 1 ? 'stop' : null,
          },
        ],
      };
    }
  }

  async listModels(): Promise<ModelListResponse> {
    this._calls.push({ method: 'listModels' });
    return {
      object: 'list',
      data: [{ id: 'mock-model', object: 'model', owned_by: 'aegisgate-mock' }],
    };
  }

  async health(): Promise<HealthResponse> {
    this._calls.push({ method: 'health' });
    return { status: 'ok', version: 'mock' };
  }
}
