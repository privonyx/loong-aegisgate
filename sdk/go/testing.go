package aegisgate

import (
	"context"
	"fmt"
	"strings"
	"sync"
)

// MockCall 单次 Mock 客户端调用的记录
type MockCall struct {
	Method string
	Prompt string
	Model  string
}

type mockChatRule struct {
	pattern string
	content string
}

type mockErrRule struct {
	pattern   string
	aegisCode string
	message   string
}

// MockClient 无 HTTP 的测试替身，与 Client 的关键方法签名对齐
type MockClient struct {
	mu sync.Mutex

	chatRules []mockChatRule
	errRules  []mockErrRule

	defaultContent string
	defaultOK      bool

	nextID int
	calls  []MockCall
}

// NewMockClient 创建 Mock 客户端；opts 与 NewClient 相同，用于保持配置 API 一致（当前不用于网络）
func NewMockClient(opts ...Option) *MockClient {
	_ = NewClient(opts...)
	return &MockClient{}
}

// MockChat 按子串匹配注册非流式回复
func (m *MockClient) MockChat(promptPattern, responseContent string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.chatRules = append(m.chatRules, mockChatRule{pattern: promptPattern, content: responseContent})
}

// MockError 按子串匹配注册错误（返回 *APIError，含 AegisCode）
func (m *MockClient) MockError(promptPattern string, aegisCode string, message string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.errRules = append(m.errRules, mockErrRule{pattern: promptPattern, aegisCode: aegisCode, message: message})
}

// SetDefaultResponse 未匹配规则时的默认回复内容
func (m *MockClient) SetDefaultResponse(content string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.defaultContent = content
	m.defaultOK = true
}

func mockLastUserPrompt(msgs []Message) string {
	for i := len(msgs) - 1; i >= 0; i-- {
		if msgs[i].Role == "user" {
			return msgs[i].Content
		}
	}
	return ""
}

func mockFullPrompt(msgs []Message) string {
	var b string
	for i, m := range msgs {
		if i > 0 {
			b += "\n"
		}
		b += m.Content
	}
	return b
}

func mockEstimateUsage(prompt, completion string) *Usage {
	pt := len(prompt) / 4
	if pt < 1 {
		pt = 1
	}
	ct := len(completion) / 4
	if ct < 1 {
		ct = 1
	}
	return &Usage{
		PromptTokens:     pt,
		CompletionTokens: ct,
		TotalTokens:      pt + ct,
	}
}

func (m *MockClient) recordLocked(method, prompt, model string) {
	m.calls = append(m.calls, MockCall{Method: method, Prompt: prompt, Model: model})
}

func (m *MockClient) nextChatIDLocked() string {
	m.nextID++
	return fmt.Sprintf("mock-chatcmpl-%d", m.nextID)
}

func firstMockChatMatch(prompt string, rules []mockChatRule) *mockChatRule {
	for i := range rules {
		if rules[i].pattern != "" && strings.Contains(prompt, rules[i].pattern) {
			return &rules[i]
		}
	}
	return nil
}

func firstMockErrMatch(prompt string, rules []mockErrRule) *mockErrRule {
	for i := range rules {
		if rules[i].pattern != "" && strings.Contains(prompt, rules[i].pattern) {
			return &rules[i]
		}
	}
	return nil
}

// ChatCompletions 返回注册的补全结果（非流式）
func (m *MockClient) ChatCompletions(ctx context.Context, req *ChatCompletionsRequest) (*ChatCompletionsResponse, error) {
	if req.Stream {
		return nil, &AegisGateError{Message: "use ChatCompletionsStream for streaming"}
	}
	_ = ctx

	m.mu.Lock()
	defer m.mu.Unlock()

	prompt := mockLastUserPrompt(req.Messages)
	m.recordLocked("ChatCompletions", prompt, req.Model)

	if hit := firstMockErrMatch(prompt, m.errRules); hit != nil {
		return nil, &APIError{
			Message:    hit.message,
			StatusCode: 400,
			ErrorCode:  hit.aegisCode,
			AegisCode:  hit.aegisCode,
		}
	}

	if hit := firstMockChatMatch(prompt, m.chatRules); hit != nil {
		content := hit.content
		return &ChatCompletionsResponse{
			ID:     m.nextChatIDLocked(),
			Object: "chat.completion",
			Model:  req.Model,
			Choices: []Choice{
				{
					Index: 0,
					Message: Message{
						Role:    "assistant",
						Content: content,
					},
					FinishReason: "stop",
				},
			},
			Usage: mockEstimateUsage(mockFullPrompt(req.Messages), content),
		}, nil
	}

	if m.defaultOK {
		return &ChatCompletionsResponse{
			ID:     m.nextChatIDLocked(),
			Object: "chat.completion",
			Model:  req.Model,
			Choices: []Choice{
				{
					Index: 0,
					Message: Message{
						Role:    "assistant",
						Content: m.defaultContent,
					},
					FinishReason: "stop",
				},
			},
			Usage: mockEstimateUsage(mockFullPrompt(req.Messages), m.defaultContent),
		}, nil
	}

	return nil, &APIError{
		Message:    fmt.Sprintf("未匹配的 prompt，请使用 MockChat / SetDefaultResponse: %q", prompt),
		StatusCode: 404,
		ErrorCode:  "MOCK_UNMATCHED",
		AegisCode:  "MOCK_UNMATCHED",
	}
}

// Health 返回固定健康数据
func (m *MockClient) Health(ctx context.Context) (*HealthResponse, error) {
	_ = ctx
	m.mu.Lock()
	defer m.mu.Unlock()
	m.recordLocked("Health", "", "")
	return &HealthResponse{Status: "ok", Version: "mock"}, nil
}

// ListModels 返回固定模型列表
func (m *MockClient) ListModels(ctx context.Context) (*ModelListResponse, error) {
	_ = ctx
	m.mu.Lock()
	defer m.mu.Unlock()
	m.recordLocked("ListModels", "", "")
	return &ModelListResponse{
		Object: "list",
		Data: []ModelInfo{
			{ID: "mock-model", Object: "model", OwnedBy: "aegisgate-mock"},
		},
	}, nil
}

// Calls 返回调用记录的副本
func (m *MockClient) Calls() []MockCall {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]MockCall, len(m.calls))
	copy(out, m.calls)
	return out
}

// CallCount 返回调用次数
func (m *MockClient) CallCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.calls)
}

// Reset 清空规则与调用记录
func (m *MockClient) Reset() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.chatRules = nil
	m.errRules = nil
	m.defaultOK = false
	m.defaultContent = ""
	m.nextID = 0
	m.calls = nil
}
