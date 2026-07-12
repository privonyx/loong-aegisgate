# AegisGate SDK 集成指南

本指南演示如何使用官方客户端 SDK 将 AegisGate 集成到你的应用程序中。每个 SDK 都提供聊天补全（流式和非流式）、模型列表、健康检查、指标获取和配置重载功能。

## 目录

- [概述](#概述)
- [Python SDK](#python-sdk)
- [Node.js SDK](#nodejs-sdk)
- [Go SDK](#go-sdk)
- [Java / Kotlin SDK](#java--kotlin-sdk)
- [Rust SDK](#rust-sdk)
- [错误处理模式](#错误处理模式)
- [配置参考](#配置参考)

---

## 概述

所有 SDK 遵循相同的设计原则：

- **OpenAI 兼容 API**：使用与 OpenAI API 相同的请求/响应格式
- **自动重试**：针对 429/5xx 错误的指数退避 + 随机抖动
- **流式支持**：基于 SSE 的流式传输，逐块交付
- **连接池复用**：复用 HTTP 连接以提升性能
- **分布式追踪**：注入追踪头（W3C Traceparent、自定义 X-Trace-Id）
- **类型安全**：所有请求/响应对象均有强类型定义

### 通用配置

所有 SDK 接受以下配置参数：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `base_url` | `http://localhost:8080` | AegisGate 服务器地址 |
| `api_key` | — | 认证 API 密钥 |
| `timeout` | 60s | 请求超时时间 |
| `max_retries` | 2-3 | 最大重试次数 |
| `retry_delay` | 0.5-1s | 重试间隔基础延迟 |

---

## Python SDK

### 安装

```bash
pip install aegisgate
```

Python SDK 依赖 `httpx`。

### 同步客户端

```python
from aegisgate import AegisGateClient

client = AegisGateClient(
    api_key="your-api-key",
    base_url="http://localhost:8080",
    timeout=60.0,
    max_retries=3,
)

# 简单对话
response = client.chat(
    "法国的首都是哪里？",
    model="gpt-4o",
    system="你是一个有用的助手。",
    temperature=0.7,
    max_tokens=1000,
)
print(response.content)

# 完整的聊天补全 API
response = client.chat_completions(
    messages=[
        {"role": "system", "content": "你是一个有用的助手。"},
        {"role": "user", "content": "用 3 句话解释量子计算。"},
    ],
    model="deepseek-chat",
    temperature=0.5,
    max_tokens=500,
)
print(response.choices[0].message.content)
print(f"Token 用量: {response.usage.total_tokens}")

# 列出可用模型
models = client.models()
for model in models.data:
    print(f"  {model.id} (提供者: {model.owned_by})")

# 健康检查
health = client.health()
print(f"状态: {health.status}, 版本: {health.version}")

# Prometheus 指标
metrics_text = client.metrics()
print(metrics_text)

# 清理
client.close()
```

### 上下文管理器

```python
from aegisgate import AegisGateClient

with AegisGateClient(api_key="your-api-key") as client:
    response = client.chat("你好！")
    print(response.content)
```

### 异步客户端

```python
import asyncio
from aegisgate import AsyncAegisGateClient

async def main():
    client = AsyncAegisGateClient(
        api_key="your-api-key",
        base_url="http://localhost:8080",
    )

    # 非流式
    response = await client.chat(
        "什么是机器学习？",
        model="gpt-4o",
    )
    print(response.content)

    # 流式
    async for chunk in client.stream_chat(
        "写一首关于编程的俳句。",
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

### 异步上下文管理器

```python
import asyncio
from aegisgate import AsyncAegisGateClient

async def main():
    async with AsyncAegisGateClient(api_key="your-api-key") as client:
        response = await client.chat("你好！")
        print(response.content)

asyncio.run(main())
```

### 流式传输（同步）

```python
from aegisgate import AegisGateClient

client = AegisGateClient(api_key="your-api-key")

for chunk in client.stream_chat(
    "讲一个关于机器人的短故事。",
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

### 高级配置

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

### 错误处理（Python）

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
    response = client.chat("你好！")
    print(response.content)
except AegisGateAuthenticationError as e:
    print(f"认证失败: {e}")
except AegisGateRateLimitError as e:
    retry_after = getattr(e, "retry_after", None)
    print(f"请求频率超限，请在 {retry_after} 秒后重试")
except AegisGateTimeoutError as e:
    print(f"请求超时: {e}")
except AegisGateConnectionError as e:
    print(f"连接失败: {e}")
except AegisGateAPIError as e:
    print(f"API 错误 {e.status_code}: {e}")
except AegisGateError as e:
    print(f"通用错误: {e}")
finally:
    client.close()
```

---

## Node.js SDK

### 安装

```bash
npm install @aegisgate/sdk
```

Node.js SDK 使用 TypeScript 编写，基于原生 `fetch` API（需要 Node.js 18+）。

### 基本用法

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({
  apiKey: 'your-api-key',
  baseUrl: 'http://localhost:8080',
  timeout: 60_000,
  maxRetries: 2,
});

// 简单对话
const result = await client.chat('TypeScript 是什么？', {
  model: 'gpt-4o',
  systemPrompt: '你是一个有用的助手。',
  temperature: 0.7,
  maxTokens: 1000,
});
console.log(result.content);
console.log(`结束原因: ${result.finishReason}`);
console.log(`Token 用量: ${result.usage?.total_tokens}`);
```

### 聊天补全

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

const response = await client.chatCompletions({
  model: 'deepseek-chat',
  messages: [
    { role: 'system', content: '你是一个简洁的技术文档撰写者。' },
    { role: 'user', content: '用 2 句话解释 Docker。' },
  ],
  temperature: 0.5,
  max_tokens: 200,
});

console.log(response.choices[0].message.content);
```

### 流式传输（TypeScript）

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

const stream = client.chatCompletionsStream({
  model: 'gpt-4o',
  messages: [
    { role: 'user', content: '写一首关于开源软件的诗。' },
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

### 其他操作

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'your-api-key' });

// 健康检查
const health = await client.health();
console.log(`状态: ${health.status}, 版本: ${health.version}`);

// 列出模型
const models = await client.listModels();
for (const model of models.data) {
  console.log(`  ${model.id}`);
}

// Prometheus 指标
const metrics = await client.metrics();
console.log(metrics);

// 重载配置
await client.reloadConfig();
```

### 高级配置（Node.js）

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

### 错误处理（Node.js）

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
  const result = await client.chat('你好！');
  console.log(result.content);
} catch (err) {
  if (err instanceof AegisGateAuthenticationError) {
    console.error('认证失败:', err.message);
  } else if (err instanceof AegisGateRateLimitError) {
    console.error('请求频率超限:', err.message);
  } else if (err instanceof AegisGateTimeoutError) {
    console.error('请求超时:', err.message);
  } else if (err instanceof AegisGateConnectionError) {
    console.error('连接错误:', err.message);
  } else if (err instanceof AegisGateAPIError) {
    console.error(`API 错误 ${err.statusCode}:`, err.message);
  } else {
    console.error('意外错误:', err);
  }
}
```

---

## Go SDK

### 安装

```bash
go get github.com/privonyx/loong-aegisgate/sdk/go
```

Go SDK **零外部依赖** — 仅使用 Go 标准库。

### 基本用法

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

	// 非流式聊天
	resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
		Model: "gpt-4o",
		Messages: []aegisgate.Message{
			{Role: "system", Content: "你是一个有用的助手。"},
			{Role: "user", Content: "Go 编程语言是什么？"},
		},
		Temperature: 0.7,
		MaxTokens:   1000,
	})
	if err != nil {
		log.Fatal(err)
	}

	fmt.Println(resp.Choices[0].Message.Content)
	if resp.Usage != nil {
		fmt.Printf("Token 用量: %d\n", resp.Usage.TotalTokens)
	}
}
```

### 带超时和取消的上下文

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

	// 30 秒超时的上下文
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
		Model: "deepseek-chat",
		Messages: []aegisgate.Message{
			{Role: "user", Content: "你好！"},
		},
	})
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(resp.Choices[0].Message.Content)
}
```

### 流式传输（回调方式）

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
			{Role: "user", Content: "写一首关于 Kubernetes 的俳句。"},
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

### 流式传输（Channel 方式）

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
			{Role: "user", Content: "用 3 句话解释微服务。"},
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

### 其他操作（Go）

```go
// 健康检查
health, err := client.Health(ctx)
fmt.Printf("状态: %s, 版本: %s\n", health.Status, health.Version)

// 列出模型
models, err := client.ListModels(ctx)
for _, m := range models.Data {
    fmt.Printf("  %s (提供者: %s)\n", m.ID, m.OwnedBy)
}

// Prometheus 指标
metrics, err := client.Metrics(ctx)
fmt.Println(metrics)

// 重载配置
reload, err := client.Reload(ctx)
fmt.Printf("重载: %s - %s\n", reload.Status, reload.Message)
```

### 高级配置（Go）

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

### 错误处理（Go）

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
		log.Printf("认证失败: %s", authErr.Message)
	case errors.As(err, &rateLimitErr):
		log.Printf("请求频率超限 (状态码 %d): %s", rateLimitErr.StatusCode, rateLimitErr.Message)
	case errors.As(err, &timeoutErr):
		log.Printf("请求超时: %s", timeoutErr.Message)
	case errors.As(err, &connErr):
		log.Printf("连接失败: %s", connErr.Message)
	case errors.As(err, &apiErr):
		log.Printf("API 错误 %d: %s", apiErr.StatusCode, apiErr.Message)
	default:
		log.Printf("意外错误: %v", err)
	}
}
```

---

## Java / Kotlin SDK

> **注意**：Java/Kotlin SDK 为社区贡献的示例实现。官方 SDK 计划在未来版本中发布。

### 依赖（Gradle）

```kotlin
// build.gradle.kts
dependencies {
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.google.code.gson:gson:2.10.1")
    // Kotlin 协程（可选）
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.0")
}
```

### Java 客户端（OkHttp）

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
                throw new IOException("API 错误: " + response.code()
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
                throw new IOException("API 错误: " + response.code());
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

### Java 使用示例

```java
public class Main {
    public static void main(String[] args) throws Exception {
        AegisGateClient client = new AegisGateClient(
            "http://localhost:8080", "your-api-key");

        // 构建消息
        JsonArray messages = new JsonArray();
        JsonObject msg = new JsonObject();
        msg.addProperty("role", "user");
        msg.addProperty("content", "Java 是什么？");
        messages.add(msg);

        // 非流式
        JsonObject response = client.chatCompletions(
            "gpt-4o", messages, 0.7, 1000);
        String content = response.getAsJsonArray("choices")
            .get(0).getAsJsonObject()
            .getAsJsonObject("message")
            .get("content").getAsString();
        System.out.println(content);

        // 流式
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

### Kotlin 协程与 Flow

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
            throw RuntimeException("API 错误: ${response.code}")
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
            throw RuntimeException("API 错误: ${response.code}")
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

// 使用示例
fun main() = runBlocking {
    val client = AegisGateClient("http://localhost:8080", "your-api-key")

    // 非流式
    val response = client.chatCompletions(
        model = "gpt-4o",
        messages = listOf(mapOf("role" to "user", "content" to "Kotlin 你好！"))
    )
    println(response["choices"].asJsonArray[0].asJsonObject["message"]
        .asJsonObject["content"].asString)

    // 使用 Flow 流式传输
    client.streamChatCompletions(
        model = "gpt-4o",
        messages = listOf(mapOf("role" to "user", "content" to "写一首打油诗。"))
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

> **注意**：Rust SDK 为社区贡献的示例实现。官方 SDK 计划在未来版本中发布。

### 依赖（Cargo.toml）

```toml
[dependencies]
reqwest = { version = "0.12", features = ["json", "stream"] }
tokio = { version = "1", features = ["full"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
futures = "0.3"
```

### 类型定义

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

### 客户端实现

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
            .expect("创建 HTTP 客户端失败");

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
        let response = self
            .client
            .post(format!("{}/v1/chat/completions", self.base_url))
            .header("Authorization", format!("Bearer {}", self.api_key))
            .json(request)
            .send()
            .await?;

        if !response.status().is_success() {
            let status = response.status();
            let body = response.text().await.unwrap_or_default();
            return Err(format!("API 错误 {}: {}", status, body).into());
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

### 使用 Tokio 流式传输

```rust
use futures::StreamExt;

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
            return Err(format!("API 错误 {}: {}", status, body).into());
        }

        let stream = response.bytes_stream();
        let buffer = String::new();

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
                                Err(_) => continue,
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

### Rust 使用示例

```rust
use futures::StreamExt;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client = AegisGateClient::new("http://localhost:8080", "your-api-key");

    // 健康检查
    let health = client.health().await?;
    println!("状态: {}, 版本: {}", health.status, health.version);

    // 非流式
    let request = ChatCompletionsRequest {
        model: "gpt-4o".to_string(),
        messages: vec![
            Message { role: "user".to_string(), content: "Rust 你好！".to_string() },
        ],
        temperature: Some(0.7),
        max_tokens: Some(1000),
        stream: None,
    };

    let response = client.chat_completions(&request).await?;
    println!("{}", response.choices[0].message.content);

    // 流式
    let stream_request = ChatCompletionsRequest {
        model: "gpt-4o".to_string(),
        messages: vec![
            Message { role: "user".to_string(), content: "写一首关于 Rust 的诗。".to_string() },
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
            Err(e) => eprintln!("流式错误: {}", e),
        }
    }
    println!();

    Ok(())
}
```

---

## 错误处理模式

### 错误层级

所有 SDK 实现一致的错误层级结构：

```
AegisGateError（基类）
├── ConnectionError          — 网络连接失败
├── TimeoutError             — 请求超时
├── APIError                 — HTTP 错误响应
│   ├── AuthenticationError  — 401 未授权
│   ├── ForbiddenError       — 403 禁止访问
│   ├── RateLimitError       — 429 请求频率超限
│   ├── SecurityError        — 安全护栏拦截
│   ├── BadGatewayError      — 502 网关错误
│   └── ServiceUnavailableError — 503 服务不可用
```

### 重试策略

所有 SDK 实现相同的重试策略：

1. **可重试错误**：429、500、502、503、504 状态码；连接错误；超时
2. **不可重试错误**：400、401、403、404（限流除外）
3. **退避公式**：`base_delay * 2^attempt * (0.5 + random() * 0.5)`
4. **Retry-After 头**：存在时优先使用（覆盖计算的退避时间）

### 最佳实践

- 始终设置适当的超时（流式请求需要更长的读取超时）
- 特别处理 `RateLimitError` 以实现背压控制
- 使用上下文/取消机制实现优雅关闭
- 使用完毕后关闭客户端以释放连接池资源
- 使用连接池（所有 SDK 默认启用）

---

## 配置参考

### 环境变量

| 变量 | 说明 |
|------|------|
| `AEGISGATE_API_KEY` | 默认的网关认证 API 密钥 |
| `AEGISGATE_URL` | 默认的网关基础 URL |

### AegisGate 响应头

| 响应头 | 说明 |
|--------|------|
| `X-AegisGate-Tokens-Saved` | 通过提示词优化节省的 Token 数量 |
| `X-AegisGate-Cache-Hit` | 如果响应来自语义缓存则为 `true` |
| `X-AegisGate-Request-Id` | 用于调试的唯一请求标识符 |

### SSE 元数据事件（流式）

在 `[DONE]` 事件之前，AegisGate 会发送一个包含以下内容的元数据 SSE 事件：

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

SDK 会将此作为普通块暴露 — 检查 `aegisgate` 字段以识别元数据事件。
