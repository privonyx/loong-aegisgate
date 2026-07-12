package aegisgate

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func TestBackoffDelay(t *testing.T) {
	d0 := backoffDelay(0, 100*time.Millisecond, false)
	if d0 != 100*time.Millisecond {
		t.Errorf("expected 100ms, got %v", d0)
	}

	d1 := backoffDelay(1, 100*time.Millisecond, false)
	if d1 != 200*time.Millisecond {
		t.Errorf("expected 200ms, got %v", d1)
	}

	d2 := backoffDelay(2, 100*time.Millisecond, false)
	if d2 != 400*time.Millisecond {
		t.Errorf("expected 400ms, got %v", d2)
	}
}

func TestBackoffDelayJitter(t *testing.T) {
	for i := 0; i < 50; i++ {
		d := backoffDelay(1, 100*time.Millisecond, true)
		if d < 100*time.Millisecond || d > 200*time.Millisecond {
			t.Errorf("jitter delay %v out of expected range [100ms, 200ms]", d)
		}
	}
}

func TestParseRetryAfterHeader(t *testing.T) {
	tests := []struct {
		header string
		want   time.Duration
	}{
		{"3", 3 * time.Second},
		{"1.5", time.Duration(1.5 * float64(time.Second))},
		{"", 0},
		{"invalid", 0},
	}

	for _, tt := range tests {
		resp := &http.Response{Header: http.Header{}}
		if tt.header != "" {
			resp.Header.Set("Retry-After", tt.header)
		}
		got := parseRetryAfter(resp)
		if got != tt.want {
			t.Errorf("parseRetryAfter(%q) = %v, want %v", tt.header, got, tt.want)
		}
	}
}

func TestClientTraceHeaders(t *testing.T) {
	var capturedHeaders http.Header
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		capturedHeaders = r.Header.Clone()
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(HealthResponse{Status: "ok", Version: "1.0"})
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithTraceID("trace-abc-123"),
		WithTraceHeaders(map[string]string{
			"traceparent": "00-abc-def-01",
			"b3":          "abc-def-1",
		}),
		WithDefaultHeaders(map[string]string{
			"X-App-Name": "test-app",
		}),
	)
	defer c.Close()

	_, err := c.Health(context.Background())
	if err != nil {
		t.Fatalf("Health() error: %v", err)
	}

	if got := capturedHeaders.Get("X-Trace-Id"); got != "trace-abc-123" {
		t.Errorf("X-Trace-Id = %q, want %q", got, "trace-abc-123")
	}
	if got := capturedHeaders.Get("traceparent"); got != "00-abc-def-01" {
		t.Errorf("traceparent = %q, want %q", got, "00-abc-def-01")
	}
	if got := capturedHeaders.Get("b3"); got != "abc-def-1" {
		t.Errorf("b3 = %q, want %q", got, "abc-def-1")
	}
	if got := capturedHeaders.Get("X-App-Name"); got != "test-app" {
		t.Errorf("X-App-Name = %q, want %q", got, "test-app")
	}
}

func TestClientRetryOn5xx(t *testing.T) {
	var attempts int32
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := atomic.AddInt32(&attempts, 1)
		if n <= 2 {
			w.WriteHeader(http.StatusBadGateway)
			w.Write([]byte(`{"error":"bad gateway"}`))
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(HealthResponse{Status: "ok", Version: "1.0"})
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithMaxRetries(3),
		WithRetryDelay(10*time.Millisecond),
		WithRetryJitter(false),
	)
	defer c.Close()

	resp, err := c.Health(context.Background())
	if err != nil {
		t.Fatalf("Health() error: %v", err)
	}
	if resp.Status != "ok" {
		t.Errorf("Status = %q, want %q", resp.Status, "ok")
	}
	if got := atomic.LoadInt32(&attempts); got != 3 {
		t.Errorf("attempts = %d, want 3", got)
	}
}

func TestClientRetryWithRetryAfter(t *testing.T) {
	var attempts int32
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := atomic.AddInt32(&attempts, 1)
		if n == 1 {
			w.Header().Set("Retry-After", "0")
			w.WriteHeader(http.StatusTooManyRequests)
			w.Write([]byte(`{"error":"rate limited"}`))
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(HealthResponse{Status: "ok", Version: "1.0"})
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithMaxRetries(2),
		WithRetryDelay(10*time.Millisecond),
		WithRetryJitter(false),
	)
	defer c.Close()

	resp, err := c.Health(context.Background())
	if err != nil {
		t.Fatalf("Health() error: %v", err)
	}
	if resp.Status != "ok" {
		t.Errorf("Status = %q, want %q", resp.Status, "ok")
	}
	if got := atomic.LoadInt32(&attempts); got != 2 {
		t.Errorf("attempts = %d, want 2", got)
	}
}

func TestClientNoRetryOnAuth(t *testing.T) {
	var attempts int32
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusUnauthorized)
		w.Write([]byte(`{"error":"unauthorized"}`))
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithAPIKey("bad-key"),
		WithMaxRetries(3),
		WithRetryDelay(10*time.Millisecond),
	)
	defer c.Close()

	_, err := c.Health(context.Background())
	if err == nil {
		t.Fatal("expected error for 401")
	}
	if got := atomic.LoadInt32(&attempts); got != 1 {
		t.Errorf("attempts = %d, want 1 (no retry on 401)", got)
	}
}

func TestClientConnectionPoolConfig(t *testing.T) {
	c := NewClient(
		WithMaxIdleConns(50),
		WithMaxIdleConnsPerHost(10),
		WithIdleConnTimeout(30*time.Second),
	)

	transport, ok := c.httpClient.Transport.(*http.Transport)
	if !ok {
		t.Fatal("expected *http.Transport")
	}

	if transport.MaxIdleConns != 50 {
		t.Errorf("MaxIdleConns = %d, want 50", transport.MaxIdleConns)
	}
	if transport.MaxIdleConnsPerHost != 10 {
		t.Errorf("MaxIdleConnsPerHost = %d, want 10", transport.MaxIdleConnsPerHost)
	}
	if transport.IdleConnTimeout != 30*time.Second {
		t.Errorf("IdleConnTimeout = %v, want 30s", transport.IdleConnTimeout)
	}
}

func TestClientClose(t *testing.T) {
	c := NewClient()
	c.Close()
}

func TestClientCustomRetryOnStatus(t *testing.T) {
	var attempts int32
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := atomic.AddInt32(&attempts, 1)
		if n <= 2 {
			w.WriteHeader(http.StatusServiceUnavailable)
			w.Write([]byte(`{"error":"unavailable"}`))
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(HealthResponse{Status: "ok", Version: "1.0"})
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithMaxRetries(3),
		WithRetryDelay(10*time.Millisecond),
		WithRetryJitter(false),
		WithRetryOnStatus(503),
	)
	defer c.Close()

	resp, err := c.Health(context.Background())
	if err != nil {
		t.Fatalf("Health() error: %v", err)
	}
	if resp.Status != "ok" {
		t.Errorf("Status = %q, want %q", resp.Status, "ok")
	}
}

func TestClientChatCompletionsWithTracing(t *testing.T) {
	var capturedHeaders http.Header
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		capturedHeaders = r.Header.Clone()
		w.Header().Set("Content-Type", "application/json")
		resp := ChatCompletionsResponse{
			ID:     "chatcmpl-test",
			Object: "chat.completion",
			Model:  "gpt-4o",
			Choices: []Choice{
				{
					Index:        0,
					Message:      Message{Role: "assistant", Content: "Hello!"},
					FinishReason: "stop",
				},
			},
			Usage: &Usage{PromptTokens: 10, CompletionTokens: 5, TotalTokens: 15},
		}
		json.NewEncoder(w).Encode(resp)
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithAPIKey("test-key"),
		WithTraceID("req-trace-001"),
		WithTraceHeaders(map[string]string{"traceparent": "00-trace-span-01"}),
	)
	defer c.Close()

	req := &ChatCompletionsRequest{
		Model: "gpt-4o",
		Messages: []Message{
			{Role: "user", Content: "Hi"},
		},
	}

	resp, err := c.ChatCompletions(context.Background(), req)
	if err != nil {
		t.Fatalf("ChatCompletions() error: %v", err)
	}

	if resp.Model != "gpt-4o" {
		t.Errorf("Model = %q, want %q", resp.Model, "gpt-4o")
	}

	if got := capturedHeaders.Get("X-Trace-Id"); got != "req-trace-001" {
		t.Errorf("X-Trace-Id = %q, want %q", got, "req-trace-001")
	}
	if got := capturedHeaders.Get("traceparent"); got != "00-trace-span-01" {
		t.Errorf("traceparent = %q, want %q", got, "00-trace-span-01")
	}
	if got := capturedHeaders.Get("Authorization"); got != "Bearer test-key" {
		t.Errorf("Authorization = %q, want %q", got, "Bearer test-key")
	}
}

func TestClientExhaustedRetriesReturnsError(t *testing.T) {
	var attempts int32
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusBadGateway)
		fmt.Fprint(w, `{"error":"bad gateway"}`)
	}))
	defer ts.Close()

	c := NewClient(
		WithBaseURL(ts.URL),
		WithMaxRetries(2),
		WithRetryDelay(10*time.Millisecond),
		WithRetryJitter(false),
	)
	defer c.Close()

	_, err := c.Health(context.Background())
	if err == nil {
		t.Fatal("expected error after exhausted retries")
	}

	if got := atomic.LoadInt32(&attempts); got != 3 {
		t.Errorf("attempts = %d, want 3", got)
	}
}

func TestDefaultRetryOnStatus(t *testing.T) {
	for _, code := range []int{429, 500, 502, 503, 504} {
		if !defaultRetryOnStatus[code] {
			t.Errorf("expected %d in defaultRetryOnStatus", code)
		}
	}
	for _, code := range []int{400, 401, 403, 404} {
		if defaultRetryOnStatus[code] {
			t.Errorf("unexpected %d in defaultRetryOnStatus", code)
		}
	}
}
