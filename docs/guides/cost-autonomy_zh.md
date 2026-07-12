# 成本自治与审批工作流指南

> 特性：跨切面自治审批工作流 + 预算守卫（Phase 11.5）
> 版本：v1.1+（TASK-20260518-02）

Cost Autonomy 是所有"花钱类"自治决策的**决策治理面**。在此之前，
CostOptimizer v1 等模块只能输出只读建议——运维仍需手动把推荐模型复制到
config 并重载。Phase 11.5 关闭了这个闭环：

- **CostOptimizer 2.0** 通过 `propose()` 把决策投递到一个统一审计的工作流
  （PROPOSED → APPROVED → APPLIED）。
- **BudgetGuardStage** 位于 inbound pipeline 中 `ResolveStage` 之后、Router
  之前，对租户 24h 累计花费 fail-closed 限额，并在调用上游前将请求降级到
  `economy` 质量层。
- **AutonomyApprovalWorkflow** 是跨切面统一漏斗，未来的自治阶段
  （AdaptiveGuard、AutoRecovery、BanditRouter、Workflow 2.0）都会复用它，
  保证所有"提案 → 批准 → 应用 → 回滚"生命周期共享同一审计轨、同一熔断
  开关和同一 Admin UI。

本文涵盖七个主题：总体模型、状态机、三种 `auto_apply` 模式、Budget Guard
旋钮、FinOps Admin UI、常见错误码排查，以及从旧版 CostOptimizer v1 的
迁移路径。

---

## 1. 概览

整套特性由三个相互独立的 YAML 开关驱动，**全部默认 false**，因此升级后
旧的 `aegisgate.yaml` 行为完全保持：

```yaml
autonomy:
  enabled: false                 # 审批工作流主开关
  auto_apply_mode: manual_only   # manual_only | auto_low_risk | auto_all
  proposal_retention_days: 90
  cost_optimizer:
    enabled: false               # CostOptimizer v2 propose() 路径

budget_guard:
  enabled: false                 # inbound 阶段，对花费限额硬执行
  per_tenant_24h_usd: 100.0
  per_request_max_usd: 1.0
  fail_open_on_error: true
  downgrade_tier: economy
```

环境变量 `AEGISGATE_DISABLE_AUTONOMY=1` 是全局熔断器，**优先级高于** YAML
和内存 override —— 详见后文"排查"章节。

启用后你将获得：

| 接口 | 行为 |
|---|---|
| `CostOptimizer::proposeRecommendations()` | 既返回建议列表，又把每条作为 `PROPOSED` 提交到工作流。 |
| `BudgetGuardStage` | 当 24h 或单次花费触顶时，盖上 `quality_tier=economy` 并设置响应头 `X-AegisGate-Budget-Guard=triggered`。 |
| `AutonomyApprovalWorkflow` | 拥有 5 态生命周期、`payload_sha256` 校验（T01 防御）、按 source 分派 applier、审计每一次状态转换。 |
| Admin UI `/admin/finops` | 提案列表、状态/来源过滤、payload + decision_trace 详情、TenantAdmin 批准/拒绝/回滚（带二次确认）。 |
| API `/admin/api/autonomy/*` | 5 个端点：list / approve / reject / delete（回滚）/ report，每次写一条审计。 |

---

## 2. 状态机

```
PROPOSED ──approve()──► APPROVED ──apply()──► APPLIED ──rollback()──► ROLLED_BACK
   │                       │                     ▲
   │ reject()              │ reject()            │  apply() 失败时自动回滚
   ▼                       ▼                     │
REJECTED               REJECTED                  └──────► ROLLED_BACK
```

转换矩阵：

| 起 → 目  | PROPOSED | APPROVED | APPLIED | REJECTED | ROLLED_BACK |
|---|:---:|:---:|:---:|:---:|:---:|
| PROPOSED    | —        | approve  | —       | reject   | —            |
| APPROVED    | —        | —        | apply   | reject   | —            |
| APPLIED     | —        | —        | —       | —        | rollback / 自动 |
| REJECTED    | —        | —        | —       | —        | —            |
| ROLLED_BACK | —        | —        | —       | —        | —            |

每次转换写一条 `autonomy.<动词>` 审计（例如 `autonomy.approve`）。
`apply()` 先校验 `payload_sha256`，防止 propose 与 apply 之间被篡改（T01）；
不匹配会以 `AEGIS-6005 (PayloadTampered)` 拒绝并写取证审计。

---

## 3. `auto_apply` 三种模式

`autonomy.auto_apply_mode` 决定新提案的自动批准激进程度：

### `manual_only`（默认）
所有提案保持在 `PROPOSED`，直到 TenantAdmin 通过 Admin UI 或 `aegisctl`
手动处理。**推荐首次上线和合规环境**使用。

### `auto_low_risk`
工作流在收到提案时调用 `IApprovalApplier::isLowRisk(p)`。参考实现
`CostAutonomyApplier::isLowRisk` 编码了 4 条保守规则：

- **R1** —— 只允许**降级**（rank(to) < rank(from)）。
- **R2** —— 禁止跨两级（`rank(from) - rank(to) ≤ 1`）。
- **R3** —— 估算 24h 节省不超过 $50。
- **R4** —— 影响请求量不超过 1000 RPS。

四条全部命中则同步自动 approve + apply；否则该条提案回退到
`manual_only`。审计中 `auto_approved=true` 字段标记。

### `auto_all`
工作流自动批准并应用每一条提案。**除影子流量 A/B 等 dev 场景外不
推荐**。生产启用前请先在影子环境验证至少一个周末。

> 切换 `auto_apply_mode` 是热重载安全的（位于 `autonomy:` 段，
> `reloadConfig()` 会重新读取）。已经处于 `PROPOSED` 的提案保持原状，
> 新模式只对后续新增提案生效。

---

## 4. Budget Guard

`BudgetGuardStage` 是 `budget_guard.enabled: true` 时由 `GatewayRuntime`
注入的 inbound 阶段。位置在 `ResolveStage` 之后（模型已解析）、Router 之前
（降级决策可改写 `ctx.chat_request.extra["quality_tier"]`）。

### 配置项

```yaml
budget_guard:
  enabled: true
  per_tenant_24h_usd: 100.0     # 租户 24h 累计花费上限
  per_request_max_usd: 1.0      # 单次请求成本硬上限
  fail_open_on_error: true      # 见"失败模式"
  downgrade_tier: economy        # 触发时切到的 MLRouter 质量层
  downgrade_header_name: X-AegisGate-Budget-Guard
  downgrade_header_value: triggered
```

### 触发逻辑

任一条件满足则降级：

- `tenant_24h_cost_so_far + estimated_request_cost > per_tenant_24h_usd`
- `estimated_request_cost > per_request_max_usd`

`estimated_request_cost` 优先取 `tokens_estimated`，缺省时使用小额 floor，
避免窗口首请求 0 成本误判。响应头被写入，
`budget_guard_triggered{tenant_id}` 指标自增。

请求**继续执行**而非拒绝。这是有意的 UX 设计：调用方体验到的是 graceful
质量降级，而不是 `429 Too Many Requests` 风暴。

### 失败模式

| `fail_open_on_error` | BudgetGuardStage 自身抛异常（如 CostTracker mutex 不可达）时的行为 |
|---|---|
| `true`（默认） | 请求按未触发处理，日志写 ERROR 让 SRE 知晓。 |
| `false`        | 阶段返回 `PipelineResult::Error`，请求 503。 |

默认值保护数据平面不被可观测性 bug 拖垮。

### 关键指标

- `budget_guard_triggered_total{tenant_id, reason}` —— counter，`reason`
  取 `tenant_24h` 或 `per_request`。
- `budget_guard_error_total` —— counter，内部异常计数。
- `budget_guard_estimated_cost_usd{tenant_id}` —— histogram，请求级估算。

---

## 5. Admin UI 操作流

`/admin/finops` 页面对所有 `TenantAdmin` 及以上角色可见。

### 布局

- **顶部 KPI** —— 待审批数、累计已应用数、估算 24h 节省金额。
- **过滤器** —— 状态 + 来源下拉；右侧"共 N 条"实时更新。
- **Top-5 表** —— 按 `estimated_savings_usd_24h` 排序，便于先处理大头。
- **卡片网格** —— 全量提案（主题、来源、状态徽章、推荐模型、tier 变化、
  节省、租户）。
- **详情抽屉** —— 状态/审阅人、payload JSON、`decision_trace`
  美化输出、`payload_sha256`；按状态显示操作按钮：
  - PROPOSED → 批准 / 拒绝
  - APPLIED → 回滚

### 操作流

1. 点击卡片 → 打开抽屉。
2. 点击「批准」→ 二次确认 modal → 确认 → `POST /approve` →
   Toast「已批准」→ 抽屉关闭 → 列表刷新。
3. 点击「拒绝」→ 含原因 textarea 的 modal → 原因非空 → `POST /reject` →
   Toast「已拒绝」。
4. 点击「回滚」（APPLIED 提案）→ 二次确认 modal → 确认 →
   `DELETE /proposals/{id}` → Toast「已发起回滚」。

### 对应 API

| 端点 | 方法 | 用途 |
|---|---|---|
| `/admin/api/autonomy/proposals` | GET | 列表 + 状态/来源 + 分页 |
| `/admin/api/autonomy/proposals/{id}/approve` | POST | PROPOSED → APPROVED（之后自动 apply） |
| `/admin/api/autonomy/proposals/{id}/reject` | POST | PROPOSED \| APPROVED → REJECTED（body.reason 必填） |
| `/admin/api/autonomy/proposals/{id}` | DELETE | APPLIED → ROLLED_BACK |
| `/admin/api/autonomy/report` | GET | 按 source × state 聚合 + 24h 节省总和 |

所有端点要求 `TenantAdmin` 及以上。SuperAdmin 跨租户可见；TenantAdmin
仅看本租户（工作流层在 list 中按租户过滤）。

---

## 6. 排查

### 常见错误码

| 代码 | HTTP | 含义 | 处置 |
|---|:---:|---|---|
| `AEGIS-6001 BudgetExceeded` | 429 | 触达租户 24h 上限或单次上限（响应同时带 `X-AegisGate-Budget-Guard`） | 上调 `per_tenant_24h_usd` 或升级租户套餐 |
| `AEGIS-6002 AutonomyDisabled` | 503 | 工作流为 null：`autonomy.enabled: false` 或 `AEGISGATE_DISABLE_AUTONOMY=1` | 启用 autonomy + 清环境变量，`kill -HUP <pid>`（或重启） |
| `AEGIS-6003 ApprovalNotFound` | 404 | 提案 id 未知（拼写错误 / 超出 `proposal_retention_days` 被裁剪 / 未持久化） | 在 FinOps 页面刷新；查审计中是否有 prune 事件 |
| `AEGIS-6004 ApprovalStateInvalid` | 409 | 从 REJECTED 批准、从 PROPOSED 回滚等非法迁移 | 重新拉取提案，确认当前状态 |
| `AEGIS-6005 PayloadTampered` | 422 | propose 与 apply 之间 `payload_sha256` 不一致（T01 防御） | 重新提交提案；排查存储是否被注入 / 损坏 |

### 全局熔断器

最快关停所有自治决策的方式：

```bash
export AEGISGATE_DISABLE_AUTONOMY=1
# 然后重启或热重载 —— 工作流在每次 propose() / approve() / apply()
# 时都会调用 isAutonomyEnabled() 检查。
```

环境变量**优先级高于** YAML 中的 `autonomy.enabled: true`。设计上**不提供
租户级 override**（SR17 —— autonomy 必须能一键关停）。

### 健康哨兵

`/admin/api/autonomy/report` 是一个不写状态的廉价 GET，可用于验证装配
是否健康：返回 `AEGIS-6002` 即 autonomy 关闭；返回带 `sample_size >= 0`
的 JSON 即说明 workflow + queue 都已就绪。

---

## 7. 从 CostOptimizer v1 迁移

CostOptimizer v1 (`getRecommendations()`) **未被移除**，只读 dashboard
路径仍可用，方便不想接入工作流的消费方继续工作。v2 是纯增量：

- 同时打开 `autonomy.enabled: true` 和 `autonomy.cost_optimizer.enabled: true`。
- 可选 `auto_apply_mode: auto_low_risk`，让符合 R1–R4 的降级自动落地。
- 旧版 `getRecommendations()` 消费方继续工作，看到的是工作流接收的同一份
  建议列表。

### 对照表

| | v1 (`getRecommendations`) | v2 (`proposeRecommendations`) |
|---|---|---|
| 输出 | 只读建议列表 | 同一列表 + 投递到工作流 |
| 审计 | 无 | 每条 `autonomy.propose` |
| 应用 | 手动改 config + reload | `CostAutonomyApplier` 同步应用 |
| 回滚 | 手动改 config 还原 | `applier->rollback(p)`（审计） |
| 风险闸 | 无 | R1–R4 via `isLowRisk()` |

### 推荐的渐进式上线

1. **Week 0** ——
   `autonomy.enabled: true`，`cost_optimizer.enabled: false`。
   只装配 queue + workflow + Admin UI，CostOptimizer 行为不变。验证
   FinOps 页面能正常加载。
2. **Week 1** —— 打开 `cost_optimizer.enabled: true`，保持
   `auto_apply_mode: manual_only`。CostOptimizer 开始投递，但每条都
   需要人工点击。
3. **Week 2+** —— 审批节奏稳定后切到
   `auto_apply_mode: auto_low_risk`。关注 `budget_guard_triggered_total`
   和 FinOps Top-5 是否有意外。

### 回退迁移

设置 `autonomy.enabled: false` 并重载即可。CostOptimizer v1 的只读
路径保留，工作流进入休眠，`/admin/finops` 会显示
`AEGIS-6002`（UI 上呈现为"自治已关闭"空状态）。**不会丢数据** ——
提案仍按 `proposal_retention_days` 持久存留在 `autonomy_proposals` 表。

---

**相关链接：**

- `docs/guides/cost-optimization_zh.md` —— CostOptimizer v1 背景
- `docs/guides/feedback-bus_zh.md` —— Phase 11.0 事件总线
- `docs/guides/admin-savings_zh.md` —— 节省 dashboard（Phase 10）
- `docs/guides/error-codes_zh.md` —— 完整错误码参考
