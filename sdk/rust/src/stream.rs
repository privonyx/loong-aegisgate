use bytes::Bytes;
use futures_util::stream::Stream;
use std::pin::Pin;
use std::task::{Context, Poll};

use crate::error::AegisGateError;
use crate::types::ChatCompletionChunk;

pub struct SseStream {
    inner: Pin<Box<dyn Stream<Item = reqwest::Result<Bytes>> + Send>>,
    buffer: String,
}

impl SseStream {
    pub fn new(byte_stream: impl Stream<Item = reqwest::Result<Bytes>> + Send + 'static) -> Self {
        Self {
            inner: Box::pin(byte_stream),
            buffer: String::new(),
        }
    }
}

impl Stream for SseStream {
    type Item = Result<ChatCompletionChunk, AegisGateError>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = self.get_mut();

        loop {
            if let Some(pos) = this.buffer.find('\n') {
                let line = this.buffer[..pos].to_string();
                this.buffer.drain(..=pos);

                let line = line.trim_end_matches('\r');
                if line.is_empty() {
                    continue;
                }

                if let Some(data) = line.strip_prefix("data: ") {
                    if data == "[DONE]" {
                        return Poll::Ready(None);
                    }

                    match serde_json::from_str::<ChatCompletionChunk>(data) {
                        Ok(chunk) => return Poll::Ready(Some(Ok(chunk))),
                        Err(e) => {
                            tracing::warn!(error = %e, data = data, "failed to parse SSE chunk");
                            continue;
                        }
                    }
                }

                continue;
            }

            match this.inner.as_mut().poll_next(cx) {
                Poll::Ready(Some(Ok(bytes))) => {
                    match std::str::from_utf8(&bytes) {
                        Ok(s) => this.buffer.push_str(s),
                        Err(e) => {
                            return Poll::Ready(Some(Err(AegisGateError::Parse(
                                format!("invalid UTF-8 in SSE stream: {e}"),
                            ))));
                        }
                    }
                }
                Poll::Ready(Some(Err(e))) => {
                    return Poll::Ready(Some(Err(AegisGateError::Connection(e.to_string()))));
                }
                Poll::Ready(None) => {
                    if !this.buffer.is_empty() {
                        let remaining = std::mem::take(&mut this.buffer);
                        let line = remaining.trim();
                        if let Some(data) = line.strip_prefix("data: ") {
                            if data == "[DONE]" {
                                return Poll::Ready(None);
                            }
                            match serde_json::from_str::<ChatCompletionChunk>(data) {
                                Ok(chunk) => return Poll::Ready(Some(Ok(chunk))),
                                Err(_) => return Poll::Ready(None),
                            }
                        }
                    }
                    return Poll::Ready(None);
                }
                Poll::Pending => return Poll::Pending,
            }
        }
    }
}
