# AegisGate 文档总览

AegisGate 所有指南与参考文档的中心索引。初次使用？从 [5 分钟快速上手](quickstart_zh.md) 开始。

> English documentation index: [Documentation](README.md)

## 快速上手

| 指南 | 说明 |
|------|------|
| [5 分钟快速上手](quickstart_zh.md) | 零构建 Docker 体验 — 5 分钟看到缓存节省 |
| [快速开始](guides/quick-start_zh.md) | 从源码构建、最小配置、首次对话调用 |
| [使用示例](guides/usage-examples_zh.md) | 覆盖所有核心功能的端到端 curl 会话 |
| [节省估算](estimate_zh.md) | 用 `aegisctl estimate` 估算 token/成本节省 |

## 架构与运维

| 指南 | 说明 |
|------|------|
| [架构](guides/architecture_zh.md) | 管道框架、路由、存储、时序图 |
| [生产部署](guides/production-deployment_zh.md) | Docker、Helm、集群模式、硬件要求 |
| [性能调优](guides/performance-tuning_zh.md) | 吞吐、延迟与资源优化 |
| [多区域](guides/multi-region_zh.md) | 地理分布式部署拓扑 |
| [灰度发布](guides/rollout_zh.md) | 渐进式发布与金丝雀策略 |
| [控制平面](guides/control-plane_zh.md) | 集中式配置与集群管理 |
| [故障排查](guides/troubleshooting_zh.md) | 常见问题与解决步骤 |
| [OTEL 离线依赖](guides/otel-offline-deps_zh.md) · [OTEL 验证](guides/otel-verification_zh.md) | OpenTelemetry 配置与校验 |

## 安全与合规

| 指南 | 说明 |
|------|------|
| [安全最佳实践](guides/security-best-practices_zh.md) | 加固、密钥、部署安全 |
| [外部安全 API](guides/external-safety_zh.md) | OpenAI Moderation / Perspective API 集成 |
| [自适应护栏](guides/adaptive-guard_zh.md) · [Guard 模型](guides/guard-model.md) | ONNX 安全分类 |
| [错误码](guides/error-codes_zh.md) | 完整 `AEGIS-xxxx` 错误码参考 |
| [合规对标](compliance/README_zh.md) | 欧盟 AI Act / ISO 42001 / 国内办法对标 |

## 功能与集成

| 指南 | 说明 |
|------|------|
| [功能列表清单](feature-list_zh.md) | 完整能力盘点：按模块分组、标注社区/企业版归属、逐项列出端点/配置键与相关指南链接 |
| [SDK 集成](guides/sdk-integration_zh.md) | Python / Node.js / Go 客户端 SDK |
| [管理 API](guides/admin-api_zh.md) | 管理 REST API 与 WebSocket 端点 |
| [节省看板](guides/admin-savings_zh.md) | 节省看板与报表 |
| [成本优化](guides/cost-optimization_zh.md) · [成本自治](guides/cost-autonomy_zh.md) | 预算控制与自主成本治理 |
| [多模态](guides/multimodal_zh.md) | 嵌入、图像、音频代理端点 |
| [会话缓存](guides/conversation-cache_zh.md) · [缓存迁移](guides/cache-migration_zh.md) | 语义缓存行为与迁移 |
| [供应商清单](guides/provider-manifest_zh.md) | 模型/供应商定义格式 |
| [反馈总线](guides/feedback-bus_zh.md) | 事件/反馈管道 |

## 示例应用

| 应用 | 说明 |
|------|------|
| [Showcase Demo](../apps/showcase/README_zh.md) | 大模型→AegisGate→应用的落地参考 Demo：通用骨架 + 可插拔场景（AI 漫剧旗舰 / 电商验证），价值面板直观呈现省钱与合规 |

## API 参考

| 参考 | 说明 |
|------|------|
| [OpenAPI 规范](openapi.yaml) | 机器可读 API 定义 |
| [OpenAI 兼容矩阵](openai-compat-matrix.md) | 支持的 OpenAI API 范围 |

## 定位与愿景

| 文档 | 说明 |
|------|------|
| [AegisOps 愿景](positioning/aegisops-vision_zh.md) | 产品/平台层战略 |

## 贡献与开发

| 指南 | 说明 |
|------|------|
| [Good First Issues](guides/good-first-issues.md) | 按技能级别整理的入门任务 |
| [Workflow 2.0](guides/workflow-2.0_zh.md) | 开发工作流 |
| [macOS 开发](guides/macos-development.md) | macOS 构建/开发说明 |
| [升级到 v1.0](guides/upgrade-v1.0.md) | 升级指南 |

## 项目根目录参考

- [更新日志](../CHANGELOG.md) · [版本管理](../VERSIONING.md) · [参与贡献](../CONTRIBUTING_zh.md) · [安全策略](../SECURITY.md) · [行为准则](../CODE_OF_CONDUCT.md) · [采用者](../ADOPTERS.md)
