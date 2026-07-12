# AegisGate 发展蓝图 v2.0

本文档描述 AegisGate 的 v2.0 战略方向和功能路线图。Phase 0-4 已于 2026-03-26 全部完成，本路线图聚焦于从"功能构建"到"生产验证 → 平台进化 → 生态扩展"的全新战略转型。

> 最后更新：2026-03-30
> 前序路线图：Phase 0-4 完成记录见文末附录

---

## 战略定位

### 愿景

**成为高性能 AI 网关的事实标准** — 在 LiteLLM (Python) 统治易用性、Portkey (SaaS) 主导托管市场的格局下，AegisGate 以 C++ 原生性能 + 内建安全护栏 + 零外部依赖部署的差异化三角，占据自托管高性能 AI 网关的头部位置。

### 核心战略：从构建到验证，从单机到平台

```
Phase 0-4 ✅            v2.0 新战略
┌──────────────┐      ┌──────────────────────────────────────────────┐
│  功能构建     │  →   │  Phase 5  生产验证与 GA 发布                   │
│  (13 天完成)  │      │  Phase 6  平台进化（多模态 + 外部集成）         │
│              │      │  Phase 7  生态与社区建设                       │
│              │      │  Phase 8  下一代智能（RAG + Agent）             │
│              │      │  Phase 9  全球化规模部署                       │
└──────────────┘      └──────────────────────────────────────────────┘
```

### 差异化三角

| 维度 | AegisGate 优势 | 竞品短板 |
|------|---------------|---------|
| **性能** | 47μs 管道延迟（C++ 原生），16 分片锁并发 | LiteLLM: Python GIL 限制；Portkey: SaaS 网络开销 |
| **安全** | 三层护栏内建（正则→规则→ONNX），Unicode 归一化+编码检测 | LiteLLM: 无内建护栏；Kong: 插件级安全 |
| **部署** | 社区版零外部依赖单二进制，企业版 Feature Gate 同一镜像 | Portkey: 仅 SaaS；LiteLLM: 依赖 Python 运行时 |

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
| Function Calling / Tool Use 全链路支持 | ✅ | ✅ |
| ConnectorFactory 零代码新增 provider | ✅ | ✅ |
| 基础路由（指定模型 / 成本感知） | ✅ | ✅ |
| 失败降级 + 负载均衡 | ✅ | ✅ |
| SSE 流式 + 双缓冲 | ✅ | ✅ |
| 令牌桶限流（per-key） | ✅ | ✅ |
| Token 优化（压缩 + 智能 max_tokens） | ✅ | ✅ |
| 语义缓存（LRU + hnswlib） | ✅ | ✅ |
| 多模态 API 代理（embedding / image / audio） | ✅ ᴺᴱᵂ | ✅ ᴺᴱᵂ |
| ML 辅助高级路由（成本/质量/延迟优化） | — | ✅ |
| 外部向量数据库集成（Milvus / Qdrant） | — | ✅ ᴺᴱᵂ |

### 安全护栏

| 功能 | 社区版 | 企业版 |
|------|:------:|:------:|
| Prompt 注入检测（正则 + 可选 ONNX） | ✅ | ✅ |
| PII 敏感信息脱敏（RE2） | ✅ | ✅ |
| 话题边界（白名单 / 黑名单） | ✅ | ✅ |
| 出站内容过滤 | ✅ | ✅ |
| 幻觉检测 | ✅ | ✅ |
| Function Calling 工具调用安全审计 | ✅ | ✅ |
| 审计日志（文件 + 链式哈希） | ✅ | ✅ |
| 外部安全 API 集成（OpenAI Moderation / Perspective） | — | ✅ ᴺᴱᵂ |
| 自定义规则引擎（声明式定义 / 版本管理） | — | ✅ |
| 合规报告（审计导出 / 留存策略） | — | ✅ |

### 管理与运维

| 功能 | 社区版 | 企业版 |
|------|:------:|:------:|
| CLI 管理工具（aegisctl） | ✅ | ✅ |
| Prometheus 指标端点 | ✅ | ✅ |
| 成本跟踪 | ✅ | ✅ |
| CHANGELOG + 语义版本管理 | ✅ ᴺᴱᵂ | ✅ ᴺᴱᵂ |
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
| Java / Kotlin | ᴺᴱᵂ | ᴺᴱᵂ |
| Rust | ᴺᴱᵂ | ᴺᴱᵂ |

---

## Phase 5：生产验证与 GA 发布

> 从"可以工作"到"值得信赖"。以发布 v1.0 GA 为里程碑，建立 API 稳定性承诺、补全 CHANGELOG、完成生产环境验证。

### 5.1 API 稳定性与版本治理

| 项目 | 内容 | 交付物 |
|------|------|--------|
| API 版本策略 | 建立 `/v1/` 前缀语义版本承诺，breaking change 需双版本共存一个 major cycle | 版本策略文档 |
| CHANGELOG 补全 | 追溯 v0.1.0→当前所有功能变更，建立 Keep a Changelog 规范流程 | `CHANGELOG.md` 完整版 |
| 弃用策略 | deprecated 注解 + 迁移指南 + 最少两个 minor 版本缓冲期 | 弃用策略文档 |
| SDK 向后兼容 | SDK 版本与 API 版本对齐，major SDK 版本锁定 API 版本 | SDK 版本矩阵 |

### 5.2 生产环境验证

| 项目 | 内容 | 成功标准 |
|------|------|---------|
| 灰度部署方案 | 选 1-2 个内部真实 AI 应用场景接入 AegisGate | 承载真实流量 7 天无 P0 |
| 长时间压力测试 | 72 小时连续压测：混合读写 + 流式/非流式 + 故障注入 | 零内存泄漏、零崩溃、P99 < 100ms |
| 混沌工程 | 上游断连 / Redis 宕机 / 磁盘满 / OOM 边界 / 高并发 key 轮转 | 所有场景优雅降级，不丢请求 |
| 安全渗透测试 | 自动化安全扫描（OWASP ZAP / Nuclei）+ 手动渗透测试 | 零高危/严重漏洞 |
| 内存安全专项 | ASAN + UBSAN + TSAN 24 小时长跑 + Valgrind memcheck | 零 ASAN/TSAN 报告 |

### 5.3 CI/CD 完善

| 项目 | 内容 | 价值 |
|------|------|------|
| OTEL ON CI 常态化 | OpenTelemetry 条件编译路径加入 CI 矩阵 | 消除 P1 遗留技术债 |
| Helm Chart 集成测试 | CI 中 kind 集群 + Helm install + smoke test | 部署可靠性验证 |
| Release 自动化 | Tag 触发自动构建多架构 Docker 镜像 + GitHub Release | 发布效率 |
| 基准回归门禁 | PR 门禁中运行核心路径 benchmark，性能回退 >10% 阻断 | 性能安全网 |

### 5.4 v1.0 GA 发布清单

| 项目 | 内容 |
|------|------|
| 语义版本号 | v1.0.0 正式发布 |
| 完整 CHANGELOG | v0.1.0→v1.0.0 全部变更记录 |
| API 稳定性声明 | `/v1/` 端点向后兼容承诺 |
| 安全公告流程 | `SECURITY.md` + 漏洞报告 + 响应 SLA |
| License 条款 | 社区版 Apache 2.0 / 企业版商业 License 条款明确 |
| 官方 Docker 镜像 | `ghcr.io/privonyx/loong-aegisgate:1.0.0` |
| 升级指南 | v0.x → v1.0 迁移文档 |

---

## Phase 6：平台进化 ✅ 已完成（2026-05-15，TASK-20260513-01）

> 从"LLM Chat 代理"进化为"全模态 AI 平台网关"。覆盖 embedding、image、audio 等多模态端点，集成外部向量数据库和安全 API。
>
> **完成证据：** ctest CP+ON 175/175 + vitest 32/32。

### 6.1 多模态 API 代理 ✅ 已完成

| 端点 | 内容 | 价值 |
|------|------|------|
| `/v1/embeddings` | 统一 embedding API（OpenAI / Cohere / 本地 ONNX），自动路由到最优模型 | 覆盖 RAG 场景 |
| `/v1/images/generations` | 统一图像生成（DALL·E / Stability / Midjourney API） | 多模态应用 |
| `/v1/audio/transcriptions` | 统一语音转写（Whisper / Azure Speech） | 语音场景 |
| `/v1/audio/speech` | 统一语音合成（OpenAI TTS / Azure / ElevenLabs） | 语音场景 |
| `/v1/moderations` | 内容审核端点，统一 OpenAI Moderation / Google Perspective | 安全合规 |

**架构设计要点：**
- 每种模态实现独立的 `ModalityHandler` 接口（CR2 方案 A 瘦 Handler + 胖 Router 落地）
- 复用现有管道（认证→限流→护栏→路由→审计）
- 模态级成本追踪和配额管理（`CostTracker.modality` + `ModalityRateLimiter`）
- 输入/输出大小限制（图像/音频文件体积阈值）

### 6.2 外部向量数据库集成 ✅ 已完成

| 项目 | 内容 | 价值 |
|------|------|------|
| VectorStore 抽象接口 | `VectorStore::insert()/search()/delete()`，与 hnswlib 解耦 | 可扩展性基础 |
| Milvus 适配器 | gRPC 客户端，支持分区和索引管理 | 百万级向量 |
| Qdrant 适配器 | REST/gRPC 客户端，支持 payload 过滤 | 百万级向量 |
| 双模式运行 | 社区版 hnswlib（进程内）/ 企业版 Milvus/Qdrant | 按需扩展 |
| 缓存迁移工具 | hnswlib → Milvus/Qdrant **离线 dump+restore CLI**（D5=B），SHA-256 + 租户白名单 + API key 三重安全门 | 平滑升级 |

### 6.3 外部安全 API 集成 ✅ 已完成

| 项目 | 内容 | 价值 |
|------|------|------|
| L4 外部安全层 | 架构上新增第 4 层安全检测：外部 API 调用 | 安全纵深 |
| OpenAI Moderation | 集成 `/v1/moderations` 端点，异步 fire-and-forget 或同步拦截 | 语义级安全 |
| Google Perspective | Perspective API 毒性/威胁/侮辱评分 | 多维度安全 |
| 异步模式 | 影子分析模式（不阻塞请求，异步记录），可配置切换为同步拦截 — Epic 4 落地，500ms provider 实测 process() < 10ms | 延迟/安全平衡 |
| 失败策略 | 外部 API 不可用时 fail-open + 降级到 L1/L2/L3 | 可用性保障 |
| Shadow audit + 背压 | **SR3** 每次 shadow 扫描必写审计 entry；**SR6** `shadow_max_inflight` atomic counter 防 worker 堆积 | 可观测 + 防止云 API 抖动放大成内部资源耗尽 |

### 6.4 多轮对话缓存升级 ✅ 已完成

| 项目 | 内容 | 价值 |
|------|------|------|
| 对话摘要缓存键 | `SHA-256(tenant_id) + SHA-256(conversation_id) + summary` partition_key V2（**SR1 跨租户硬隔离**） | 避免上下文不同时误命中 + 多租户安全 |
| 轻量摘要生成 | `CompositeSummarizer`：ONNX 主路径 + RuleBased fallback（CR1 方案 B 装饰器，primary 返空自动回落 + `fallback_count_` 上报） | 零外部依赖 + 运行时容错 |
| 对话 ID 关联 | 支持 `metadata.conversation_id`（D2=C 客户端优先 + 服务端 SHA-256 历史推导 + request_id 兜底） | 多轮场景 |
| 缓存淘汰策略 | `ConversationCacheEvictor` 4 因子评分（频次 / 最近性 / 体量 / TTL） | 缓存质量 |

---

## Phase 7：生态与社区建设

> 从"技术产品"到"开发者生态"。降低贡献门槛，建立社区基础设施，完善文档和 SDK。

### 7.1 社区基础设施

| 项目 | 内容 | 价值 |
|------|------|------|
| GitHub Discussions | 启用 Q&A / Ideas / Show and Tell 分区 | 社区交流 |
| Discord 频道 | 开发者实时交流 + 中英文频道 | 社区粘性 |
| Good First Issues | 标注 20+ 入门友好的 Issue（文档/测试/小功能） | 降低贡献门槛 |
| CONTRIBUTING 增强 | 开发环境搭建一键脚本 + 架构导览 + 常见开发任务 Cookbook | 贡献者友好 |
| 贡献者公约 | Contributor License Agreement (CLA) + Code of Conduct | 开源治理 |
| 社区治理 | Maintainer 权限分级 + PR Review 流程 + Release 审批 | 可持续运营 |

### 7.2 文档国际化

| 项目 | 内容 | 价值 |
|------|------|------|
| 核心文档英文化 | 快速入门、架构概览、API 参考、部署指南 | 国际社区 |
| API Reference 自动生成 | Doxygen/Sphinx 从代码注释自动生成 API 文档 | 文档一致性 |
| 教程系列 | 5 分钟快速入门 / 安全最佳实践 / 性能调优 / 生产部署 / SDK 集成 | 采用率 |
| 交互式 Playground | 在线 Demo 环境，无需本地安装即可体验 | 降低试用门槛 |

### 7.3 SDK 生产化增强

| SDK | 增强内容 | 价值 |
|-----|---------|------|
| **通用增强** | 指数退避重试 + 可配超时 + 连接池复用 + OpenTelemetry 追踪注入 | 生产级可靠性 |
| **Python** | async/await 原生支持 + Pydantic 类型模型 + streaming iterator | Python 生态融合 |
| **Node.js** | TypeScript 类型安全 + streaming ReadableStream + 自动重连 | TS 生态融合 |
| **Go** | context.Context 传播 + 结构化错误 + streaming io.Reader | Go 生态融合 |
| **Java/Kotlin** ᴺᴱᵂ | OkHttp/Ktor 客户端 + Kotlin coroutines + Spring Boot starter | JVM 生态覆盖 |
| **Rust** ᴺᴱᵂ | reqwest + tokio async + serde 序列化 + tracing 集成 | Rust 生态覆盖 |

### 7.4 竞品基准对比

| 项目 | 内容 | 价值 |
|------|------|------|
| 标准化基准套件 | 统一测试场景（单请求延迟 / 并发吞吐 / 流式首字 / 缓存命中） | 公平对比 |
| vs LiteLLM | 单机吞吐、延迟分位数、内存占用对比 | 性能差异化 |
| vs Portkey | 功能覆盖度 + 安全能力 + 自托管灵活性对比 | 场景差异化 |
| 对比报告 | 发布在官方文档和博客 | 市场认知 |

---

## Phase 8：下一代智能

> 从"AI 代理网关"到"AI 应用基础设施"。支持 Agent 编排、RAG 管道集成、智能化缓存，为复杂 AI 应用提供基础设施级支持。

### 8.1 Agent 编排基础

| 项目 | 内容 | 价值 |
|------|------|------|
| Tool Registry | 注册和管理可供 AI 调用的工具集，支持版本化 | Agent 基础 |
| Tool 执行沙箱 | 工具调用安全隔离执行环境（syscall 过滤 + 超时 + 资源限制） | Agent 安全 |
| 多步编排 | 支持 ReAct / Plan-and-Execute 模式的多步工具调用链 | 复杂 Agent |
| 工具调用审计 | 每次工具调用的输入/输出/耗时/成本完整记录 | 可追溯性 |
| 工具级限流 | per-tool 速率限制和并发控制 | 安全防护 |

### 8.2 RAG 管道集成

| 项目 | 内容 | 价值 |
|------|------|------|
| 知识库管理 | 文档上传 → 分块 → embedding → 向量存储管理 API | RAG 基础 |
| 检索增强阶段 | 新增 `RetrievalStage`：请求到达时检索相关知识注入 context | RAG 管道 |
| 幻觉检测增强 | 将检索到的事实作为 ground truth 对照 LLM 输出 | 可信度提升 |
| 引用追踪 | 输出中标注信息来源（哪个文档的哪个片段） | 可验证性 |

### 8.3 智能化缓存 2.0

| 项目 | 内容 | 价值 |
|------|------|------|
| 语义压缩缓存 | 对长响应进行语义压缩存储，命中时按需展开 | 缓存容量 |
| 预测性缓存预热 | 基于历史查询模式预测热门查询，提前填充缓存 | 命中率 |
| 跨租户匿名缓存 | 去除租户标识的通用知识缓存层，在安全策略允许的前提下共享 | 全局效率 |
| 缓存质量反馈 | 用户对缓存命中结果的满意度反馈，动态调整阈值 | 自适应 |

### 8.4 高级可观测性

| 项目 | 内容 | 价值 |
|------|------|------|
| AI 调用成本归因 | 按应用 / 功能模块 / 用户粒度的成本归因分析 | 成本优化 |
| 异常检测 | 基于统计模型的异常请求模式检测（突增/突降/异常分布） | 主动运维 |
| 质量趋势监控 | 模型输出质量随时间变化的趋势追踪和告警 | 模型退化预警 |
| 成本优化建议 | 基于使用模式自动推荐最优路由策略和模型组合 | 智能运维 |

---

## Phase 9：全球化规模部署

> 从"单集群"到"全球分布式"。支持多区域部署、边缘缓存、合规数据驻留。

### 9.1 多区域部署

| 项目 | 内容 | 价值 |
|------|------|------|
| 区域感知路由 | 请求就近路由到最低延迟的 AI 提供商区域端点 | 延迟优化 |
| 跨区域缓存同步 | 语义缓存跨区域异步复制，热数据全球可用 | 全局命中率 |
| 数据驻留策略 | 按租户配置数据驻留区域（审计日志、缓存数据不跨区） | 合规要求 |
| 多集群联邦 | 中央控制面 + 区域数据面架构 | 全球规模 |

### 9.2 边缘部署

| 项目 | 内容 | 价值 |
|------|------|------|
| 轻量级边缘节点 | 精简 AegisGate binary（仅认证+限流+缓存+路由） | 边缘延迟 |
| 边缘缓存 | CDN 层面的语义缓存命中 | 极低延迟 |
| 边缘安全 | 在边缘执行基础安全检查，可疑请求回源深度检查 | 安全+性能 |

### 9.3 高级合规

| 项目 | 内容 | 价值 |
|------|------|------|
| SOC 2 合规框架 | 安全控制映射 + 证据收集自动化 | 企业合规 |
| GDPR 支持 | 数据删除 API + 访问权请求 + 同意管理 | 欧洲市场 |
| ISO 27001 对齐 | 信息安全管理体系控制项映射 | 国际认证 |
| 审计报告自动化 | 定时生成合规报告 + 异常自动通知 | 合规效率 |

---

## 发展阶段总览

```
Phase 0-4 ✅ 已完成      功能构建基础
   │
Phase 5  生产验证 + GA    API 稳定性 + 生产验证 + CI/CD + v1.0 发布
   │
Phase 6  平台进化         多模态 + 外部向量 DB + 安全 API + 缓存升级
   │
Phase 7  生态与社区       社区基础 + 文档国际化 + SDK 增强 + 竞品对比
   │
Phase 8  下一代智能       Agent 编排 + RAG 集成 + 智能缓存 + 高级可观测
   │
Phase 9  全球化规模       多区域 + 边缘部署 + 高级合规
```

### 复杂度与技术栈要求

| Phase | 核心技能要求 | 侵入性 |
|-------|-------------|--------|
| Phase 5 | CI/CD 工程、压力测试、安全审计 | 低（主要文档和配置） |
| Phase 6 | gRPC 客户端、多模态 API、向量数据库 | 中（新增模块，复用管道） |
| Phase 7 | 技术写作、SDK 多语言、社区运营 | 低（主要文档和 SDK） |
| Phase 8 | Agent 编排、RAG 架构、ML 工程 | 高（核心管道扩展） |
| Phase 9 | 分布式系统、全球网络、合规工程 | 高（架构级变更） |

---

## 关键里程碑

| 里程碑 | 状态 | 标志性成果 |
|--------|------|----------|
| v0.1.0 — 首个发布 | ✅ 完成 | 统一 AI 网关 + 安全护栏 + 语义缓存 + SDK |
| v0.2.0 — 多 Provider | ✅ 完成 | DeepSeek/豆包 + ConnectorFactory 重构 |
| v0.3.0 — 生产就绪 | ✅ 完成 | 技术债清零 + Docker + 优雅关闭 + Chaos 测试 |
| v0.4.0 — 社区版进化 | ✅ 完成 | OpenTelemetry + 安全升级 + 缓存进化 + 开发者体验 |
| v0.5.0 — 企业版基础 | ✅ 完成 | RBAC + 多租户 + Web 管理面板 |
| v0.6.0 — 企业版高级 | ✅ 完成 | SSO + 规则引擎 + ML 路由 + A/B 测试 + 质量评分 |
| v0.7.0 — 规模化 | ✅ 完成 | 集群部署 + 插件系统 + 规则市场 |
| v0.8.0 — 安全加固 | ✅ 完成 | Ed25519 License + 审计加密 + IP 允许列表 |
| v0.9.0 — AI 增强 | ✅ 完成 | Function Calling + Token 优化系统 |
| **v1.0.0 GA — 正式发布** | **✅ 完成** | **API 稳定承诺 + Release 自动化 + Agent 编排 + RAG + 智能缓存 2.0 + 高级可观测** |
| **v1.3 — 社区版增强** | **✅ 完成** | **5 语言 SDK（+Java +Rust）+ 竞品基准对比报告 + 社区治理** |
| v2.1 — 全球部署 | 🔜 计划 | 多区域路由 + 跨区缓存同步 + 合规框架 |

> 详细版本变更记录请参阅 [CHANGELOG.md](../CHANGELOG.md)，API 稳定性策略请参阅 [VERSIONING.md](../VERSIONING.md)。

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
Phase 5.1 API 稳定性 ──────→ Phase 5.4 v1.0 GA
Phase 5.2 生产验证   ──────→ Phase 5.4 v1.0 GA
Phase 5.3 CI/CD 完善 ──────→ Phase 5.4 v1.0 GA
                                    │
              ┌─────────────────────┼─────────────────────┐
              ↓                     ↓                     ↓
Phase 6.1 多模态 API        Phase 6.2 外部向量 DB    Phase 6.3 外部安全 API
Phase 6.4 对话缓存升级             │
              │                     │
              ↓                     ↓
       Phase 7.1-7.4          Phase 8.1 Agent 编排
       生态与社区建设          Phase 8.2 RAG 集成
                               Phase 8.3 智能缓存 2.0
                                    │
                                    ↓
                              Phase 9.1-9.3
                              全球化规模部署
```

---

## 技术风险与缓解

| 风险 | 影响 | 缓解策略 |
|------|------|---------|
| C++ 社区贡献门槛高 | 生态增长受限 | SDK/文档/CLI 用其他语言接受贡献；核心 C++ 维护者招募 |
| 多模态 API 变化快 | 维护成本 | 抽象 ModalityHandler 接口，provider 变更不影响核心 |
| 向量 DB 生态碎片化 | 适配成本 | VectorStore 抽象接口，按用户需求优先适配 Top 2 |
| Agent 安全风险 | 工具调用滥用 | 沙箱隔离 + 审批机制 + 资源限制三层防护 |
| 全球部署网络复杂度 | 运维难度 | 先单区域集群验证，再渐进式多区域 |

---

## 附录：Phase 0-4 完成记录

| Phase | 完成日期 | 核心交付 |
|-------|---------|---------|
| Phase 0 夯实根基 | 2026-03-21 | 技术债清零 + Docker + Chaos 测试 |
| Phase 1 社区版进化 | 2026-03-22 | OTEL 追踪 + 安全升级 + 缓存进化 + 开发者体验 |
| Phase 2 企业版基础 | 2026-03-22 | RBAC 多租户 + Web 管理面板 |
| Phase 3 企业版高级 | 2026-03-26 | SSO + 规则引擎 + ML 路由 + A/B 测试 + 质量评分 + 用量预测 |
| Phase 4.1 集群部署 | 2026-03-26 | RedisStateStore + Helm Chart + docker-compose 集群 |
| Phase 4.2 生态系统 | 2026-03-26 | 插件系统 (dlopen .so) + 规则市场 (aegisctl rules) |
| 补充: Token 优化 | 2026-03-28 | PromptCompressor + SmartMaxTokens + TokenEstimator |
| 补充: 安全保证 | 2026-03-29 | Ed25519 License + 审计链哈希 + 审计加密 + IP 白名单 |
| 补充: Function Calling | 2026-03-30 | Tool Use 全链路支持（类型系统+连接器+Runtime+护栏+缓存） |

**96/96 测试通过。** 源码 171 文件 ~29,000 行 C++，测试 100 文件 ~16,000 行。

---

## 贡献

欢迎社区贡献！请参阅 [CONTRIBUTING.md](../CONTRIBUTING.md) 了解开发流程。

社区版相关的 Issue 和 PR 优先处理。企业版功能的设计讨论也欢迎参与。

---

## 后续路线

本文档记录 Phase 5-8 的战略与交付。后续路线分为两个并行轨道：

- **开源核心工程路线**：全球化 × 平台化 × 智能化（Phase 9-14）
- **产品 / 商业化叠加路线**：品牌升级 → 行业合规包 → SaaS 控制平面

两条轨道并存：工程与商业化叠加互不取代。
