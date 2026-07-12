import { describe, expect, it } from 'vitest';
import { AegisGateError, createAegisClient } from '../src/aegis/client';

interface CapturedRequest {
  url: string;
  init: RequestInit;
}

function fakeFetch(responder: (req: CapturedRequest) => Response): {
  fetchImpl: typeof fetch;
  calls: CapturedRequest[];
} {
  const calls: CapturedRequest[] = [];
  const fetchImpl = (async (input: string | URL, init?: RequestInit) => {
    const req = { url: String(input), init: init ?? {} };
    calls.push(req);
    return responder(req);
  }) as unknown as typeof fetch;
  return { fetchImpl, calls };
}

describe('createAegisClient.chat', () => {
  it('解析正文内容 + 从响应头抽取价值元数据', async () => {
    const { fetchImpl, calls } = fakeFetch(
      () =>
        new Response(JSON.stringify({ model: 'gpt-4o', choices: [{ message: { content: '你好' } }], usage: { total_tokens: 10 } }), {
          status: 200,
          headers: {
            'content-type': 'application/json',
            'x-aegisgate-tokens-saved': '128',
            'x-aegisgate-cache': 'hit',
            'x-aegisgate-model': 'gpt-4o-mini',
          },
        })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k-secret', fetchImpl });
    const res = await client.chat({ model: 'gpt-4o', messages: [{ role: 'user', content: 'hi' }] });

    expect(res.content).toBe('你好');
    expect(res.meta.tokensSaved).toBe(128);
    expect(res.meta.cacheHit).toBe(true);
    expect(res.meta.model).toBe('gpt-4o-mini'); // 头部优先于正文 model
    expect(res.guardrail?.blocked).toBe(false);
    // 请求落在 /v1/chat/completions
    expect(calls[0].url).toBe('http://gw/v1/chat/completions');
  });

  it('SR-1：apiKey 仅出现在 Authorization 头，不泄露到结果', async () => {
    const { fetchImpl, calls } = fakeFetch(
      () => new Response(JSON.stringify({ model: 'm', choices: [{ message: { content: 'x' } }] }), { status: 200 })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k-secret', fetchImpl });
    const res = await client.chat({ model: 'm', messages: [{ role: 'user', content: 'hi' }] });
    const headers = calls[0].init.headers as Record<string, string>;
    expect(headers.authorization).toBe('Bearer k-secret');
    expect(JSON.stringify(res)).not.toContain('k-secret');
  });

  it('解析 tool_calls', async () => {
    const { fetchImpl } = fakeFetch(
      () =>
        new Response(
          JSON.stringify({
            model: 'm',
            choices: [
              {
                message: {
                  content: null,
                  tool_calls: [{ id: 't1', type: 'function', function: { name: 'getX', arguments: '{}' } }],
                },
              },
            ],
          }),
          { status: 200 }
        )
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });
    const res = await client.chat({ model: 'm', messages: [] });
    expect(res.toolCalls).toHaveLength(1);
    expect(res.toolCalls?.[0].function.name).toBe('getX');
  });

  it('HTTP 403 → 护栏拦截（不抛错）', async () => {
    const { fetchImpl } = fakeFetch(
      () => new Response(JSON.stringify({ error: { message: '内容越界', code: 'topic_violation' } }), { status: 403 })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });
    const res = await client.chat({ model: 'm', messages: [] });
    expect(res.guardrail?.blocked).toBe(true);
    expect(res.guardrail?.reason).toBe('内容越界');
    expect(res.guardrail?.code).toBe('topic_violation');
  });

  it('错误码含 guardrail 关键字 → 护栏拦截', async () => {
    const { fetchImpl } = fakeFetch(
      () => new Response(JSON.stringify({ error: { message: 'blocked by guardrail', code: 'guardrail_injection' } }), { status: 400 })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });
    const res = await client.chat({ model: 'm', messages: [] });
    expect(res.guardrail?.blocked).toBe(true);
  });

  it('普通网关错误 → 抛 AegisGateError', async () => {
    const { fetchImpl } = fakeFetch(
      () => new Response(JSON.stringify({ error: { message: '上游超时', code: 'upstream_timeout' } }), { status: 502 })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });
    await expect(client.chat({ model: 'm', messages: [] })).rejects.toBeInstanceOf(AegisGateError);
  });

  it('checkGuardrail 包装文本并返回护栏 + 元数据', async () => {
    const { fetchImpl, calls } = fakeFetch(
      () => new Response(JSON.stringify({ error: { message: '血腥越界' } }), { status: 403 })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });
    const res = await client.checkGuardrail('血腥场面描写', 'm');
    expect(res.guardrail.blocked).toBe(true);
    const body = JSON.parse(String(calls[0].init.body));
    expect(body.messages[0].content).toBe('血腥场面描写');
  });
});

describe('createAegisClient.chatStream', () => {
  function sseResponse(body: string): Response {
    return new Response(body, {
      status: 200,
      headers: { 'content-type': 'text/event-stream' },
    });
  }

  it('逐块回调文本增量并累积内容 + 解析价值元数据事件', async () => {
    const sse =
      'data: {"model":"m","choices":[{"delta":{"content":"你"}}]}\n\n' +
      'data: {"choices":[{"delta":{"content":"好"}}]}\n\n' +
      'data: {"aegisgate":{"tokens_saved":42,"usage":{"total_tokens":7}}}\n\n' +
      'data: [DONE]\n\n';
    const { fetchImpl, calls } = fakeFetch(() => sseResponse(sse));
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });

    const tokens: string[] = [];
    const res = await client.chatStream!(
      { model: 'm', messages: [{ role: 'user', content: 'hi' }] },
      (d) => tokens.push(d.content)
    );

    expect(tokens).toEqual(['你', '好']);
    expect(res.content).toBe('你好');
    expect(res.meta.tokensSaved).toBe(42);
    expect(res.meta.model).toBe('m');
    expect(res.guardrail?.blocked).toBe(false);
    // 请求体带 stream:true
    expect(JSON.parse(String(calls[0].init.body)).stream).toBe(true);
  });

  it('跨多个 chunk 累积 tool_calls 的 arguments', async () => {
    const sse =
      'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"t1","type":"function","function":{"name":"getX","arguments":"{\\"a\\""}}]}}]}\n\n' +
      'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":":1}"}}]}}]}\n\n' +
      'data: [DONE]\n\n';
    const { fetchImpl } = fakeFetch(() => sseResponse(sse));
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });

    const res = await client.chatStream!({ model: 'm', messages: [] }, () => undefined);
    expect(res.toolCalls).toHaveLength(1);
    expect(res.toolCalls?.[0].function.name).toBe('getX');
    expect(res.toolCalls?.[0].function.arguments).toBe('{"a":1}');
  });

  it('流式 error 帧含护栏关键字 → 护栏拦截（不抛错）', async () => {
    const sse =
      'data: {"error":{"message":"Request blocked by security guardrail","code":"injection_detected"}}\n\n' +
      'data: [DONE]\n\n';
    const { fetchImpl } = fakeFetch(() => sseResponse(sse));
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });

    const res = await client.chatStream!({ model: 'm', messages: [] }, () => undefined);
    expect(res.guardrail?.blocked).toBe(true);
  });

  it('非 2xx 仍按非流式处理：普通错误抛 AegisGateError', async () => {
    const { fetchImpl } = fakeFetch(
      () => new Response(JSON.stringify({ error: { message: '上游超时', code: 'upstream_timeout' } }), { status: 502 })
    );
    const client = createAegisClient({ baseUrl: 'http://gw', apiKey: 'k', fetchImpl });
    await expect(client.chatStream!({ model: 'm', messages: [] }, () => undefined)).rejects.toBeInstanceOf(AegisGateError);
  });
});
