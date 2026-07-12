use std::collections::HashMap;
use std::time::Duration;

use futures_util::Stream;
use reqwest::header::{HeaderMap, HeaderName, HeaderValue, AUTHORIZATION, CONTENT_TYPE};

use crate::error::AegisGateError;
use crate::stream::SseStream;
use crate::types::*;

const DEFAULT_BASE_URL: &str = "http://localhost:8080";
const RETRYABLE_STATUS_CODES: &[u16] = &[429, 500, 502, 503, 504];

#[derive(Debug, Clone)]
pub struct AegisGateConfig {
    pub api_key: String,
    pub base_url: String,
    pub timeout: Duration,
    pub connect_timeout: Duration,
    pub max_retries: u32,
    pub retry_delay: Duration,
    pub retry_jitter: bool,
    pub trace_id: Option<String>,
    pub trace_headers: HashMap<String, String>,
    pub default_headers: HashMap<String, String>,
}

impl Default for AegisGateConfig {
    fn default() -> Self {
        Self {
            api_key: String::new(),
            base_url: DEFAULT_BASE_URL.to_string(),
            timeout: Duration::from_secs(60),
            connect_timeout: Duration::from_secs(10),
            max_retries: 2,
            retry_delay: Duration::from_secs(1),
            retry_jitter: true,
            trace_id: None,
            trace_headers: HashMap::new(),
            default_headers: HashMap::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct AegisGateClient {
    config: AegisGateConfig,
    http: reqwest::Client,
}

impl AegisGateClient {
    pub fn new(config: AegisGateConfig) -> Result<Self, AegisGateError> {
        let mut default_headers = HeaderMap::new();
        default_headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));

        for (key, value) in &config.default_headers {
            let name = HeaderName::try_from(key)
                .map_err(|e| AegisGateError::Parse(format!("invalid header name '{key}': {e}")))?;
            let val = HeaderValue::try_from(value)
                .map_err(|e| AegisGateError::Parse(format!("invalid header value for '{key}': {e}")))?;
            default_headers.insert(name, val);
        }

        for (key, value) in &config.trace_headers {
            let name = HeaderName::try_from(key)
                .map_err(|e| AegisGateError::Parse(format!("invalid trace header name '{key}': {e}")))?;
            let val = HeaderValue::try_from(value)
                .map_err(|e| AegisGateError::Parse(format!("invalid trace header value for '{key}': {e}")))?;
            default_headers.insert(name, val);
        }

        if let Some(ref trace_id) = config.trace_id {
            let name = HeaderName::from_static("x-trace-id");
            default_headers
                .entry(name)
                .or_insert_with(|| HeaderValue::try_from(trace_id).unwrap_or_else(|_| HeaderValue::from_static("")));
        }

        let http = reqwest::Client::builder()
            .timeout(config.timeout)
            .connect_timeout(config.connect_timeout)
            .default_headers(default_headers)
            .build()
            .map_err(|e| AegisGateError::Connection(format!("failed to build HTTP client: {e}")))?;

        Ok(Self { config, http })
    }

    pub fn with_api_key(api_key: impl Into<String>) -> Result<Self, AegisGateError> {
        let config = AegisGateConfig {
            api_key: api_key.into(),
            ..Default::default()
        };
        Self::new(config)
    }

    fn base_url(&self) -> &str {
        self.config.base_url.trim_end_matches('/')
    }

    fn auth_header(&self) -> Option<String> {
        if self.config.api_key.is_empty() {
            None
        } else {
            Some(format!("Bearer {}", self.config.api_key))
        }
    }

    async fn request_with_retry(
        &self,
        method: reqwest::Method,
        path: &str,
        body: Option<&[u8]>,
        auth_required: bool,
    ) -> Result<reqwest::Response, AegisGateError> {
        let url = format!("{}{}", self.base_url(), path);
        let mut last_err: Option<AegisGateError> = None;

        for attempt in 0..=self.config.max_retries {
            if attempt > 0 {
                let delay = backoff_delay(
                    attempt - 1,
                    self.config.retry_delay,
                    self.config.retry_jitter,
                );
                tracing::debug!(attempt, delay_ms = delay.as_millis(), "retrying request");
                tokio::time::sleep(delay).await;
            }

            let mut req = self.http.request(method.clone(), &url);
            if auth_required {
                if let Some(ref auth) = self.auth_header() {
                    req = req.header(AUTHORIZATION, auth);
                }
            }
            if let Some(b) = body {
                req = req.body(b.to_vec());
            }

            let resp = match req.send().await {
                Ok(r) => r,
                Err(e) => {
                    if e.is_timeout() {
                        last_err = Some(AegisGateError::Timeout(e.to_string()));
                    } else {
                        last_err = Some(AegisGateError::Connection(e.to_string()));
                    }
                    tracing::warn!(attempt, error = %last_err.as_ref().unwrap(), "request failed");
                    continue;
                }
            };

            let status = resp.status().as_u16();
            if is_retryable_status(status) && attempt < self.config.max_retries {
                let retry_after = parse_retry_after(&resp);
                let body_text = resp.text().await.unwrap_or_default();
                tracing::warn!(attempt, status, "retryable status, will retry");

                if let Some(ra) = retry_after {
                    tokio::time::sleep(ra).await;
                }

                last_err = Some(AegisGateError::from_status(status, &body_text));
                continue;
            }

            return Ok(resp);
        }

        Err(last_err.unwrap_or_else(|| AegisGateError::Connection("max retries exceeded".to_string())))
    }

    pub async fn chat_completions(
        &self,
        request: &ChatCompletionRequest,
    ) -> Result<ChatCompletionResponse, AegisGateError> {
        let mut req = request.clone();
        req.stream = Some(false);

        let body = serde_json::to_vec(&req)
            .map_err(|e| AegisGateError::Parse(format!("failed to serialize request: {e}")))?;

        let resp = self
            .request_with_retry(reqwest::Method::POST, "/v1/chat/completions", Some(&body), true)
            .await?;

        let status = resp.status().as_u16();
        let body_text = resp
            .text()
            .await
            .map_err(|e| AegisGateError::Connection(format!("failed to read response body: {e}")))?;

        if status != 200 {
            return Err(AegisGateError::from_status(status, &body_text));
        }

        serde_json::from_str(&body_text)
            .map_err(|e| AegisGateError::Parse(format!("failed to parse response: {e}")))
    }

    pub async fn chat_completions_stream(
        &self,
        request: &ChatCompletionRequest,
    ) -> Result<impl Stream<Item = Result<ChatCompletionChunk, AegisGateError>>, AegisGateError> {
        let mut req = request.clone();
        req.stream = Some(true);

        let body = serde_json::to_vec(&req)
            .map_err(|e| AegisGateError::Parse(format!("failed to serialize request: {e}")))?;

        let url = format!("{}/v1/chat/completions", self.base_url());
        let mut http_req = self.http.post(&url).body(body);
        if let Some(ref auth) = self.auth_header() {
            http_req = http_req.header(AUTHORIZATION, auth);
        }

        let resp = http_req
            .send()
            .await
            .map_err(|e| {
                if e.is_timeout() {
                    AegisGateError::Timeout(e.to_string())
                } else {
                    AegisGateError::Connection(e.to_string())
                }
            })?;

        let status = resp.status();
        if !status.is_success() {
            let status_code = status.as_u16();
            let body_text = resp.text().await.unwrap_or_default();
            return Err(AegisGateError::from_status(status_code, &body_text));
        }

        Ok(SseStream::new(resp.bytes_stream()))
    }

    pub async fn chat(
        &self,
        content: &str,
        model: &str,
    ) -> Result<String, AegisGateError> {
        let request = ChatCompletionRequest::new(
            model,
            vec![ChatMessage::user(content)],
        );
        let response = self.chat_completions(&request).await?;
        response
            .content()
            .map(|s| s.to_string())
            .ok_or_else(|| AegisGateError::Parse("response contained no choices".to_string()))
    }

    pub async fn list_models(&self) -> Result<ModelListResponse, AegisGateError> {
        let resp = self
            .request_with_retry(reqwest::Method::GET, "/v1/models", None, true)
            .await?;

        let status = resp.status().as_u16();
        let body_text = resp
            .text()
            .await
            .map_err(|e| AegisGateError::Connection(format!("failed to read response body: {e}")))?;

        if status != 200 {
            return Err(AegisGateError::from_status(status, &body_text));
        }

        serde_json::from_str(&body_text)
            .map_err(|e| AegisGateError::Parse(format!("failed to parse model list: {e}")))
    }

    pub async fn health(&self) -> Result<HealthResponse, AegisGateError> {
        let resp = self
            .request_with_retry(reqwest::Method::GET, "/health", None, false)
            .await?;

        let status = resp.status().as_u16();
        let body_text = resp
            .text()
            .await
            .map_err(|e| AegisGateError::Connection(format!("failed to read response body: {e}")))?;

        if status != 200 {
            return Err(AegisGateError::from_status(status, &body_text));
        }

        serde_json::from_str(&body_text)
            .map_err(|e| AegisGateError::Parse(format!("failed to parse health response: {e}")))
    }

    pub async fn metrics(&self) -> Result<String, AegisGateError> {
        let resp = self
            .request_with_retry(reqwest::Method::GET, "/metrics", None, true)
            .await?;

        let status = resp.status().as_u16();
        let body_text = resp
            .text()
            .await
            .map_err(|e| AegisGateError::Connection(format!("failed to read response body: {e}")))?;

        if status != 200 {
            return Err(AegisGateError::from_status(status, &body_text));
        }

        Ok(body_text)
    }
}

fn is_retryable_status(status: u16) -> bool {
    RETRYABLE_STATUS_CODES.contains(&status)
}

fn backoff_delay(attempt: u32, base_delay: Duration, jitter: bool) -> Duration {
    let multiplier = 2u64.pow(attempt);
    let delay_ms = base_delay.as_millis() as u64 * multiplier;
    if jitter {
        let jitter_factor = 0.5 + rand::random::<f64>() * 0.5;
        Duration::from_millis((delay_ms as f64 * jitter_factor) as u64)
    } else {
        Duration::from_millis(delay_ms)
    }
}

fn parse_retry_after(resp: &reqwest::Response) -> Option<Duration> {
    resp.headers()
        .get("retry-after")
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.parse::<f64>().ok())
        .map(|secs| Duration::from_secs_f64(secs))
}
