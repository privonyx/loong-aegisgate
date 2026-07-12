# @aegisgate/sdk

Official Node.js/TypeScript SDK for the AegisGate AI Gateway, compatible with the OpenAI API format.

## Requirements

- Node.js >= 18.0.0 (uses native `fetch`)
- TypeScript 5.x (optional, for type support)

## Installation

```bash
npm install @aegisgate/sdk
```

> Current version: `0.1.0` (Beta). MVP release; API may evolve before `1.0.0`.

## Quick Start

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'sk-xxx' });
const response = await client.chat('Hello!', { model: 'gpt-4o' });
console.log(response.content);
```

## Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `apiKey` | `string` | required | API key |
| `baseUrl` | `string` | `http://localhost:8080` | Gateway base URL |
| `timeout` | `number` | `60000` | Request timeout in milliseconds |
| `maxRetries` | `number` | `2` | Max retry attempts |
| `retryDelay` | `number` | `1000` | Base retry delay in ms (exponential backoff) |
| `retryJitter` | `boolean` | `true` | Add random jitter to retry delay |
| `retryOnStatus` | `number[]` | `[429,500,502,503,504]` | HTTP status codes to retry |
| `traceId` | `string` | `undefined` | Static trace ID, injected as `X-Trace-Id` header |
| `traceHeaders` | `Record<string,string>` | `undefined` | Custom tracing headers (e.g. W3C `traceparent`) |
| `defaultHeaders` | `Record<string,string>` | `undefined` | Extra headers to include in every request |

```typescript
const client = new AegisGateClient({
  apiKey: 'sk-xxx',
  baseUrl: 'https://gateway.example.com',
  timeout: 30000,
  maxRetries: 3,
  retryJitter: true,
  retryOnStatus: [429, 502, 503],
  traceId: 'req-001',
  traceHeaders: { traceparent: '00-abc-def-01' },
});
```

### Distributed Tracing

Inject W3C TraceContext or custom tracing headers into every request:

```typescript
const client = new AegisGateClient({
  apiKey: 'sk-xxx',
  traceHeaders: {
    traceparent: '00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01',
    tracestate: 'rojo=00f067aa0ba902b7',
  },
});
```

## API Methods

### Simple Chat — `chat()`

Single-turn conversation, returns the assistant's reply directly:

```typescript
const result = await client.chat('Tell me about yourself', {
  model: 'gpt-4o',
  temperature: 0.7,
  maxTokens: 1000,
  systemPrompt: 'You are a helpful assistant.',
});
console.log(result.content);
console.log(result.usage);  // { prompt_tokens, completion_tokens, total_tokens }
```

### Chat Completions (Non-streaming)

Full OpenAI-compatible interface:

```typescript
const response = await client.chatCompletions({
  model: 'gpt-4o',
  messages: [
    { role: 'system', content: 'You are an assistant' },
    { role: 'user', content: 'Hello' },
  ],
  temperature: 0.7,
  max_tokens: 1000,
});
console.log(response.choices[0].message.content);
```

### Chat Completions (Streaming)

Receive response chunks via async iterator:

```typescript
for await (const chunk of client.chatCompletionsStream({
  model: 'gpt-4o',
  messages: [{ role: 'user', content: 'Write a poem' }],
  stream: true,
})) {
  const content = chunk.choices[0]?.delta?.content ?? '';
  if (content) process.stdout.write(content);
}
```

### List Models — `listModels()`

```typescript
const { data } = await client.listModels();
data.forEach((m) => console.log(m.id, m.owned_by));
```

### Health Check — `health()`

No authentication required:

```typescript
const { status, version } = await client.health();
console.log(`${status} - v${version}`);
```

### Metrics — `metrics()`

Retrieve Prometheus-format metrics (requires authentication):

```typescript
const metrics = await client.metrics();
console.log(metrics);
```

### Reload Config — `reloadConfig()`

Reload gateway configuration (requires authentication):

```typescript
await client.reloadConfig();
```

## Error Handling

The SDK provides the following custom error classes:

| Error Class | Description |
|-------------|-------------|
| `AegisGateError` | Base error |
| `AegisGateAPIError` | API request failed (includes statusCode, responseBody) |
| `AegisGateAuthenticationError` | 401 authentication failure |
| `AegisGateRateLimitError` | 429 rate limit exceeded |
| `AegisGateConnectionError` | Network connection error |
| `AegisGateTimeoutError` | Request timeout |

```typescript
import { AegisGateClient, AegisGateAuthenticationError, AegisGateTimeoutError } from '@aegisgate/sdk';

try {
  const result = await client.chat('Hello');
  console.log(result.content);
} catch (err) {
  if (err instanceof AegisGateAuthenticationError) {
    console.error('Invalid API Key');
  } else if (err instanceof AegisGateTimeoutError) {
    console.error('Request timed out');
  } else {
    throw err;
  }
}
```

## Module Format

This package uses ESM. Set `"type": "module"` in your `package.json`, or use the `.mjs` extension.

## License

[Apache License 2.0](../../LICENSE)
