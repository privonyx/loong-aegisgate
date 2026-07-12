[English](feature-list.md) | [中文](feature-list_zh.md)

# AegisGate 功能列表清单

> **面向：** 产品选型、能力盘点、对外/对内 pitch 与 RFP 应答。
> **版本基线：** v1.0.0 GA（单一二进制，运行时 Feature Gate 决定社区版 / 企业版能力）。
> **对齐来源：** `README_zh.md` Features 段、[架构指南](guides/architecture_zh.md)、`docs/guides/*.md`、[OpenAPI](openapi.yaml)、[OpenAI 兼容矩阵](openai-compat-matrix.md)、[ROADMAP v4](ROADMAP_v4_zh.md)。

## 图例

| 记号 | 含义 |
|:----:|------|
| 🟢 社区版 | Apache-2.0 开源，单二进制默认可用 |
| 🔵 企业版 | 需运行时 Feature Gate 授权 |
| ⚪ 双版通用 | 两个版本均可用（同一实现，无 Feature Gate 门控） |
| 🧪 预览 | 已可用但仍在打磨（预览期，接口可能调整） |
| 📐 规划中 | 路线图，尚未 GA |

关键约束：**单一 OpenAI 兼容端点** + **单二进制** + **配置驱动**（一份 YAML 即可切换所有能力开关）。

---

## 目录

1. [统一 AI 网关](#1-统一-ai-网关)
2. [Token 优化](#2-token-优化)
3. [安全护栏](#3-安全护栏)
4. [语义缓存](#4-语义缓存)
5. [可观测性](#5-可观测性)
6. [存储抽象](#6-存储抽象)
7. [多租户 · RBAC · 认证](#7-多租户--rbac--认证企业版)
8. [控制平面](#8-控制平面)
9. [成本自治与 AegisOps](#9-成本自治与-aegisops)
10. [部署与集群](#10-部署与集群)
11. [插件与生态](#11-插件与生态)
12. [客户端 SDK](#12-客户端-sdk)
13. [CLI 工具](#13-cli-工具)
14. [API 端点索引](#14-api-端点索引)
15. [版本能力矩阵](#15-版本能力矩阵)

---

## 1. 统一 AI 网关

单一 `/v1/chat/completions` 端点对外，统一 OpenAI 兼容协议接入 7 家主流大模型与任意 OpenAI 兼容后端。

| 能力 | 说明 | 归属 | 关键端点 / 配置 | 相关指南 |
|------|------|:----:|-----------------|----------|
| OpenAI 兼容 Chat Completions | 单一 `POST /v1/chat/completions`，请求/响应完全对齐 OpenAI；`stream=true` 支持逐块 SSE。 | ⚪ | `/v1/chat/completions` · `providers.*` | [架构指南](guides/architecture_zh.md) · [使用示例](guides/usage-examples_zh.md) |
| 多提供商内置连接器 | OpenAI / Anthropic Claude / DeepSeek / 火山豆包 / 通义千问 / Google Gemini / Mistral 7 家开箱即用；claude 走 OpenAI ↔ Anthropic 双向翻译。 | ⚪ | `api/providers/definitions/*.yaml` | [OpenAI 兼容矩阵](openai-compat-matrix.md) · [供应商清单](guides/provider-manifest_zh.md) |
| 任意 OpenAI 兼容后端 | 通过声明式 Manifest 注册第三方模型端点（本地推理、私有部署、代理层等）。 | ⚪ | `api/providers/definitions/<name>.yaml` | [供应商清单](guides/provider-manifest_zh.md) |
| 智能路由（BasicRouter） | 显式 `model` → tag → 默认模型，无 ML 依赖。 | 🟢 | `routing.type: basic` | [架构指南 · 路由](guides/architecture_zh.md) |
| 智能路由（CostAwareRouter） | 按 prompt 字符数分档（economy / standard / premium），成本感知。 | ⚪ | `routing.type: cost_aware` | [成本优化](guides/cost-optimization_zh.md) |
| 智能路由（MLRouter） | 三维加权评分（成本 × 质量 × 延迟），EMA 动态更新；权重可配。 | 🔵 | `routing.type: ml` | [架构指南 · MLRouter](guides/architecture_zh.md) · [成本优化](guides/cost-optimization_zh.md) |
| A/B 测试路由（ABTestRouter） | 装饰器包装任意 Router，`hash(experiment + request_id) % weight` 确定性流量分配。 | 🔵 | `routing.ab_tests[]` | [架构指南 · ABTestRouter](guides/architecture_zh.md) |
| 地理路由（GeoRouter） | 装饰器按客户端地域路由到最近区域后端。 | 🔵 | `routing.geo.*` | [多区域](guides/multi-region_zh.md) |
| API Key 负载均衡 | 单模型多 Key 池轮转（round-robin / random），配额独立、失败隔离。 | ⚪ | `providers.<name>.api_keys[]` | [架构指南](guides/architecture_zh.md) |
| 自动降级与熔断 | FallbackManager 主备切换 + CircuitBreaker（Closed → Open → HalfOpen），阈值/超时可配。 | ⚪ | `providers.<name>.fallback` · `circuit_breaker.*` | [架构指南 · 降级时序](guides/architecture_zh.md) |
| 令牌桶限流 | 每 API Key / 每租户令牌桶配额（`rate_limit.max_tokens` + `rate_limit.refill_rate`；可按请求数或 token 成本扣减）。支持 Redis 集群级配额。 | ⚪ | `rate_limit.*` | [架构指南](guides/architecture_zh.md) · [安全最佳实践](guides/security-best-practices_zh.md) |
| SSE 流式（真逐块） | 每 chunk 独立可观测，与出站护栏、成本计算、缓存写回全兼容；`stream: true`。 | ⚪ | `/v1/chat/completions` `stream=true` | [使用示例](guides/usage-examples_zh.md) |
| Function Calling / Tool Use | OpenAI `tools` / `tool_calls` 与 Anthropic `tool_use` / `tool_result` 双向翻译。 | ⚪ | `tools[]` / `tool_choice` | [OpenAI 兼容矩阵](openai-compat-matrix.md) |
| 多模态代理 · 嵌入 | `POST /v1/embeddings`（文本→向量）。 | ⚪ | `/v1/embeddings` | [多模态](guides/multimodal_zh.md) |
| 多模态代理 · 图像生成 | `POST /v1/images/generations`。 | ⚪ | `/v1/images/generations` | [多模态](guides/multimodal_zh.md) |
| 多模态代理 · 音频转写 | `POST /v1/audio/transcriptions`（ASR）。 | ⚪ | `/v1/audio/transcriptions` | [多模态](guides/multimodal_zh.md) |
| 多模态代理 · 音频翻译 | `POST /v1/audio/translations`。 | ⚪ | `/v1/audio/translations` | [多模态](guides/multimodal_zh.md) |
| 多模态代理 · 语音合成 | `POST /v1/audio/speech`（TTS）。 | ⚪ | `/v1/audio/speech` | [多模态](guides/multimodal_zh.md) |
| 模型列表 | `GET /v1/models`（OpenAI 兼容）。 | ⚪ | `/v1/models` | [OpenAPI](openapi.yaml) |

---

## 2. Token 优化

在不改变业务语义的前提下，压缩 Prompt、精算 `max_tokens`，把节省显式回传给应用侧。

| 能力 | 说明 | 归属 | 关键配置 / 端点 | 相关指南 |
|------|------|:----:|-----------------|----------|
| Prompt 压缩 · 上下文截断 | 保留系统消息与最新窗口，安全丢弃过老历史。 | ⚪ | `token_optimization.prompt_compression.max_context_messages` | [成本优化](guides/cost-optimization_zh.md) |
| Prompt 压缩 · 空白标准化 | 折叠冗余空白、换行、缩进。 | ⚪ | `token_optimization.prompt_compression.compress_whitespace: true` | [成本优化](guides/cost-optimization_zh.md) |
| Prompt 压缩 · 消息去重 | 相邻同角色重复消息合并。 | ⚪ | `token_optimization.prompt_compression.dedup_system_prompts: true` | [成本优化](guides/cost-optimization_zh.md) |
| 自动 `max_tokens` 计算 | 按模型上下文 - 输入 token 数动态计算安全上限，避免 upstream 拒绝或截断。 | ⚪ | `token_optimization.smart_max_tokens.enabled: true` | [成本优化](guides/cost-optimization_zh.md) |
| CJK 感知 Token 估算 | 针对中日韩场景优化的近似 tokenizer，避免用 word-count 低估。 | ⚪ | 内置（无需配置） | [成本优化](guides/cost-optimization_zh.md) |
| 节省可见 · 响应头 | 每次响应回传 `X-AegisGate-Tokens-Saved` HTTP 头。 | ⚪ | 内置 | [使用示例](guides/usage-examples_zh.md) |
| 节省可见 · SSE metadata | 流式模式在结束 chunk 附带节省 metadata。 | ⚪ | 内置 | [使用示例](guides/usage-examples_zh.md) |
| 节省汇总 API | `GET /admin/api/savings/summary` 汇总节省统计。 | 🔵 | `/admin/api/savings/*` | [节省看板](guides/admin-savings_zh.md) |
| 节省估算 CLI | `aegisctl estimate` 基于历史日志估算迁移到 AegisGate 后的节省。 | ⚪ | `aegisctl estimate` | [节省估算](estimate_zh.md) |

---

## 3. 安全护栏

多层纵深防御，覆盖 Prompt 注入、PII 泄漏、内容合规、幻觉检测与审计留痕。

### 3.1 入站护栏（Inbound Pipeline · 8 Stage）

| Stage / 能力 | 说明 | 归属 | 关键配置 | 相关指南 |
|------|------|:----:|----------|----------|
| ① AuditLogger（入站） | 请求入库前留痕；防篡改哈希链（FNV-1a）+ AES-256-GCM 详情加密。 | ⚪ | `audit.*` | [安全最佳实践](guides/security-best-practices_zh.md) |
| ② Preprocessor · NFKC 归一化 | Unicode NFKC + 零宽字符（ZWSP/ZWNJ/ZWJ）剥离；防同形字/宽窄字符绕过。 | ⚪ | `security.unicode_normalization: true` | [架构指南 · 护栏层级](guides/architecture_zh.md) |
| ③ Injection Detection · L1 关键词 | CJK / 西里尔 / 英文 多语种关键词与启发式（角色切换、特殊字符密度）。 | ⚪ | `security.encoding_detection: true`（L1 关键词内建） | [安全最佳实践](guides/security-best-practices_zh.md) |
| ③ Injection Detection · L2 编码检测 | Base64 / Hex / URL 解码后二次扫描。 | ⚪ | `security.encoding_detection: true` · `security.encoding_min_base64_length` | [安全最佳实践](guides/security-best-practices_zh.md) |
| ④ GuardModel · L3 ONNX 分类 | 可选神经网络安全分类器（~5ms），提供 confidence + explanation。 | 🔵 | `security.guard_model.model_path` | [Guard 模型](guides/guard-model.md) · [自适应护栏](guides/adaptive-guard_zh.md) |
| ⑤ PII Filter | RE2 线性时间正则（防 ReDoS）：手机 / 邮箱 / 身份证 / 银行卡 / API Key / 私钥；可配置 mask 或 reject。 | ⚪ | `—（管线 stage · 无顶层 YAML 根键 · 见指南）` | [安全最佳实践](guides/security-best-practices_zh.md) |
| ⑥ TopicGuard | 话题白名单 / 黑名单；归一化文本消费防绕过。 | ⚪ | `—（管线 stage · 无顶层 YAML 根键 · 见指南）` | [架构指南](guides/architecture_zh.md) |
| ⑦ RuleEngine（自定义规则） | YAML 声明式规则引擎；per-tenant 规则集；热重载。 | 🔵 | `config/rules` + `/admin/api/rules/*` | [管理 API](guides/admin-api_zh.md) |
| ⑧ SemanticCache（作为最后 Stage） | 见 [语义缓存](#4-语义缓存)。 | ⚪ | `cache.*` | [会话缓存](guides/conversation-cache_zh.md) |
| 外部安全 API | 可选 OpenAI Moderation / Perspective API 级联；异步/同步。 | ⚪ | `security.external_safety.*` | [外部安全 API](guides/external-safety_zh.md) |
| 滥用检测 · Abuse Detection | 频次异常 / 内容相似度聚类识别恶意刷量。 | ⚪ | `security.abuse_detection.*` | [安全最佳实践](guides/security-best-practices_zh.md) |

### 3.2 出站护栏（Outbound Pipeline · 6 Stage）

| Stage / 能力 | 说明 | 归属 | 关键配置 | 相关指南 |
|------|------|:----:|----------|----------|
| ① ContentFilter | 出站有害内容检测（暴力 / 色情 / 仇恨等），可配置策略级别。 | ⚪ | `—（管线 stage · 无顶层 YAML 根键 · 见指南）` | [安全最佳实践](guides/security-best-practices_zh.md) |
| ② Hallucination Scorer | 幻觉可能性评分（0-1），可选拦截或标注。 | ⚪ | `—（管线 stage · 无顶层 YAML 根键 · 见指南）` | [架构指南](guides/architecture_zh.md) |
| ③ Quality Scorer | 4 维输出质量评分（相关性 / 完整性 / 一致性 / 流畅性），反馈到 MLRouter EMA。 | ⚪ | `—（管线 stage · 无顶层 YAML 根键 · 见指南）` | [架构指南 · 成本优化](guides/architecture_zh.md) |
| ④ CostTracker（出站） | Token 计费与成本记录，回写 `PersistentStore`。 | ⚪ | `cost.*` | [成本优化](guides/cost-optimization_zh.md) |
| ⑤ AlertManager | 出站阈值告警（错误率 / 延迟 / 幻觉率），每规则冷却窗口。 | ⚪ | `alerting.rules[]` | [故障排查](guides/troubleshooting_zh.md) |
| ⑥ RequestLogger（出站） | 完整请求生命周期日志，含最终状态与成本。 | ⚪ | `logging.*` | — |

### 3.3 自适应护栏（Adaptive Guard）

| 能力 | 说明 | 归属 | 关键端点 | 相关指南 |
|------|------|:----:|----------|----------|
| 决策解释查询 | `GET /admin/api/guard/explanation/{request_id}`：查看单次拦截的 trigger_layer / trigger_rule / model_version / explanation_text。 | 🔵 | `/admin/api/guard/explanation/{id}` | [自适应护栏](guides/adaptive-guard_zh.md) |
| 误报 / 漏报反馈 | `POST /admin/api/guard/feedback`：4 label（false_positive / false_negative / confirmed_block / confirmed_allow）人类标注回环。 | 🔵 | `/admin/api/guard/feedback` | [自适应护栏](guides/adaptive-guard_zh.md) |
| Guard 模型晋升 | `POST /admin/api/guard/model/promote`：护栏 ONNX 版本从 shadow → active，走 Autonomy 审批闭环。 | 🔵 | `/admin/api/guard/model/promote` | [自适应护栏](guides/adaptive-guard_zh.md) · [Guard 模型](guides/guard-model.md) |

### 3.4 审计与合规

| 能力 | 说明 | 归属 | 关键端点 / 配置 | 相关指南 |
|------|------|:----:|-----------------|----------|
| 防篡改审计链 | FNV-1a 哈希链 + AES-256-GCM 详情加密；断链检测。 | ⚪ | `audit.encrypt: true` | [安全最佳实践](guides/security-best-practices_zh.md) |
| 审计查询与解密回读 | `GET /admin/api/audits`（分页 / 过滤 / 解密），支持时间窗、租户、事件类型。 | 🔵 | `/admin/api/audits` | [管理 API](guides/admin-api_zh.md) |
| 合规报告导出 | `GET /admin/api/export/audit`（CSV/JSON），供 GDPR / 等保合规审计。 | 🔵 | `/admin/api/export/audit` | [合规对标](compliance/README_zh.md) |
| 错误码规范 | 统一 `AEGIS-xxxx` 错误码，覆盖鉴权 / 限流 / 护栏 / 上游 / 内部。 | ⚪ | 见指南 | [错误码](guides/error-codes_zh.md) |

---

## 4. 语义缓存

命中即短路模型调用，零 API 成本；可插拔向量库 + 可插拔 Embedder。

| 能力 | 说明 | 归属 | 关键配置 | 相关指南 |
|------|------|:----:|----------|----------|
| 进程内向量库（hnswlib） | 默认后端，HNSW 索引，纯内存 / 单机部署首选。 | ⚪ | `vector_store.backend: hnswlib` | [架构指南](guides/architecture_zh.md) |
| Milvus 向量库 | 分布式向量数据库集成。 | 🔵 | `vector_store.backend: milvus` | [缓存迁移](guides/cache-migration_zh.md) |
| Qdrant 向量库 | 分布式向量数据库集成。 | 🔵 | `vector_store.backend: qdrant` | [缓存迁移](guides/cache-migration_zh.md) |
| Hash Embedder（默认） | 零依赖 hash-based 特征，快速起步。 | ⚪ | Built-in hash embedder (default) | [架构指南](guides/architecture_zh.md) |
| ONNX BGE Embedder | `BGE-small-zh-v1.5` 512 维，中文优化。 | ⚪ | `embedding.model_path` · `embedding.vocab_path` | [架构指南](guides/architecture_zh.md) |
| 按模型分区 | 不同 model 独立命名空间，避免污染。 | ⚪ | `cache.max_partitions` · `cache.context_aware` | [缓存迁移](guides/cache-migration_zh.md) |
| TTL + LRU 淘汰 | 时间 + 容量双维度淘汰。 | ⚪ | `cache.ttl_seconds` · `cache.max_entries` | [架构指南](guides/architecture_zh.md) |
| 自适应相似度阈值 | 命中率与召回率自平衡，per-model 调整。 | ⚪ | `cache.adaptive_threshold.enabled: true` | [架构指南](guides/architecture_zh.md) |
| 缓存命中短路 | ShortCircuit 状态跳过路由 + 上游调用，直接返回缓存响应。 | ⚪ | 内置 | [架构指南 · 缓存命中时序](guides/architecture_zh.md) |
| 会话级缓存 | 面向多轮对话的会话缓存策略。 | ⚪ | `cache.conversation_cache.*` · `cache.conversation_hash.*` | [会话缓存](guides/conversation-cache_zh.md) |
| 缓存导入 | `POST /admin/cache/import` 批量导入预热。 | ⚪ | `/admin/cache/import` | [OpenAPI](openapi.yaml) |
| 缓存统计 | 查询命中率、条目数、内存占用。 | ⚪ | 内部 metrics | [架构指南](guides/architecture_zh.md) |
| Redis 分布式缓存 | 集群模式共享缓存 + 限流状态。 | 🔵 | `storage.cache_backend: redis` | [生产部署](guides/production-deployment_zh.md) |

---

## 5. 可观测性

Prometheus + Grafana + OTEL 三件套开箱即用，成本 / 质量 / 用量三维监控。

| 能力 | 说明 | 归属 | 关键端点 / 配置 | 相关指南 |
|------|------|:----:|-----------------|----------|
| Prometheus 指标 | `GET /metrics` 暴露完整 metric 集合（RPS / 延迟 / 成本 / 命中率 / 护栏触发 / 熔断态等），支持 label 聚合。 | ⚪ | `/metrics` | [架构指南](guides/architecture_zh.md) |
| Grafana 仪表盘 | 预置 Dashboard JSON（`deploy/grafana/`）覆盖网关健康、成本、质量、护栏。 | ⚪ | `deploy/grafana/*.json` | [生产部署](guides/production-deployment_zh.md) |
| OpenTelemetry 追踪 | 分布式追踪导出到 Jaeger / Tempo / OTLP collector。 | ⚪ | `telemetry.enabled` · `telemetry.otlp_endpoint` · `telemetry.service_name` · `telemetry.sample_ratio` | [OTEL 验证](guides/otel-verification_zh.md) · [OTEL 离线依赖](guides/otel-offline-deps_zh.md) |
| 结构化日志 · Key 脱敏 | JSON 格式；API Key / Token / PII 自动脱敏。 | ⚪ | `logging.*` | — |
| 实时日志流 | `GET /admin/logs/stream`（SSE）实时推送日志。 | 🔵 | `/admin/logs/stream` | [OpenAPI](openapi.yaml) |
| 成本追踪 | 逐请求 token / cost 记录，按 tenant / model / 时间聚合。 | ⚪ | `cost.*` · `/admin/api/costs` | [成本优化](guides/cost-optimization_zh.md) |
| 成本预算 | 日 / 月 / 单次成本上限，超限则降级 quality tier 或告警（见 `budget_guard`）。 | ⚪ | `budget_guard.*` | [成本优化](guides/cost-optimization_zh.md) |
| 用量预测 | `GET /admin/api/predict/usage` + `/predict/budget` 趋势预测。 | 🔵 | `/admin/api/predict/*` | [管理 API](guides/admin-api_zh.md) |
| 告警规则引擎 | 配置驱动的告警规则（多指标、每规则冷却），触发 Webhook / 邮件。 | ⚪ | `alerting.rules[]` | [故障排查](guides/troubleshooting_zh.md) |
| 输出质量评分 | 4 维质量评分，反馈到 MLRouter 与仪表盘。 | ⚪ | `—（管线 stage · 无顶层 YAML 根键 · 见指南）` | [架构指南](guides/architecture_zh.md) |
| 健康探针 | `GET /health` / `/health/live` / `/health/ready`（k8s liveness/readiness）。 | ⚪ | `/health` · `/health/live` · `/health/ready` | [生产部署](guides/production-deployment_zh.md) |
| 安全事件时间线 | `GET /admin/api/security/events` 展示护栏拦截、鉴权失败等安全事件。 | 🔵 | `/admin/api/security/events` | [管理 API](guides/admin-api_zh.md) |

---

## 6. 存储抽象

`CacheStore` + `PersistentStore` 双接口 × 4 后端 × 优雅降级。

| 能力 | 说明 | 归属 | 关键配置 | 相关指南 |
|------|------|:----:|----------|----------|
| `CacheStore` 接口 | KV + TTL 抽象；Memory / Redis 后端。 | ⚪ | `storage.cache_backend` | [架构指南 · 存储架构](guides/architecture_zh.md) |
| `PersistentStore` 接口 | 表级操作抽象（tenant / user / api_key / audit / cost / savings / mfa / sso / sessions / rulesets / templates / autonomy）。 | ⚪ | `storage.persistent_backend` | [架构指南 · 存储架构](guides/architecture_zh.md) |
| Memory 后端 | 内存实现，覆盖全部接口，用于测试与单机默认。 | ⚪ | `storage.persistent_backend: memory` | — |
| SQLite（WAL）后端 | 单机持久化默认；WAL 模式；三后端 parity（排序 / prune 时间格式对齐）。 | 🟢 | `storage.persistent_backend: sqlite` · `storage.sqlite.path` | [架构指南](guides/architecture_zh.md) |
| PostgreSQL 后端 | 生产 / 集群模式；DB 级下推查询（成本导出、审计过滤）。 | 🔵 | `storage.persistent_backend: postgres` · `AEGISGATE_PG_URL` | [生产部署](guides/production-deployment_zh.md) |
| Redis 缓存后端 | 集群模式共享缓存与限流状态；熔断器状态机（Closed/Open/HalfOpen）持久化。 | 🔵 | `storage.cache_backend: redis` · `storage.redis.host`/`port` | [生产部署](guides/production-deployment_zh.md) |
| 优雅降级 | 后端不可用时降级到 Memory，SECURITY 级告警但不阻塞请求。 | ⚪ | 内置 | [架构指南](guides/architecture_zh.md) |
| 锁顺序约定 | 多表事务的锁获取顺序全局约定，防死锁。 | ⚪ | 见指南 | [锁顺序](LOCK_ORDERING_zh.md) |

---

## 7. 多租户 · RBAC · 认证（企业版）

| 能力 | 说明 | 归属 | 关键端点 / 配置 | 相关指南 |
|------|------|:----:|-----------------|----------|
| 4 级角色层级 | `SuperAdmin > TenantAdmin > Developer > Viewer`；跨级授予受 `auth::canGrantRole` 咽喉校验。 | 🔵 | `Feature::RBAC`（Enterprise license · FeatureGate）；仅改 YAML `rbac.enabled` 不会启用运行时 RBAC | [安全最佳实践](guides/security-best-practices_zh.md) |
| 租户隔离 | Per-tenant 数据隔离（audit / cost / rules / templates / apikeys）；`effectiveTenantId` 强制过滤。 | 🔵 | `tenants.*` · `/admin/api/tenants/*` | [管理 API](guides/admin-api_zh.md) |
| 用户管理 CRUD | `/admin/api/users/*` 创建 / 列表 / 更新 / 删除。 | 🔵 | `/admin/api/users` | [管理 API](guides/admin-api_zh.md) |
| API Key 生命周期 | 创建 / 列表 / 轮转 / 撤销；受角色 ≤ 自身校验。 | 🔵 | `/admin/api/keys/*` | [管理 API](guides/admin-api_zh.md) |
| SSO · OIDC / OAuth2 | OIDC + PKCE 登录流；Provider CRUD API。 | 🔵 | `/admin/auth/sso/*` · `/admin/api/sso/providers/*` | [管理 API](guides/admin-api_zh.md) |
| MFA · TOTP | 首次绑定 / 验证 / 禁用 / 恢复；`enforcement=required` 时闸门拒绝无 MFA 会话；失败锁定（`mfa_failures` 表 + 三后端 parity）。 | 🔵 | `/admin/api/mfa/*` · `mfa.enforcement` · `mfa.lockout_max_failures` | [安全最佳实践](guides/security-best-practices_zh.md) |
| MFA 恢复码 | 80-bit 熵恢复码（`RECOVERY_CODE_BYTES=10`），`CRYPTO_memcmp` 常量时间校验。 | 🔵 | `/admin/api/mfa/recovery` | [安全最佳实践](guides/security-best-practices_zh.md) |
| SCIM 2.0 | `/scim/v2/Users` + `/scim/v2/Groups` 完整 CRUD，供 Okta / Azure AD / OneLogin 自动同步。 | 🔵 | `/scim/v2/*` | — |
| Web 管理面板 | React + TypeScript + Vite SPA，`web/admin/`。 | 🔵 | `/admin/*`（静态资源） | [管理 API](guides/admin-api_zh.md) |
| Web 管理 · 仪表盘 | 请求数 / 成本 / 缓存命中率总览。 | 🔵 | `/admin/api/dashboard/summary` | [节省看板](guides/admin-savings_zh.md) |
| Web 管理 · Adaptive Guard 页 | 三端点闭环 UI（解释 / 反馈 / 晋升）。 | 🔵 | `/admin/api/guard/*` | [自适应护栏](guides/adaptive-guard_zh.md) |
| CORS 咽喉决策 | 抽出 `admin::decideCors` 纯函数：具体 origin 回显 + 凭证 + `Vary`；通配符 `*` 不发凭证；配置违规首次 warn。 | 🔵 | `admin.cors.*` | — |
| 可信代理 XFF | `admin.trusted_proxies` + `resolveClientIp` 仅信可信 peer 的 XFF 最右非可信段。 | 🔵 | `admin.trusted_proxies` | [安全最佳实践](guides/security-best-practices_zh.md) |
| IP allowlist（CIDR） | `isAdminIpAllowed` 支持 IPv4 CIDR；管理端点粒度控制。 | 🔵 | `admin.ip_allowlist[]` | [安全最佳实践](guides/security-best-practices_zh.md) |
| 会话 cookie 作用域 | `aegis_session` 严格 `Path=/admin`，与 `/admin/api/*` 命名空间对齐；数据面 `/v1/*` 不受管理面 cookie 影响。 | 🔵 | 内置 | [自适应护栏](guides/adaptive-guard_zh.md) |

---

## 8. 控制平面

配置驱动的运营时能力，`/admin/api/*` 命名空间。

| 能力 | 说明 | 归属 | 关键端点 / 配置 | 相关指南 |
|------|------|:----:|-----------------|----------|
| 配置热重载 | `POST /admin/reload` 重载 YAML；GatewayRuntime 重新装配 pipeline 与规则。 | ⚪ | `/admin/reload` | [安全最佳实践](guides/security-best-practices_zh.md) |
| 规则集版本化 | `/admin/api/rules/*` 列表 / 创建 / 激活；global + per-tenant；`activateRuleSet` 后 RuleEngine 即时刷新。 | 🔵 | `/admin/api/rules/*` | [管理 API](guides/admin-api_zh.md) |
| Prompt 模板管理 | `/admin/api/templates/*` 完整 CRUD；当 Chat Completions 尚无 `system` 消息时，按租户 active 模板（weight 加权）前置注入 system。可选覆盖头：`X-AegisGate-Template: <name>`。响应头：`X-AegisGate-Template-Applied`。 | 🔵 | `/admin/api/templates/*` · `X-AegisGate-Template` | [管理 API](guides/admin-api_zh.md) |
| 灰度发布 · 金丝雀 | Rollout 控制器：progressing → completed，onTick 幂等对账（activate + updateRollout 补偿）。状态存于控制平面 / env（非 `aegisgate.yaml` 顶层 `rollout.*` 根键）。 | 🔵 | 控制平面 Rollout API · [灰度发布](guides/rollout_zh.md) | [灰度发布](guides/rollout_zh.md) |
| 供应商 Manifest 管理 | 声明式模型 / Provider 元数据；`aegisctl conformance` 校验。 | ⚪ | `api/providers/definitions/*.yaml` | [供应商清单](guides/provider-manifest_zh.md) |
| 集群控制平面 | 当 `features.deployment.mode: cluster` 时集中式配置分发与节点协调（Redis 共享态）。 | 🔵 | `features.deployment.mode: cluster` | [控制平面](guides/control-plane_zh.md) |
| Feedback Bus | 事件 / 反馈 pub-sub，护栏反馈 / autonomy 提案流转。 | ⚪ | `feedback_bus.*` | [反馈总线](guides/feedback-bus_zh.md) |

---

## 9. 成本自治与 AegisOps

`AegisOps` 战略层能力：让网关不只是省钱管道，也是"会自己优化的运营员"。

| 能力 | 说明 | 归属 | 关键端点 | 相关指南 |
|------|------|:----:|----------|----------|
| Autonomy Proposals · 列表 | `GET /admin/api/autonomy/proposals`：查看待审 / 已决提案。 | 🔵 | `/admin/api/autonomy/proposals` | [成本自治](guides/cost-autonomy_zh.md) |
| Autonomy Proposals · 审批 | `POST /admin/api/autonomy/proposals/{id}/approve`。 | 🔵 | `/admin/api/autonomy/proposals/{id}/approve` | [成本自治](guides/cost-autonomy_zh.md) |
| Autonomy Proposals · 拒绝 | `POST /admin/api/autonomy/proposals/{id}/reject`。 | 🔵 | `/admin/api/autonomy/proposals/{id}/reject` | [成本自治](guides/cost-autonomy_zh.md) |
| Autonomy Proposals · 回滚 | `DELETE /admin/api/autonomy/proposals/{id}` (rollback)。 | 🔵 | `DELETE /admin/api/autonomy/proposals/{id}` | [成本自治](guides/cost-autonomy_zh.md) |
| Autonomy 报告 | `GET /admin/api/autonomy/report`：省钱效果汇总。 | 🔵 | `/admin/api/autonomy/report` | [成本自治](guides/cost-autonomy_zh.md) |
| Savings 事件流 | `savings_events` 表 + `POST/GET /admin/api/savings/events`；三后端 parity（含 PG）。 | 🔵 | `/admin/api/savings/events` | [节省看板](guides/admin-savings_zh.md) |
| Case Study Headline | `GET /admin/api/case-study/headline` 生成对外可 pitch 的节省 headline。 | 🔵 | `/admin/api/case-study/headline` | [管理 API](guides/admin-api_zh.md) |
| AegisOps 战略愿景 | 由"AI 网关"演进为"AI 治理平台"的能力矩阵与路线图。 | 📐 | — | [AegisOps 愿景](positioning/aegisops-vision_zh.md) · [ROADMAP v4](ROADMAP_v4_zh.md) |

---

## 10. 部署与集群

| 能力 | 说明 | 归属 | 关键路径 | 相关指南 |
|------|------|:----:|----------|----------|
| Docker 单机 | 单容器起 AegisGate + SQLite；`Dockerfile` + `docker-compose.yaml`。 | 🟢 | `Dockerfile` · `docker-compose.yaml` | [生产部署](guides/production-deployment_zh.md) |
| 5 分钟 Quickstart 容器 | 自动生成 API Key + 自装 SQLite 卷 + 一键起。 | ⚪ | `scripts/quickstart-entrypoint.sh` | [5 分钟快速上手](quickstart_zh.md) |
| Helm Chart | `helm/aegisgate/` 官方 Chart；支持 values.yaml 完整覆盖。 | 🔵 | `helm/aegisgate/*` | [生产部署](guides/production-deployment_zh.md) |
| K8s 部署 | k8s 原生 Deployment / Service / ConfigMap 示例。 | 🔵 | `deploy/kubernetes/*` | [生产部署](guides/production-deployment_zh.md) |
| Redis 集群模式 | 共享缓存 + 限流 + 熔断态；节点间无状态。 | 🔵 | `cluster.enabled: true` | [控制平面](guides/control-plane_zh.md) |
| 多区域 | 地理分布式部署拓扑与 GeoRouter 联动。 | 🔵 | `routing.geo.*` | [多区域](guides/multi-region_zh.md) |
| 生产验证 Runbook | 上线前 / 上线中 / 上线后核查清单。 | ⚪ | — | [生产验证 Runbook](guides/production-validation-runbook_zh.md) |
| 性能调优 | 吞吐 / 延迟 / 资源三维优化。 | ⚪ | — | [性能调优](guides/performance-tuning_zh.md) |
| macOS 开发 | Apple Silicon / Intel 本地开发指引。 | ⚪ | — | [macOS 开发](guides/macos-development.md) |

---

## 11. 插件与生态

| 能力 | 说明 | 归属 | 关键配置 / CLI | 相关指南 |
|------|------|:----:|----------------|----------|
| C-ABI 插件系统 | `dlopen` 动态加载，稳定 C ABI；供第三方扩展护栏 / 路由 / 存储。 | 🔵 | `plugins.*` | — |
| 本地规则包管理 | `aegisctl rules list|install|remove|info|apply` 管理本地已安装规则包（`RulePackManager`）；非远程规则市场客户端。 | 🔵 | `aegisctl rules` | — |
| Prompt 模板库 | 租户 Prompt 模板经由 `/admin/api/templates/*` CRUD；可选 Chat Completions system 注入（无随仓内置模板包目录）。 | ⚪ | `/admin/api/templates/*` | [管理 API](guides/admin-api_zh.md) |
| Showcase Demo | 大模型 → AegisGate → 应用落地参考 Demo（AI 漫剧旗舰 / 电商验证）。 | ⚪ | `apps/showcase/` | [Showcase](../apps/showcase/README_zh.md) |

---

## 12. 客户端 SDK

| 语言 | 包名 | 特性 | 归属 | 相关指南 |
|------|------|------|:----:|----------|
| Python | `aegisgate` | 同步 + 异步（httpx-based） | ⚪ | [SDK 集成](guides/sdk-integration_zh.md) · [Python SDK](../sdk/python/) |
| Node.js | `@aegisgate/sdk` | TypeScript，原生 fetch，ESM | ⚪ | [SDK 集成](guides/sdk-integration_zh.md) · [Node.js SDK](../sdk/nodejs/) |
| Go | `aegisgate-go` | 零依赖，仅标准库 | ⚪ | [SDK 集成](guides/sdk-integration_zh.md) · [Go SDK](../sdk/go/) |
| Java / Kotlin | `dev.aegisgate`（Gradle） | JVM 客户端 | ⚪ | [Java/Kotlin SDK](../sdk/java/) |
| Rust | `aegisgate`（crate） | 异步客户端 | ⚪ | [Rust SDK](../sdk/rust/) |

Python / Node.js / Go SDK 支持：对话补全（流式 + 非流式）、模型列表、健康检查、`/metrics` 拉取、配置重载。Java/Kotlin 与 Rust 覆盖各 SDK README 中记录的核心客户端能力（此处不宣称与前三者完全对等）。

---

## 13. CLI 工具

`aegisctl` 单一 CLI，涵盖运维 / 估算 / 校验 / 本地规则包。

| 子命令 | 说明 | 相关指南 |
|--------|------|----------|
| `aegisctl estimate` | 历史日志 → 迁移到 AegisGate 后的 token/成本节省估算。 | [节省估算](estimate_zh.md) |
| `aegisctl conformance check <manifest>` | 供应商 Manifest 一致性校验。 | [供应商清单](guides/provider-manifest_zh.md) |
| `aegisctl conformance check-all <dir>` | 目录级批量校验。 | [供应商清单](guides/provider-manifest_zh.md) |
| `aegisctl rules list|install|remove|info|apply` | 本地规则包管理器（`RulePackManager`）。 | — |
| `aegisctl bench` | 基准压测（吞吐 / 延迟）。 | [性能调优](guides/performance-tuning_zh.md) |

---

## 14. API 端点索引

### 14.1 数据面（OpenAI 兼容 · `/v1/*`）

| 端点 | 方法 | 用途 |
|------|:----:|------|
| `/v1/chat/completions` | POST | 对话补全（流式 / 非流式） |
| `/v1/embeddings` | POST | 文本向量化 |
| `/v1/images/generations` | POST | 图像生成 |
| `/v1/audio/transcriptions` | POST | 音频转写（ASR） |
| `/v1/audio/translations` | POST | 音频翻译 |
| `/v1/audio/speech` | POST | 语音合成（TTS） |
| `/v1/models` | GET | 模型列表 |

### 14.2 观测面

| 端点 | 方法 | 用途 |
|------|:----:|------|
| `/health` · `/health/live` · `/health/ready` | GET | 健康 / k8s liveness / readiness |
| `/metrics` | GET | Prometheus 指标 |
| `/admin/reload` | POST | 配置热重载 |
| `/admin/cache/import` | POST | 缓存批量导入 |
| `/admin/logs/stream` | GET (SSE) | 实时日志流 |

### 14.3 管理面（`/admin/api/*` · 企业版）

- **认证 / 会话：** `/admin/api/auth/login` · `/admin/api/auth/logout` · `/admin/api/me`
- **租户：** `/admin/api/tenants` · `/{id}`
- **用户：** `/admin/api/users` · `/{id}`
- **API Key：** `/admin/api/keys` · `/{id}/revoke` · `/{id}/rotate`
- **审计与成本：** `/admin/api/audits` · `/admin/api/costs` · `/admin/api/dashboard/summary` · `/admin/api/savings/summary` · `/admin/api/security/events` · `/admin/api/case-study/headline`
- **SSO / MFA：** `/admin/auth/sso/*` · `/admin/api/sso/providers/*` · `/admin/api/mfa/*`
- **Prompt 模板：** `/admin/api/templates/*`
- **规则集：** `/admin/api/rules` · `/admin/api/rules/active` · `POST /admin/api/rules/activate`
- **预测：** `/admin/api/predict/usage` · `/admin/api/predict/budget`
- **报告导出：** `/admin/api/export/audit` · `/admin/api/export/cost`
- **Autonomy 提案：** `/admin/api/autonomy/proposals` · `POST .../{id}/approve` · `POST .../{id}/reject` · `DELETE .../{id}` (rollback) · `/admin/api/autonomy/report`
- **Adaptive Guard：** `/admin/api/guard/{feedback,explanation/{id},model/promote}`

### 14.4 SCIM 2.0 · 企业版

| 端点 | 方法 | 用途 |
|------|:----:|------|
| `/scim/v2/Users` · `/{id}` | GET / POST / PUT / DELETE | 用户同步 |
| `/scim/v2/Groups` · `/{id}` | GET / POST / PUT / DELETE | 组同步 |

完整机器可读定义：[OpenAPI 规范](openapi.yaml)。

---

## 15. 版本能力矩阵

对齐 [`README_zh.md` Editions 段](../README_zh.md#版本体系)，展开到子能力粒度：

| 能力域 | 社区版 | 企业版 |
|--------|:------:|:------:|
| 统一 API 代理（7 家 + OpenAI 兼容） | ✅ | ✅ |
| Token 优化（压缩 / 精算 / CJK） | ✅ | ✅ |
| 多模态代理（Embeddings / Images / Audio） | ✅ | ✅ |
| 路由 · BasicRouter | ✅ | ✅ |
| 路由 · CostAwareRouter | ✅ | ✅ |
| 路由 · MLRouter | — | ✅ |
| 路由 · A/B Test · Geo | — | ✅ |
| 熔断降级 / 负载均衡 / 限流 | ✅ | ✅ |
| 护栏 · 基础（NFKC / L1-L2 注入 / PII / 话题） | ✅ | ✅ |
| 护栏 · L3 ONNX GuardModel | — | ✅ |
| 护栏 · YAML 规则引擎 + per-tenant | — | ✅ |
| 护栏 · Adaptive Guard（解释 / 反馈 / 晋升） | — | ✅ |
| 语义缓存 · hnswlib 进程内 | ✅ | ✅ |
| 语义缓存 · Milvus / Qdrant / Redis 分布式 | — | ✅ |
| 存储 · SQLite | ✅ | ✅ |
| 存储 · PostgreSQL | — | ✅ |
| 存储 · Redis 集群共享 | — | ✅ |
| 观测 · Prometheus + Grafana | ✅ | ✅ |
| 观测 · OTEL 追踪 | ✅ | ✅ |
| 观测 · 实时日志流 / 安全事件 | — | ✅ |
| 管理 · CLI（`aegisctl`） | ✅ | ✅ |
| 管理 · Web 面板（React SPA） | — | ✅ |
| 多租户 · RBAC · SSO · MFA · SCIM | — | ✅ |
| 审计 · 防篡改链 + 加密 | ✅ | ✅ |
| 审计 · 合规报告导出 / 长期归档 | — | ✅ |
| 控制平面 · 配置热重载 | ✅ | ✅ |
| 控制平面 · 规则集版本 / 灰度 / 金丝雀 | — | ✅ |
| 成本自治 · Autonomy 提案闭环 | — | ✅ |
| 部署 · Docker 单机 | ✅ | ✅ |
| 部署 · Helm / K8s / 集群 / 多区域 | — | ✅ |
| 插件系统 · C-ABI dlopen | — | ✅ |
| 本地规则包管理 · `aegisctl rules` | — | ✅ |
| Feedback Bus | ✅ | ✅ |
| Provider Manifest | ✅ | ✅ |
| Prompt 模板库（Admin CRUD） | — | ✅ |
| Showcase Demo（`apps/showcase/`） | ✅ | ✅ |
| 客户端 SDK（Python / Node / Go / Java / Rust） | ✅ | ✅ |

图例：✅ = 支持 · — = 该版本不支持。

---

## 相关文档

- [README](../README_zh.md) — 项目概览与快速上手
- [架构指南](guides/architecture_zh.md) — 系统全景、请求管道、时序图
- [OpenAPI 规范](openapi.yaml) · [OpenAI 兼容矩阵](openai-compat-matrix.md) — 机器可读 API 定义
- [ROADMAP v4](ROADMAP_v4_zh.md) — AegisOps 战略路线图
- [文档总览](README_zh.md) — 所有指南入口

---

*本清单在能力交付或废弃时同步更新，作为对外能力盘点的单一真相来源。异常或缺项请通过 [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions) 反馈。*
