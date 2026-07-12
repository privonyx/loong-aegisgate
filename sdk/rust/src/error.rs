#[derive(Debug, thiserror::Error)]
pub enum AegisGateError {
    #[error("API error: {status_code} - {message}")]
    Api {
        status_code: u16,
        message: String,
        error_code: Option<String>,
        response_body: String,
    },

    #[error("Authentication error: {0}")]
    Authentication(String),

    #[error("Rate limit exceeded: {0}")]
    RateLimit(String),

    #[error("Request timeout: {0}")]
    Timeout(String),

    #[error("Connection error: {0}")]
    Connection(String),

    #[error("Parse error: {0}")]
    Parse(String),
}

impl AegisGateError {
    pub fn from_status(status: u16, body: &str) -> Self {
        let (message, error_code) = parse_error_body(body);

        match status {
            401 => AegisGateError::Authentication(message),
            429 => AegisGateError::RateLimit(message),
            _ => AegisGateError::Api {
                status_code: status,
                message,
                error_code,
                response_body: body.to_string(),
            },
        }
    }

    pub fn is_retryable(&self) -> bool {
        matches!(
            self,
            AegisGateError::RateLimit(_)
                | AegisGateError::Connection(_)
                | AegisGateError::Timeout(_)
        ) || matches!(self, AegisGateError::Api { status_code, .. }
            if [500, 502, 503, 504].contains(status_code))
    }
}

fn parse_error_body(body: &str) -> (String, Option<String>) {
    if let Ok(v) = serde_json::from_str::<serde_json::Value>(body) {
        let message = v
            .get("message")
            .or_else(|| v.get("error").and_then(|e| e.get("message")))
            .and_then(|m| m.as_str())
            .unwrap_or(body)
            .to_string();
        let error_code = v
            .get("code")
            .or_else(|| v.get("error_code"))
            .or_else(|| v.get("error").and_then(|e| e.get("code")))
            .and_then(|c| c.as_str())
            .map(|s| s.to_string());
        (message, error_code)
    } else {
        (body.to_string(), None)
    }
}
