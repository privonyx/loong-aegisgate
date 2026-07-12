# AegisGate 架构指南

本文档详细描述 AegisGate 的系统架构、请求处理流程、核心组件交互以及关键时序。

## 系统全景

```
                         ┌──────────────────────────────────────────────────────────────┐
                         │                        AegisGate                             │
                         │                                                              │
   Client ──── HTTP ────►│  API Controller ─► GatewayRuntime ─► Pipeline Engine         │
   (SDK/curl)            │       │                  │                │                   │
                         │       │           ┌──────┴──────┐   ┌────┴────┐              │
                         │       │           │   Router    │   │ Inbound │              │
                         │       │           │ (ML/AB/Cost)│   │ Pipeline│              │
                         │       │           └──────┬──────┘   └────┬────┘              │
                         │       │                  │               │                   │
                         │       │           ┌──────┴──────┐   ┌────┴────┐              │
                         │       │           │  Fallback   │   │Outbound │              │
                         │       │           │  Manager    │   │Pipeline │              │
                         │       │           └──────┬──────┘   └────┬────┘              │
                         │       │                  │               │                   │
                         │  Admin Controller        │          Observability             │
                         │  (REST + WebSocket)      │   (Metrics/Cost/Quality/Predict)  │
                         │                          │                                   │
                         └──────────────────────────┼───────────────────────────────────┘
                                                    │
                                          ┌─────────┴─────────┐
                                          │   AI Providers    │
                                          │ OpenAI / Claude / │
                                          │ DeepSeek / Doubao │
                                          │ Qwen / Gemini ... │
                                          └───────────────────┘
```

## 请求处理流程

### 完整请求流水线

```
客户端请求
    │
    ▼
┌─────────────────┐
│  认证 & 限流     │  API Key 验证 → RBAC 鉴权 → 令牌桶限流 → 滥用检测
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  入站管道        │  8 个 Stage 依次执行
│                 │
│  ① AuditLogger  │  记录请求审计日志
│  ② Preprocessor │  Unicode NFKC 规范化 + 编码检测
│  ③ Injection    │  L1 关键词 + L2 启发式注入检测
│  ④ GuardModel   │  L3 ONNX 分类器（可选）
│  ⑤ PIIFilter    │  RE2 正则脱敏（手机/邮箱/身份证/密钥）
│  ⑥ TopicGuard   │  话题白名单/黑名单过滤
│  ⑦ RuleEngine   │  YAML 声明式自定义规则（企业版）
│  ⑧ SemanticCache│  语义相似性匹配 → 命中则 ShortCircuit
└────────┬────────┘
         │
         ▼  （未命中缓存时）
┌─────────────────┐
│  路由选择        │  BasicRouter / CostAwareRouter / MLRouter / ABTestRouter
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  租户限制检查     │  模型白名单 → 日/月成本上限
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  模型调用        │  FallbackManager 自动降级 → LoadBalancer 轮转 API Key
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  出站管道        │  6 个 Stage 依次执行
│                 │
│  ① ContentFilter│  有害内容过滤
│  ② Hallucination│  幻觉检测评分
│  ③ QualityScorer│  输出质量 4 维评分
│  ④ CostTracker  │  Token 成本计算与记录
│  ⑤ AlertManager │  阈值告警检查
│  ⑥ RequestLogger│  最终状态日志
└────────┬────────┘
         │
         ▼
    返回响应给客户端
```

### Pipeline Stage 状态机

每个 PipelineStage 返回以下状态之一：

```
           ┌──────────┐
           │ Continue │─────────► 传递到下一个 Stage
           └──────────┘
           ┌──────────────┐
           │ ShortCircuit │─────► 跳过剩余 Stage（缓存命中）
           └──────────────┘
           ┌──────────┐
           │  Reject  │─────────► 拒绝请求（安全违规，返回 403）
           └──────────┘
           ┌──────────┐
           │  Error   │─────────► 内部错误（返回 500）
           └──────────┘
```

## 路由系统架构

AegisGate 支持 4 种路由策略，通过配置切换：

```
                    ┌─────────────────────────┐
                    │     Router 接口          │
                    │  selectModel(ctx, reg)  │
                    └────────────┬────────────┘
                                 │
            ┌────────────────────┼────────────────────┐
            │                    │                    │
   ┌────────┴─────────┐ ┌───────┴────────┐ ┌────────┴────────┐
   │   BasicRouter    │ │CostAwareRouter │ │    MLRouter     │
   │                  │ │                │ │                 │
   │  显式模型 → tag  │ │ 字符数分档     │ │ 三维加权评分    │
   │  → 默认模型      │ │ economy/premium│ │ 成本×质量×延迟  │
   └──────────────────┘ └────────────────┘ │ EMA 动态更新    │
                                           └────────┬────────┘
                                                    │
                                           ┌────────┴────────┐
                                           │  ABTestRouter   │
                                           │  (装饰器模式)    │
                                           │                 │
                                           │ 包装任意 Router  │
                                           │ 确定性流量分配   │
                                           │ hash % weight   │
                                           └─────────────────┘
```

### MLRouter 评分公式

```
综合分 = w_cost × CostScore + w_quality × QualityScore + w_latency × LatencyScore

其中:
  CostScore    = 1.0 − (model_cost − min_cost) / (max_cost − min_cost)
  QualityScore = success_rate（EMA 更新）
  LatencyScore = 1.0 − (avg_latency − min_latency) / (max_latency − min_latency)

默认权重: cost=0.4, quality=0.35, latency=0.25
```

### ABTestRouter 分配算法

```
slot = hash(experiment_name + request_id) % total_weight

variants:  [  model-a (weight=70)  |  model-b (weight=30)  ]
           [  slot 0-69 → model-a  |  slot 70-99 → model-b ]

同一 request_id 始终分配到同一变体（确定性）
```

## 核心时序图

### 非流式请求完整时序

```
Client            ApiController     GatewayRuntime      InboundPipeline    Router         FallbackMgr      OutboundPipeline
  │                    │                  │                   │               │                │                  │
  │── POST /v1/chat ──►│                  │                   │               │                │                  │
  │                    │── processReq ───►│                   │               │                │                  │
  │                    │                  │── Auth+RateLimit──│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── execute ────────►│               │                │                  │
  │                    │                  │                   │── Audit ──►   │                │                  │
  │                    │                  │                   │── Inject ──►  │                │                  │
  │                    │                  │                   │── PII ────►   │                │                  │
  │                    │                  │                   │── Cache ──►   │                │                  │
  │                    │                  │                   │◄── Continue ──│                │                  │
  │                    │                  │◄── Continue ──────│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── selectModel ────────────────────►│                │                  │
  │                    │                  │◄── "gpt-4o-mini" ────────────────│                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── costLimitCheck──│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── executeWithFallback ────────────────────────────►│                  │
  │                    │                  │                   │               │  ┌── OpenAI ──►│                  │
  │                    │                  │                   │               │  │◄── resp ────│                  │
  │                    │                  │◄── ChatResponse ──────────────────────────────────│                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── reportOutcome ──────────────────►│ (MLRouter)    │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── cacheStore ─────│               │                │                  │
  │                    │                  │                   │               │                │                  │
  │                    │                  │── execute ────────────────────────────────────────────────────────────►│
  │                    │                  │                   │               │                │  ── ContentFilter │
  │                    │                  │                   │               │                │  ── Hallucination │
  │                    │                  │                   │               │                │  ── QualityScorer │
  │                    │                  │                   │               │                │  ── CostTracker   │
  │                    │                  │                   │               │                │  ── AlertManager  │
  │                    │                  │                   │               │                │  ── RequestLogger │
  │                    │                  │◄── Continue ──────────────────────────────────────────────────────────│
  │                    │                  │                   │               │                │                  │
  │                    │◄── ProcessResult─│                   │               │                │                  │
  │◄── JSON Response ──│                  │                   │               │                │                  │
```

### 流式请求时序

```
Client            ApiController     GatewayRuntime      FallbackMgr       Provider
  │                    │                  │                   │               │
  │── POST (stream) ──►│                  │                   │               │
  │                    │── processStream─►│                   │               │
  │                    │                  │── Auth+Inbound ──►│               │
  │                    │                  │── selectModel ────│               │
  │                    │                  │                   │               │
  │                    │                  │── streamWithFallback ─────────────►│
  │                    │                  │                   │── SSE req ────►│
  │                    │                  │                   │               │
  │◄── SSE: chunk 1 ──│◄─── onChunk ────│◄── chunk ────────│◄── chunk ──────│
  │◄── SSE: chunk 2 ──│◄─── onChunk ────│◄── chunk ────────│◄── chunk ──────│
  │◄── SSE: chunk N ──│◄─── onChunk ────│◄── chunk ────────│◄── chunk ──────│
  │                    │                  │                   │               │
  │                    │                  │◄── onDone ───────│◄── [DONE] ────│
  │                    │                  │── outbound.exec──│               │
  │                    │                  │── cacheStore ────│               │
  │                    │                  │── reportOutcome ─│               │
  │◄── SSE: [DONE] ───│◄─── onDone ─────│                   │               │
```

### 缓存命中时序（短路）

```
Client            GatewayRuntime      SemanticCache       Embedder
  │                    │                   │                  │
  │── request ────────►│                   │                  │
  │                    │── inbound.exec ──►│                  │
  │                    │                   │── embed(prompt) ─►│
  │                    │                   │◄── vector ───────│
  │                    │                   │── hnswlib search ─│
  │                    │                   │── similarity > θ ─│
  │                    │                   │                  │
  │                    │◄── ShortCircuit ──│                  │
  │                    │   ctx.cache_hit   │                  │
  │                    │   ctx.cached_resp │                  │
  │                    │                   │                  │
  │◄── cached resp ───│   (跳过模型调用，零 API 成本)         │
```

### 降级容错时序

```
Client            GatewayRuntime      FallbackMgr       Primary          Fallback
  │                    │                  │                │                │
  │── request ────────►│                  │                │                │
  │                    │── execute ───────►│                │                │
  │                    │                  │── call ────────►│                │
  │                    │                  │◄── timeout ────│                │
  │                    │                  │                │                │
  │                    │                  │── call ─────────────────────────►│
  │                    │                  │◄── response ────────────────────│
  │                    │                  │                │                │
  │                    │◄── response ─────│                │                │
  │◄── response ──────│                  │                │                │
```

## 存储架构

```
┌──────────────────────────────────────────────────────────┐
│                    应用层                                 │
│  AdminController / CostTracker / AuditLogger / AuthService│
└─────────────────────────┬────────────────────────────────┘
                          │
            ┌─────────────┴─────────────┐
            │                           │
   ┌────────┴────────┐       ┌─────────┴─────────┐
   │   CacheStore    │       │  PersistentStore  │
   │   (KV + TTL)    │       │  (表级操作)        │
   └────────┬────────┘       └─────────┬─────────┘
            │                          │
      ┌─────┴─────┐            ┌──────┼──────┐
      │           │            │      │      │
   Memory     Redis         Memory  SQLite  PG
   (社区版)   (企业版)      (测试)  (社区版) (企业版)
```

## 安全护栏层级

```
       请求输入
          │
          ▼
   ┌──────────────┐
   │ L1: 关键词    │  正则模式匹配（最快，~1μs）
   │    + 启发式   │  特殊字符密度、角色切换检测
   └──────┬───────┘
          │ 通过
          ▼
   ┌──────────────┐
   │ L2: 编码检测  │  Base64/Hex/URL 解码后二次检查
   │  + Unicode   │  NFKC 规范化防混淆攻击
   └──────┬───────┘
          │ 通过
          ▼
   ┌──────────────┐
   │ L3: ONNX 分类│  神经网络安全分类（可选，~5ms）
   └──────┬───────┘
          │ 通过
          ▼
   ┌──────────────┐
   │ L4: PII 脱敏 │  RE2 线性时间正则（防 ReDoS）
   └──────┬───────┘
          │ 通过
          ▼
   ┌──────────────┐
   │ L5: 话题边界 │  白名单/黑名单
   └──────┬───────┘
          │ 通过
          ▼
   ┌──────────────┐
   │ L6: 自定义规则│  YAML 声明式规则引擎（企业版）
   └──────┬───────┘
          │ 通过
          ▼
      继续处理
```

## 成本优化架构

```
                    ┌──────────────────┐
                    │   请求入口        │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  语义缓存拦截     │ ──── 命中 ──► 零成本返回
                    │  (省重复调用)     │
                    └────────┬─────────┘
                             │ 未命中
                    ┌────────▼─────────┐
                    │  智能路由选择     │
                    │                  │
                    │  ┌─ MLRouter ──┐ │  根据成本/质量/延迟
                    │  │ 三维评分    │ │  动态选最优模型
                    │  └─────────────┘ │
                    │  ┌─ ABTest ────┐ │  A/B 对比找性价比
                    │  │ 流量分配    │ │  最优模型组合
                    │  └─────────────┘ │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  租户成本限额     │ ──── 超限 ──► 429 拒绝
                    │  (日/月上限)      │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  模型调用         │
                    └────────┬─────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
     ┌────────▼────┐  ┌─────▼─────┐  ┌────▼──────┐
     │ QualityScore│  │CostTracker│  │ Usage     │
     │ 质量反馈    │  │ 成本记录  │  │ Predictor │
     │ → MLRouter  │  │ → 仪表盘  │  │ → 趋势预测│
     └─────────────┘  └───────────┘  └───────────┘
```

详细的成本优化指南请参见 [成本优化指南](./cost-optimization_zh.md)。

## 企业版扩展架构

```
   ┌────────────────────────────────────────────────┐
   │              企业版附加组件                      │
   │                                                │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
   │  │   RBAC   │  │   SSO    │  │  Web 管理面板 │ │
   │  │ 角色权限 │  │OIDC/OAuth│  │  React SPA   │ │
   │  └──────────┘  └──────────┘  └──────────────┘ │
   │                                                │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
   │  │  多租户  │  │   MFA    │  │  规则引擎     │ │
   │  │  隔离    │  │  TOTP    │  │  热加载       │ │
   │  └──────────┘  └──────────┘  └──────────────┘ │
   │                                                │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
   │  │PostgreSQL│  │  Redis   │  │  合规报告     │ │
   │  │ 持久化   │  │  缓存    │  │  CSV 导出     │ │
   │  └──────────┘  └──────────┘  └──────────────┘ │
   └────────────────────────────────────────────────┘
```

## 部署架构

### 单机部署（社区版）

```
   ┌─────────────────────────────┐
   │      Docker Container       │
   │                             │
   │  aegisgate binary           │
   │       │                     │
   │       ├── SQLite DB         │
   │       ├── ONNX 模型（可选）  │
   │       └── config/*.yaml     │
   │                             │
   └─────────────┬───────────────┘
                 │
          ┌──────┴──────┐
          │  Prometheus  │
          │  + Grafana   │
          └─────────────┘
```

### 集群部署（企业版，规划中）

```
                    ┌──────────┐
                    │   LB     │
                    └────┬─────┘
               ┌─────────┼─────────┐
               │         │         │
         ┌─────┴───┐ ┌───┴───┐ ┌──┴──────┐
         │AegisGate│ │AegisGa│ │AegisGate│
         │ Node 1  │ │ Node 2│ │ Node 3  │
         └────┬────┘ └───┬───┘ └────┬────┘
              │          │          │
         ┌────┴──────────┴──────────┴────┐
         │          共享状态              │
         │   Redis（缓存 + 限流状态）     │
         │   PostgreSQL（持久化数据）      │
         └───────────────────────────────┘
```

## 相关文档

- [快速开始](./quick-start_zh.md) — 编译、配置、首次调用
- [成本优化指南](./cost-optimization_zh.md) — 省钱策略详解
- [管理 API 参考](./admin-api_zh.md) — Admin REST API 完整参考
- [错误码参考](./error-codes_zh.md) — AEGIS-xxxx 错误码
- [性能调优](./performance-tuning_zh.md) — 缓存、限流、线程优化
- [安全最佳实践](./security-best-practices_zh.md) — 密钥、TLS、护栏规则
