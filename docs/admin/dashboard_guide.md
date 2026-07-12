# AegisGate Admin Dashboard 指南

> **TASK-20260527-02 引入 → TASK-20260528-01 深化** — MVP-5 prep + 意向用户体验震撼化。
> 本文档介绍 admin 仪表盘的 5 行布局，重点说明 Row 0 **Case Study Numbers** hero
> 区域（28-01 由 Row 4 升级而来）。

## 仪表盘布局总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│  ⭐ Row 0: Case Study Numbers — Hero 区（TASK-20260528-01 升级）          │
│           SavingsAnalogy + CacheInterceptStat + QualityTrendBadge + CTA  │
├─────────────────────────────────────────────────────────────────────────┤
│  Row 1: 5 KPI 卡片  (总请求 / 活跃租户 / 总成本 / 缓存命中率 / 已节省)  │
├─────────────────────────────────────────────────────────────────────────┤
│  Row 2: 24h 请求趋势 (左)  +  最近审计事件 (右)                          │
├─────────────────────────────────────────────────────────────────────────┤
│  Row 3: 模型节省金额分布 (左)  +  安全事件统计 (右)                       │
└─────────────────────────────────────────────────────────────────────────┘
```

Row 0 hero 在 TASK-20260528-01 中由原 Row 4 升级而来 — counting-up 动效 +
多档自适应类比单位 + 缓存拦截叙事 + 双语 CTA。
Row 1-3 在 TASK-20260510-01 + TASK-20260512-01 系列任务中已稳定。
原 Row 4 整 block 已迁移到 Row 0，所有数据流（HTTP + WS 30s push）保持不变。

## Row 0 — Case Study Numbers Hero（TASK-20260528-01）

> **设计目标**：让通过 MVP-4 outreach toolkit 邀请进来的**意向用户**打开 dashboard
> 第一眼就被「**强大缓存命中** + **累计省了多少钱**」震撼住，形成体验级转化推动力。
>
> **设计原则**（spec §1.3）：
> 1. **真实数据兜底优先** — 永不显示假数据；`data=null` 时显示引导文案 + ADOPTERS CTA
> 2. **0 新依赖** — counting-up 自写 React + `requestAnimationFrame` hook（连续 6 任务守住）
> 3. **0 多语言扩散** — 不引 i18n 库 / 中英混排（中文 UI label + 英文 case-study 标准用语）
> 4. **K2 wording 合规** — 不含 `certified` / `production-grade` / `enterprise-ready` / `guaranteed`
> 5. **不浮夸** — `< $0.10` 时类比单位 unit=`'none'` 不显示（避免"0 杯咖啡"尴尬）

### Hero 三大组件构成

```
┌──────────────────────────────────────────────────────────────────────────┐
│  🚀 Case Study Numbers      [全局聚合/租户 X] · since 2026-05-XX         │
├────────────────────┬────────────────────┬──────────────────────────────┤
│  ↘ saved vs        │  ⚡ cache hit       │  ⚙ quality (EMA)              │
│    baseline        │                    │                              │
│    $1.44 ↓ 96.0%   │  50.0% hit rate    │  0.823 ↑ 0.012                │
│   (baseline $1.50) │  已为你拦截 12 次  │  fact 5 · refusal 2 · lat 1   │
│   相当于 14 杯咖啡 │  / 24 total (Y%)   │                              │
│   ≈ 14 coffees     │  [exact|sem|conv]  │                              │
│                    │  e7·s3·c2          │                              │
├────────────────────┴────────────────────┴──────────────────────────────┤
│  想在你的系统看到这些数字？Want this for your stack?                    │
│  5-min quickstart / ADOPTERS.md / 跑 `aegisctl estimate` 预估你的节省     │
└──────────────────────────────────────────────────────────────────────────┘
```

### 卡片 1 — `SavingsAnalogy`（累计省钱故事化）

- **核心数字**：`saved_vs_baseline.cost_saved` USD，用 `useCountUp` hook 做
  ease-out-cubic counting-up 动画（duration 1200ms，2 位小数）。
- **类比单位映射**（`web/admin/src/lib/savingsStorytelling.ts` pickAnalogy）：
  | 金额段 | 单位 | 中文 label | 英文 label | 单价 |
  |--------|------|-----------|-----------|------|
  | `< $0.10` | `none` | — | — | 不显示 |
  | `$0.10 ≤ x < $50` | `coffee` | 杯咖啡 | coffees | $5 |
  | `$50 ≤ x < $500` | `lunch` | 次开发者午餐 | developer lunches | $15 |
  | `x ≥ $500` | `subscription` | 个月度 SaaS 订阅 | monthly SaaS subs | $20 |
- **副信息**：savings_percent + baseline_cost（saved vs baseline 等式锚）。
- **SR2 防浮夸**：`count=0` 或 `unit='none'` 时**整行不渲染**（避免"相当于 0 杯咖啡"
  反向浮夸效果）。

### 卡片 2 — `CacheInterceptStat`（强大缓存命中叙事）

- **核心数字**：`total_hit_rate * 100%`（不做 counting-up / 静态）+
  `intercepted = hit_exact + hit_semantic + hit_conversation` counting-up 跳数。
- **双叙事**（D5=C）：
  - 中文 UI："**已为你拦截 12 次重复请求**"（SR4 字面量 #2）
  - 英文标准用语："**50.0% intercepted vs LLM API**"
- **三色横向柱**：按 `intercepted` 内部占比渲染（exact 蓝 / semantic 紫 / conv 青）。
- **细分计数**：`exact 7 · semantic 3 · conversation 2`（保留 27-02 既有信息）。

### 卡片 3 — `QualityTrendBadge`（保留 27-02 既有功能）

- 由原 Row 4 第 3 张卡迁移而来，渲染逻辑不变。
- EMA + slope 双色（slope ≥ 0 success 绿 / < 0 warning 黄）。
- factuality / refusal / latency_degraded 三档计数细分。

### Hero Header (左) — Scope + Since 锚

- **Scope label**：`scope === 'global'` → "全局聚合 / global"；
  `scope === 'tenant'` → "租户 \<tenant_id\> / tenant"。
- **Since label**（TASK-20260528-01 新增）：
  - `aggregator_since` 有效 → `since 2026-05-XX`（仅日期片段避免时区混淆）
  - `aggregator_since === null` → `自启动以来`（SavingsAggregator 未注入兜底）

### Hero Footer — 双语 CTA（D9=B）

```
想在你的系统看到这些数字？ Want this for your stack?
5-min quickstart / ADOPTERS.md / 跑 `aegisctl estimate` 预估你的节省
```

- 链接目标：
  - 5-min quickstart → `/docs/quickstart`（MVP-1 交付）
  - ADOPTERS.md → `https://github.com/privonyx/loong-aegisgate/blob/main/ADOPTERS.md`（MVP-4 交付）
  - `aegisctl estimate`（SR4 字面量 #4） → MVP-2 交付的 CLI pre-flight 节省估算工具

### 空数据兜底（Demo Mode v2 占位）

`data === null` 时（如 SavingsAggregator + CostTracker + SemanticCache 都未注入），
hero 显示：

```
🚀 Case Study Numbers

暂无数据 / no data yet — adopter 接入并积累请求后，这里会显示真实的
saved vs baseline、cache hit rate 和 quality trend 数字。

→ 5-min quickstart · 加入 ADOPTERS.md
```

> 不引入 Demo Mode（合成数据）— 拆 v2 TASK-W36（SR-NEW 防真实数据混入需独立设计审查）。

## SR4 字面量审计（N2 第 5 次实战）

本任务锁定 4 字面量 × 4 方 = 16 命中点（`scripts/audit_sr4_admin_experience.sh`）：

| # | 字面量 | spec | plan | 测试 | impl |
|---|--------|------|------|------|------|
| 1 | `Case Study Numbers` | §1.2 §3.1 §4.1 | §1 §3 Epic 4 | `HeroCaseStudy.test.tsx` + `dashboard_case_study.test.tsx` | `HeroCaseStudy.tsx` h2 |
| 2 | `已为你拦截` | §1.2 §3.1 | §3 Epic 3 | `HeroCaseStudy.test.tsx` | `CacheInterceptStat.tsx` 文案 |
| 3 | `相当于` | §1.2 §3.1 §4.1 | §3 Epic 2 | `HeroCaseStudy.test.tsx` + `savingsStorytelling.test.ts` | `SavingsAnalogy.tsx` 文案 |
| 4 | `aegisctl estimate` | §1.2 §4.3 | §3 Epic 5 | `dashboard_guide.md` 引用 | `HeroCaseStudy.tsx` CTA `<code>` |

## 数据流（27-02 已稳定 / 28-01 复用）

```
┌──────────────────┐
│ HTTP GET /admin/ │
│ api/case-study/  │  ← initial fetch on mount
│ headline         │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐     ┌──────────────────┐
│ Dashboard state  │ ←── │ WS 'case_study'  │  ← 30s throttle push
│ caseStudy        │     │ AdminWsController│
└────────┬─────────┘     └──────────────────┘
         │
         ▼
┌──────────────────┐
│ HeroCaseStudy    │     ← Row 0 (28-01) / 原 Row 4 (27-02)
│  ├ SavingsAnalogy│     ← useCountUp + pickAnalogy
│  ├ CacheInterceptStat │ ← intercepted derivation
│  └ QualityTrendBadge  │ ← inline (no separate component)
└──────────────────┘
```



## Row 4 — Case Study Numbers

> 设计原则：**3 个头条数字 + 副文本细分**（spec §3.5 / D4=A）。
> 数字不是越多越好，太多会稀释震撼。`saved vs baseline` / `cache hit Y%` /
> `quality_reason` 这 3 个数字直接对应 adopters 在 MVP-5 案例博客中需要展示的
> 核心证据。

### 卡片 1 — `saved vs baseline` ($X)

```
┌────────────────────────────────────┐
│  ↘  saved vs baseline              │
│     $X.XX                          │
│     节省 Y.Y% (baseline $Z.ZZ)     │
└────────────────────────────────────┘
```

- **数字含义：** 与 baseline 模型相比累计节省的成本。
- **数据源：** `CostRecord.baseline_cost`（`CostTracker::record()` 内部计算）。
- **baseline 模型：** 默认 `setPricing()` 注册的第一个 model（通常是 GPT-4 等
  贵价模型），可通过 `CostTracker::setBaselineModel()` 显式覆盖。
- **SR2 锁定：** baseline_cost 必须由 CostTracker 内部根据 pricing 表计算，
  外部传入的 `CostRecord{baseline_cost=...}` 字段会被 `record()` 覆盖。这避免
  恶意构造 / 上游 bug 注入虚假节省数字误导 case study。
- **savings_percent 公式：** `cost_saved / baseline_cost * 100%`，baseline=0
  时显示 0%。

### 卡片 2 — `cache_hit_by_type` Y%

```
┌────────────────────────────────────┐
│  ◎  cache hit                      │
│     Y.Y%                           │
│     exact A / semantic B / conv C  │
└────────────────────────────────────┘
```

- **数字含义：** 缓存总命中率 + 三档细分计数。
- **数据源：** `SemanticCache::CacheStats` 的 `hit_exact` /
  `hit_semantic` / `hit_conversation` 字段。
- **三档分类（D6=A）：**
  - `hit_exact` — `partition_key` 为空且 `max_similarity ≥ 0.9999`（V1 path
    完全相同 prompt 的命中）。
  - `hit_semantic` — `partition_key` 为空且 `threshold ≤ max_similarity < 0.9999`
    （V1 path 语义相似命中）。
  - `hit_conversation` — `partition_key` 非空（V2 conversation-scoped 命中）。
- **保证：** `hit_exact + hit_semantic + hit_conversation == hit_count`。

### 卡片 3 — `quality_reason`

```
┌────────────────────────────────────┐
│  ◔  quality (EMA)                  │
│     0.XXX                          │
│     fact A / refusal B / latency C │
└────────────────────────────────────┘
```

- **数字含义：** 质量 EMA + 3 类质量降级原因计数。
- **数据源：** `QualityMonitor::QualityTrend` 的 `current_ema` / `slope` /
  `reason_factuality` / `reason_refusal` / `reason_latency_degraded`。
- **slope 决定 accent 颜色：** ≥ 0 → 绿色（质量稳定/上升）；< 0 → 黄色（质量下降）。
- **3 档 reason 白名单（D5=A / SR3）：** `recordQuality()` 接受可选 reason
  参数，仅 `"factuality"` / `"refusal"` / `"latency_degraded"` 4 个值（含
  默认空串 = 不归因）会增加对应计数；任何其他值（如 `"hallucination"`）静默
  丢弃，避免 free-form 字符串污染头条数字。

## 数据流

```
HTTP/REST 路径（首次加载）：
  Dashboard.tsx ──fetch──► /admin/api/case-study/headline
                                │
                                ▼
                    AdminController::caseStudyHeadline(ctx)
                                │
                  ┌─────────────┼──────────────┐
                  ▼             ▼              ▼
            CostTracker  SemanticCache  QualityMonitor
            totalSummary getStats       getTrends
            (or summaryByTenant if RBAC SR1 限本租户)

WebSocket 路径（30s 节流推送）：
  AdminWsController::startCaseStudyTimer()
    └── runEvery(30.0) → buildCaseStudySnapshot() → broadcastJson({type:"case_study",...})
                                                          │
                                                          ▼
                                          Dashboard.tsx handleWsMessage
                                          (合并到现有 caseStudy state)
```

## RBAC（SR1）

`AdminController::caseStudyHeadline` 复用项目通用的 `effectiveTenantId(ctx, ...)`
模式：

| 角色 | scope | tenant_id | saved/cache/quality 数据源 |
|------|-------|-----------|---------------------------|
| SuperAdmin | `"global"` | （不设置） | `totalSummary()` 全局聚合 |
| TenantAdmin | `"tenant"` | `ctx.tenant_id` | `summaryByTenant(ctx.tenant_id)` |
| Developer / Viewer | `"tenant"` | `ctx.tenant_id` | 同上 |

> **注：** `cache_hit_by_type` 与 `quality_reason` 当前是进程级单例（与
> `dashboardSummary` 一致），不区分租户。Per-tenant cache stats / per-tenant
> quality trends 是 v2 backlog（plan §6.2）。

## 监控建议

- **开发环境：** 用 `aegisctl estimate` 跑示例 prompt 后，刷新 dashboard 应
  能看到 cache hit 计数与 cost saved 实时增长。
- **生产环境：** 把 `cache hit %` 接入 alerting（如 Grafana），命中率
  突然跌破阈值通常意味着 prompt 模式发生变化，需要调整 cache key 或 threshold。
- **数据归零：** 在 `setBaselineModel("gpt-4")` 后，所有未设置 `baseline_cost`
  的既有 CostRecord 都会按新 baseline 重算（相当于"软 reset"）。如需硬 reset，
  调用 `CostTracker::clear()`。

## 相关任务

- TASK-20260510-01 — Savings Dashboard（节省金额分布、5 维聚合）
- TASK-20260518-02 — Phase 11.5 Autonomy approval workflow
- TASK-20260527-02 — Case Study Numbers + reason taxonomy + hit_by_type
- TASK-20260527-01 — MVP-4 Seed User Outreach Toolkit（向种子用户介绍这个
  dashboard 时，重点引导他们看 Row 0 hero / Row 4 数字）
- TASK-20260528-01 — Row 0 hero 深化（counting-up + 多档自适应类比 + 缓存拦截叙事 + CTA）
- TASK-20260602-01 — **本任务**：后台体系化优化与改造（WS envelope 修复 + builder 抽取 +
  withRbac decorator + 前端 fetch 层 DRY + 测试盲区补齐 + recharts 独立 chunk）

## TASK-20260602-01 关键架构变更（2026-06-02）

### WebSocket envelope schema 一致性（Epic 1 / P0 真 bug 修复）

后端 `admin_ws_controller.cpp` 自始就以 nested envelope 推送：

```json
// metrics 推送（2s）
{ "type": "metrics", "data": { "total_requests": N, "active_tenants": N, "total_cost_records": N, "cache_hit_rate": 0.0-1.0 } }

// audit 推送（事件驱动）
{ "type": "audit",   "data": { "request_id": "...", "timestamp": "...", "tenant_id": "...", "action": "...", "stage": "...", "detail": "..." } }

// case_study 推送（30s / per-tenant after Epic 2 + D5）
{ "type": "case_study", "data": { "scope": "global|tenant", "tenant_id?": "...", "saved_vs_baseline": {...}, "cache_hit_by_type": {...}, "quality_reason": {...} } }
```

> **修复前**：前端 `WsMetrics` / `WsAuditEvent` 类型定义为 flat（没有 `data` 嵌套），
> Dashboard `handleWsMessage` 用 `{ ...prev, ...msg }` 展开/`as unknown as
> AuditRecord` cast → 字段读取错位 → **Dashboard KPI 实时更新与 Audits liveMode
> 实际无效**。
>
> **修复后**：所有 WS 客户端 handler 统一从 `msg.data.*` 路径读取字段；前端
> `WsMessage` discriminated union 三态全部 nested。Audits 内部从 `data.stage`
> 映射到 `AuditRecord.stage_name`。

### `CaseStudySnapshotBuilder` 抽取（Epic 2 / 跨层契约统一）

`caseStudyHeadline` 的 JSON 构造逻辑 (~100 行) 抽取到 `src/server/case_study_builder.{h,cpp}`：

```cpp
namespace aegisgate::admin {
struct CaseStudyInputs {
    CostTracker* cost_tracker = nullptr;
    SemanticCache* semantic_cache = nullptr;
    QualityMonitor* quality_monitor = nullptr;
    SavingsAggregator* savings_aggregator = nullptr;
    bool is_super = true;
    std::string tenant_id;
    bool include_envelope = false;  // HTTP=true / WS=false
};
nlohmann::json buildCaseStudySnapshot(const CaseStudyInputs& in);
}
```

HTTP endpoint (`AdminController::caseStudyHeadline`) 与 WS push
(`AdminWsController::startCaseStudyTimer`) 共用同一函数 → **0 schema 漂移风险**。
`test_case_study_builder.cpp` 中 `HttpAndWsShareDataBlocks_SR_NEW1` 测试断言
两侧产生**字节级一致**的 3 数据块。

### WS per-tenant push（Epic 2 + D5 / SR-NEW1 WS-RBAC 一致性）

30s `case_study` push 由 broadcast 改为 **per-connection**：每个 WS 连接
根据 `JwtPayload.role + tenant_id` 单独调 builder 生成 scoped snapshot，
与 HTTP endpoint RBAC 行为一致。SuperAdmin 仍看全局聚合；TenantAdmin / Viewer
只看本租户。

| 角色 | WS case_study scope | 数据范围 |
|------|---------------------|----------|
| SuperAdmin | `"global"` | `totalSummary()` |
| TenantAdmin | `"tenant"` | `summaryByTenant(ctx.tenant_id)` |
| Developer / Viewer | `"tenant"` | 同上 |

> **注：** `cache_hit_by_type` 与 `quality_reason` 仍是进程级单例（与
> `dashboardSummary` 一致），不区分租户；per-tenant cache stats 是 v2 backlog
> (`TASK-W-PerTenantCache`)。WS RBAC 在 `scope` / `tenant_id` envelope 字段层
> 落实，cache stats 数据本身的多租户隔离独立任务承接。

### `withRbac` decorator（Epic 5 / handler 样板代码集中化）

新建 `src/server/admin_handler_helpers.h`：

```cpp
namespace aegisgate::admin {
template <typename Fn>
AdminResult withRbac(const AuthContext& ctx, Role min_role, Fn&& fn) {
    if (!auth::requireRole(ctx, min_role)) {
        return AdminResult::error(ErrorCode::InsufficientPermissions);
    }
    return fn();
}
}
```

本任务局部应用到 6 高频 read-only handler（`queryAudits` / `queryCosts` /
`dashboardSummary` / `caseStudyHeadline` / `getSavingsSummary` /
`getSecurityEvents`）作为模式验证。全量替换 ~40 处 handler 是 v2
`TASK-W-ControllerSplit` 范围。

### 前端 API request 层 DRY（Epic 4）

`api/client.ts` + `api/autonomy.ts` 各持有的 identical `request()` 实现
合并到共享 `api/request.ts` 的 `adminFetch<T>(base, path, options)`。
两个 client 现在只 1 行 import + 1 行薄封装；任何 401 / 错误处理变更只需
在 `adminFetch` 一处维护。

### Bundle 优化：recharts 独立 chunk（Epic 8）

`web/admin/vite.config.ts` `build.rollupOptions.output.manualChunks`：

```
{ recharts: ['recharts'] }
```

recharts (~558 KB) 抽到独立 vendor chunk，被 Dashboard / Savings / Costs 共享
缓存命中，多页导航场景下只下载 1 次（vs 之前 3 次重复）。
