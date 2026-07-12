# Agent Workflow 2.0（Phase 11.3）指南

> 功能：声明式 DAG 工作流 + 人审节点（human-in-the-loop）
> 可用版本：v1.1+（TASK-20260523-02）

Workflow 2.0 让运维以 YAML DAG 描述多步 Agent 行为——工具调用、人审、条件分
支——交由 `WorkflowEngine` 执行。引擎通过 `ToolSandbox` 派发每个节点（无任
何捷径绕过），把 `HumanApproval` 节点接入 Phase 11.5 引入的
`AutonomyApprovalWorkflow`，并通过 `IWorkflowStateStore` 持久化每次状态变
迁（测试用内存后端，生产用 SQLite）。

本指南覆盖 7 个主题：总览、运行时架构、YAML DSL、引擎状态机、人审接缝、
持久化层、安全/运维开关。

---

## 1. 总览

运行时把 Workflow 2.0 拆解为复用现有的基础设施：

| 模块 | 文件 | 角色 |
|------|------|------|
| `WorkflowDsl` / 解析器 | `src/workflow/workflow_dsl{,_parser}.{h,cpp}` | YAML/JSON DAG schema + 校验器 |
| `WorkflowEngine` | `src/workflow/workflow_engine.{h,cpp}` | DAG 调度 + 重试 + DLQ |
| `IWorkflowStateStore` | `src/workflow/{memory,sqlite}_workflow_state_store.{h,cpp}` | 单次 run 的持久化 |
| `HumanApprovalNodeHandler` | `src/workflow/human_approval_node_handler.{h,cpp}` | 暂停 -> 提案 桥 |
| `WorkflowApprovalApplier` | `src/workflow/workflow_approval_applier.{h,cpp}` | 审批 -> resume 桥 |

总开关 `AEGISGATE_DISABLE_AUTONOMY=1` 双层短路——引擎层 + Applier 层
（决策 D9=C 纵深防御）。

---

## 2. 运行时架构

```
        +---------------------+
        |   YAML DSL（文本）   |
        +---------------------+
                  | parseWorkflowDslYaml()
                  v
        +---------------------+
        | WorkflowDsl（POD）   |
        +---------------------+
                  | execute(dsl, run_id, ctx)
                  v
   +--------------------------------+   submit()    +-------------+
   |       WorkflowEngine           | ------------> | ThreadPool  |
   |  （Kahn ready 队列 + 重试 +    |               +-------------+
   |   DLQ + SR17 第 1 层判定）     |
   +--------------------------------+
       |                  |                    |
       | Tool 节点        | HumanApproval      | persist
       v                  v                    v
  +---------+   +------------------------+   +----------------------+
  | Sandbox |   | HumanApprovalHandler   |   | IWorkflowStateStore  |
  | execute |   |  -> AutonomyApproval-  |   |（Memory / SQLite）   |
  +---------+   |     Workflow.propose() |   +----------------------+
                +-----------+------------+
                            |
                  approve() v   apply()
                +----------------------+
                |  WorkflowApproval-   |
                |  Applier（SR17 第2层）|
                +----------------------+
                            | engine.resume()
                            v
                 +-------------------+
                 |  WorkflowEngine   |  -> 下游 Tool 节点接续执行
                 +-------------------+
```

5 个逻辑层全部由 `GatewayRuntime::initialize()` 装配，单个配置开关即可启用。

### 2.1 五层

1. **DSL**（`WorkflowDsl`、`NodeSpec`、`RetryPolicy`、`NodeType`）。
   `canonicalHash()` 对 id/version/nodes 计算稳定的 sha256；该值随 run
   持久化，并在 `resume()` / `apply()` 时再次比对（T01 防篡改锚点）。
2. **Engine**（`WorkflowEngine`）。Kahn ready 队列调度；Tool 节点必经
   `ToolSandbox`；HumanApproval 节点回调到 `HumanApprovalNodeHandler`；
   每节点独立 `RetryPolicy`，重试耗尽 -> 节点 DLQ -> run dead_letter。
3. **状态存储**（`IWorkflowStateStore`）。Memory 后端用于测试/瞬态运行；
   SQLite 后端所有写入包在 `BEGIN IMMEDIATE` 内，且 status 列由
   `CHECK (status IN ...)` 在 schema 层强制（SR-NEW3 的硬约束放在
   schema，而不是应用层 toString 链）。
4. **人审接缝**（`HumanApprovalNodeHandler` + `WorkflowApprovalApplier`）。
   Handler 构造 `source = AutonomySource::Workflow` 的 `ApprovalProposal`，
   递归把 payload + decision_trace 的每个字符串走 `PIIFilter::mask`
   （SR-NEW4），再交给 `AutonomyApprovalWorkflow::propose`。Applier
   注册到 `AutonomySource::Workflow`；当 proposal 走完 approve + apply
   时，它调用 `engine.resume(run_id)`。
5. **GatewayRuntime 装配** 拥有 engine、state store、applier 三者。析构
   顺序遵守 A2 对称：applier -> engine -> state store。

---

## 3. YAML DSL

一个工作流是一个 YAML 文档，包含 id、version 和 `nodes` 序列：

```yaml
id: customer_support_v1
version: v1
description: enrich -> approve -> dispatch
nodes:
  - id: enrich
    type: tool
    tool_id: enrich_request
    arguments:
      lookup_user: true
    timeout_ms: 5000
    retry:
      max_attempts: 3
      backoff_ms: 200
      exponential: true
  - id: review
    type: human_approval
    tool_id: security_admin     # 审批人角色（不是 ToolRegistry 工具）
    depends_on: [enrich]
    timeout_ms: 600000
  - id: dispatch
    type: tool
    tool_id: dispatch_response
    depends_on: [review]
```

5 种 `NodeType`：`tool`、`human_approval`、`conditional`、`fan_out`、
`fan_in`。v1 实现前两个为可执行节点；后三个解析器接受但仅做拓扑占位
（跳过），v2 可在不破坏 wire format 的前提下接续实现。

解析器拒绝以下输入：

- 重复的 node id。
- `depends_on` 指向不存在的节点。
- 任何循环（自环或更长）——由 `validateNoCycle()` 暴露。
- 任何可执行节点的 `tool_id` 为空（SR3 不变式 #1，由
  `validateNoSandboxBypass()` 暴露）。

---

## 4. 引擎状态机

| WorkflowRunStatus | 触发 |
|-------------------|------|
| `pending`              | ready 队列首个 tick 前持久化 |
| `running`              | 首个 ready 节点即将派发 |
| `waiting_for_approval` | 抵达 HumanApproval 节点暂停 |
| `succeeded`            | 所有节点抵达 `succeeded` 或 `skipped` |
| `failed`               | 预检失败（cycle / sandbox bypass） |
| `cancelled`            | SR17 在任何副作用前短路 |
| `dead_letter`          | 任一节点重试耗尽（开启 `stop_on_first_failure`） |

节点状态镜像 run 状态再加 `running`、`waiting_for_approval`、
`succeeded`、`failed`、`dead_letter`、`skipped`。两列的 CHECK 约束在
schema 层强制——遗漏 `toString()` 不会让坏值落库。

`WorkflowEngine::resume(run_id)`：

1. 从存储读取 DSL + run 记录；
2. 校验 `canonicalHash(replayed_dsl) == run.dsl_hash`（T01）；
3. 把 `WaitingForApproval` 节点标记为 `Succeeded`；
4. 以**剪枝后的 DSL**（剔除已完成节点，过滤 `depends_on`）重新进入
   `execute()`，复用同一 `run_id`。

---

## 5. 人审接缝

`HumanApprovalNodeHandler` 是 Engine 回调实际调用的对象：

- 构造提案，payload 字段足以让 applier 独立完成 resume：`run_id`、
  `workflow_id`、`node_id`、`dsl_hash`、`tool_id`、`arguments`、
  `context`。
- 写 `decision_trace.{source_id, algorithm_name, input_hash_sha256,
  proposed_at_ms}` 通过 `AutonomyApprovalWorkflow::propose()` 内部的
  严格校验器。
- 递归对每个字符串值跑 `PIIFilter::mask`（SR-NEW4）。DSL 注入的电话/
  邮箱/身份证号永远不会进入 approval 队列或审计日志。

`WorkflowApprovalApplier::isLowRisk()` 是 4 规则谓词（SR2）：

1. `tool_id` 在精选白名单内（`read_only_metrics_lookup`、
   `audit_log_query`、`shadow_inference`、`tracing_lookup`）。
2. `arguments.size() < 5`。
3. `timeout_ms <= 10000`。
4. `tags` 包含 `"low_risk_audited"`——必须由运维显式贴标。

四条必须全部满足；任意一条失败 -> 回落到手动审批。

---

## 6. 持久化

`MemoryWorkflowStateStore` 是测试和 v1 `GatewayRuntime` 装配默认。runs +
node runs 存在 `std::unordered_map` 内，由 `std::mutex` 守护（锁层 3，
见 `docs/LOCK_ORDERING.md`）。

`SQLiteWorkflowStateStore` 是耐久后端。Schema：

```sql
CREATE TABLE workflow_runs (
    run_id              TEXT PRIMARY KEY,
    workflow_id         TEXT NOT NULL,
    dsl_hash            TEXT NOT NULL,
    status              TEXT NOT NULL
        CHECK (status IN ('pending','running','waiting_for_approval',
                           'succeeded','failed','cancelled','dead_letter')),
    created_at_ms       INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,
    dsl_json            TEXT NOT NULL DEFAULT '',
    context_json        TEXT NOT NULL DEFAULT '{}',
    initiator_user_id   TEXT NOT NULL DEFAULT ''
);
CREATE TABLE workflow_node_runs (
    run_id                  TEXT NOT NULL,
    node_id                 TEXT NOT NULL,
    attempt                 INTEGER NOT NULL DEFAULT 1,
    status                  TEXT NOT NULL
        CHECK (status IN ('pending','running','succeeded','failed',
                           'skipped','waiting_for_approval','dead_letter')),
    started_at_ms           INTEGER NOT NULL DEFAULT 0,
    ended_at_ms             INTEGER NOT NULL DEFAULT 0,
    result_json             TEXT NOT NULL DEFAULT '',
    error_message           TEXT NOT NULL DEFAULT '',
    approval_proposal_id    TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (run_id, node_id),
    FOREIGN KEY (run_id) REFERENCES workflow_runs(run_id) ON DELETE CASCADE
);
```

WAL 开启；所有写路径包在 `BEGIN IMMEDIATE; ... ; COMMIT;` 内，
engine 与 applier 对同一 run 的并发状态变迁因此串行化而不会写-写死锁。

`pruneOldRuns(cutoff_ms)` 删除 `updated_at_ms < cutoff_ms` 的 run，
`ON DELETE CASCADE` 自动清理 node 行。

---

## 7. 运维开关

| 关注点 | 开关 |
|--------|------|
| 关闭所有自治（总开关） | `AEGISGATE_DISABLE_AUTONOMY=1` |
| 引擎并发线程数 | `WorkflowEngineConfig.worker_count` |
| 每节点重试 | DSL `retry.{max_attempts, backoff_ms, exponential}` |
| 每节点超时 | DSL `timeout_ms` |
| 首次失败后是否继续 | `WorkflowEngineConfig.stop_on_first_failure = false` |
| 状态后端 | 装配时替换 `IWorkflowStateStore` 实现 |
| SR drift CI | `bash tests/rules/test-phase11.3-sr-presence.sh` |

总开关在两处被检查：引擎边界
（`WorkflowEngine::isAutonomyEnabled`，第 1 层）+ Applier 边界
（`AutonomyApprovalWorkflow::isAutonomyEnabled`，第 2 层）。任一层
触发都让 run 安全终态化，不会触碰任何工具副作用。

---

## 8. 走查：v1 全生命周期

集成测试 `tests/unit/workflow/test_workflow_e2e.cpp` 是一个可执行的
artefact，演示完整生命周期：

1. `parseWorkflowDslYaml(text)` -> `WorkflowDsl`。
2. `engine.execute(dsl, "run-E2E", ctx)` 运行首个 Tool 节点
   （`enrich_request` 经 `ToolSandbox`）。
3. Engine 触达 HumanApproval 节点；回调进入
   `HumanApprovalNodeHandler::enqueue(...)`。提案此时位于
   `AutonomyApprovalWorkflow` 队列，状态 `PROPOSED`，source = `Workflow`。
   Run 转入 `waiting_for_approval`。
4. 运维调用 `approve_proposal(id, "alice")`。状态 -> `APPROVED`。
5. 运维调用 `apply(id)`。Workflow 派发到
   `WorkflowApprovalApplier::apply()`，后者完成 SR17 第 2 层判定 +
   `dsl_hash` 校验，然后调用 `engine.resume(run_id)`。
6. Engine 用剪枝后的 DAG 续跑下游 Tool 节点
   （`dispatch_response`），run 转入 `succeeded`。

每步都经过 state store 持久化，故第 3 步与第 5 步之间发生进程重启
也能正确续跑（SR-NEW3）。
