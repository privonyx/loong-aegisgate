# AegisGate Java/Kotlin SDK

Official Java/Kotlin SDK for the AegisGate AI Gateway, built with Kotlin coroutines, OkHttp, and Gson.

## Requirements

- JDK 17+
- Kotlin 1.9+ (for Kotlin usage) or Java 17+ (for Java usage)

## Installation

### Gradle (Kotlin DSL)

```kotlin
dependencies {
    implementation("dev.aegisgate:aegisgate-sdk:1.0.0")
}
```

### Gradle (Groovy)

```groovy
dependencies {
    implementation 'dev.aegisgate:aegisgate-sdk:1.0.0'
}
```

### Maven

```xml
<dependency>
    <groupId>dev.aegisgate</groupId>
    <artifactId>aegisgate-sdk</artifactId>
    <version>1.0.0</version>
</dependency>
```

## Quick Start

### Kotlin

```kotlin
import dev.aegisgate.AegisGateClient
import dev.aegisgate.AegisGateConfig

val client = AegisGateClient(AegisGateConfig(apiKey = "sk-xxx"))

// Simple chat — returns the assistant's reply directly
val reply = client.chat("Hello, how are you?")
println(reply)

client.close()
```

### Java

```java
import dev.aegisgate.AegisGateClient;
import dev.aegisgate.AegisGateConfig;

var config = new AegisGateConfig("sk-xxx", "http://localhost:8080",
        60000, 10000, 2, 1000, true, null, Map.of(), Map.of());
var client = new AegisGateClient(config);

String reply = client.chat("Hello!", "gpt-4o", null, 0.7, 1000);
System.out.println(reply);

client.close();
```

## Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `apiKey` | `String` | required | API key (Bearer auth) |
| `baseUrl` | `String` | `http://localhost:8080` | Gateway base URL |
| `timeoutMs` | `Long` | `60000` | Read/write timeout in milliseconds |
| `connectTimeoutMs` | `Long` | `10000` | TCP connect timeout in milliseconds |
| `maxRetries` | `Int` | `2` | Max retry attempts on retryable errors |
| `retryDelayMs` | `Long` | `1000` | Base retry delay in ms (exponential backoff) |
| `retryJitter` | `Boolean` | `true` | Add random jitter to retry delay |
| `traceId` | `String?` | `null` | Static trace ID, injected as `X-Trace-Id` header |
| `traceHeaders` | `Map<String, String>` | `emptyMap()` | Custom tracing headers (e.g. W3C `traceparent`) |
| `defaultHeaders` | `Map<String, String>` | `emptyMap()` | Extra headers for every request |

```kotlin
val client = AegisGateClient(
    AegisGateConfig(
        apiKey = "sk-xxx",
        baseUrl = "https://gateway.example.com",
        timeoutMs = 30_000,
        maxRetries = 3,
        retryJitter = true,
        traceId = "req-001",
        traceHeaders = mapOf("traceparent" to "00-abc-def-01"),
    )
)
```

### Distributed Tracing

Inject W3C TraceContext or custom tracing headers into every request:

```kotlin
val client = AegisGateClient(
    AegisGateConfig(
        apiKey = "sk-xxx",
        traceHeaders = mapOf(
            "traceparent" to "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
            "tracestate" to "rojo=00f067aa0ba902b7",
        ),
    )
)
```

## API Methods

### Simple Chat — `chat()`

Single-turn conversation, returns the assistant's reply directly:

```kotlin
val reply = client.chat(
    content = "Tell me about yourself",
    model = "gpt-4o",
    system = "You are a helpful assistant.",
    temperature = 0.7,
    maxTokens = 1000,
)
println(reply)
```

### Chat Completions (Non-streaming)

Full OpenAI-compatible interface with typed response:

```kotlin
import dev.aegisgate.ChatCompletionRequest
import dev.aegisgate.ChatMessage

val response = client.chatCompletions(
    ChatCompletionRequest(
        model = "gpt-4o",
        messages = listOf(
            ChatMessage(role = "system", content = "You are an assistant"),
            ChatMessage(role = "user", content = "Hello"),
        ),
        temperature = 0.7,
        maxTokens = 1000,
    )
)
println(response.choices[0].message.content)
println(response.usage?.totalTokens)
```

### Chat Completions (Streaming)

Receive response chunks via Kotlin coroutines `Flow`:

```kotlin
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.runBlocking

runBlocking {
    client.chatCompletionsStream(
        ChatCompletionRequest(
            model = "gpt-4o",
            messages = listOf(ChatMessage(role = "user", content = "Write a poem")),
        )
    ).collect { chunk ->
        val content = chunk.choices.firstOrNull()?.delta?.content ?: ""
        if (content.isNotEmpty()) print(content)
    }
}
```

### List Models — `listModels()`

```kotlin
val models = client.listModels()
models.data.forEach { println("${it.id} — ${it.ownedBy}") }
```

### Health Check — `health()`

No authentication required:

```kotlin
val health = client.health()
println("${health.status} — v${health.version}")
```

### Metrics — `metrics()`

Retrieve Prometheus-format metrics (requires authentication):

```kotlin
val metrics = client.metrics()
println(metrics)
```

## Error Handling

The SDK provides the following custom exception classes:

| Exception Class | Description |
|-----------------|-------------|
| `AegisGateException` | Base exception |
| `ApiException` | API request failed (includes `statusCode`, `errorCode`, `responseBody`) |
| `AuthenticationException` | 401 authentication failure |
| `RateLimitException` | 429 rate limit exceeded (includes `retryAfterSeconds`) |
| `TimeoutException` | Request timeout |
| `ConnectionException` | Network connection error |

```kotlin
import dev.aegisgate.*

try {
    val reply = client.chat("Hello")
    println(reply)
} catch (e: AuthenticationException) {
    println("Invalid API Key")
} catch (e: RateLimitException) {
    println("Rate limited — retry after ${e.retryAfterSeconds}s")
} catch (e: TimeoutException) {
    println("Request timed out")
} catch (e: ConnectionException) {
    println("Connection failed")
} catch (e: ApiException) {
    println("API error: ${e.message}, status: ${e.statusCode}")
}
```

## Retry Behavior

Automatic retries on 429/5xx status codes with exponential backoff + jitter:

- Retries on HTTP status codes: 429, 500, 502, 503, 504
- Exponential backoff: `retryDelayMs * 2^attempt`
- Optional random jitter (enabled by default)
- Respects the `Retry-After` response header
- Authentication errors (401) are never retried

## Resource Management

The client implements `Closeable`. Use Kotlin's `use` or Java's try-with-resources:

```kotlin
AegisGateClient(config).use { client ->
    val reply = client.chat("Hello")
    println(reply)
}
```

```java
try (var client = new AegisGateClient(config)) {
    String reply = client.chat("Hello", "gpt-4o", null, 0.7, 1000);
    System.out.println(reply);
}
```

## License

[Apache License 2.0](../../LICENSE)
