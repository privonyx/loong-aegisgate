# AegisGate 错误码参考（AEGIS-xxxx）

本文档说明网关返回的结构化错误码，与 `include/aegisgate/error_codes.h` 中的定义一致。完整 OpenAPI 描述见仓库根目录 `docs/openapi.yaml`。

## 错误响应格式

非流式接口在失败时返回 JSON，字段如下：

| 字段 | 说明 |
|------|------|
| `error.code` | 业务错误码，形如 `AEGIS-1001` |
| `error.type` | 错误类别，如 `authentication_error`、`security_error` |
| `error.message` | 人类可读说明（可被网关覆盖） |
| `error.doc_url` | 文档锚点链接（部分路径可能返回） |

**示例：**

```json
{
  "error": {
    "code": "AEGIS-1001",
    "type": "authentication_error",
    "message": "Invalid or missing API key",
    "doc_url": "https://aegisgate.dev/docs/errors#AEGIS-1001"
  }
}
```

## 错误码总表

| Code | HTTP | Type | 默认消息（英文） |
|------|------|------|------------------|
| AEGIS-1001 | 401 | authentication_error | Invalid or missing API key |
| AEGIS-1002 | 403 | authentication_error | Insufficient permissions |
| AEGIS-1003 | 401 | authentication_error | Invalid or missing admin API key |
| AEGIS-2001 | 429 | rate_limit_error | Rate limit exceeded |
| AEGIS-2002 | 429 | rate_limit_error | Quota exceeded |
| AEGIS-2003 | 429 | rate_limit_error | Temporarily blocked due to repeated security violations |
| AEGIS-3001 | 403 | security_error | Request blocked: injection attack detected |
| AEGIS-3002 | 403 | security_error | Request blocked: sensitive information detected |
| AEGIS-3003 | 403 | security_error | Request blocked: topic not allowed |
| AEGIS-3004 | 403 | security_error | Response blocked by output guardrail |
| AEGIS-3005 | 403 | security_error | Request blocked: encoding attack detected |
| AEGIS-4001 | 503 | routing_error | No model available for routing |
| AEGIS-4002 | 503 | routing_error | Service temporarily unavailable (circuit breaker open) |
| AEGIS-4003 | 504 | routing_error | Upstream model request timed out |
| AEGIS-4004 | 502 | routing_error | All upstream models failed |
| AEGIS-5001 | 400 | validation_error | Invalid request body |
| AEGIS-5002 | 413 | validation_error | Request body exceeds maximum allowed size |
| AEGIS-5003 | 400 | validation_error | Required field is missing |
| AEGIS-9001 | 503 | system_error | Gateway not initialized |
| AEGIS-9002 | 500 | system_error | Internal server error |
| AEGIS-9003 | 503 | system_error | Semantic cache is not enabled |

## 分码说明

### AEGIS-1xxx（认证与权限）

- **1001**：未携带 `Authorization: Bearer`、Key 不在 `auth.api_keys`、或环境变量未展开。**处理**：检查 `AEGISGATE_API_KEY` 与 `config/aegisgate.yaml` 中 `auth` 配置。
- **1002**：已认证但当前操作不被允许（例如企业版功能在 Community 下不可用）。**处理**：核对 `edition` / 许可证与路由特性开关。
- **1003**：访问 `/admin/*` 时管理员密钥错误或未配置 `auth.admin_key`。**处理**：配置管理员 Key 或使用 `${ENV}` 从环境注入；`aegisctl` 调用管理接口时用 `--api-key` 传入**管理员**密钥。

### AEGIS-2xxx（限流与配额）

- **2001**：令牌桶耗尽（`rate_limit.max_tokens` / `refill_rate`）。**处理**：客户端退避重试；或调高配额（见[性能调优](./performance-tuning_zh.md)）。
- **2002**：超出业务配额（若启用）。**处理**：检查成本与配额策略、换 Key 或等待重置窗口。
- **2003**：滥用检测触发（`security.abuse_detection`）。**处理**：减少试探性恶意请求；按需放宽阈值或缩短封禁时间。

### AEGIS-3xxx（安全护栏）

- **3001**：注入检测命中（规则见 `config/rules/injection_patterns.yaml`）。**处理**：审查提示词；调整规则或误报白名单。
- **3002**：PII 规则命中（`config/rules/pii_patterns.yaml`）。**处理**：脱敏后再发送；或收紧/放宽 PII 规则。
- **3003**：主题越界（`config/rules/topic_whitelist.yaml` 等）。**处理**：修改话题策略或用户提示。
- **3004**：输出侧内容过滤拦截。**处理**：检查输出规则与动作（替换/截断/告警）。
- **3005**：编码攻击检测（如异常 Base64、混淆编码）。**处理**：简化编码内容；调整 `security.encoding_*` 配置。

### AEGIS-4xxx（路由与上游）

- **4001**：`config/models.yaml` 中无可用模型或路由无法选中目标。**处理**：确认 `providers` 与 `models` 列表、模型名与请求一致。
- **4002**：熔断器打开。**处理**：等待恢复；检查上游错误率并修复根因。
- **4003**：上游超时（各 provider `timeout_ms`）。**处理**：增大超时、优化网络、换近端线路。
- **4004**：上游返回错误或全部重试失败。**处理**：查看审计日志与上游状态码；校验 API Key 与 `base_url`。

### AEGIS-5xxx（请求校验）

- **5001**：JSON 结构不符合 OpenAI Chat Completions 约定。**处理**：对照 OpenAPI 校验字段类型与枚举。
- **5002**：Body 超过 `limits.max_request_body_size`。**处理**：压缩上下文或调大限制（注意内存与安全）。
- **5003**：缺少必填字段（如 `model`、`messages`）。**处理**：补齐字段后重试。

### AEGIS-9xxx（系统）

- **9001**：网关未完成初始化（配置未加载等）。**处理**：检查启动日志与配置文件路径。
- **9002**：未分类内部异常。**处理**：开启 `logging.level: debug` 并查看栈信息；升级版本或提交 Issue。
- **9003**：请求语义缓存但缓存子系统不可用。**处理**：检查嵌入模型路径、ONNX 依赖与 `cache`/`storage` 配置。

## 在 SDK 中处理错误

以下示例演示如何读取 `error.code`（SDK 中常映射为 `aegis_code` / `AegisCode`）。实际字段名以各 SDK 版本为准。

### Python

```python
import json
import httpx

from aegisgate import AegisGateAPIError

try:
    # 若使用底层 httpx，可解析 body
    r = httpx.post(
        "http://127.0.0.1:8080/v1/chat/completions",
        headers={"Authorization": "Bearer sk-xxx"},
        json={"model": "gpt-4o", "messages": [{"role": "user", "content": "hi"}]},
        timeout=60.0,
    )
    r.raise_for_status()
except httpx.HTTPStatusError as e:
    body = e.response.text
    try:
        data = e.response.json()
        code = data.get("error", {}).get("code")
        print("AEGIS code:", code)
    except json.JSONDecodeError:
        print("Raw body:", body)
```

使用官方客户端时，捕获 `AegisGateAPIError` 并检查 `status_code` 与 `aegis_code`。

### Node.js（TypeScript）

```typescript
import { AegisGateClient, AegisGateAPIError } from '@aegisgate/sdk';

const client = new AegisGateClient({ apiKey: 'sk-xxx', baseUrl: 'http://127.0.0.1:8080' });

try {
  await client.chat('hello', { model: 'gpt-4o' });
} catch (e) {
  if (e instanceof AegisGateAPIError) {
    console.error(e.statusCode, e.aegisCode, e.message);
  }
  throw e;
}
```

### Go

```go
package main

import (
    "errors"
    "fmt"
    aegisgate "github.com/privonyx/loong-aegisgate-go"
)

func main() {
    _, err := client.ChatCompletions(ctx, req)
    var apiErr *aegisgate.APIError
    if errors.As(err, &apiErr) {
        fmt.Println(apiErr.StatusCode, apiErr.AegisCode, apiErr.Message)
    }
}
```

## 相关文档

- [快速开始](./quick-start_zh.md)
- [故障排查](./troubleshooting_zh.md)
- [安全最佳实践](./security-best-practices_zh.md)
