# 学习型护栏（Adaptive Guard，Phase 11.1）指南

> 特性：在现有 L1-L4 三层护栏之上叠加学习型 L5
> 起始版本：v1.1+（TASK-20260523-01）

学习型护栏把现有四层护栏（`L1 regex` / `L2 RuleEngine` / `L3 ONNX 分类器` /
`L4 ExternalSafetyStage`）升级为**会学习的**系统。审核员通过专门的 admin
API 提交带签名的反馈，运行时进程内收集训练样本，运维人员通过 Phase 11.5
引入的同一套 `AutonomyApprovalWorkflow` 把候选模型晋升到线上。整个闭环复用
现有基础设施（`PIIFilter` / `AuditLogger` / `FeedbackBus` /
`IApprovalApplier`），不引入并行存储和并行链路。

本指南覆盖七个主题：整体概览、运行时架构、三个 admin 端点、模型注册表语义、
反馈链路与反投毒防御、晋升流程、以及安全与运维抓手。

---

## 1. 概览

运行时在已有 Drogon HTTP 层（同一栈服务 `/admin/api/autonomy/*`）暴露三个
端点：

| 端点 | 方法 | 用途 |
|------|------|------|
| `/admin/api/guard/feedback` | POST | 审核员提交 FP / FN / confirmed_block 标签 |
| `/admin/api/guard/explanation/{request_id}` | GET | 运维查询请求被拦截的原因 |
| `/admin/api/guard/model/promote` | POST | 运维提议把 shadow 模型晋升为 live |

反馈走三段路径（PII 脱敏 → 审计日志 → 反馈总线）。独立的 trainer 进程消费
反馈总线产出新的候选模型。晋升提议必须经过 `AutonomyApprovalWorkflow`，
与 Phase 11.5 给成本通道引入的治理面一致。Kill switch
`AEGISGATE_DISABLE_AUTONOMY` 会同时短路 workflow 和 applier（双重防御）。

---

## 2. 架构

五个逻辑层参与本闭环。每一层都由 `GatewayRuntime::initialize` 装配，单个
配置开关即可启用整个平面。

### 2.1 五层结构

1. **现有护栏层（L1-L4）**：仍然产生 `InjectionResult` / `RuleResult` /
   `GuardResult`，新加的 `GuardExplanationBuilder` 把每一种结果翻译成
   规范的 `GuardExplanation`（7 个稳定字段）。
2. **反馈接入**：进入 `GuardAdminController::postFeedback`，按
   "角色白名单 → 限流 → 异常检测 → sink" 顺序串行执行。
3. **模型注册表**：保存 `(model_id, version, status, artifact_sha256,
   created_at)` 行。`MemoryGuardModelRegistry` 默认在生产中运行；
   `SQLiteGuardModelRegistry` 作为冷路径接入。
4. **Trainer**：进程内采集器，订阅 `guard.feedback` topic，把训练样本快照
   为 JSONL，再交给外部 sidecar 训练新候选模型。
5. **Applier**：`GuardModelApplier`（`AutonomySource::AdaptiveGuard`）。
   实现四方法 `IApprovalApplier` 契约，复用 workflow 的 audit / rollback /
   dry-run 设施。

---

## 3. API 参考

三个端点都要求已认证的 admin 会话。HTTP 层把 `Role::SuperAdmin` 映射为
`security_admin`、`Role::TenantAdmin` 映射为 `platform_admin`，再转发
给 `GuardAdminController`；后者执行规范的审核员白名单
`{security_admin, platform_admin, trust_safety}`。

### 3.1 POST /admin/api/guard/feedback

提交审核员标签。请求体是包含以下字段的 JSON：`request_id`、`trace_id`、
`label`（取值 `false_positive` / `false_negative` / `confirmed_block`）、
`reviewer_user_id`、`reviewer_role`、`comment`（自由文本，入库前 PII
脱敏）、`original_text_redacted`（可选）。提交成功返回 HTTP 200 +
`{accepted: true, request_id}`。常见错误码：`AEGIS-1002`（角色拒绝）、
`AEGIS-2001`（限流）、`AEGIS-5001`（body 异常，含被异常检测器拦截的
反馈）。

### 3.2 GET /admin/api/guard/explanation/{request_id}

返回该请求被拦截时记录的 `GuardExplanation` 结构，含 `trigger_layer`、
`trigger_rule_id`、`model_version`、`threshold`、`matched_pattern`、
`confidence`、`explanation_text` 七个字段。请求 id 未知返回
`AEGIS-6003`（复用 `ApprovalNotFound`）。

### 3.3 POST /admin/api/guard/model/promote

在自治 workflow 上创建一个 `ApprovalProposal`。请求体必须含
`action`（`promote_shadow_to_live` 或 `revert_to_previous`）、`model_id`、
`version`，以及一个包含 `win_rate` / `shadow_duration_min` /
`fp_rate_delta` 的 `shadow_metrics` 对象。响应返回提议 id；只有当
workflow 走完 `APPROVED` 且 applier 返回 `ok`，晋升才真正生效。
Kill switch 检查两次：一次在 `propose`，一次在
`GuardModelApplier::apply` 内部。

---

## 4. 模型注册表

注册表是一张带类型的表，记录 trainer 产生过的每一个候选模型，加上当前正在
服务流量的唯一 Live 模型。

### 4.1 状态机

允许的转换：`Shadow → Live`（把原 Live 行降级）、`Live → Retired`
（revert）、`Retired → Retired`（幂等）。其他转换一律拒绝并返回
`illegal_transition`。`Live` 不变量在**两处**被强制：C++ 层
`MemoryGuardModelRegistry::promote` 先抢全表互斥锁再切换 status 字段；
SQLite 层 partial `UNIQUE INDEX ... WHERE status = 'live'`。schema 层
`CHECK (status IN ('shadow','live','retired'))` 让手写 INSERT 也无法
绕过约束。

### 4.2 存储

v1 自带两种存储后端：`MemoryGuardModelRegistry`（进程内，零 IO，默认
启用、所有单测使用）以及 `SQLiteGuardModelRegistry`（文件持久化，启动时
跑 schema 迁移，prepared statement + `BEGIN IMMEDIATE` 事务保证晋升
原子性）。PostgreSQL 镜像作为 v2 工作项（TASK-W17）跟踪。

---

## 5. 反馈链路与异常检测

反馈链路面对的是敌意输入。v1 内置三道独立防御；每一道都由
`tests/rules/test-phase11.1-sr-presence.sh` 锚定，并由专门的 mutation
测试验证。

### 5.1 Sink

`GuardFeedbackSink::ingest` 是进入 trainer 管线的唯一入口。它先用
`PIIFilter::mask` 脱敏 `comment` 和 `original_text_redacted`；再把脱敏后
的载荷写入 `AuditLogger`（action `guard_feedback`、stage
`AdaptiveGuard`），让链式哈希把这条审计向后传递；最后把事件发布到
`FeedbackBus` 的 `guard.feedback` topic。缺失 audit logger → 硬失败
（`audit_unavailable`）；缺失 PII filter → 仍工作，但记录 warning。

### 5.2 限流

`GuardFeedbackRateLimiter` 运行三个 1 分钟滑动窗口计数器（强制顺序按
tenant、reviewer、global 三桶）。默认配额：每分钟每租户 100、每审核员
30、全局 10000。被拒绝的请求返回 HTTP 429 + 稳定的 `reject_reason`
字符串（`tenant_quota_exceeded` / `reviewer_quota_exceeded` /
`global_quota_exceeded`）。

### 5.3 异常检测

`GuardFeedbackAnomalyDetector` 用 1 小时滑动窗口统计每个审核员的
`false_positive` 数量（默认阈值 50）。一旦某审核员越过阈值，后续提交
返回 HTTP 409 + `error_code = reviewer_fp_burst`，并写一条审计记录
（`action = feedback_anomaly_flag`）。Sink 永远不会被触达，trainer
语料库因此保持干净。

---

## 6. 晋升流程

候选模型不允许无人值守上线。即便 auto-approve 路径也会复用
`AutonomyApprovalWorkflow`，让 audit、kill-switch、rollback 行为在不同
phase 之间保持一致。

### 6.1 isLowRisk 四规则

`GuardModelApplier::isLowRisk` 顺序评估四条规则；全部通过才允许
workflow 自动批准：

1. **R1**：action 不是 `revert_to_previous`（revert 永远需要人工签核）。
2. **R2**：shadow `win_rate >= 0.55`。
3. **R3**：`shadow_duration_min >= 60`（最低 1 小时灰度）。
4. **R4**：`fp_rate_delta >= -0.10`（候选模型 FP 率回退不得超过 10 个
   百分点）。

Mutation M6 验证 R1 分支；SR drift 测试把 `return false;` 分支数锁定为
5（schema 占 1，4 条规则各占 1）。

### 6.2 Kill Switch

`AEGISGATE_DISABLE_AUTONOMY=1` 关闭整个自治平面。Workflow 拒绝入队新提议
（`AEGIS-6002 AutonomyDisabled`），`GuardModelApplier::apply` 也用同一原因
短路 —— 双重检查存在的意义是让带外调用者（测试 fixture、未来的调度器）
无法绕过 workflow。

---

## 7. 安全与运维

学习型护栏的运维表面薄：同一份审计日志、同一份指标分类、同一个 kill
switch。v1 不引入新依赖、不开新端口、不带新 sidecar。

### 7.1 审计链路

每一条被接受的反馈、每一次异常 flag、每一个晋升提议、每一次 apply
结果都落入 `AuditLogger`，并携带链式哈希。`SR5` 由 mutation M4 验证
（跳过 audit 调用必须让测试 FAIL）。运维可在 `/admin/api/audits` 上用
`stage=AdaptiveGuard` 拉取完整链路。

---

## 参考资料

- [Cost Autonomy & Approval Workflow 指南](./cost-autonomy_zh.md)：跨
  phase 共享同一套 `AutonomyApprovalWorkflow` 治理面。
- [Feedback Bus 指南](./feedback-bus_zh.md)：`guard.feedback` topic
  背后的语义与投递保证。
- `docs/specs/2026-05-23-phase11.1-adaptive-guard-design.md`：设计规格
  含 STRIDE 威胁模型与 SR-NEW1 / SR-NEW2 四元组。
