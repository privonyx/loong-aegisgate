# AegisGate SDK Integration Guide

This guide demonstrates how to integrate AegisGate into your applications using the official client SDKs. Each SDK provides chat completions (streaming and non-streaming), model listing, health checks, metrics retrieval, and configuration reload.

## Table of Contents

- [Overview](#overview)
- [Python SDK](#python-sdk)
- [Node.js SDK](#nodejs-sdk)
- [Go SDK](#go-sdk)
- [Java / Kotlin SDK](#java--kotlin-sdk)
- [Rust SDK](#rust-sdk)
- [Error Handling Patterns](#error-handling-patterns)
- [Configuration Reference](#configuration-reference)

---

## Overview

All SDKs follow the same design principles:

- **OpenAI-compatible API**: Use the same request/response format as the OpenAI API
- **Automatic retry**: Exponential backoff with jitter for 429/5xx errors
- **Streaming support**: SSE-based streaming with per-chunk delivery
- **Connection pooling**: Reuse HTTP connections for performance
- **Distributed tracing**: Inject trace headers (W3C Traceparent, custom X-Trace-Id)
- **Type safety**: Strong typing for all request/response objects

### Common Configuration

All SDKs accept these configuration parameters:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `base_url` | `http://localhost:8080` | AegisGate server URL |
| `api_key` | — | API key for authentication |
| `timeout` | 60s | Request timeout |
| `max_retries` | 2-3 | Maximum retry attempts |
| `retry_delay` | 0.5-1s | Base delay between retries |

---

## Python SDK

### Installation

```bash
pip install aegisgate
```

The Python SDK requires `httpx` as a dependency.

### Synchronous Client

```python
from aegisgate import AegisGateClient

client = AegisGateClient(
    api_key="your-api-key",
    base_url="http://localhost:8080",
    timeout=60.0,
    max_retries=3,
)

# Simple chat
response = client.chat(
    "What is the capital of France?",
    model="gpt-4o",
    system="You are a helpful assistant.",
    temperature=0.7,
    max_tokens=1000,
)
print(response.content)

# Full chat completions API
response = client.chat_completions(
    messages=[
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": "Explain quantum computing in 3 sentences."},
    ],
    model="deepseek-chat",
    temperature=0.5,
    max_tokens=500,
)
print(response.choices[0].message.content)
print(f"Tokens used: {response.usage.total_tokens}")

# List available models
models = client.models()
for model in models.data:
    print(f"  {model.id} (owned by: {model.owned_by})")

# Health check
health = client.health()
print(f"Status: {health.status}, Version: {health.version}")

# Prometheus metrics
metrics_text = client.metrics()
print(metrics_text)

# Clean up
client.close()
```

### Context Manager

```python
from aegisgate import AegisGateClient

with AegisGateClient(api_key="your-api-key") as client:
    response = client.chat("Hello!")
    print(response.content)
```

### Async Client

```python
import asyncio
from aegisgate import AsyncAegisGateClient

async def main():
    client = AsyncAegisGateClient(
        api_key="your-api-key",
        base_url="http://localhost:8080",
    )

    # Non-streaming
    response = await client.chat(
        "What is machine learning?",
        model="gpt-4o",
    )
    print(response.content)

    # Streaming
    async for chunk in client.stream_chat(
        "Write a haiku about programming.",
        model="gpt-4o",
    ):
        for choice in chunk.choices:
            delta = choice.get("delta", {})
            if "content" in delta:
                print(delta["content"], end="", flush=True)
    print()

    await client.close()

asyncio.run(main())
```

### Async Context Manager

```python
import asyncio
from aegisgate import AsyncAegisGateClient

async def main():
    async with AsyncAegisGateClient(api_key="your-api-key") as client:
        response = await client.chat("Hello!")
        print(response.content)

asyncio.run(main())
```

### Streaming (Sync)

```python
from aegisgate import AegisGateClient

client = AegisGateClient(api_key="your-api-key")

for chunk in client.stream_chat(
    "Tell me a short story about a robot.",
    model="deepseek-chat",
):
    for choice in chunk.choices:
        delta = choice.get("delta", {})
        content = delta.get("content", "")
        if content:
            print(content, end="", flush=True)
print()

client.close()
```

### Advanced Configuration

```python
from aegisgate import AegisGateClient

client = AegisGateClient(
    api_key="your-api-key",
    base_url="https://aegisgate.example.com",
    timeout=120.0,
    connect_timeout=5.0,
    read_timeout=120.0,
    max_retries=5,
    retry_delay=2.0,
    retry_jitter=True,
    retry_on_status=frozenset({429, 500, 502, 503, 504}),
    trace_id="req-12345",
    trace_headers={"traceparent": "00-abc123-def456-01"},
    default_headers={"X-Custom-Header": "value"},
    pool_max_connections=200,
    pool_max_keepalive=50,
)
```

### Error Handling (Python)

```python
from aegisgate import (
    AegisGateClient,
    AegisGateError,
    AegisGateAPIError,
    AegisGateAuthenticationError,
    AegisGateRateLimitError,
    AegisGateSecurityError,
    AegisGateConnectionError,
    AegisGateTimeoutError,
)

client = AegisGateClient(api_key="your-api-key")

try:
    response = client.chat("Hello!")
    print(response.content)
except AegisGateAuthenticationError as e:
    print(f"Authentication failed: {e}")
except AegisGateRateLimitError as e:
    retry_after = getattr(e, "retry_after", None)
    print(f"Rate limited. Retry after: {retry_after}s")
except AegisGateTimeoutError as e:
    print(f"Request timed out: {e}")
except AegisGateConnectionError as e:
    print(f"Connection failed: {e}")
except AegisGateAPIError as e:
    print(f"API error {e.status_code}: {e}")
except AegisGateError as e:
    print(f"General error: {e}")
finally:
    client.close()
```

---

## Node.js SDK

### Installation

```bash
npm install @aegisgate/sdk
```

The Node.js SDK is written in TypeScript and uses the native `fetch` API (Node.js 18+).

### Basic Usage

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({
  apiKey: 'your-api-key',
  baseUrl: 'http://localhost:8080',
  timeout: 60_000,
  maxRetries: 2,
});

// Simple chat
const result = await client.chat('What is TypeScript?', {
  model: 'gpt-4o',
  systemPrompt: 'You are a helpful assistant.',
  temperature: 0.7,
  maxTokens: 1000,
});
console.log(result.content);
console.log(`Finish reason: ${result.finishReason}`);
console.log(`Tokens: ${result.usage?.total_tokens}`);
```

### Chat Completions

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

const response = await client.chatCompletions({
  model: 'deepseek-chat',
  messages: [
    { role: 'system', content: 'You are a concise technical writer.' },
    { role: 'user', content: 'Explain Docker in 2 sentences.' },
  ],
  temperature: 0.5,
  max_tokens: 200,
});

console.log(response.choices[0].message.content);
```

### Streaming (TypeScript)

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

const stream = client.chatCompletionsStream({
  model: 'gpt-4o',
  messages: [
    { role: 'user', content: 'Write a poem about open source software.' },
  ],
  temperature: 0.8,
  max_tokens: 500,
});

for await (const chunk of stream) {
  const delta = chunk.choices[0]?.delta;
  if (delta?.content) {
    process.stdout.write(delta.content);
  }
}
console.log();
```

### Other Operations

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

// Health check
const health = await client.health();
console.log(`Status: ${health.status}, Version: ${health.version}`);

// List models
const models = await client.listModels();
for (const model of models.data) {
  console.log(`  ${model.id}`);
}

// Prometheus metrics
const metrics = await client.metrics();
console.log(metrics);

// Reload configuration
await client.reloadConfig();
```

### Advanced Configuration (Node.js)

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({
  apiKey: 'your-api-key',
  baseUrl: 'https://aegisgate.example.com',
  timeout: 120_000,
  maxRetries: 5,
  retryDelay: 2000,
  retryJitter: true,
  retryOnStatus: [429, 500, 502, 503, 504],
  traceId: 'req-12345',
  traceHeaders: { traceparent: '00-abc123-def456-01' },
  defaultHeaders: { 'X-Custom-Header': 'value' },
});
```

### Error Handling (Node.js)

```typescript
import {
  AegisGateClient,
  AegisGateAPIError,
  AegisGateAuthenticationError,
  AegisGateRateLimitError,
  AegisGateConnectionError,
  AegisGateTimeoutError,
} from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

try {
  const result = await client.chat('Hello!');
  console.log(result.content);
} catch (err) {
  if (err instanceof AegisGateAuthenticationError) {
    console.error('Authentication failed:', err.message);
  } else if (err instanceof AegisGateRateLimitError) {
    console.error('Rate limited:', err.message);
  } else if (err instanceof AegisGateTimeoutError) {
    console.error('Timeout:', err.message);
  } else if (err instanceof AegisGateConnectionError) {
    console.error('Connection error:', err.message);
  } else if (err instanceof AegisGateAPIError) {
    console.error(`API error ${err.statusCode}:`, err.message);
  } else {
    console.error('Unexpected error:', err);
  }
}
```

---

## Go SDK

### Installation

```bash
go get github.com/privonyx/loong-aegisgate/sdk/go
```

The Go SDK has **zero external dependencies** — it uses only the Go standard library.

### Basic Usage

```go
package main

import (
	"context"
	"fmt"
	"log"
	"time"

	aegisgate "github.com/privonyx/loong-aegisgate/sdk/go"
)

func main() {
	client := aegisgate.NewClient(
		aegisgate.WithBaseURL("http://localhost:8080"),
		aegisgate.WithAPIKey("your-api-key"),
		aegisgate.WithTimeout(60*time.Second),
		aegisgate.WithMaxRetries(3),
	)
	defer client.Close()

	ctx := context.Background()

	// Non-streaming chat
	resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
		Model: "gpt-4o",
		Messages: []aegisgate.Message{
			{Role: "system", Content: "You are a helpful assistant."},
			{Role: "user", Content: "What is Go programming language?"},
		},
		Temperature: 0.7,
		MaxTokens:   1000,
	})
	if err != nil {
		log.Fatal(err)
	}

	fmt.Println(resp.Choices[0].Message.Content)
	if resp.Usage != nil {
		fmt.Printf("Tokens: %d\n", resp.Usage.TotalTokens)
	}
}
```

### Context with Timeout and Cancellation

```go
package main

import (
	"context"
	"fmt"
	"log"
	"time"

	aegisgate "github.com/privonyx/loong-aegisgate/sdk/go"
)

func main() {
	client := aegisgate.NewClient(
		aegisgate.WithAPIKey("your-api-key"),
	)
	defer client.Close()

	// Context with 30-second timeout
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
		Model: "deepseek-chat",
		Messages: []aegisgate.Message{
			{Role: "user", Content: "Hello!"},
		},
	})
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(resp.Choices[0].Message.Content)
}
```

### Streaming with Callback

```go
package main

import (
	"context"
	"fmt"
	"log"

	aegisgate "github.com/privonyx/loong-aegisgate/sdk/go"
)

func main() {
	client := aegisgate.NewClient(
		aegisgate.WithAPIKey("your-api-key"),
	)
	defer client.Close()

	ctx := context.Background()

	err := client.ChatCompletionsStream(ctx, &aegisgate.ChatCompletionsRequest{
		Model: "gpt-4o",
		Messages: []aegisgate.Message{
			{Role: "user", Content: "Write a haiku about Kubernetes."},
		},
		Temperature: 0.8,
		MaxTokens:   200,
	}, func(chunk *aegisgate.ChatCompletionChunk) error {
		for _, choice := range chunk.Choices {
			if choice.Delta.Content != "" {
				fmt.Print(choice.Delta.Content)
			}
		}
		return nil
	})
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println()
}
```

### Streaming with Channel

```go
package main

import (
	"context"
	"fmt"
	"log"

	aegisgate "github.com/privonyx/loong-aegisgate/sdk/go"
)

func main() {
	client := aegisgate.NewClient(
		aegisgate.WithAPIKey("your-api-key"),
	)
	defer client.Close()

	ctx := context.Background()

	chunkCh, errCh := client.ChatCompletionsStreamChan(ctx, &aegisgate.ChatCompletionsRequest{
		Model: "gpt-4o",
		Messages: []aegisgate.Message{
			{Role: "user", Content: "Explain microservices in 3 sentences."},
		},
	})

	for chunk := range chunkCh {
		for _, choice := range chunk.Choices {
			if choice.Delta.Content != "" {
				fmt.Print(choice.Delta.Content)
			}
		}
	}
	fmt.Println()

	if err := <-errCh; err != nil {
		log.Fatal(err)
	}
}
```

### Other Operations (Go)

```go
// Health check
health, err := client.Health(ctx)
fmt.Printf("Status: %s, Version: %s\n", health.Status, health.Version)

// List models
models, err := client.ListModels(ctx)
for _, m := range models.Data {
    fmt.Printf("  %s (owned by: %s)\n", m.ID, m.OwnedBy)
}

// Prometheus metrics
metrics, err := client.Metrics(ctx)
fmt.Println(metrics)

// Reload configuration
reload, err := client.Reload(ctx)
fmt.Printf("Reload: %s - %s\n", reload.Status, reload.Message)
```

### Advanced Configuration (Go)

```go
client := aegisgate.NewClient(
	aegisgate.WithBaseURL("https://aegisgate.example.com"),
	aegisgate.WithAPIKey("your-api-key"),
	aegisgate.WithTimeout(120*time.Second),
	aegisgate.WithConnectTimeout(5*time.Second),
	aegisgate.WithMaxRetries(5),
	aegisgate.WithRetryDelay(2*time.Second),
	aegisgate.WithRetryJitter(true),
	aegisgate.WithRetryOnStatus(429, 500, 502, 503, 504),
	aegisgate.WithTraceID("req-12345"),
	aegisgate.WithTraceHeaders(map[string]string{
		"traceparent": "00-abc123-def456-01",
	}),
	aegisgate.WithDefaultHeaders(map[string]string{
		"X-Custom-Header": "value",
	}),
	aegisgate.WithMaxIdleConns(200),
	aegisgate.WithMaxIdleConnsPerHost(50),
	aegisgate.WithIdleConnTimeout(90*time.Second),
)
```

### Error Handling (Go)

```go
import (
	"errors"
	aegisgate "github.com/privonyx/loong-aegisgate/sdk/go"
)

resp, err := client.ChatCompletions(ctx, req)
if err != nil {
	var authErr *aegisgate.AuthenticationError
	var rateLimitErr *aegisgate.RateLimitError
	var timeoutErr *aegisgate.TimeoutError
	var connErr *aegisgate.ConnectionError
	var apiErr *aegisgate.APIError

	switch {
	case errors.As(err, &authErr):
		log.Printf("Authentication failed: %s", authErr.Message)
	case errors.As(err, &rateLimitErr):
		log.Printf("Rate limited (status %d): %s", rateLimitErr.StatusCode, rateLimitErr.Message)
	case errors.As(err, &timeoutErr):
		log.Printf("Request timed out: %s", timeoutErr.Message)
	case errors.As(err, &connErr):
		log.Printf("Connection failed: %s", connErr.Message)
	case errors.As(err, &apiErr):
		log.Printf("API error %d: %s", apiErr.StatusCode, apiErr.Message)
	default:
		log.Printf("Unexpected error: %v", err)
	}
}
```

---

## Java / Kotlin SDK

> **Note**: The Java/Kotlin SDK is a community-contributed example. An official SDK is planned for a future release.

### Dependencies (Gradle)

```kotlin
// build.gradle.kts
dependencies {
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.google.code.gson:gson:2.10.1")
    // For Kotlin coroutines (optional)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.0")
}
```

### Java Client (OkHttp)

```java
import okhttp3.*;
import com.google.gson.*;
import java.io.IOException;
import java.util.concurrent.TimeUnit;

public class AegisGateClient {
    private final OkHttpClient httpClient;
    private final String baseUrl;
    private final String apiKey;
    private static final MediaType JSON = MediaType.parse("application/json");

    public AegisGateClient(String baseUrl, String apiKey) {
        this.baseUrl = baseUrl;
        this.apiKey = apiKey;
        this.httpClient = new OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .writeTimeout(10, TimeUnit.SECONDS)
            .connectionPool(new ConnectionPool(20, 5, TimeUnit.MINUTES))
            .addInterceptor(new RetryInterceptor(3))
            .build();
    }

    public JsonObject chatCompletions(String model, JsonArray messages,
                                       double temperature, int maxTokens)
            throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("model", model);
        body.add("messages", messages);
        body.addProperty("temperature", temperature);
        body.addProperty("max_tokens", maxTokens);
        body.addProperty("stream", false);

        Request request = new Request.Builder()
            .url(baseUrl + "/v1/chat/completions")
            .header("Authorization", "Bearer " + apiKey)
            .header("Content-Type", "application/json")
            .post(RequestBody.create(body.toString(), JSON))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                throw new IOException("API error: " + response.code()
                    + " " + response.body().string());
            }
            return JsonParser.parseString(response.body().string())
                .getAsJsonObject();
        }
    }

    public void streamChatCompletions(String model, JsonArray messages,
                                       StreamCallback callback)
            throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("model", model);
        body.add("messages", messages);
        body.addProperty("stream", true);

        Request request = new Request.Builder()
            .url(baseUrl + "/v1/chat/completions")
            .header("Authorization", "Bearer " + apiKey)
            .header("Content-Type", "application/json")
            .post(RequestBody.create(body.toString(), JSON))
            .build();

        try (Response response = httpClient.newCall(request).execute()) {
            if (!response.isSuccessful()) {
                throw new IOException("API error: " + response.code());
            }

            BufferedSource source = response.body().source();
            while (!source.exhausted()) {
                String line = source.readUtf8Line();
                if (line == null) break;
                if (line.startsWith("data: ")) {
                    String data = line.substring(6);
                    if (data.equals("[DONE]")) break;
                    JsonObject chunk = JsonParser.parseString(data)
                        .getAsJsonObject();
                    callback.onChunk(chunk);
                }
            }
        }
    }

    @FunctionalInterface
    public interface StreamCallback {
        void onChunk(JsonObject chunk);
    }

    public void close() {
        httpClient.dispatcher().executorService().shutdown();
        httpClient.connectionPool().evictAll();
    }
}
```

### Java Usage Example

```java
public class Main {
    public static void main(String[] args) throws Exception {
        AegisGateClient client = new AegisGateClient(
            "http://localhost:8080", "your-api-key");

        // Build messages
        JsonArray messages = new JsonArray();
        JsonObject msg = new JsonObject();
        msg.addProperty("role", "user");
        msg.addProperty("content", "What is Java?");
        messages.add(msg);

        // Non-streaming
        JsonObject response = client.chatCompletions(
            "gpt-4o", messages, 0.7, 1000);
        String content = response.getAsJsonArray("choices")
            .get(0).getAsJsonObject()
            .getAsJsonObject("message")
            .get("content").getAsString();
        System.out.println(content);

        // Streaming
        client.streamChatCompletions("gpt-4o", messages, chunk -> {
            JsonArray choices = chunk.getAsJsonArray("choices");
            if (choices.size() > 0) {
                JsonObject delta = choices.get(0).getAsJsonObject()
                    .getAsJsonObject("delta");
                if (delta.has("content")) {
                    System.out.print(delta.get("content").getAsString());
                }
            }
        });
        System.out.println();

        client.close();
    }
}
```

### Kotlin Coroutines with Flow

```kotlin
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import okhttp3.*
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import com.google.gson.*
import java.util.concurrent.TimeUnit

class AegisGateClient(
    private val baseUrl: String,
    private val apiKey: String,
) {
    private val json = "application/json".toMediaType()
    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(120, TimeUnit.SECONDS)
        .connectionPool(ConnectionPool(20, 5, TimeUnit.MINUTES))
        .build()

    suspend fun chatCompletions(
        model: String,
        messages: List<Map<String, String>>,
        temperature: Double = 0.7,
        maxTokens: Int = 1000,
    ): JsonObject = withContext(Dispatchers.IO) {
        val body = JsonObject().apply {
            addProperty("model", model)
            add("messages", Gson().toJsonTree(messages))
            addProperty("temperature", temperature)
            addProperty("max_tokens", maxTokens)
            addProperty("stream", false)
        }

        val request = Request.Builder()
            .url("$baseUrl/v1/chat/completions")
            .header("Authorization", "Bearer $apiKey")
            .post(body.toString().toRequestBody(json))
            .build()

        val response = httpClient.newCall(request).execute()
        if (!response.isSuccessful) {
            throw RuntimeException("API error: ${response.code}")
        }
        JsonParser.parseString(response.body!!.string()).asJsonObject
    }

    fun streamChatCompletions(
        model: String,
        messages: List<Map<String, String>>,
        temperature: Double = 0.7,
        maxTokens: Int = 1000,
    ): Flow<JsonObject> = flow {
        val body = JsonObject().apply {
            addProperty("model", model)
            add("messages", Gson().toJsonTree(messages))
            addProperty("temperature", temperature)
            addProperty("max_tokens", maxTokens)
            addProperty("stream", true)
        }

        val request = Request.Builder()
            .url("$baseUrl/v1/chat/completions")
            .header("Authorization", "Bearer $apiKey")
            .post(body.toString().toRequestBody(json))
            .build()

        val response = withContext(Dispatchers.IO) {
            httpClient.newCall(request).execute()
        }

        if (!response.isSuccessful) {
            throw RuntimeException("API error: ${response.code}")
        }

        val source = response.body!!.source()
        while (!source.exhausted()) {
            val line = withContext(Dispatchers.IO) { source.readUtf8Line() }
                ?: break
            if (line.startsWith("data: ")) {
                val data = line.removePrefix("data: ")
                if (data == "[DONE]") break
                emit(JsonParser.parseString(data).asJsonObject)
            }
        }
    }.flowOn(Dispatchers.IO)

    fun close() {
        httpClient.dispatcher.executorService.shutdown()
        httpClient.connectionPool.evictAll()
    }
}

// Usage
fun main() = runBlocking {
    val client = AegisGateClient("http://localhost:8080", "your-api-key")

    // Non-streaming
    val response = client.chatCompletions(
        model = "gpt-4o",
        messages = listOf(mapOf("role" to "user", "content" to "Hello from Kotlin!"))
    )
    println(response["choices"].asJsonArray[0].asJsonObject["message"]
        .asJsonObject["content"].asString)

    // Streaming with Flow
    client.streamChatCompletions(
        model = "gpt-4o",
        messages = listOf(mapOf("role" to "user", "content" to "Write a limerick."))
    ).collect { chunk ->
        val choices = chunk["choices"].asJsonArray
        if (choices.size() > 0) {
            val delta = choices[0].asJsonObject["delta"].asJsonObject
            if (delta.has("content")) {
                print(delta["content"].asString)
            }
        }
    }
    println()

    client.close()
}
```

---

## Rust SDK

> **Note**: The Rust SDK is a community-contributed example. An official SDK is planned for a future release.

### Dependencies (Cargo.toml)

```toml
[dependencies]
reqwest = { version = "0.12", features = ["json", "stream"] }
tokio = { version = "1", features = ["full"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
futures = "0.3"
```

### Types

```rust
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize)]
pub struct ChatCompletionsRequest {
    pub model: String,
    pub messages: Vec<Message>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stream: Option<bool>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Message {
    pub role: String,
    pub content: String,
}

#[derive(Debug, Deserialize)]
pub struct ChatCompletionsResponse {
    pub id: String,
    pub model: String,
    pub choices: Vec<Choice>,
    pub usage: Option<Usage>,
}

#[derive(Debug, Deserialize)]
pub struct Choice {
    pub index: usize,
    pub message: Message,
    pub finish_reason: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct Usage {
    pub prompt_tokens: u32,
    pub completion_tokens: u32,
    pub total_tokens: u32,
}

#[derive(Debug, Deserialize)]
pub struct ChatCompletionChunk {
    pub id: String,
    pub model: String,
    pub choices: Vec<ChunkChoice>,
}

#[derive(Debug, Deserialize)]
pub struct ChunkChoice {
    pub index: usize,
    pub delta: Delta,
    pub finish_reason: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct Delta {
    pub role: Option<String>,
    pub content: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct HealthResponse {
    pub status: String,
    pub version: String,
}
```

### Client Implementation

```rust
use reqwest::Client;

pub struct AegisGateClient {
    client: Client,
    base_url: String,
    api_key: String,
}

impl AegisGateClient {
    pub fn new(base_url: &str, api_key: &str) -> Self {
        let client = Client::builder()
            .timeout(std::time::Duration::from_secs(60))
            .pool_max_idle_per_host(20)
            .build()
            .expect("Failed to create HTTP client");

        Self {
            client,
            base_url: base_url.trim_end_matches('/').to_string(),
            api_key: api_key.to_string(),
        }
    }

    pub async fn chat_completions(
        &self,
        request: &ChatCompletionsRequest,
    ) -> Result<ChatCompletionsResponse, Box<dyn std::error::Error>> {
        let mut req = request.clone_with_stream(false);
        let response = self
            .client
            .post(format!("{}/v1/chat/completions", self.base_url))
            .header("Authorization", format!("Bearer {}", self.api_key))
            .json(&req)
            .send()
            .await?;

        if !response.status().is_success() {
            let status = response.status();
            let body = response.text().await.unwrap_or_default();
            return Err(format!("API error {}: {}", status, body).into());
        }

        Ok(response.json().await?)
    }

    pub async fn health(&self) -> Result<HealthResponse, Box<dyn std::error::Error>> {
        let response = self
            .client
            .get(format!("{}/health", self.base_url))
            .send()
            .await?;
        Ok(response.json().await?)
    }
}
```

### Streaming with Tokio

```rust
use futures::StreamExt;
use reqwest::Client;

impl AegisGateClient {
    pub async fn stream_chat_completions(
        &self,
        request: &ChatCompletionsRequest,
    ) -> Result<
        impl futures::Stream<Item = Result<ChatCompletionChunk, Box<dyn std::error::Error>>>,
        Box<dyn std::error::Error>,
    > {
        let mut req_body = serde_json::to_value(request)?;
        req_body["stream"] = serde_json::Value::Bool(true);

        let response = self
            .client
            .post(format!("{}/v1/chat/completions", self.base_url))
            .header("Authorization", format!("Bearer {}", self.api_key))
            .json(&req_body)
            .send()
            .await?;

        if !response.status().is_success() {
            let status = response.status();
            let body = response.text().await.unwrap_or_default();
            return Err(format!("API error {}: {}", status, body).into());
        }

        let stream = response.bytes_stream();
        let mut buffer = String::new();

        Ok(futures::stream::unfold(
            (stream, buffer),
            |(mut stream, mut buffer)| async move {
                loop {
                    if let Some(newline_pos) = buffer.find('\n') {
                        let line = buffer[..newline_pos].to_string();
                        buffer = buffer[newline_pos + 1..].to_string();

                        if line.starts_with("data: ") {
                            let data = &line[6..];
                            if data == "[DONE]" {
                                return None;
                            }
                            match serde_json::from_str::<ChatCompletionChunk>(data) {
                                Ok(chunk) => return Some((Ok(chunk), (stream, buffer))),
                                Err(e) => continue,
                            }
                        }
                        continue;
                    }

                    match stream.next().await {
                        Some(Ok(bytes)) => {
                            buffer.push_str(&String::from_utf8_lossy(&bytes));
                        }
                        Some(Err(e)) => {
                            return Some((Err(Box::new(e) as Box<dyn std::error::Error>), (stream, buffer)));
                        }
                        None => return None,
                    }
                }
            },
        ))
    }
}
```

### Rust Usage Example

```rust
use futures::StreamExt;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client = AegisGateClient::new("http://localhost:8080", "your-api-key");

    // Health check
    let health = client.health().await?;
    println!("Status: {}, Version: {}", health.status, health.version);

    // Non-streaming
    let request = ChatCompletionsRequest {
        model: "gpt-4o".to_string(),
        messages: vec![
            Message { role: "user".to_string(), content: "Hello from Rust!".to_string() },
        ],
        temperature: Some(0.7),
        max_tokens: Some(1000),
        stream: None,
    };

    let response = client.chat_completions(&request).await?;
    println!("{}", response.choices[0].message.content);

    // Streaming
    let stream_request = ChatCompletionsRequest {
        model: "gpt-4o".to_string(),
        messages: vec![
            Message { role: "user".to_string(), content: "Write a poem about Rust.".to_string() },
        ],
        temperature: Some(0.8),
        max_tokens: Some(500),
        stream: Some(true),
    };

    let mut stream = client.stream_chat_completions(&stream_request).await?;
    while let Some(result) = stream.next().await {
        match result {
            Ok(chunk) => {
                for choice in &chunk.choices {
                    if let Some(content) = &choice.delta.content {
                        print!("{}", content);
                    }
                }
            }
            Err(e) => eprintln!("Stream error: {}", e),
        }
    }
    println!();

    Ok(())
}
```

---

## Error Handling Patterns

### Error Hierarchy

All SDKs implement a consistent error hierarchy:

```
AegisGateError (base)
├── ConnectionError          — Network connection failure
├── TimeoutError             — Request timeout
├── APIError                 — HTTP error response
│   ├── AuthenticationError  — 401 Unauthorized
│   ├── ForbiddenError       — 403 Forbidden
│   ├── RateLimitError       — 429 Too Many Requests
│   ├── SecurityError        — Guardrail block (varies by SDK)
│   ├── BadGatewayError      — 502 Bad Gateway
│   └── ServiceUnavailableError — 503 Service Unavailable
```

### Retry Strategy

All SDKs implement the same retry strategy:

1. **Retryable errors**: 429, 500, 502, 503, 504 status codes; connection errors; timeouts
2. **Non-retryable errors**: 400, 401, 403, 404 (except rate limits)
3. **Backoff formula**: `base_delay * 2^attempt * (0.5 + random() * 0.5)`
4. **Retry-After header**: Respected when present (overrides calculated backoff)

### Best Practices

- Always set appropriate timeouts (streaming requests need longer read timeouts)
- Handle `RateLimitError` specifically to implement backpressure
- Use context/cancellation for graceful shutdown
- Close clients when done to release connection pool resources
- Use connection pooling (all SDKs enable this by default)

---

## Configuration Reference

### Environment Variables

| Variable | Description |
|----------|-------------|
| `AEGISGATE_API_KEY` | Default API key for gateway authentication |
| `AEGISGATE_URL` | Default base URL for the gateway |

### AegisGate Response Headers

| Header | Description |
|--------|-------------|
| `X-AegisGate-Tokens-Saved` | Number of tokens saved by prompt optimization |
| `X-AegisGate-Cache-Hit` | `true` if response was served from semantic cache |
| `X-AegisGate-Request-Id` | Unique request identifier for debugging |

### SSE Metadata Events (Streaming)

Before the `[DONE]` event, AegisGate sends a metadata SSE event containing:

```json
{
  "aegisgate": {
    "tokens_saved": 42,
    "cache_hit": false,
    "usage": {
      "prompt_tokens": 150,
      "completion_tokens": 200,
      "total_tokens": 350
    }
  }
}
```

SDKs expose this as a regular chunk — check for the `aegisgate` field to identify metadata events.
