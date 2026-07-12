# 自愈运维指南：编写 RCA 规则与 Runbook（v1）

**任务：** TASK-20260519-01 Phase 11.4 Self-Healing Operations
**适用版本：** v1（创建日期 2026-05-20）
**目标读者：** 运维工程师、SRE、平台开发者

---

## 1. 总览

AegisGate v1 自愈运维由 **3 个组件** 串联：

```
RootCauseAnalyzer (RCA)  →  RunbookEngine  →  RecoveryApplier
   规则化根因评分            触发条件 + 动作      执行 / 提案 / 审计
```

数据流：

```
metrics + FeedbackBus + spdlog ringbuffer  →  RcaSignals
                       ↓
                 RootCauseAnalyzer.analyze()  →  hypotheses
                       ↓
                 RunbookEngine.evaluate()      →  matches
                       ↓
                 buildProposal()               →  ApprovalProposal
                       ↓
        AutonomyApprovalWorkflow + RecoveryApplier
                       ↓
               MLRouter / BudgetGuardStage / audit
```

**v1 关键设计取舍（YAGNI）：**

- 5 个 advisory 动作（`switch_router_fallback` / `switch_connector` /
  `propose_hpa_scale` / `send_webhook` / `audit_only`）只写 audit + spdlog
  warn，由审批者读 audit 后人工执行 kubectl/切流脚本。
- 2 个 active 动作（`override_quality_tier` / `apply_budget_cap`）真正
  修改运行时状态。
- v1 不直接调 K8s API；HPA 提案以 `kubectl scale ...` 命令字符串形式
  给运维。

---

## 2. 编写 RCA 规则

### 2.1 文件位置与加载

- 默认路径：`config/rca_rules/rca_rules_v1.yaml`
- SR-NEW1：路径 canonical 化后必须以 `.yaml`/`.yml` 结尾，不允许包含
  `/..`。加载时 sha256 写日志便于审计。

### 2.2 规则结构

```yaml
rules:
  - id: "rca_<unique_snake_case>"            # 规则唯一 id（强制）
    category: "internal | upstream | abuse | config"
    summary: "人类可读概述"
    min_matched_conditions: 1                 # 至少命中 N 个 condition 才算"匹配"
    score_when_all_matched: 0.9               # 全部命中时的分数（上限）
    score_per_condition: 0.45                 # 每多命中 1 个的增量
    conditions:
      - kind: "metric_threshold"              # 见 §2.3
        signal: "p99_latency_ms"
        op: ">"
        value: 1500
        evidence_label: "p99=${current}ms 超过 ${expected}ms"
      - kind: "feedback_event_count"
        event_type: "OpsIncident"
        topic_match: "upstream"
        window_seconds: 300
        count_threshold: 3
        evidence_label: "上游事件 ${count} 次"
      - kind: "log_pattern_count"
        pattern: "5\\d\\d Bad Gateway"
        window_seconds: 300
        count_threshold: 5
        evidence_label: "5xx 日志 ${count}"
    suggested_actions:
      - "rb_upstream_provider_failover"
```

**评分公式：**

```
score = clamp(
    matched_count >= min_matched_conditions
      ? min(score_per_condition * matched_count, score_when_all_matched)
      : 0.0,
    [0, 1]
)
```

**全局阈值：** 仅当 `score >= 0.3` 时输出 hypothesis（避免噪音）。

### 2.3 三种 Condition Kind

| Kind | 关键字段 | 用途 |
|---|---|---|
| `metric_threshold` | `signal` / `op` / `value` | 单点 metric 阈值（p99 / error_rate / 自定义） |
| `feedback_event_count` | `event_type` / `topic_match` / `window_seconds` / `count_threshold` | FeedbackBus 事件计数（OpsIncident 等） |
| `log_pattern_count` | `pattern`（RE2 正则） / `window_seconds` / `count_threshold` | 日志 ringbuffer 正则匹配计数 |

**evidence_label 模板变量：**

- `${current}` `${expected}`（metric_threshold）
- `${count}` `${window}`（feedback_event_count）
- `${count}` `${sample}`（log_pattern_count）

### 2.4 安全注意事项

- **PII**：evidence label / current / expected 三字段在输出前走
  `PIIFilter::mask` 兜底（depth-in-defence），即使日志 sink 漏 mask
  也不会泄漏。
- **正则灾难性回溯**：`pattern` 走 RE2，无回溯爆炸风险。
- **热加载失败**：reloadRules 失败时保留旧规则集，不会清空 RCA 引擎。

---

## 3. 编写 Runbook

### 3.1 文件位置与加载

- 默认目录：`config/runbooks/`
- 每个 runbook 是独立 `.yaml` 文件，目录扫描时只读 `.yaml`/`.yml`。
- SR-NEW1 同 RCA：canonical 检查 + 路径白名单。

### 3.2 Runbook 结构

```yaml
id: "rb_<unique_snake_case>"
description: "人类可读概述"
triggers:
  # OR 语义 — 任一 trigger 命中即激活 runbook
  - kind: "rca_required"                      # 见 §3.3
    rca_rule_id: "rca_high_p99_internal"
    rca_min_score: 0.7
  - kind: "metric"
    signal: "error_rate"
    op: ">"
    value: 0.10
actions:
  # 顺序执行（v1 仅支持 single-action runbook 通过 ApprovalProposal
  # 上调 propose-first 路径自动 dispatch；多 action 由 RunbookEngine
  # 在 v2 串行执行）
  - action: "override_quality_tier"
    payload:
      tenant_id: "${tenant_id}"
      to_quality_tier: "economy"
      from_quality_tier: "standard"
rollback_actions:
  - action: "override_quality_tier"
    payload:
      tenant_id: "${tenant_id}"
      to_quality_tier: "standard"
approval_required: true                       # true=人工审批；false=auto-approve
cooldown_seconds: 600                         # 同 runbook 触发后冷却时间
```

### 3.3 三种 Trigger Kind

| Kind | 关键字段 | 何时使用 |
|---|---|---|
| `metric` | `signal` / `op` / `value` | 简单单点 metric 触发 |
| `feedback_event_count` | `event_type` / `topic_match` / `count_threshold` | FeedbackBus 事件累计触发 |
| `rca_required` | `rca_rule_id` / `rca_min_score` | **SR7 双信号确认**：要求 RCA 已输出对应 hypothesis 且 score 达标 |

**强烈推荐：** 高风险 active 动作（apply_budget_cap / override_quality_tier）
应使用 `rca_required` trigger，确保至少 2 个独立信号源同时确认。

### 3.4 七种 Action 类型

| Action | 类型 | 关键 payload | 行为 |
|---|---|---|---|
| `override_quality_tier` | active | `tenant_id` / `to_quality_tier` | MLRouter::overrideQualityTier |
| `apply_budget_cap` | active | `new_per_tenant_24h_usd` | BudgetGuardStage::setConfig |
| `switch_router_fallback` | advisory | `model_id` / `off_until_ms` | audit + spdlog warn |
| `switch_connector` | advisory | `from_provider` / `to_provider` | audit + spdlog warn |
| `propose_hpa_scale` | advisory | `target_replicas` / `suggested_kubectl_command` | audit + spdlog warn |
| `send_webhook` | advisory | `url` / `body` | audit + spdlog warn (v1 stub) |
| `audit_only` | advisory | 任意 | 仅写 audit |

**isLowRisk 规则**（auto-approve 才适用）：

- `override_quality_tier`：仅降级（rank(to) < rank(from)）/ 不跨 2 级 /
  ≤$50/24h savings / ≤1000 RPS
- `apply_budget_cap`：new_cap ≥ current_cap × 0.5
- 其余 advisory：永远 false（必须 approval）

### 3.5 cooldown 与 approval 选择

- `approval_required: false` → auto-approve 但仍走 propose → approve →
  apply 三步审计链（SR2）。仅低风险（isLowRisk=true）可这样配置。
- `approval_required: true` → 提案落 ApprovalQueue，等待运维通过
  `/admin/approvals/<id>/approve` 接口。
- `cooldown_seconds`：runbook 命中后多久内不再二次触发（M3 防重复）。

### 3.6 安全注意事项

- **payload schema**：`override_quality_tier` 的 `tenant_id`/`to_quality_tier`
  缺失 → fail closed（apply 返回 schema_invalid）。
- **dry_run**：所有 active 动作支持 `apply(p, dry_run=true)`，可在
  Admin UI 预览 before/after。
- **rollback**：每个 action 都应配 rollback_actions（至少 audit_only），
  方便 ROLLED_BACK 状态切换。

---

## 4. 调试与运维

### 4.1 查看活跃规则与 runbook

```bash
# RCA 规则数量与最近重载时间
curl http://localhost:18080/admin/rca/health

# Runbook 加载状态
curl http://localhost:18080/admin/runbooks
```

### 4.2 查看 audit 链

```bash
curl 'http://localhost:18080/admin/audit?action_prefix=auto_recovery&limit=20'
```

每条 audit entry 包含：`action="auto_recovery.apply.<action_type>"`，
`detail` 字段含 before/after 摘要。

### 4.3 紧急熔断（SR17）

```bash
# 全局禁用 autonomy（含自愈）
export AEGISGATE_DISABLE_AUTONOMY=1
# 重启进程；下次 RCA / RunbookEngine 在 evaluate 入口短路
```

### 4.4 PII mask 兜底验证

```bash
# 故意打一条含邮箱的日志
curl -X POST http://localhost:18080/admin/test/log \
  -d '{"msg":"failed for alice@example.com"}'

# dump 最近 logs（仅 SuperAdmin 可见）
curl http://localhost:18080/admin/logs/dump?limit=10
```

response 中 `alice@example.com` 必须替换为 `[EMAIL]`。

---

## 5. v1 不支持 / 计划事项

- 多 action runbook 的串行执行（v2，由 RunbookEngine 驱动）
- HPA 直接调 K8s API（v2，需引入 K8s client；v1 仅 propose）
- 跨 runbook 的 DAG 依赖（v3，Workflow 2.0）
- ML 学习的根因评分（v3）

---
