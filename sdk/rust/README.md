# AegisGate Rust SDK

Rust SDK for [AegisGate AI Gateway](https://github.com/privonyx/loong-aegisgate) — an OpenAI-compatible API gateway with built-in authentication, rate limiting, and observability.

## Installation

Add to your `Cargo.toml`:

```toml
[dependencies]
aegisgate = "1.0"
tokio = { version = "1", features = ["full"] }
futures-util = "0.3"
```

## Quick Start

```rust
use aegisgate::{AegisGateClient, AegisGateConfig, ChatCompletionRequest, ChatMessage};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let config = AegisGateConfig {
        api_key: "your-api-key".to_string(),
        base_url: "http://localhost:8080".to_string(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config)?;

    // Simple chat
    let reply = client.chat("Hello!", "gpt-4o").await?;
    println!("{reply}");

    Ok(())
}
```

## Chat Completions

```rust
use aegisgate::{AegisGateClient, AegisGateConfig, ChatCompletionRequest, ChatMessage};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client = AegisGateClient::with_api_key("your-api-key")?;

    let request = ChatCompletionRequest {
        model: "gpt-4o".to_string(),
        messages: vec![
            ChatMessage::system("You are a helpful assistant."),
            ChatMessage::user("What is Rust?"),
        ],
        temperature: Some(0.7),
        max_tokens: Some(1000),
        ..ChatCompletionRequest::new("gpt-4o", vec![])
    };

    let response = client.chat_completions(&request).await?;
    if let Some(content) = response.content() {
        println!("{content}");
    }

    Ok(())
}
```

## Streaming

```rust
use aegisgate::{AegisGateClient, AegisGateConfig, ChatCompletionRequest, ChatMessage};
use futures_util::StreamExt;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client = AegisGateClient::with_api_key("your-api-key")?;

    let request = ChatCompletionRequest::new(
        "gpt-4o",
        vec![ChatMessage::user("Tell me a story")],
    );

    let mut stream = client.chat_completions_stream(&request).await?;
    while let Some(result) = stream.next().await {
        match result {
            Ok(chunk) => {
                for choice in &chunk.choices {
                    if let Some(ref content) = choice.delta.content {
                        print!("{content}");
                    }
                }
            }
            Err(e) => eprintln!("Stream error: {e}"),
        }
    }
    println!();

    Ok(())
}
```

## Error Handling

```rust
use aegisgate::{AegisGateClient, AegisGateError, ChatCompletionRequest, ChatMessage};

#[tokio::main]
async fn main() {
    let client = AegisGateClient::with_api_key("your-api-key").unwrap();
    let request = ChatCompletionRequest::new("gpt-4o", vec![ChatMessage::user("hi")]);

    match client.chat_completions(&request).await {
        Ok(response) => println!("{}", response.content().unwrap_or("")),
        Err(AegisGateError::Authentication(msg)) => eprintln!("Auth failed: {msg}"),
        Err(AegisGateError::RateLimit(msg)) => eprintln!("Rate limited: {msg}"),
        Err(AegisGateError::Timeout(msg)) => eprintln!("Timed out: {msg}"),
        Err(AegisGateError::Api { status_code, message, .. }) => {
            eprintln!("API error {status_code}: {message}");
        }
        Err(e) => eprintln!("Error: {e}"),
    }
}
```

## Other Endpoints

```rust
use aegisgate::{AegisGateClient, AegisGateConfig};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client = AegisGateClient::with_api_key("your-api-key")?;

    // List models
    let models = client.list_models().await?;
    for model in &models.data {
        println!("{}: {}", model.id, model.owned_by);
    }

    // Health check (no auth required)
    let health = client.health().await?;
    println!("Status: {}, Version: {}", health.status, health.version);

    // Prometheus metrics
    let metrics = client.metrics().await?;
    println!("{metrics}");

    Ok(())
}
```

## Configuration

| Field | Default | Description |
|---|---|---|
| `api_key` | `""` | API key for authentication |
| `base_url` | `http://localhost:8080` | Gateway base URL |
| `timeout` | `60s` | Request timeout |
| `connect_timeout` | `10s` | TCP connection timeout |
| `max_retries` | `2` | Max retry attempts on retryable errors |
| `retry_delay` | `1s` | Base delay for exponential backoff |
| `retry_jitter` | `true` | Add randomized jitter to retry delays |
| `trace_id` | `None` | Static trace ID injected as `X-Trace-Id` header |
| `trace_headers` | `{}` | Custom trace headers (e.g. W3C traceparent) |
| `default_headers` | `{}` | Additional default request headers |

Retries use exponential backoff with optional jitter and respect `Retry-After` response headers. The following status codes are retried: `429`, `500`, `502`, `503`, `504`.

## License

MIT
