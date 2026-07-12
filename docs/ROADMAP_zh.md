# AegisGate 发展蓝图

本文档描述 AegisGate 的版本规划和功能路线图，包含社区版持续增强和企业版分阶段推进。

> 最后更新：2026-03-21

---

## 版本模式

AegisGate 采用 **Open-Core** 模式：

- **社区版 (Community)** — 完全开源，功能完整，无人为限制，适合开发者和小团队
- **企业版 (Enterprise)** — 在社区版基础上增加规模化运维、合规管理、多租户等企业级能力，需要 License 激活

同一份二进制文件，运行时通过 Feature Gate + License 控制功能开放。

---

## 功能矩阵

### 统一 AI 网关

| 功能 | 社区版 | 企业版 |
|------|:------:|:------:|
| 统一 API（OpenAI / Claude / DeepSeek / 豆包 / 通义 / Gemini / Mistral） | ✅ | ✅ |
| ConnectorFactory 零代码新增 provider | ✅ | ✅ |
| 基础路由（指定模型 / 成本感知） | ✅ | ✅ |
| 失败降级 + 负载均衡 | ✅ | ✅ |
| SSE 流式 + 双缓冲 | ✅ | ✅ |
| 令牌桶限流（per-key） | ✅ | ✅ |
| 语义缓存（LRU + hnswlib） | ✅ | ✅ |
| ML 辅助高级路由（成本/质量/延迟优化） | — | ✅ |

### 安全护栏

| 功能 | 社区版 | 企业版 |
|------|:------:|:------:|
| Prompt 注入检测（正则 + 可选 ONNX） | ✅ | ✅ |
| PII 敏感信息脱敏（RE2） | ✅ | ✅ |
| 话题边界（白名单 / 黑名单） | ✅ | ✅ |
| 出站内容过滤 | ✅ | ✅ |
| 幻觉检测 | ✅ | ✅ |
| 审计日志（文件） | ✅ | ✅ |
| 自定义规则引擎（声明式定义 / 版本管理） | — | ✅ |
| 合规报告（审计导出 / 留存策略） | — | ✅ |

### 管理与运维

| 功能 | 社区版 | 企业版 |
|------|:------:|:------:|
| CLI 管理工具（aegisctl） | ✅ | ✅ |
| Prometheus 指标端点 | ✅ | ✅ |
| 成本跟踪 | ✅ | ✅ |
| Web 管理面板（仪表盘 / 模型管理 / 成本分析） | — | ✅ |
| 高级告警（Webhook / 钉钉 / 飞书） | — | ✅ |

### 访问控制与部署

| 功能 | 社区版 | 企业版 |
|------|:------:|:------:|
| API Key 认证 | ✅ | ✅ |
| SQLite 存储 | ✅ | ✅ |
| 单机 Docker 部署 | ✅ | ✅ |
| RBAC（角色权限控制） | — | ✅ |
| 多租户隔离 | — | ✅ |
| SSO（OIDC / OAuth2） | — | ✅ |
| PostgreSQL + Redis | — | ✅ |
| 集群部署 + 水平扩展 | — | ✅ |

### 客户端 SDK

| SDK | 社区版 | 企业版 |
|-----|:------:|:------:|
| Python (httpx) | ✅ | ✅ |
| Node.js (native fetch) | ✅ | ✅ |
| Go (stdlib) | ✅ | ✅ |

---

## 发展阶段总览

核心策略：**先还债、再增强、后扩展**

```
Phase 0  夯实根基        技术债务清理 + 生产就绪加固
   │
Phase 1  社区版进化      可观测性深化 + 安全升级 + 缓存进化 + 开发者体验
   │
Phase 2  企业版基础      P2 RBAC 多租户 + P3 Web 管理面板
   │
Phase 3  企业版高级      P4 SSO + P5 高级功能
   │
Phase 4  规模化与生态    P6 集群部署 + 插件系统 + 生态建设
```

---

## Phase 0：夯实根基（2026 Q2 前半）

> 消除已知技术债务，为后续开发建立坚实基础。不增加新功能，专注于修内功。

### 0.1 技术债务清理

| 编号 | 债务 | 方案 | 预期收益 |
|------|------|------|---------|
| TD-AuditSync | AuditLogger 同步 INSERT 57K 行后退化 56x | 异步批量写入：后台线程 + 环形缓冲区，每 100ms 或 1000 条批量 INSERT | 写入延迟从 1.7ms 降回 <50μs |
| TD-CBLock | CircuitBreaker 单锁不可扩展 | per-model 独立 mutex（unordered_map<string, ModelState>） | 8 线程吞吐提升 ~7x |
| TD-MetricsAlloc | Counter DiffLabels 路径字符串拼接开销 | LabelSet 预计算 hash key 并缓存 | 热路径减少一次 string alloc |
| TD-UpstreamBuf | Drogon HttpClient 无流式回调，LLM 响应先缓冲 | 调研 Drogon 最新版或引入 libcurl multi / Boost.Beast 作为可选上游客户端 | 真正的端到端流式，降低首字延迟 |

### 0.2 生产就绪加固

| 项目 | 内容 |
|------|------|
| 优雅关闭 | SIGTERM/SIGINT 信号处理 → 停止接收新请求 → 等待在途请求完成（超时 30s）→ flush 缓冲区 → 关闭存储连接 → 退出 |
| 健康检查增强 | `/health` 区分 liveness（进程存活）和 readiness（依赖就绪：SQLite 连接、ONNX 模型加载） |
| 配置热加载 | `SIGHUP` 或 `/admin/reload` 触发配置重新加载（模型列表、安全规则、限流参数），不重启进程 |
| Dockerfile | 多阶段构建：builder 阶段编译 + runtime 阶段仅含二进制和 ONNX 模型，基于 distroless 或 alpine |
| docker-compose | 单机一键启动模板：aegisgate + SQLite 卷映射 + Prometheus + Grafana 仪表盘模板 |
| Chaos 测试 | 模拟上游超时、连接断开、错误率飙升场景的集成测试，验证熔断/降级/限流行为 |

---

## Phase 1：社区版进化（2026 Q2 后半）

> 提升社区版的可观测性、安全深度、缓存智能、开发者体验，保持社区版的竞争力和吸引力。

### 1.1 可观测性深化

| 项目 | 内容 | 价值 |
|------|------|------|
| OpenTelemetry Trace | 集成 opentelemetry-cpp SDK，每个请求生成 TraceID，每个 PipelineStage 生成 Span | 请求链路可视化，快速定位瓶颈 |
| Trace 上下文传播 | 入站请求携带 `traceparent` header 时继承，出站请求向上游传播 TraceID | 跨服务追踪 |
| 结构化日志关联 | spdlog 输出中嵌入 trace_id/span_id 字段 | 日志-指标-链路三支柱打通 |
| Grafana 仪表盘模板 | 预置 JSON 仪表盘：QPS、延迟分位数、缓存命中率、安全拦截率、成本分布、Stage 耗时 | 开箱即用的监控 |
| 请求回放 | 审计日志导出为可重放格式，`aegisctl replay` 回放历史请求 | 问题复现 |

### 1.2 安全护栏升级

| 项目 | 内容 | 价值 |
|------|------|------|
| L3 模型级安全分类 | 引入 Llama Guard 3 / ShieldGemma ONNX 量化模型作为可选第三层检测（`ENABLE_GUARD_MODEL=ON`） | 对抗语义级注入攻击 |
| Unicode 规范化 | 入站护栏前增加 NFKC 规范化预处理，统一视觉相似字符（全角/半角/同形字/组合字符） | 防御 Unicode 混淆攻击 |
| 编码检测与解码 | 检测用户输入中的 Base64、Hex、URL 编码片段，解码后二次安全检查 | 防御编码绕过 |
| 多语言注入模式 | 扩展注入检测规则，覆盖中文、日文、韩文、俄文等常见注入模式 | 国际化安全覆盖 |
| 频率异常检测 | per-key 追踪被拦截请求频率，短时间内多次触发安全护栏的 key 自动降级限流 | 主动防御 |
| 安全规则热加载 | `config/rules/*.yaml` 支持运行时热加载，无需重启 | 快速响应新威胁 |

### 1.3 语义缓存进化

| 项目 | 内容 | 价值 |
|------|------|------|
| 多轮对话感知 | 缓存键升级为"对话摘要"（hash(system_prompt) + embedding(last_message)） | 避免上下文不同的请求被错误命中 |
| 缓存统计端点 | `/cache/stats` 暴露命中率、条目数、平均相似度、Top-K 热门查询 | 可观测，调优依据 |
| 自适应阈值 | 基于历史命中反馈动态调整相似度阈值 | 提高缓存质量 |
| 选择性缓存 | 按 model/api_key/请求特征（如 temperature > 0.8）决定是否缓存 | 减少无效缓存 |
| 缓存预热增强 | 支持从 JSON 文件导入预定义 QA 对 | 重启后立即可用 |

### 1.4 开发者体验

| 项目 | 内容 | 价值 |
|------|------|------|
| aegisctl 增强 | `config validate` 配置校验、`bench` 内置压测、`logs tail` 实时日志流 | 运维效率 |
| OpenAPI 交互式文档 | 内置 Swagger UI 或 Scalar 在 `/docs` 端点 | 降低集成门槛 |
| SDK 测试模式 | 各语言 SDK 内置 mock server 支持 | SDK 用户体验 |
| 错误码体系 | 统一错误码规范（`AEGIS-1xxx` 认证、`AEGIS-2xxx` 限流、`AEGIS-3xxx` 安全、`AEGIS-4xxx` 路由） | 调试效率 |
| 教程与最佳实践 | 快速入门教程、性能调优指南、安全最佳实践、常见问题排查手册 | 降低采用门槛 |

---

## Phase 2：企业版基础（2026 Q3）

> P1 存储基础已在 TASK-20260319-02/03 中完成（SQLite + Redis + PG + 存储抽象层 + 迁移工具）。

### 2.1 P2 RBAC + 多租户

- RBAC 角色权限（SuperAdmin / TenantAdmin / Developer / Viewer）
- 多租户隔离（每个请求绑定 tenant，数据按 tenant 分区）
- 租户级配额与限流
- API Key 关联角色与租户
- 租户级模型白名单（每个租户可配置允许使用的模型列表）
- 租户级成本上限（月度/日度硬上限，超限自动拒绝）
- API Key 生命周期管理（创建、轮转、吊销、过期时间，`aegisctl key` 子命令）
- 审计日志按租户查询（PersistentStore 层面 `WHERE tenant_id = ?` 过滤）

### 2.2 P3 Web 管理面板

- React + TypeScript + Vite 前端
- 仪表盘（概览 / 实时统计）
- 模型管理、用户管理
- 可视化规则编辑器
- 成本分析仪表盘
- 实时 WebSocket 仪表盘（请求数/延迟/安全事件推送，不依赖轮询）
- 拖拽式安全规则编辑器（实时预览匹配效果）
- 配置变更审计（所有管理面板操作记录审计日志，支持变更回滚）

---

## Phase 3：企业版高级（2026 Q3-Q4）

### 3.1 P4 SSO 集成

- OIDC / OAuth2 身份提供商集成（Okta、Azure AD、Keycloak 等）
- 自动映射外部身份到本地租户与角色
- SCIM 用户同步（从企业 IdP 自动同步用户和组）
- MFA 二次认证（管理面板关键操作强制 TOTP 二次验证）
- 会话管理（在线用户列表、强制下线、会话超时策略）

### 3.2 P5 高级功能

- 自定义规则引擎增强（YAML 声明式定义 / 热加载 / 版本管理）
- 高级告警（Webhook 通用通道 / 钉钉 / 飞书 / Slack 适配器）
- 合规报告（定时导出 / 留存策略 / CSV 导出）
- ML 辅助智能路由（基于历史数据的成本/质量/延迟三维优化）
- A/B 测试路由（按流量百分比将请求路由到不同模型，对比质量/成本/延迟）
- Prompt 模板管理（企业级 system prompt 版本管理、A/B 测试、回滚）
- 输出质量评分（基于规则 + 轻量模型对 LLM 输出自动质量评分）
- 用量预测（基于历史数据的用量趋势预测和成本预估）

---

## Phase 4：规模化与生态（2026 Q4 - 2027 Q1）

### 4.1 P6 集群部署

- 多节点部署（Redis 共享状态总线）
- 节点发现与健康检查
- 水平扩展（无状态设计 + 共享存储）
- Kubernetes Helm Chart（HPA 自动伸缩、PDB 滚动更新、ConfigMap 配置管理）
- 灰度发布（节点级灰度：新版本先部署到 1 个节点，验证后全量推送）
- 分布式追踪融合（集群环境下 OpenTelemetry Trace 跨节点传播）

### 4.2 生态系统建设

| 项目 | 内容 | 价值 |
|------|------|------|
| 插件系统 | `PluginStage` 接口，支持动态加载 `.so` 插件作为 Pipeline Stage | 社区可扩展性 |
| 社区规则市场 | 托管社区贡献的安全规则集（行业模板：金融/医疗/教育），`aegisctl rules install` 一键导入 | 降低安全配置门槛 |
| 多语言 SDK 扩展 | Java/Kotlin、Rust、C#/.NET SDK | 覆盖更多技术栈 |
| Terraform Provider | 基础设施即代码管理 AegisGate 配置和部署 | DevOps 集成 |
| VS Code 扩展 | 配置文件 YAML schema 补全 + 安全规则语法高亮 + 实时连接测试 | 开发者体验 |

---

## 关键里程碑

| 里程碑 | 目标日期 | 标志性成果 |
|--------|---------|----------|
| v0.2 — 生产就绪 | 2026-04 | 技术债务清零 + Docker 一键部署 + 优雅关闭 + Chaos 测试通过 |
| v0.3 — 可观测性 | 2026-05 | OpenTelemetry 全链路追踪 + Grafana 预置仪表盘 |
| v0.4 — 安全深度 | 2026-06 | L3 模型级安全分类 + Unicode 规范化 + 编码检测 + 安全规则热加载 |
| v1.0 — 社区版 GA | 2026-07 | 社区版功能冻结，通过生产环境验证，文档完整，API 稳定承诺 |
| v1.1 — 企业版 Beta | 2026-09 | RBAC + 多租户 + Web 管理面板 |
| v1.2 — 企业版 GA | 2026-11 | SSO + 高级功能 + 集群部署 |
| v2.0 — 生态 | 2027-Q1 | 插件系统 + 规则市场 + Terraform Provider |

## 资源估算

| Phase | 预估工时 | 核心技能要求 |
|-------|---------|-------------|
| Phase 0 — 夯实根基 | 4-5 周 | C++ 并发编程、容器化 |
| Phase 1 — 社区版进化 | 8-10 周 | OpenTelemetry、ML 模型推理、安全工程 |
| Phase 2 — 企业版基础 | 9-12 周 | C++ 后端 + React/TypeScript 前端 |
| Phase 3 — 企业版高级 | 6-8 周 | OAuth2/OIDC 协议、数据分析 |
| Phase 4 — 规模化与生态 | 10-14 周 | 分布式系统、K8s、插件架构 |

---

## 依赖关系

```
Phase 0.1 技术债务清理 ──→ Phase 1.1 可观测性深化 ──→ Phase 2.1 RBAC + 多租户
Phase 0.2 生产就绪加固 ──→ Phase 1.4 开发者体验       │
                                                      ↓
Phase 1.2 安全护栏升级 ──→ Phase 2.1 RBAC + 多租户    Phase 2.2 Web 管理面板
Phase 1.3 语义缓存进化 ──→ Phase 3.2 高级功能              │
                                                      ↓
                          Phase 3.1 SSO 集成 ────→ Phase 4.1 集群部署
                          Phase 3.2 高级功能 ────→ Phase 4.1 集群部署
                                                      │
                                                      ↓
                                                 Phase 4.2 生态系统
```

---

## 贡献

欢迎社区贡献！请参阅 [CONTRIBUTING.md](../CONTRIBUTING.md) 了解开发流程。

社区版相关的 Issue 和 PR 优先处理。企业版功能的设计讨论也欢迎参与。

---

## 后续路线

本文档是初始版本路线图。后续已扩展为三代并存：

- **v2.0**（生产验证 → 平台进化 → 生态扩展）→ [ROADMAP.md](ROADMAP.md)
- **v3.0**（全球化 × 平台化 × 智能化 / 工程路线）→ [ROADMAP_v3.md](ROADMAP_v3.md)
- **v4.0**（AegisOps 产品 / 商业化叠加）→ [ROADMAP_v4_zh.md](ROADMAP_v4_zh.md) · [ROADMAP_v4.md](ROADMAP_v4.md)
