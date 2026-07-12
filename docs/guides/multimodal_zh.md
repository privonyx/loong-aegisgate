# 多模态路由指南

> 覆盖功能：Phase 6.1 多模态 API + ModalityRouter（CR2 方案 A 瘦 Handler + 胖 Router）
> 可用版本：v1.2+（TASK-20260513-01 落地）

本指南说明 AegisGate 如何把多模态请求（embedding / 图像生成 / 语音转写 / 语音翻译 / 语音合成 / Moderation）接入和现有 chat completion 同一套护栏 / 计费 / 审计链路。

## 它解决什么问题

之前多模态端点（含 `/v1/audio/translations`）只是裸 passthrough，缺乏：

- 模态级别的成本归因（搞不清这个月在 image_gen 上花了多少）
- 模态级别的限流（一个客户突然上 1 万张图把整个网关打爆）
- 多 provider 间的策略性路由（"我有 OpenAI + Voyage 两家 embedding，按价格选便宜的"）

升级后一套接口同时解决三件事：

| 组件 | 文件 |
|---|---|
| `Modality` enum + `ModalityRouter` | `src/multimodal/modality.{h,cpp}` + `modality_router.{h,cpp}` |
| 5 个 `OpenAI{Embedding,ImageGen,AudioTranscribe,AudioSpeech,Moderation}Handler` | `src/multimodal/openai_modality_handlers.{h,cpp}` |
| `OpenAIModalityUpstreamAdapter`（适配现有 `OpenAIConnector`） | `src/multimodal/openai_modality_upstream_adapter.h` |
| `CostTracker.modality` 维度 | `src/observe/cost_tracker.{h,cpp}` |
| `ModalityRateLimiter`（per-modality token bucket） | `src/multimodal/modality_rate_limiter.{h,cpp}` |

## 启用步骤

```yaml
multimodal:
  enabled: true
  policy: cheapest          # cheapest | round_robin | fastest_p99
  cost_attribution:
    enabled: true           # CostTracker.modality 维度记录（默认 on）
  rate_limit:
    enabled: true
    quotas:
      - modality: image_gen
        identity: "*"        # "*" 表示全局默认
        max_tokens: 60
        refill_rate: 1.0     # 60 tokens / 60 s ≈ 1 req/s
      - modality: image_gen
        identity: "tenant-acme"
        max_tokens: 10
        refill_rate: 0.0028  # ≈ 10 / hour，更严格的针对性限流
```

启动日志：

```
ModalityRouter: wired with 5 OpenAI handlers (policy=cheapest)
ModalityRateLimiter: 2 quotas configured (per-request enforcement deferred to Epic 5.1c)
```

> ⚠️ 当前版本（TASK-20260513-01）已经把 `ModalityRateLimiter` 装配并按 yaml 配额配置好了，但 `processProxyRequest` 路径里的 per-request quota 检查留待下一次任务（Epic 5.1c）接入。配额配置不会丢失，只是暂时未生效。

## 决策树：应该开吗

| 现状 | 建议 |
|---|---|
| 只做 chat completion，没有多模态需求 | `multimodal.enabled: false`（默认），完全不影响 |
| 用 5 个多模态端点中任意一个 | `enabled: true`，至少能拿到模态归因 + 启动日志可见 |
| 同一模态有多家 provider | 配 `policy`：成本敏感选 `cheapest`，可观测延迟选 `fastest_p99`，公平负载选 `round_robin` |
| 担心某模态被滥用 | 配 `rate_limit.quotas`，`identity: "*"` 设全局默认，再单独覆盖问题租户 |

## RoutingPolicy 行为

| 策略 | 选择规则 | 适用场景 |
|---|---|---|
| `cheapest` | `estimateCost(req)` 最小的 handler | 成本最敏感；多 provider 价格差异显著 |
| `round_robin` | 单调递增 index 取余 | 公平摊派；新 provider 灰度引入 |
| `fastest_p99` | 历史 p99 延迟最低 | 用户体验导向；牺牲少量成本换稳定性 |

N=1 时所有策略统一走 `front()` 快速路径，O(1) 无策略开销。N=0 时 router 返空指针，请求走 legacy ConnectorRegistry。

## 安全（SR5 + SR6）

- **SR5 RBAC modality 视图**：`CostTracker.modality` 维度的查询遵循现有 RBAC，SuperAdmin 看全局聚合，TenantAdmin/Viewer 只看本租户。在 admin 后台 Savings 页可以按模态归因预览（`web/admin/src/pages/Savings.tsx`）。
- **SR6 不扩张限流绕过面**：`ModalityRateLimiter` 在 `RateLimiter` token bucket 之上做薄装饰，key prefix 为 `modality:<name>:<identity>`，不会跟现有 IP/tenant key 冲突；超限直接复用 RateLimiter 的 429 路径，不发明新的拒绝信号。

## 验证

- ctest（CP+ON）会跑 `ModalityTest`、`ModalityRouterTest`、`ModalityHandlersTest`、`ModalityRateLimiterTest`、`CostTrackerModalityTest`。
- 启动后调用 `POST /v1/embeddings` 应该看到 `ModalityRouter:` 路径选 handler 的日志（DEBUG 级），并且 `CostTracker` 的 modality 字段被填充。

## 常见问题

- **N=1 时和原来 passthrough 比有性能差吗？** 几乎没有。Router 的 fast path 是 `vector::front()` + 一次虚函数调用，O(1)。
- **`ModalityRateLimiter` 现在的 yaml 配额会被强制吗？** 暂时不会（Epic 5.1c 待接入）。但配额已被读取并日志汇总。
- **能不能用别的 provider 而不是 OpenAI？** 当前装配代码硬编码 `findByProvider("openai")`。要支持别家 provider，扩展 `GatewayRuntime::initialize` 的 modality wire 段（参照 OpenAI adapter 模式新增 adapter）。

## 相关链接

- 设计文档：`docs/specs/2026-05-13-phase6-completion-design.md` §6
- 创意决策：`memory-bank/creative/creative-modality-handler.md`（CR2 方案 A 推导过程）
- 实现计划：`docs/plans/2026-05-13-phase6-completion.md` §5
