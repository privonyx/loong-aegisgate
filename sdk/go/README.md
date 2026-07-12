# AegisGate Go SDK

Go client SDK for the AegisGate AI Gateway — zero external dependencies, stdlib only.

## Installation

```bash
go get github.com/privonyx/loong-aegisgate-go
```

## Quick Start

```go
package main

import (
    "context"
    "fmt"
    "log"
    "time"

    "github.com/privonyx/loong-aegisgate-go"
)

func main() {
    client := aegisgate.NewClient(
        aegisgate.WithBaseURL("http://localhost:8080"),
        aegisgate.WithAPIKey("sk-your-api-key"),
        aegisgate.WithTimeout(30*time.Second),
    )

    resp, err := client.ChatCompletions(context.Background(), &aegisgate.ChatCompletionsRequest{
        Model: "gpt-4o",
        Messages: []aegisgate.Message{
            {Role: "user", Content: "Hello"},
        },
        Temperature: 0.7,
        MaxTokens:   1000,
        Stream:      false,
    })
    if err != nil {
        log.Fatal(err)
    }

    if len(resp.Choices) > 0 {
        fmt.Println(resp.Choices[0].Message.Content)
    }
}
```

## Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `WithBaseURL` | Gateway base URL | `http://localhost:8080` |
| `WithAPIKey` | API key (Bearer auth) | empty |
| `WithTimeout` | Overall request timeout | 60s |
| `WithConnectTimeout` | TCP connect timeout (separate from overall) | inherited |
| `WithMaxRetries` | Max retry attempts | 2 |
| `WithRetryDelay` | Base retry delay (exponential backoff) | 500ms |
| `WithRetryJitter` | Add random jitter to retry delay | true |
| `WithRetryOnStatus` | HTTP status codes to retry | 429,500,502,503,504 |
| `WithHTTPClient` | Custom HTTP client | default client |
| `WithTraceID` | Static trace ID → `X-Trace-Id` header | empty |
| `WithTraceHeaders` | Custom tracing headers (e.g. W3C `traceparent`) | nil |
| `WithDefaultHeaders` | Extra headers for every request | nil |
| `WithMaxIdleConns` | Connection pool max idle connections | 100 |
| `WithMaxIdleConnsPerHost` | Connection pool max idle per host | 20 |
| `WithIdleConnTimeout` | Idle connection timeout | 90s |

## API Methods

### ChatCompletions

Non-streaming chat completions:

```go
resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
    Model:       "gpt-4o",
    Messages:    []aegisgate.Message{{Role: "user", Content: "Hello"}},
    Temperature: 0.7,
    MaxTokens:   1000,
})
```

### ChatCompletionsStream (Callback)

Streaming chat completions via callback function:

```go
err := client.ChatCompletionsStream(ctx, &aegisgate.ChatCompletionsRequest{
    Model:    "gpt-4o",
    Messages: []aegisgate.Message{{Role: "user", Content: "Hello"}},
    Stream:   true,
}, func(chunk *aegisgate.ChatCompletionChunk) error {
    for _, c := range chunk.Choices {
        fmt.Print(c.Delta.Content)
    }
    return nil
})
```

### ChatCompletionsStreamChan (Channel)

Streaming chat completions via channel:

```go
ch, errCh := client.ChatCompletionsStreamChan(ctx, req)
for chunk := range ch {
    for _, c := range chunk.Choices {
        fmt.Print(c.Delta.Content)
    }
}
if err := <-errCh; err != nil {
    log.Fatal(err)
}
```

### ListModels

List available models:

```go
models, err := client.ListModels(ctx)
```

### Health

Health check (no authentication required):

```go
health, err := client.Health(ctx)
// health.Status, health.Version
```

### Metrics

Retrieve Prometheus metrics (requires authentication):

```go
metrics, err := client.Metrics(ctx)
// metrics is text/plain format
```

### Reload

Reload configuration (requires authentication):

```go
reload, err := client.Reload(ctx)
// reload.Status, reload.Message
```

## Context Support

All methods accept `context.Context` for cancellation and timeout control:

```go
ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
defer cancel()

resp, err := client.ChatCompletions(ctx, req)
```

## Error Types

| Type | Description |
|------|-------------|
| `AegisGateError` | Base error |
| `APIError` | Generic API error |
| `AuthenticationError` | 401 authentication failure |
| `ForbiddenError` | 403 forbidden |
| `RateLimitError` | 429 rate limit exceeded |
| `BadGatewayError` | 502 bad gateway |
| `ServiceUnavailableError` | 503 service unavailable |
| `ConnectionError` | Network connection error |
| `TimeoutError` | Request timeout |

Example error handling:

```go
import "errors"

if err != nil {
    // Extract HTTP status code (works for all API error types)
    if sc, ok := err.(aegisgate.HTTPStatusCoder); ok {
        fmt.Println("Status:", sc.HTTPStatusCode())
    }
    var rateLimitErr *aegisgate.RateLimitError
    if errors.As(err, &rateLimitErr) {
        // Handle 429
    }
}
```

## Retry Behavior

Automatic retries on 429/5xx status codes with exponential backoff + jitter:

```go
client := aegisgate.NewClient(
    aegisgate.WithMaxRetries(3),
    aegisgate.WithRetryDelay(time.Second),
    aegisgate.WithRetryJitter(true),
    aegisgate.WithRetryOnStatus(429, 502, 503),
)
```

The SDK also respects the `Retry-After` header from rate-limited responses.

## Distributed Tracing

Inject W3C TraceContext or custom tracing headers into every request:

```go
client := aegisgate.NewClient(
    aegisgate.WithAPIKey("sk-xxx"),
    aegisgate.WithTraceID("req-trace-001"),
    aegisgate.WithTraceHeaders(map[string]string{
        "traceparent": "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
    }),
)
```

## Connection Pool

The SDK uses `http.Transport` with configurable connection pooling. Call `Close()` to release idle connections:

```go
client := aegisgate.NewClient(
    aegisgate.WithMaxIdleConns(100),
    aegisgate.WithMaxIdleConnsPerHost(20),
    aegisgate.WithIdleConnTimeout(90*time.Second),
)
defer client.Close()
```

## Requirements

Go 1.21+ — zero external dependencies, stdlib only.

## License

[Apache License 2.0](../../LICENSE)
