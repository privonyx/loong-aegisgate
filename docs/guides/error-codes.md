# AegisGate Error Code Reference (AEGIS-xxxx)

This document describes the structured error codes returned by the gateway, consistent with the definitions in `include/aegisgate/error_codes.h`. For the full OpenAPI description, see `docs/openapi.yaml` at the repository root.

## Error response format

Non-streaming APIs return JSON on failure with the following fields:

| Field | Description |
|------|-------------|
| `error.code` | Business error code, e.g. `AEGIS-1001` |
| `error.type` | Error category, e.g. `authentication_error`, `security_error` |
| `error.message` | Human-readable message (may be overridden by the gateway) |
| `error.doc_url` | Documentation anchor URL (may be returned on some paths) |

**Example:**

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

## Error code reference table

| Code | HTTP | Type | Default message |
|------|------|------|-----------------|
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

## Per-code notes

### AEGIS-1xxx (Authentication and authorization)

- **1001**: Missing `Authorization: Bearer`, key not in `auth.api_keys`, or environment variable not expanded. **Remediation**: Check `AEGISGATE_API_KEY` and the `auth` section in `config/aegisgate.yaml`.
- **1002**: Authenticated but the operation is not allowed (e.g. enterprise features unavailable on Community). **Remediation**: Verify `edition` / license and routing feature flags.
- **1003**: Wrong admin key or `auth.admin_key` not set when calling `/admin/*`. **Remediation**: Configure the admin key or inject via `${ENV}` from the environment; when `aegisctl` calls admin APIs, pass the **admin** key with `--api-key`.

### AEGIS-2xxx (Rate limiting and quota)

- **2001**: Token bucket exhausted (`rate_limit.max_tokens` / `refill_rate`). **Remediation**: Back off and retry on the client; or raise quotas (see [performance tuning](./performance-tuning.md)).
- **2002**: Business quota exceeded (when enabled). **Remediation**: Review cost/quota policy, rotate keys, or wait for the reset window.
- **2003**: Abuse detection triggered (`security.abuse_detection`). **Remediation**: Reduce probing or abusive requests; relax thresholds or shorten ban duration as needed.

### AEGIS-3xxx (Security guardrails)

- **3001**: Injection detection matched (rules in `config/rules/injection_patterns.yaml`). **Remediation**: Review prompts; tune rules or false-positive allowlists.
- **3002**: PII rules matched (`config/rules/pii_patterns.yaml`). **Remediation**: Redact before sending; or tighten or relax PII rules.
- **3003**: Topic out of scope (`config/rules/topic_whitelist.yaml`, etc.). **Remediation**: Adjust topic policy or user prompts.
- **3004**: Blocked by output-side content filtering. **Remediation**: Review output rules and actions (replace / truncate / alert).
- **3005**: Encoding attack detected (e.g. abnormal Base64, obfuscated encoding). **Remediation**: Simplify encoded payloads; adjust `security.encoding_*` settings.

### AEGIS-4xxx (Routing and upstream)

- **4001**: No usable model in `config/models.yaml` or routing cannot select a target. **Remediation**: Confirm `providers` and `models`, and that model names match the request.
- **4002**: Circuit breaker open. **Remediation**: Wait for recovery; inspect upstream error rate and fix the root cause.
- **4003**: Upstream timeout (per-provider `timeout_ms`). **Remediation**: Increase timeout, improve connectivity, or use a closer endpoint.
- **4004**: Upstream returned errors or all retries failed. **Remediation**: Check audit logs and upstream status codes; verify API keys and `base_url`.

### AEGIS-5xxx (Request validation)

- **5001**: JSON does not conform to the OpenAI Chat Completions shape. **Remediation**: Validate field types and enums against OpenAPI.
- **5002**: Body exceeds `limits.max_request_body_size`. **Remediation**: Compress context or raise the limit (mind memory and security).
- **5003**: Missing required fields (e.g. `model`, `messages`). **Remediation**: Add the fields and retry.

### AEGIS-9xxx (System)

- **9001**: Gateway not fully initialized (e.g. config not loaded). **Remediation**: Check startup logs and config file paths.
- **9002**: Unclassified internal error. **Remediation**: Set `logging.level: debug` and inspect stack traces; upgrade or file an issue.
- **9003**: Semantic cache requested but the cache subsystem is unavailable. **Remediation**: Check embedding model paths, ONNX dependencies, and `cache`/`storage` configuration.

## Handling errors in the SDK

The following examples show how to read `error.code` (often mapped to `aegis_code` / `AegisCode` in SDKs). Actual field names depend on each SDK version.

### Python

```python
import json
import httpx

from aegisgate import AegisGateAPIError

try:
    # When using raw httpx, parse the response body
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

With the official client, catch `AegisGateAPIError` and inspect `status_code` and `aegis_code`.

### Node.js (TypeScript)

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

## Related documents

- [Quick start](./quick-start.md)
- [Troubleshooting](./troubleshooting.md)
- [Security best practices](./security-best-practices.md)
