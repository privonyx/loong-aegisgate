use aegisgate::*;
use futures_util::StreamExt;
use std::collections::HashMap;
use std::time::Duration;
use wiremock::matchers::{header, method, path};
use wiremock::{Mock, MockServer, ResponseTemplate};

#[test]
fn test_config_defaults() {
    let config = AegisGateConfig::default();
    assert_eq!(config.base_url, "http://localhost:8080");
    assert_eq!(config.timeout, Duration::from_secs(60));
    assert_eq!(config.connect_timeout, Duration::from_secs(10));
    assert_eq!(config.max_retries, 2);
    assert_eq!(config.retry_delay, Duration::from_secs(1));
    assert!(config.retry_jitter);
    assert!(config.api_key.is_empty());
    assert!(config.trace_id.is_none());
    assert!(config.trace_headers.is_empty());
    assert!(config.default_headers.is_empty());
}

#[test]
fn test_chat_completions_parse() {
    let json = r#"{
        "id": "chatcmpl-123",
        "object": "chat.completion",
        "model": "gpt-4o",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "Hello! How can I help you?"
                },
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": 10,
            "completion_tokens": 8,
            "total_tokens": 18
        }
    }"#;

    let response: ChatCompletionResponse = serde_json::from_str(json).unwrap();
    assert_eq!(response.id, "chatcmpl-123");
    assert_eq!(response.object, "chat.completion");
    assert_eq!(response.model, "gpt-4o");
    assert_eq!(response.choices.len(), 1);
    assert_eq!(response.choices[0].index, 0);
    assert_eq!(response.choices[0].message.role, "assistant");
    assert_eq!(response.choices[0].message.content, "Hello! How can I help you?");
    assert_eq!(response.choices[0].finish_reason, "stop");

    let usage = response.usage.as_ref().unwrap();
    assert_eq!(usage.prompt_tokens, 10);
    assert_eq!(usage.completion_tokens, 8);
    assert_eq!(usage.total_tokens, 18);

    assert_eq!(response.content(), Some("Hello! How can I help you?"));
}

#[test]
fn test_stream_chunk_parse() {
    let json = r#"{
        "id": "chatcmpl-456",
        "object": "chat.completion.chunk",
        "model": "gpt-4o",
        "choices": [
            {
                "index": 0,
                "delta": {
                    "content": "Hello"
                },
                "finish_reason": null
            }
        ]
    }"#;

    let chunk: ChatCompletionChunk = serde_json::from_str(json).unwrap();
    assert_eq!(chunk.id, "chatcmpl-456");
    assert_eq!(chunk.object, "chat.completion.chunk");
    assert_eq!(chunk.model, "gpt-4o");
    assert_eq!(chunk.choices.len(), 1);
    assert_eq!(chunk.choices[0].index, 0);
    assert_eq!(chunk.choices[0].delta.content.as_deref(), Some("Hello"));
    assert!(chunk.choices[0].finish_reason.is_none());
}

#[test]
fn test_stream_chunk_with_role() {
    let json = r#"{
        "id": "chatcmpl-789",
        "object": "chat.completion.chunk",
        "model": "gpt-4o",
        "choices": [
            {
                "index": 0,
                "delta": {
                    "role": "assistant",
                    "content": ""
                },
                "finish_reason": null
            }
        ]
    }"#;

    let chunk: ChatCompletionChunk = serde_json::from_str(json).unwrap();
    assert_eq!(chunk.choices[0].delta.role.as_deref(), Some("assistant"));
}

#[test]
fn test_error_handling() {
    let err = AegisGateError::from_status(401, r#"{"message": "Invalid API key"}"#);
    assert!(matches!(err, AegisGateError::Authentication(ref msg) if msg == "Invalid API key"));

    let err = AegisGateError::from_status(429, r#"{"message": "Too many requests"}"#);
    assert!(matches!(err, AegisGateError::RateLimit(ref msg) if msg == "Too many requests"));
    assert!(err.is_retryable());

    let err = AegisGateError::from_status(500, r#"{"message": "Internal server error"}"#);
    assert!(matches!(err, AegisGateError::Api { status_code: 500, .. }));
    assert!(err.is_retryable());

    let err = AegisGateError::from_status(404, r#"{"message": "Not found"}"#);
    assert!(matches!(err, AegisGateError::Api { status_code: 404, .. }));
    assert!(!err.is_retryable());
}

#[test]
fn test_chat_message_constructors() {
    let msg = ChatMessage::user("hello");
    assert_eq!(msg.role, "user");
    assert_eq!(msg.content, "hello");

    let msg = ChatMessage::system("you are helpful");
    assert_eq!(msg.role, "system");
    assert_eq!(msg.content, "you are helpful");

    let msg = ChatMessage::assistant("hi there");
    assert_eq!(msg.role, "assistant");
    assert_eq!(msg.content, "hi there");
}

#[test]
fn test_chat_completion_request_serialization() {
    let req = ChatCompletionRequest {
        model: "gpt-4o".to_string(),
        messages: vec![ChatMessage::user("hello")],
        temperature: Some(0.7),
        max_tokens: Some(100),
        stream: Some(false),
        extra: HashMap::new(),
    };

    let json = serde_json::to_value(&req).unwrap();
    assert_eq!(json["model"], "gpt-4o");
    assert_eq!(json["messages"][0]["role"], "user");
    assert_eq!(json["messages"][0]["content"], "hello");
    assert_eq!(json["temperature"], 0.7);
    assert_eq!(json["max_tokens"], 100);
    assert_eq!(json["stream"], false);
}

#[test]
fn test_model_list_parse() {
    let json = r#"{
        "object": "list",
        "data": [
            {"id": "gpt-4o", "object": "model", "owned_by": "openai"},
            {"id": "gpt-3.5-turbo", "object": "model", "owned_by": "openai"}
        ]
    }"#;

    let list: ModelListResponse = serde_json::from_str(json).unwrap();
    assert_eq!(list.object, "list");
    assert_eq!(list.data.len(), 2);
    assert_eq!(list.data[0].id, "gpt-4o");
    assert_eq!(list.data[1].id, "gpt-3.5-turbo");
}

#[test]
fn test_health_response_parse() {
    let json = r#"{"status": "ok", "version": "1.0.0"}"#;
    let health: HealthResponse = serde_json::from_str(json).unwrap();
    assert_eq!(health.status, "ok");
    assert_eq!(health.version, "1.0.0");
}

#[tokio::test]
async fn test_chat_completions_integration() {
    let mock_server = MockServer::start().await;

    let response_body = serde_json::json!({
        "id": "chatcmpl-abc",
        "object": "chat.completion",
        "model": "gpt-4o",
        "choices": [{
            "index": 0,
            "message": {"role": "assistant", "content": "Hi!"},
            "finish_reason": "stop"
        }],
        "usage": {
            "prompt_tokens": 5,
            "completion_tokens": 2,
            "total_tokens": 7
        }
    });

    Mock::given(method("POST"))
        .and(path("/v1/chat/completions"))
        .and(header("authorization", "Bearer test-key"))
        .respond_with(ResponseTemplate::new(200).set_body_json(&response_body))
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "test-key".to_string(),
        base_url: mock_server.uri(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let req = ChatCompletionRequest::new("gpt-4o", vec![ChatMessage::user("hello")]);
    let resp = client.chat_completions(&req).await.unwrap();

    assert_eq!(resp.id, "chatcmpl-abc");
    assert_eq!(resp.content(), Some("Hi!"));
}

#[tokio::test]
async fn test_chat_convenience_method() {
    let mock_server = MockServer::start().await;

    let response_body = serde_json::json!({
        "id": "chatcmpl-conv",
        "object": "chat.completion",
        "model": "gpt-4o",
        "choices": [{
            "index": 0,
            "message": {"role": "assistant", "content": "I'm doing well!"},
            "finish_reason": "stop"
        }]
    });

    Mock::given(method("POST"))
        .and(path("/v1/chat/completions"))
        .respond_with(ResponseTemplate::new(200).set_body_json(&response_body))
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "test-key".to_string(),
        base_url: mock_server.uri(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let content = client.chat("How are you?", "gpt-4o").await.unwrap();
    assert_eq!(content, "I'm doing well!");
}

#[tokio::test]
async fn test_list_models_integration() {
    let mock_server = MockServer::start().await;

    let response_body = serde_json::json!({
        "object": "list",
        "data": [
            {"id": "gpt-4o", "object": "model", "owned_by": "openai"}
        ]
    });

    Mock::given(method("GET"))
        .and(path("/v1/models"))
        .respond_with(ResponseTemplate::new(200).set_body_json(&response_body))
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "test-key".to_string(),
        base_url: mock_server.uri(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let models = client.list_models().await.unwrap();
    assert_eq!(models.data.len(), 1);
    assert_eq!(models.data[0].id, "gpt-4o");
}

#[tokio::test]
async fn test_health_integration() {
    let mock_server = MockServer::start().await;

    Mock::given(method("GET"))
        .and(path("/health"))
        .respond_with(
            ResponseTemplate::new(200)
                .set_body_json(serde_json::json!({"status": "ok", "version": "1.0.0"})),
        )
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "test-key".to_string(),
        base_url: mock_server.uri(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let health = client.health().await.unwrap();
    assert_eq!(health.status, "ok");
    assert_eq!(health.version, "1.0.0");
}

#[tokio::test]
async fn test_metrics_integration() {
    let mock_server = MockServer::start().await;

    Mock::given(method("GET"))
        .and(path("/metrics"))
        .respond_with(
            ResponseTemplate::new(200)
                .set_body_string("# HELP http_requests_total\nhttp_requests_total 42"),
        )
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "test-key".to_string(),
        base_url: mock_server.uri(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let metrics = client.metrics().await.unwrap();
    assert!(metrics.contains("http_requests_total"));
}

#[tokio::test]
async fn test_api_error_401() {
    let mock_server = MockServer::start().await;

    Mock::given(method("GET"))
        .and(path("/v1/models"))
        .respond_with(
            ResponseTemplate::new(401)
                .set_body_json(serde_json::json!({"message": "Invalid API key"})),
        )
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "bad-key".to_string(),
        base_url: mock_server.uri(),
        max_retries: 0,
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let err = client.list_models().await.unwrap_err();
    assert!(matches!(err, AegisGateError::Authentication(_)));
}

#[tokio::test]
async fn test_stream_sse_parse() {
    let mock_server = MockServer::start().await;

    let sse_body = "\
data: {\"id\":\"chunk-1\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n\
data: {\"id\":\"chunk-1\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n\n\
data: {\"id\":\"chunk-1\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\" world\"},\"finish_reason\":null}]}\n\n\
data: [DONE]\n\n";

    Mock::given(method("POST"))
        .and(path("/v1/chat/completions"))
        .respond_with(
            ResponseTemplate::new(200)
                .insert_header("content-type", "text/event-stream")
                .set_body_string(sse_body),
        )
        .mount(&mock_server)
        .await;

    let config = AegisGateConfig {
        api_key: "test-key".to_string(),
        base_url: mock_server.uri(),
        ..Default::default()
    };
    let client = AegisGateClient::new(config).unwrap();

    let req = ChatCompletionRequest::new("gpt-4o", vec![ChatMessage::user("hello")]);
    let mut stream = client.chat_completions_stream(&req).await.unwrap();

    let mut chunks = Vec::new();
    while let Some(result) = stream.next().await {
        chunks.push(result.unwrap());
    }

    assert_eq!(chunks.len(), 3);
    assert_eq!(chunks[0].choices[0].delta.role.as_deref(), Some("assistant"));
    assert_eq!(chunks[1].choices[0].delta.content.as_deref(), Some("Hello"));
    assert_eq!(chunks[2].choices[0].delta.content.as_deref(), Some(" world"));
}
