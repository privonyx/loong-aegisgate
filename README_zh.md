[English](README.md) | [中文](README_zh.md)

# AegisGate

[![CI](https://github.com/privonyx/loong-aegisgate/actions/workflows/ci.yml/badge.svg)](https://github.com/privonyx/loong-aegisgate/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/privonyx/loong-aegisgate?label=release)](https://github.com/privonyx/loong-aegisgate/releases)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![文档](https://img.shields.io/badge/docs-reference-blue)](docs/README_zh.md)
[![GitHub Discussions](https://img.shields.io/github/discussions/privonyx/loong-aegisgate)](https://github.com/privonyx/loong-aegisgate/discussions)
[![Good First Issues](https://img.shields.io/github/issues/privonyx/loong-aegisgate/good%20first%20issue?label=good%20first%20issues&color=7057ff)](https://github.com/privonyx/loong-aegisgate/labels/good%20first%20issue)

> **面向大语言模型应用的高性能 AI 网关。** 一个 OpenAI 兼容端点，集成安全护栏、
> Token 优化、语义缓存、智能路由与全链路可观测。Apache 2.0 开源，单二进制，**v1.0.0 正式版**。

```
请求 → [鉴权 + 限流] → [入站护栏] → [Token 优化] → [语义缓存] → [智能路由] → [出站护栏] → [可观测] → 响应
```

![AegisGate 管理后台](docs/assets/admin-dashboard.png)

## 功能特性

- **统一 AI 网关** — 通过单一 `/v1/chat/completions` 端点接入 OpenAI、Claude、DeepSeek、豆包、通义千问、Gemini、Mistral 及任何 OpenAI 兼容模型。智能路由（成本感知、ML 评分、A/B）、API Key 负载均衡、带熔断器的自动降级、令牌桶限流、真逐块 SSE 流式、Function Calling / Tool Use，以及多模态代理（嵌入、图像、音频）。
- **Token 优化** — Prompt 压缩（上下文截断、空白标准化、去重）、自动 `max_tokens` 计算、CJK 感知的 Token 估算，以及通过 `X-AegisGate-Tokens-Saved` 响应头 / SSE metadata 的逐请求节省可见。
- **安全护栏** — Unicode NFKC 归一化 + 零宽字符剥离、多层 Prompt 注入检测（中日韩/西里尔/英文）、基于 RE2 的 PII 脱敏（防 ReDoS）、可选外部安全 API（OpenAI Moderation / Perspective）、滥用检测、话题边界、出站内容过滤、幻觉评分，以及防篡改审计日志（FNV-1a 链 + AES-256-GCM）。
- **语义缓存** — 可插拔向量存储（hnswlib 进程内、Milvus、Qdrant）、可插拔 Embedder（默认 Hash 或 ONNX BGE-small-zh-v1.5 512 维）、按模型分区、TTL + LRU 淘汰、自适应阈值，以及缓存命中短路完全跳过模型调用。
- **可观测性** — `/metrics` 暴露 Prometheus 指标并附预置 Grafana 仪表盘、OpenTelemetry 追踪（可选）、带 Key 脱敏的结构化日志、带预算的成本追踪、输出质量评分与用量预测。
- **存储抽象** — `CacheStore` + `PersistentStore` 接口覆盖内存（默认）、SQLite（WAL）、PostgreSQL 与 Redis 后端；配置驱动，故障时优雅降级到内存。
- **多租户与 RBAC** *(企业版)* — SuperAdmin → TenantAdmin → Developer → Viewer 层级、租户隔离、API Key 生命周期、SSO（OIDC/PKCE + MFA/TOTP + SCIM 2.0），以及 React Web 管理面板。
- **插件与部署** — `dlopen` C-ABI 插件系统、规则市场（`aegisctl rules`）、Prompt 模板；附带 Docker、Helm Chart 与 Redis 集群模式。

→ 完整能力细节见[架构指南](docs/guides/architecture_zh.md)。

## 版本体系

单一二进制通过运行时 Feature Gate 支持两个版本：

| | 社区版（开源） | 企业版（授权） |
|---|---|---|
| 统一 API 代理 | ✅ | ✅ |
| Token 优化 | ✅ | ✅ |
| 路由 | 基础 | 智能路由（ML + A/B） |
| 护栏 | 基础 | 完整 + 自定义规则 |
| 管理 | CLI | Web 管理面板 |
| 缓存 | 进程内 LRU | Redis 分布式 |
| 存储 | SQLite | PostgreSQL |
| 部署 | 单机 | 集群（Helm） |
| 可观测 | Prometheus | + OTEL 追踪 + Grafana |
| SSO / RBAC / 审计报告 | — | ✅ |
| 插件系统 / 规则市场 | — | ✅ |

## 快速开始

自带 OpenAI key，5 分钟内看到真实缓存节省：

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate
docker build -t aegisgate:latest .

export OPENAI_API_KEY=sk-...
docker run --rm -it \
  -p 8080:8080 \
  -e OPENAI_API_KEY=$OPENAI_API_KEY \
  -v aegisgate-quickstart-data:/app/data \
  --entrypoint /usr/local/bin/quickstart-entrypoint.sh \
  aegisgate:latest
```

从启动 banner 读取自动生成的 API key，然后发起两次相同请求 —— 第二次命中缓存：

```bash
export QUICKSTART_KEY=...   # 从 banner 复制
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer $QUICKSTART_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Hello"}]}'
curl http://localhost:8080/admin/api/savings/summary \
  -H "Authorization: Bearer $QUICKSTART_KEY"
```

📖 **完整 5 分钟教程：** [docs/quickstart_zh.md](docs/quickstart_zh.md) · 从源码构建？
见[快速开始指南](docs/guides/quick-start_zh.md)。

## 文档

浏览完整[文档总览](docs/README_zh.md)。重点：

- [5 分钟快速上手](docs/quickstart_zh.md) — 零构建 Docker 体验
- [快速开始](docs/guides/quick-start_zh.md) — 从源码构建 + 配置
- [使用示例](docs/guides/usage-examples_zh.md) — 端到端 curl 会话
- [架构](docs/guides/architecture_zh.md) — 管道、路由、存储内部机制
- [生产部署](docs/guides/production-deployment_zh.md) — Docker、Helm、集群
- [SDK 集成](docs/guides/sdk-integration_zh.md) — Python / Node.js / Go
- [安全最佳实践](docs/guides/security-best-practices_zh.md) — 加固与密钥

## 客户端 SDK

| 语言 | 包名 | 特性 |
|------|------|------|
| [Python](sdk/python/) | `aegisgate` | 同步 + 异步，基于 httpx |
| [Node.js](sdk/nodejs/) | `@aegisgate/sdk` | TypeScript，原生 fetch，ESM |
| [Go](sdk/go/) | `aegisgate-go` | 零依赖，仅标准库 |

所有 SDK 支持对话补全（流式 + 非流式）、模型列表、健康检查、指标查询、配置重载。
见 [SDK 集成指南](docs/guides/sdk-integration_zh.md)。

## 社区

欢迎加入我们：

- **GitHub Discussions** — 提问、分享想法、展示作品：[Discussions](https://github.com/privonyx/loong-aegisgate/discussions)
- **Discord** — 与贡献者实时交流：[加入 Discord](https://discord.gg/aegisgate)
- **Good First Issues** — 入门友好任务：[Good First Issues](https://github.com/privonyx/loong-aegisgate/labels/good%20first%20issue)
- **用过 AegisGate？告诉我们** — 用 5 分钟表单分享你的省钱故事：[seed user feedback](https://github.com/privonyx/loong-aegisgate/issues/new?template=seed_user_feedback.yml)，或出现在 [`ADOPTERS.md`](ADOPTERS.md) 中。

## 参与贡献

我们欢迎所有形式的贡献 —— 代码、文档、Bug 报告与想法。

- 参见 [CONTRIBUTING.md](CONTRIBUTING.md)（[中文版](CONTRIBUTING_zh.md)）了解环境搭建、编码规范与 PR 指南
- 阅读 [Good First Issues 指南](docs/guides/good-first-issues.md) 获取入门任务
- 参与前请阅读[行为准则](CODE_OF_CONDUCT.md)
- 通过 [SECURITY.md](SECURITY.md) 私下报告安全漏洞

## 版本管理

AegisGate 遵循[语义版本](https://semver.org/lang/zh-CN/)规范。API 稳定性保证见 [VERSIONING.md](VERSIONING.md)，版本变更记录见 [CHANGELOG.md](CHANGELOG.md)。

## 许可证

[Apache License 2.0](LICENSE)

Copyright 2026 Loong Superbank
