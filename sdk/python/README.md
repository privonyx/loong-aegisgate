# AegisGate Python SDK

Official Python SDK for the AegisGate AI Gateway, providing both synchronous and asynchronous clients with support for Chat Completions, model listing, health checks, metrics, and config reload.

## Installation

```bash
# From PyPI (recommended)
pip install aegisgate

# Or using uv
uv pip install aegisgate

# Or from source (development mode)
pip install -e /path/to/loong-aegisgate/sdk/python
```

> Current version: `0.1.0` (Beta). MVP release; API may evolve before `1.0.0`.

## Quick Start

### Synchronous Client

```python
from aegisgate import AegisGateClient

client = AegisGateClient(api_key="sk-xxx")
response = client.chat("Hello, how are you?", model="gpt-4o")
print(response.content)
```

### Asynchronous Client

```python
import asyncio
from aegisgate import AsyncAegisGateClient

async def main():
    async with AsyncAegisGateClient(api_key="sk-xxx") as client:
        response = await client.chat("Hello!", model="gpt-4o")
        print(response.content)

asyncio.run(main())
```

## Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `api_key` | `str \| None` | `None` | API key (some endpoints like `/health` work without it) |
| `base_url` | `str` | `http://localhost:8080` | Gateway base URL |
| `timeout` | `float` | `60.0` | Overall request timeout in seconds |
| `connect_timeout` | `float \| None` | `None` | TCP connect timeout (overrides `timeout` for connect phase) |
| `read_timeout` | `float \| None` | `None` | Read timeout (overrides `timeout` for response read) |
| `max_retries` | `int` | `3` | Max retries on connection failure / retryable status codes |
| `retry_delay` | `float` | `1.0` | Base retry delay in seconds (exponential backoff) |
| `retry_jitter` | `bool` | `True` | Add random jitter to retry delay |
| `retry_on_status` | `frozenset[int] \| None` | `{429,500,502,503,504}` | HTTP status codes to retry |
| `trace_id` | `str \| None` | `None` | Static trace ID, injected as `X-Trace-Id` header |
| `trace_headers` | `dict[str, str] \| None` | `None` | Custom tracing headers (e.g. W3C `traceparent`) |
| `default_headers` | `dict[str, str] \| None` | `None` | Extra headers to include in every request |
| `pool_max_connections` | `int \| None` | `100` | Max total connections in the pool |
| `pool_max_keepalive` | `int \| None` | `20` | Max keep-alive connections in the pool |

```python
client = AegisGateClient(
    api_key="sk-xxx",
    base_url="http://localhost:8080",
    timeout=30.0,
    connect_timeout=5.0,
    read_timeout=120.0,
    max_retries=5,
    retry_jitter=True,
    pool_max_connections=200,
    pool_max_keepalive=50,
    trace_id="req-001",
    trace_headers={"traceparent": "00-abc-def-01"},
)
```

### Distributed Tracing

Inject W3C TraceContext or custom tracing headers into every request:

```python
client = AegisGateClient(
    api_key="sk-xxx",
    trace_headers={
        "traceparent": "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "tracestate": "rojo=00f067aa0ba902b7",
    },
)
```

### Connection Pool

The SDK reuses HTTP connections via `httpx.Limits`. Use `with` / `async with` for proper cleanup:

```python
with AegisGateClient(api_key="sk-xxx", pool_max_connections=100) as client:
    response = client.chat("Hello")
```

## API Reference

### Chat Completions

**Simple chat (convenience method)**

```python
response = client.chat(
    "Tell me about yourself",
    model="gpt-4o",
    system="You are a helpful assistant.",
    temperature=0.7,
    max_tokens=1000,
)
print(response.content)
print(response.usage.total_tokens)
```

**Full Chat Completions API**

```python
from aegisgate import AegisGateClient, Message

client = AegisGateClient(api_key="sk-xxx")
response = client.chat_completions(
    messages=[
        Message(role="system", content="You are an assistant."),
        Message(role="user", content="1+1=?")
    ],
    model="gpt-4o",
    temperature=0.7,
    max_tokens=1000,
)
print(response.content)
```

**Streaming**

```python
for chunk in client.stream_chat("Tell me a short story", model="gpt-4o"):
    for choice in chunk.choices:
        delta = choice.get("delta", {})
        if "content" in delta:
            print(delta["content"], end="", flush=True)
```

### List Models

```python
models = client.models()
for m in models.data:
    print(m.id, m.owned_by)
```

### Health Check

```python
health = client.health()
print(health.status, health.version)  # ok 0.1.0
```

### Prometheus Metrics

```python
metrics_text = client.metrics()
print(metrics_text)
```

### Reload Config (Admin)

```python
result = client.reload()
print(result.status, result.message)
```

## Error Handling

```python
from aegisgate import (
    AegisGateClient,
    AegisGateAPIError,
    AegisGateAuthenticationError,
    AegisGateRateLimitError,
    AegisGateConnectionError,
    AegisGateTimeoutError,
)

client = AegisGateClient(api_key="sk-xxx")

try:
    response = client.chat("Hello")
except AegisGateAuthenticationError:
    print("Authentication failed — check your api_key")
except AegisGateRateLimitError:
    print("Rate limited — try again later")
except AegisGateTimeoutError:
    print("Request timed out")
except AegisGateConnectionError:
    print("Connection failed")
except AegisGateAPIError as e:
    print(f"API error: {e.message}, status: {e.status_code}")
```

## Context Manager

The synchronous client supports `with` statements for automatic cleanup:

```python
with AegisGateClient(api_key="sk-xxx") as client:
    response = client.chat("Hello")
    print(response.content)
```

The async client supports `async with`:

```python
async with AsyncAegisGateClient(api_key="sk-xxx") as client:
    response = await client.chat("Hello")
    print(response.content)
```

## Requirements

- Python >= 3.9
- httpx >= 0.24

## License

[Apache License 2.0](../../LICENSE)
