# AegisGate 省钱看板用户指南

Savings Dashboard 是后台管理面板新增的"省钱能力"可视化模块，把 AegisGate 三大
降本能力的真实数据展示给运营、销售、终端用户三类受众：

1. **语义缓存命中** — 命中后短路上游 LLM 调用，跳过整次 token 消耗。
2. **Prompt 压缩** — `PromptCompressor` 在发送给上游前压缩输入，减少 input tokens。
3. **智能路由（潜在）** — `CostOptimizer` 在数据支撑可信时推荐切换更便宜的模型。

使用场景：

- 向运维 / 财务团队证明 AegisGate 的部署 ROI；
- 销售演示中提供真实租户的节省金额；
- 帮助终端用户 / 开发者理解哪些优化路径在生效。

---

## 访问权限

| 角色          | 可见范围                              |
|---------------|---------------------------------------|
| `SuperAdmin`  | 全局视图 + `top_tenants` Top 10 排行  |
| `TenantAdmin` | 仅本租户，无排行                      |
| `Viewer`      | 仅本租户，只读                        |

进入路径：**AegisGate 后台 → 侧边栏 → 省钱**，或直接访问 `/admin/savings`。
首页 Dashboard 上的"已节省（近30天）" KPI 卡片也可点击跳转过来。

---

## 时间窗口

页面提供 4 档预设窗口；直接调 API 时使用 `from` / `to` ISO 8601 UTC 时间戳。

| 按钮     | from / to 行为                                  |
|----------|-------------------------------------------------|
| 近24小时 | `now - 24h` → `now`                             |
| 近7天    | `now - 7d` → `now`（默认）                      |
| 近30天   | `now - 30d` → `now`                             |
| 全部     | 留空 → 后端按 `aggregator since` 起始返回       |

> **DoS 防护（SR-NEW3）**：超过 365 天的时间窗口会被拒绝，返回
> `400 InvalidRequest`。

---

## API

```
GET /admin/api/savings/summary?from=<iso>&to=<iso>&tenant_id=<id>
Cookie: aegis_session=<jwt>
```

返回结构（`SavingsSummary`）：

```json
{
  "from": "2026-05-03T00:00:00Z",
  "to": "2026-05-10T00:00:00Z",
  "aggregator_since": "2026-05-01T08:00:00Z",
  "total_cost_saved": 12.34,
  "total_cost_actual": 100.00,
  "roi_percent": 11.0,
  "total_tokens_saved": 12345,
  "total_cache_hits": 50,
  "fallback_pricing_count": 2,
  "by_type": [
    { "type": "cache_hit", "cost_saved": 8.0, "tokens_saved": 8000, "event_count": 50 },
    { "type": "compression", "cost_saved": 4.34, "tokens_saved": 4345, "event_count": 100 }
  ],
  "by_model":     [ ... ],
  "time_series":  [ ... ],
  "top_tenants":  [ ... ],
  "routing_recommendations": [ ... ]
}
```

非 SuperAdmin 调用 `top_tenants` 必为空数组（设计上的多租户隔离 SR1）。

---

## 节省金额计算方法

进程内 `SavingsAggregator` 中每条事件携带 `tokens_saved` 和 `cost_saved`，
按来源分为三类：

| 来源                  | tokens_saved                                          | cost_saved                                | 备注                                              |
|-----------------------|-------------------------------------------------------|-------------------------------------------|---------------------------------------------------|
| `cache_hit`           | `request.tokens_estimated + estimate(cached_response)`| `CostTracker.calculate(model, in, out)`   | output tokens 由 `TokenEstimator::estimateTokens` 估算 |
| `compression`         | `tokens_saved_compression`（仅 input 段）             | `CostTracker.calculate(model, saved, 0)`  | 只计 input 段节省                                 |
| `routing_potential`   | 0（潜在节省）                                         | `CostOptimizer.potential_savings`         | 未真实发生；单独标记避免与已实现节省混淆          |

当 `CostTracker.calculate` 因 pricing 缺失（例如 `models.yaml` 中未配置某个
自定义模型）返回 `0` 时，聚合器会把 `fallback_pricing=true` 标记带给前端，
UI 会通过"算法说明"面板暴露此次数与受影响模型，便于运维及时补充单价表。

---

## 持久化策略

> **聚合器是进程内的，重启即重置。** v1 设计如此，目的是不引入 schema 迁移、
> 也不在热路径加额外 DB 写入。

页面顶部展示的 `aggregator_since` 时间戳让用户清楚数据的实际历史窗口。需要
长期趋势分析时，请使用 `cost_records` 表（通过 `/admin/api/costs` 查询）—
该表持久化记录每次 LLM 实际消费；本模块专门展示"避免发生的成本"，是
`cost_records` 中没有的维度。

未来版本可能把节省事件持久化到独立表；当前 API 契约向前兼容（客户端看到同样的
JSON shape）。

---

## 安全考量

| 关注点                | 缓解措施                                                          |
|-----------------------|--------------------------------------------------------------------|
| 跨租户信息泄露        | `requireTenantAccess` + 非 SuperAdmin `top_tenants=[]`（SR1）      |
| 大时间窗口 DoS        | 365 天上限拦截返回 `400 InvalidRequest`（SR-NEW3）                 |
| 热路径稳定性          | `recordCacheHit` / `recordCompression` 标 `noexcept`（SR-NEW4）    |
| Pricing 数据可信度    | UI 显式暴露 `fallback_pricing_count`（SR-NEW1 透明度合规）         |

完整 STRIDE 威胁模型（T01–T10）见
`docs/specs/2026-05-10-admin-savings-dashboard-design.md` §4.3。

---

## 故障排查

**页面所有数据全为 0。**
聚合器在 `GatewayRuntime::initialize()` 中、`CostTracker.loadPricing()`
之后构造。如果 pricing 加载失败（检查 `models.yaml` 路径），每条记录会被标
`fallback_pricing=true` 且 `cost_saved=0`，但 token 计数仍是真实值。

**`top_tenants` 数组为空。**
要么你不是 `SuperAdmin`，要么所选时间窗口内没有任何租户产生过节省事件。
切到"全部（since aggregator）"再确认。

**`aggregator_since` 显示 `null`。**
该部署是在本特性上线之前启动的，或 `SavingsAggregator` 构造失败（查 spdlog
中关于 `cost_tracker` 初始化的告警）。
