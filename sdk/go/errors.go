package aegisgate

import "fmt"

// AegisGateError 基础异常
type AegisGateError struct {
	Message string
}

func (e *AegisGateError) Error() string {
	return e.Message
}

// HTTPStatusCoder 可获取 HTTP 状态码的接口
type HTTPStatusCoder interface {
	HTTPStatusCode() int
}

// APIError API 请求失败
type APIError struct {
	Message      string
	StatusCode   int
	ErrorCode    string
	// AegisCode 网关业务错误码（AegisGate 扩展）
	AegisCode    string
	ResponseBody string
}

// HTTPStatusCode 返回 HTTP 状态码（值接收器以支持嵌入类型）
func (e APIError) HTTPStatusCode() int {
	return e.StatusCode
}

func (e *APIError) Error() string {
	if e.StatusCode > 0 {
		return fmt.Sprintf("%s (status: %d)", e.Message, e.StatusCode)
	}
	return e.Message
}

// Unwrap 实现 errors.Unwrap 以支持 errors.Is
func (e *APIError) Unwrap() error {
	return &AegisGateError{Message: e.Message}
}

// AuthenticationError 认证失败 (401)
type AuthenticationError struct {
	APIError
}

// ForbiddenError 禁止访问 (403)
type ForbiddenError struct {
	APIError
}

// SecurityError 安全策略拒绝（403，语义化别名，可与 ForbiddenError 区分使用）
type SecurityError struct {
	APIError
}

// RateLimitError 请求频率超限 (429)
type RateLimitError struct {
	APIError
}

// BadGatewayError 网关错误 (502)
type BadGatewayError struct {
	APIError
}

// ServiceUnavailableError 服务不可用 (503)
type ServiceUnavailableError struct {
	APIError
}

// ConnectionError 网络连接异常
type ConnectionError struct {
	AegisGateError
}

// TimeoutError 请求超时
type TimeoutError struct {
	AegisGateError
}
