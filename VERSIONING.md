# Versioning & API Stability

本文档定义 AegisGate 的语义版本策略、API 稳定性承诺和弃用流程。

## 语义版本 (Semantic Versioning)

AegisGate 遵循 [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html)：

```
MAJOR.MINOR.PATCH
```

| 组件 | 递增条件 | 示例 |
|------|---------|------|
| **MAJOR** | 不兼容的 API 变更 | 移除已弃用端点、请求/响应格式变更 |
| **MINOR** | 向后兼容的功能新增 | 新增端点、新增可选字段、新增配置项 |
| **PATCH** | 向后兼容的缺陷修复 | Bug 修复、性能优化、文档修正 |

### 预发布标识

- `x.y.z-alpha.N` — 功能不完整，API 可能变更
- `x.y.z-beta.N` — 功能完整，API 基本稳定，待验证
- `x.y.z-rc.N` — 发布候选，仅修复关键缺陷

## 当前版本状态

**当前版本：1.0.0 (GA)**

AegisGate v1.0.0 是首个 **General Availability (GA)** 版本。自 v1.0.0 起，公共 API 端点正式进入**稳定**状态：

| 版本范围 | 稳定性级别 | 含义 |
|---------|-----------|------|
| `0.1.0` – `0.4.0` | **Experimental** | 初始开发，API 快速演进 |
| `0.5.0` – `0.9.0` | **Preview** | 核心 API 路径稳定，字段扩展向后兼容 |
| **`1.0.0`+** | **Stable** ✅ | **正式 API 稳定承诺**，遵循完整语义版本规范 |

### 稳定性承诺

自 v1.0.0 起：

- `/v1/chat/completions`、`/v1/embeddings` 等所有 `/v1/` 前缀的端点保证**向后兼容**
- 请求/响应 JSON 结构中的**现有字段不会被移除或改变语义**
- 新增字段采用**可选字段**方式，不影响现有客户端
- 任何不兼容变更（breaking change）将在 **v2.0.0** 中引入，并提供至少两个 minor 版本的弃用缓冲期

## API 表面定义

### 公共 API（Public API）

以下构成 AegisGate 的公共 API，受语义版本保护：

1. **HTTP 端点**
   - `POST /v1/chat/completions` — OpenAI 兼容聊天补全
   - `GET /v1/models` — 模型列表
   - `GET /health/live` — 存活检查
   - `GET /health/ready` — 就绪检查
   - `GET /metrics` — Prometheus 指标端点
   - `GET /cache/stats` — 缓存统计

2. **请求/响应 JSON Schema**
   - `ChatCompletionsRequest` 及其字段
   - `ChatCompletionsResponse` 及其字段
   - `ErrorResponse` 信封格式及 AEGIS 错误码

3. **HTTP 头**
   - `Authorization: Bearer <api-key>` — 认证
   - `X-AegisGate-Version` — 网关版本响应头（所有 API 响应）
   - `X-AegisGate-Tokens-Saved` — Token 节省量响应头
   - `traceparent` — W3C Trace Context 传播

4. **配置文件格式**
   - `config/aegisgate.yaml` 的顶层结构和已文档化的配置键

5. **Client SDK 公共接口**
   - Python: `aegisgate.Client`, `aegisgate.AsyncClient`
   - Node.js: `@aegisgate/sdk` 导出的类和方法
   - Go: `aegisgate` 包导出的类型和函数

### 内部 API（Internal API）

以下**不属于**公共 API，可在任何版本中变更：

- 管理端点 (`/admin/*`) — 企业版内部管理接口
- C++ 头文件和内部类接口
- 数据库 Schema 和存储层实现
- Prometheus 指标名称（新增指标不算破坏性变更，重命名/删除算）
- WebSocket 推送消息格式
- CLI 命令输出格式（退出码除外）

## 兼容性保证

### 1.0.0 之前（当前）

在 Preview 阶段（`0.5.0`+），承诺：

- **新增字段**不破坏已有客户端（JSON 响应中的新可选字段）
- **新增端点**不影响已有端点行为
- **已有必选字段**的语义不变
- **MINOR 版本**升级不要求客户端代码变更（但建议更新 SDK）
- **不兼容变更**在 CHANGELOG 中以 `⚠️ BREAKING` 标记

### 1.0.0 及之后

正式稳定后，承诺：

- **同一 MAJOR 版本内**的所有 MINOR/PATCH 升级保持向后兼容
- **弃用的功能**至少保留一个 MINOR 版本周期
- **不兼容变更**仅在 MAJOR 版本递增时引入

## 弃用流程

```
v1.x  引入替代方案 + 标记弃用
  ↓   CHANGELOG 记录弃用，文档标注 @deprecated
v1.y  弃用功能保持运行，日志输出 WARN 级别弃用提醒
  ↓   （至少 1 个 MINOR 版本间隔）
v2.0  移除已弃用功能
```

### 弃用标记规范

**CHANGELOG 中：**
```markdown
### Deprecated
- `GET /v1/models` 的 `legacy_field` 字段 — 将在 v2.0 移除，使用 `new_field` 替代
```

**HTTP 响应中：**
```http
Deprecation: true
Sunset: Sat, 01 Jan 2027 00:00:00 GMT
Link: <https://docs.aegisgate.dev/migration/v2>; rel="deprecation"
```

**日志中：**
```
[WARN] Deprecated: /v1/legacy-endpoint will be removed in v2.0. Use /v1/new-endpoint instead.
```

## 版本号同步

以下文件中的版本号必须保持同步：

| 文件 | 版本字段 | 说明 |
|------|---------|------|
| `src/CMakeLists.txt` | `project(aegisgate VERSION x.y.z)` | 唯一真相来源 (Single Source of Truth) |
| `src/server/version.h.in` | 模板，由 CMake 自动生成 | `AEGISGATE_VERSION` 宏通过 `configure_file` 从 CMake 注入，**无需手动修改** |
| `vcpkg.json` | `"version": "x.y.z"` | |
| `docs/openapi.yaml` | `info.version: x.y.z` | |
| `sdk/python/pyproject.toml` | `version = "x.y.z"` | |
| `sdk/nodejs/package.json` | `"version": "x.y.z"` | |
| `web/admin/package.json` | `"version": "x.y.z"` | |
| `helm/aegisgate/Chart.yaml` | `appVersion: "x.y.z"` | |
| `deploy/helm/aegisgate/Chart.yaml` | `appVersion: "x.y.z"` | |
| `CHANGELOG.md` | 最新版本条目 | |

**发布检查清单：**

1. 更新 `src/CMakeLists.txt` 中的 `PROJECT_VERSION`（C++ 二进制版本自动同步）
2. 更新上述其他文件中的版本号
3. 更新 `CHANGELOG.md` — 将 `[Unreleased]` 中的内容移入新版本条目
4. 提交：`git commit -m "release: vx.y.z"`
5. 打标签：`git tag -a vx.y.z -m "Release x.y.z"`
6. 推送：`git push origin main --tags`

## SDK 版本策略

Client SDK 版本**独立于**服务端版本，但遵循以下对应关系：

| SDK 版本 | 兼容的服务端版本 |
|---------|----------------|
| `0.x.y` | 与同期服务端 `0.x.y` 配套发布 |
| `1.0.0` | 保证与服务端 `1.x.y` 全系列兼容 |

SDK 的 MAJOR 版本递增表示 SDK 公共接口的不兼容变更，与服务端 MAJOR 版本无强绑定。

## 变更类型与版本影响

| 变更类型 | 版本影响 | 示例 |
|---------|---------|------|
| 新增可选请求字段 | MINOR | 新增 `tools` 字段到 `ChatCompletionsRequest` |
| 新增响应字段 | MINOR | 新增 `X-AegisGate-Tokens-Saved` 响应头 |
| 新增端点 | MINOR | 新增 `GET /cache/stats` |
| 新增配置项（有默认值）| MINOR | 新增 `token_optimization` 配置段 |
| Bug 修复 | PATCH | 修复 token 计数不准确 |
| 性能优化 | PATCH | 优化审计写入延迟 |
| 移除端点 | MAJOR | 移除 `GET /v1/legacy` |
| 必选字段语义变更 | MAJOR | `model` 字段含义变更 |
| 响应格式变更 | MAJOR | `ErrorResponse` 结构变更 |

## FAQ

**Q: 为什么当前版本是 0.9.0 而不是 1.0.0？**

A: 1.0.0 代表公共 API 的正式稳定承诺。AegisGate 需要在生产环境中经过验证后才会发布 1.0.0，届时 API 变更将受到严格的语义版本约束。

**Q: 升级到新 MINOR 版本需要改客户端代码吗？**

A: 不需要。新 MINOR 版本仅添加功能，不改变已有行为。但建议阅读 CHANGELOG 了解新增能力。

**Q: 企业版功能影响版本号吗？**

A: 是的。社区版和企业版共享同一版本号。企业版新增功能同样遵循语义版本规则。
