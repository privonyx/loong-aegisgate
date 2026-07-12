package aegisgate

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"math/rand"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const (
	defaultBaseURL    = "http://localhost:8080"
	defaultTimeout    = 60 * time.Second
	defaultMaxRetries = 2
)

var defaultRetryOnStatus = map[int]bool{
	429: true, 500: true, 502: true, 503: true, 504: true,
}

// Client AegisGate API 客户端
type Client struct {
	baseURL        string
	apiKey         string
	httpClient     *http.Client
	maxRetries     int
	retryDelay     time.Duration
	retryJitter    bool
	retryOnStatus  map[int]bool
	traceID        string
	traceHeaders   map[string]string
	defaultHeaders map[string]string
}

// Option 客户端配置选项
type Option func(*Client)

// WithBaseURL 设置 Base URL
func WithBaseURL(baseURL string) Option {
	return func(c *Client) {
		c.baseURL = strings.TrimSuffix(baseURL, "/")
	}
}

// WithAPIKey 设置 API Key
func WithAPIKey(apiKey string) Option {
	return func(c *Client) {
		c.apiKey = apiKey
	}
}

// WithTimeout 设置请求超时
func WithTimeout(timeout time.Duration) Option {
	return func(c *Client) {
		c.httpClient.Timeout = timeout
	}
}

// WithConnectTimeout 设置 TCP 连接超时（与整体 Timeout 独立）
func WithConnectTimeout(d time.Duration) Option {
	return func(c *Client) {
		transport := c.getTransport()
		dialer := &net.Dialer{Timeout: d}
		transport.DialContext = dialer.DialContext
	}
}

// WithMaxRetries 设置最大重试次数
func WithMaxRetries(n int) Option {
	return func(c *Client) {
		c.maxRetries = n
	}
}

// WithRetryDelay 设置重试间隔
func WithRetryDelay(d time.Duration) Option {
	return func(c *Client) {
		c.retryDelay = d
	}
}

// WithRetryJitter 设置是否启用 jitter（默认 true）
func WithRetryJitter(enabled bool) Option {
	return func(c *Client) {
		c.retryJitter = enabled
	}
}

// WithRetryOnStatus 设置可重试的 HTTP 状态码集合
func WithRetryOnStatus(codes ...int) Option {
	return func(c *Client) {
		m := make(map[int]bool, len(codes))
		for _, code := range codes {
			m[code] = true
		}
		c.retryOnStatus = m
	}
}

// WithHTTPClient 使用自定义 HTTP 客户端
func WithHTTPClient(hc *http.Client) Option {
	return func(c *Client) {
		c.httpClient = hc
	}
}

// WithTraceID 设置静态追踪 ID，自动注入 X-Trace-Id 请求头
func WithTraceID(traceID string) Option {
	return func(c *Client) {
		c.traceID = traceID
	}
}

// WithTraceHeaders 设置自定义追踪头（如 W3C traceparent / b3 等）
func WithTraceHeaders(headers map[string]string) Option {
	return func(c *Client) {
		c.traceHeaders = headers
	}
}

// WithDefaultHeaders 设置额外的自定义请求头
func WithDefaultHeaders(headers map[string]string) Option {
	return func(c *Client) {
		c.defaultHeaders = headers
	}
}

// WithMaxIdleConns 设置连接池最大空闲连接数
func WithMaxIdleConns(n int) Option {
	return func(c *Client) {
		transport := c.getTransport()
		transport.MaxIdleConns = n
	}
}

// WithMaxIdleConnsPerHost 设置单主机最大空闲连接数
func WithMaxIdleConnsPerHost(n int) Option {
	return func(c *Client) {
		transport := c.getTransport()
		transport.MaxIdleConnsPerHost = n
	}
}

// WithIdleConnTimeout 设置空闲连接超时
func WithIdleConnTimeout(d time.Duration) Option {
	return func(c *Client) {
		transport := c.getTransport()
		transport.IdleConnTimeout = d
	}
}

// getTransport 获取或创建可配置的 Transport
func (c *Client) getTransport() *http.Transport {
	if t, ok := c.httpClient.Transport.(*http.Transport); ok {
		return t
	}
	t := &http.Transport{
		MaxIdleConns:        100,
		MaxIdleConnsPerHost: 20,
		IdleConnTimeout:     90 * time.Second,
	}
	c.httpClient.Transport = t
	return t
}

// NewClient 创建新客户端
func NewClient(opts ...Option) *Client {
	transport := &http.Transport{
		MaxIdleConns:        100,
		MaxIdleConnsPerHost: 20,
		IdleConnTimeout:     90 * time.Second,
	}
	c := &Client{
		baseURL: defaultBaseURL,
		httpClient: &http.Client{
			Timeout:   defaultTimeout,
			Transport: transport,
		},
		maxRetries:    defaultMaxRetries,
		retryDelay:    500 * time.Millisecond,
		retryJitter:   true,
		retryOnStatus: defaultRetryOnStatus,
	}
	for _, opt := range opts {
		opt(c)
	}
	return c
}

// Close 关闭客户端，释放底层连接池
func (c *Client) Close() {
	if t, ok := c.httpClient.Transport.(*http.Transport); ok {
		t.CloseIdleConnections()
	}
}

func backoffDelay(attempt int, baseDelay time.Duration, jitter bool) time.Duration {
	delay := float64(baseDelay) * math.Pow(2, float64(attempt))
	if jitter {
		delay = delay * (0.5 + rand.Float64()*0.5) // #nosec G404
	}
	return time.Duration(delay)
}

func parseRetryAfter(resp *http.Response) time.Duration {
	header := resp.Header.Get("Retry-After")
	if header == "" {
		return 0
	}
	seconds, err := strconv.ParseFloat(header, 64)
	if err != nil {
		return 0
	}
	return time.Duration(seconds * float64(time.Second))
}

// doRequest 执行 HTTP 请求，支持重试（指数退避 + jitter + Retry-After）
func (c *Client) doRequest(ctx context.Context, method, path string, body []byte, authRequired bool) (*http.Response, error) {
	u, err := url.JoinPath(c.baseURL, path)
	if err != nil {
		return nil, &ConnectionError{AegisGateError: AegisGateError{Message: "invalid URL: " + err.Error()}}
	}

	var lastErr error
	for attempt := 0; attempt <= c.maxRetries; attempt++ {
		if attempt > 0 {
			select {
			case <-ctx.Done():
				return nil, &TimeoutError{AegisGateError: AegisGateError{Message: ctx.Err().Error()}}
			case <-time.After(backoffDelay(attempt-1, c.retryDelay, c.retryJitter)):
			}
		}

		var reqBody io.Reader
		if body != nil {
			reqBody = bytes.NewReader(body)
		}

		req, err := http.NewRequestWithContext(ctx, method, u, reqBody)
		if err != nil {
			return nil, &ConnectionError{AegisGateError: AegisGateError{Message: err.Error()}}
		}

		req.Header.Set("Content-Type", "application/json")
		if authRequired && c.apiKey != "" {
			req.Header.Set("Authorization", "Bearer "+c.apiKey)
		}
		for k, v := range c.defaultHeaders {
			req.Header.Set(k, v)
		}
		for k, v := range c.traceHeaders {
			req.Header.Set(k, v)
		}
		if c.traceID != "" {
			if req.Header.Get("X-Trace-Id") == "" {
				req.Header.Set("X-Trace-Id", c.traceID)
			}
		}

		resp, err := c.httpClient.Do(req)
		if err != nil {
			lastErr = err
			if ctx.Err() != nil {
				return nil, &TimeoutError{AegisGateError: AegisGateError{Message: ctx.Err().Error()}}
			}
			continue
		}

		if c.retryOnStatus[resp.StatusCode] && attempt < c.maxRetries {
			retryAfter := parseRetryAfter(resp)
			resp.Body.Close()
			if retryAfter > 0 {
				select {
				case <-ctx.Done():
					return nil, &TimeoutError{AegisGateError: AegisGateError{Message: ctx.Err().Error()}}
				case <-time.After(retryAfter):
				}
			}
			lastErr = fmt.Errorf("server error %d", resp.StatusCode)
			continue
		}

		return resp, nil
	}

	if lastErr != nil {
		return nil, &ConnectionError{AegisGateError: AegisGateError{Message: lastErr.Error()}}
	}
	return nil, &ConnectionError{AegisGateError: AegisGateError{Message: "max retries exceeded"}}
}

// parseAPIError 根据状态码解析 API 错误
func parseAPIError(statusCode int, body string) error {
	apiErr := APIError{
		StatusCode:   statusCode,
		ResponseBody: body,
		Message:      body,
	}
	if apiErr.Message == "" {
		apiErr.Message = http.StatusText(statusCode)
	}

	switch statusCode {
	case 401:
		return &AuthenticationError{APIError: apiErr}
	case 403:
		return &ForbiddenError{APIError: apiErr}
	case 429:
		return &RateLimitError{APIError: apiErr}
	case 502:
		return &BadGatewayError{APIError: apiErr}
	case 503:
		return &ServiceUnavailableError{APIError: apiErr}
	default:
		return &apiErr
	}
}

// ChatCompletions 聊天补全（非流式）
func (c *Client) ChatCompletions(ctx context.Context, req *ChatCompletionsRequest) (*ChatCompletionsResponse, error) {
	if req.Stream {
		return nil, &AegisGateError{Message: "use ChatCompletionsStream for streaming"}
	}

	body, err := json.Marshal(req)
	if err != nil {
		return nil, &AegisGateError{Message: "marshal request: " + err.Error()}
	}

	resp, err := c.doRequest(ctx, http.MethodPost, "/v1/chat/completions", body, true)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, parseAPIError(resp.StatusCode, string(respBody))
	}

	var out ChatCompletionsResponse
	if err := json.Unmarshal(respBody, &out); err != nil {
		return nil, &AegisGateError{Message: "unmarshal response: " + err.Error()}
	}
	return &out, nil
}

// StreamCallback 流式回调函数
type StreamCallback func(chunk *ChatCompletionChunk) error

// ChatCompletionsStream 聊天补全（流式，回调）
func (c *Client) ChatCompletionsStream(ctx context.Context, req *ChatCompletionsRequest, fn StreamCallback) error {
	req.Stream = true
	body, err := json.Marshal(req)
	if err != nil {
		return &AegisGateError{Message: "marshal request: " + err.Error()}
	}

	resp, err := c.doRequest(ctx, http.MethodPost, "/v1/chat/completions", body, true)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		respBody, _ := io.ReadAll(resp.Body)
		return parseAPIError(resp.StatusCode, string(respBody))
	}

	return parseSSE(ctx, resp.Body, fn)
}

// ChatCompletionsStreamChan 聊天补全（流式，channel）
func (c *Client) ChatCompletionsStreamChan(ctx context.Context, req *ChatCompletionsRequest) (<-chan *ChatCompletionChunk, <-chan error) {
	ch := make(chan *ChatCompletionChunk, 8)
	errCh := make(chan error, 1)

	go func() {
		defer close(ch)
		err := c.ChatCompletionsStream(ctx, req, func(chunk *ChatCompletionChunk) error {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case ch <- chunk:
				return nil
			}
		})
		if err != nil {
			errCh <- err
		}
		close(errCh)
	}()

	return ch, errCh
}

// parseSSE 解析 Server-Sent Events
func parseSSE(ctx context.Context, r io.Reader, fn StreamCallback) error {
	scanner := bufio.NewScanner(r)
	scanner.Buffer(nil, 64*1024)

	for scanner.Scan() {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}

		line := scanner.Text()
		if strings.HasPrefix(line, "data: ") {
			payload := strings.TrimPrefix(line, "data: ")
			if payload == "[DONE]" {
				return nil
			}

			var chunk ChatCompletionChunk
			if err := json.Unmarshal([]byte(payload), &chunk); err != nil {
				continue
			}
			if err := fn(&chunk); err != nil {
				return err
			}
		}
	}
	return scanner.Err()
}

// ListModels 列出模型
func (c *Client) ListModels(ctx context.Context) (*ModelListResponse, error) {
	resp, err := c.doRequest(ctx, http.MethodGet, "/v1/models", nil, true)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, parseAPIError(resp.StatusCode, string(respBody))
	}

	var out ModelListResponse
	if err := json.Unmarshal(respBody, &out); err != nil {
		return nil, &AegisGateError{Message: "unmarshal response: " + err.Error()}
	}
	return &out, nil
}

// Health 健康检查（无需认证）
func (c *Client) Health(ctx context.Context) (*HealthResponse, error) {
	resp, err := c.doRequest(ctx, http.MethodGet, "/health", nil, false)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, parseAPIError(resp.StatusCode, string(respBody))
	}

	var out HealthResponse
	if err := json.Unmarshal(respBody, &out); err != nil {
		return nil, &AegisGateError{Message: "unmarshal response: " + err.Error()}
	}
	return &out, nil
}

// Metrics 获取 Prometheus 指标（需认证）
func (c *Client) Metrics(ctx context.Context) (string, error) {
	resp, err := c.doRequest(ctx, http.MethodGet, "/metrics", nil, true)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return "", parseAPIError(resp.StatusCode, string(respBody))
	}
	return string(respBody), nil
}

// Reload 重载配置（需认证）
func (c *Client) Reload(ctx context.Context) (*ReloadResponse, error) {
	resp, err := c.doRequest(ctx, http.MethodPost, "/admin/reload", nil, true)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, parseAPIError(resp.StatusCode, string(respBody))
	}

	var out ReloadResponse
	if err := json.Unmarshal(respBody, &out); err != nil {
		return nil, &AegisGateError{Message: "unmarshal response: " + err.Error()}
	}
	return &out, nil
}
