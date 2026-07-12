# Provider Manifest 指南

> 覆盖功能：Provider Manifest + Conformance 套件（Phase 10.1）
> 可用版本：v1.1+（在 v3.0 路线图 Phase 10 框架下渐进交付）

本指南介绍如何：

1. 读懂一个官方 AegisGate Provider Manifest
2. 为新 / 第三方 Provider 编写 Manifest
3. 用 `aegisctl conformance` 校验 Manifest
4. 在 CI 中把 Manifest 作为契约使用

完整规范见 [`api/providers/manifest.md`](../../api/providers/manifest.md) 与 [`api/providers/provider.schema.json`](../../api/providers/provider.schema.json)。

## 为什么需要 Manifest

在此之前，新增一个 provider 必须 fork C++ 源码：

```text
src/gateway/connector/myprovider.{h,cpp}  # 实现 ModelConnector
src/gateway/connector/factory.cpp         # 注册 type
config/models.yaml                        # 增加 YAML 项
```

有了 **Provider Manifest**，"契约"（身份 / 端点 / 认证 / OpenAI 兼容度 / 能力）以 YAML 表达，可以：

- 在合入前由 `aegisctl conformance` 校验
- 被 IDE / 编辑器消费做语法提示
- 随第三方插件 / SDK 一起发布，不必 fork AegisGate

Manifest 与 `config/models.yaml` **正交共存**：

| | Manifest (`api/providers/definitions/*.yaml`) | 运行期配置 (`config/models.yaml`) |
|---|---|---|
| 角色 | 契约 — "这个 provider 是什么" | 实例 — "在这里如何运行" |
| 生命周期 | 随 provider 实现发布一次 | 每次部署按需修改 |
| 读取者 | CI、IDE、`aegisctl conformance` | 网关运行期、`ConnectorFactory` |

## 1. 读懂官方 Manifest

以 `api/providers/definitions/openai.yaml` 为例：

```yaml
apiVersion: aegisgate.dev/v1alpha1
kind: ProviderManifest
metadata:
  name: openai
  display_name: OpenAI
  maturity: stable
spec:
  connector:
    kind: openai
  endpoint:
    base_url_default: https://api.openai.com/v1
  auth:
    type: bearer
    env_var: OPENAI_API_KEY
  compatibility:
    openai_chat_completions: full
  capabilities:
    - streaming
    - tools
    - vision
```

关键字段：

- `metadata.name` — 全局唯一 ID，同时作为 `config/models.yaml` 中的 connector type
- `spec.connector.kind` — 由哪个内置 connector 承载（`openai` / `claude` / 将来的 `external`）
- `spec.compatibility.openai_chat_completions` — 兼容等级：`full` / `partial` / `translated` / `none`
- `spec.capabilities` — 声明的能力枚举

## 2. 为新 provider 编写 Manifest

### Step 1 — 从模板复制

```bash
cp api/providers/definitions/openai.yaml api/providers/definitions/my-provider.yaml
```

### Step 2 — 填写 `metadata`

```yaml
metadata:
  name: my-provider                 # [a-z0-9_-]{1,64}
  display_name: My LLM Provider
  vendor: ACME Corp
  homepage: https://example.com
  documentation: https://example.com/docs
  tags: [commercial, chat]
  maturity: preview                  # experimental | preview | stable
```

### Step 3 — 声明端点与认证

```yaml
spec:
  connector:
    kind: openai                     # 如果 provider 与 OpenAI 兼容，直接复用
  endpoint:
    base_url_default: https://api.example.com/v1
    base_url_env: MY_PROVIDER_BASE_URL
  auth:
    type: bearer
    header_name: Authorization
    env_var: MY_PROVIDER_API_KEY
    supports_multi_key: true
```

### Step 4 — 声明 OpenAI 兼容度

```yaml
  compatibility:
    openai_chat_completions: full
    fields:
      - name: messages
        status: supported
      - name: tools
        status: supported
      - name: response_format
        status: unsupported
        notes: 上游尚未支持。
```

### Step 5 — 声明能力

```yaml
  capabilities:
    - streaming
    - tools
    - system_message
    - temperature
    - top_p
    - max_tokens
```

必须来自枚举：
`streaming | tools | vision | response_format | logprobs | system_message | temperature | top_p | max_tokens`

### Step 6 —（可选）声明契约

```yaml
  models:
    - id: my-model-large
      max_context_tokens: 128000
      capabilities: [streaming, tools]
      region_hints: [us-east]

  conformance:
    required_checks:
      - manifest-shape
      - auth-defined
      - compatibility-declared
      - capability-enum
      - models-unique
      - sample-request-shape
    sample_request:
      model: my-model-large
      messages:
        - role: user
          content: ping
      max_tokens: 16
```

## 3. 用 `aegisctl conformance` 校验

```bash
# 单个 Manifest
aegisctl conformance check api/providers/definitions/my-provider.yaml

# 整个目录
aegisctl conformance check-all api/providers/definitions/
```

样例输出：

```
PASS  api/providers/definitions/my-provider.yaml  (errors=0, warnings=0)
```

失败时每条问题都带稳定的机器可读 code，方便 CI 断言：

```
FAIL  api/providers/definitions/my-provider.yaml  (errors=1, warnings=0)
    [E] spec.auth.type.unknown :: Unknown auth.type; expected bearer, api_key_header, query, or none  (spec.auth.type)
```

所有 issue code 在 `api/providers/manifest.md` §11 中列出。

## 4. CI 集成

```yaml
# .github/workflows/manifest-check.yml
name: Provider Manifest Conformance
on: [push, pull_request]
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build aegisctl
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target aegisctl
      - name: Run conformance
        run: ./build/aegisctl conformance check-all api/providers/definitions/
```

任何 Error 级问题都会让 `aegisctl conformance` 以非零码退出，阻断合入。

## 5. 从代码中使用 Manifest

```cpp
#include "gateway/provider_spec/provider_manifest.h"

aegisgate::ValidationReport report;
auto manifest = aegisgate::loadManifestFromFile("api/providers/definitions/openai.yaml", report);
if (!manifest || !report.ok()) {
    // 处理错误：report.issues[]
}

auto validation = aegisgate::validateManifest(*manifest);
auto conformance = aegisgate::runConformanceChecks(*manifest);
// 均返回 ValidationReport，可用 .ok() / .errorCount() / .warningCount()
```

## 6. 后续路线图

| Phase | 项目 |
|-------|------|
| ✅ 10.1.1 | Manifest schema + JSON Schema + Conformance v0 |
| 10.1.3 | `external` connector — gRPC sidecar 协议 |
| 10.1.4 | Runtime 启动时与 `config/models.yaml` 交叉校验 |
| 10.2 | WASM 插件通过 Manifest 协商能力 |

## 7. 相关文档

- [Provider Manifest 规范](../../api/providers/manifest.md)
- [JSON Schema](../../api/providers/provider.schema.json)
- [OpenAI 兼容性矩阵](../openai-compat-matrix.md)
