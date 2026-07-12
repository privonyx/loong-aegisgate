# Agent Workflow 2.0 (Phase 11.3) Guide

> Feature: Declarative DAG workflows with human-in-the-loop approval
> Available: v1.1+ (TASK-20260523-02)

Workflow 2.0 lets operators describe multi-step agent behaviour as a YAML
DAG of nodes — tool calls, human approvals, conditional branches — then
hands execution to `WorkflowEngine`. The engine drives nodes through
`ToolSandbox` (no shortcut bypass), routes `HumanApproval` nodes through
the same `AutonomyApprovalWorkflow` that Phase 11.5 introduced for cost
autonomy, and persists every run/node state transition through
`IWorkflowStateStore` (in-memory for tests, SQLite for durability).

This guide covers seven topics: the high-level model, the runtime
architecture, the YAML DSL, the engine state machine, the human-approval
seam, the persistence layer, and the security / operational handles.

---

## 1. Overview

The runtime composes Workflow 2.0 from existing primitives:

| Building block | Provided by | Role |
|----------------|-------------|------|
| `WorkflowDsl` / parser | `src/workflow/workflow_dsl{,_parser}.{h,cpp}` | YAML/JSON DAG schema + validators |
| `WorkflowEngine` | `src/workflow/workflow_engine.{h,cpp}` | DAG scheduler + retry + DLQ |
| `IWorkflowStateStore` | `src/workflow/{memory,sqlite}_workflow_state_store.{h,cpp}` | Per-run persistence |
| `HumanApprovalNodeHandler` | `src/workflow/human_approval_node_handler.{h,cpp}` | Pause -> proposal bridge |
| `WorkflowApprovalApplier` | `src/workflow/workflow_approval_applier.{h,cpp}` | Approve -> resume bridge |

The kill switch `AEGISGATE_DISABLE_AUTONOMY=1` short-circuits both the
engine layer and the applier layer (defense-in-depth, decision D9=C).

---

## 2. Architecture

```
        +---------------------+
        |   YAML DSL (text)   |
        +---------------------+
                  | parseWorkflowDslYaml()
                  v
        +---------------------+
        | WorkflowDsl (POD)   |
        +---------------------+
                  | execute(dsl, run_id, ctx)
                  v
   +--------------------------------+   submit()    +-------------+
   |       WorkflowEngine           | ------------> | ThreadPool  |
   |  (Kahn ready-queue + retry +   |               +-------------+
   |   DLQ + SR17 layer-1 check)    |
   +--------------------------------+
       |                  |                    |
       | Tool node        | HumanApproval      | persist
       v                  v                    v
  +---------+   +------------------------+   +----------------------+
  | Sandbox |   | HumanApprovalHandler   |   | IWorkflowStateStore  |
  | execute |   |  -> AutonomyApproval-  |   | (Memory / SQLite)    |
  +---------+   |     Workflow.propose() |   +----------------------+
                +-----------+------------+
                            |
                  approve() v   apply()
                +----------------------+
                |  WorkflowApproval-   |
                |  Applier (SR17 L2)   |
                +----------------------+
                            | engine.resume()
                            v
                 +-------------------+
                 |  WorkflowEngine   |  -> downstream Tool nodes fire
                 +-------------------+
```

Five logical layers participate. `GatewayRuntime::initialize()` wires
them in one block so a single config flag enables the entire plane.

### 2.1 Five Layers

1. **DSL** (`WorkflowDsl`, `NodeSpec`, `RetryPolicy`, `NodeType`).
   Parsed from YAML/JSON. `canonicalHash()` produces a stable sha256
   over the structural part (id/version/nodes) — the value travels with
   the run and is checked again at `resume()` / `apply()` (T01 defence).
2. **Engine** (`WorkflowEngine`). Kahn-style ready queue. Tool nodes
   are dispatched through `ToolSandbox`. HumanApproval nodes call the
   registered callback (the runtime supplies one that delegates to
   `HumanApprovalNodeHandler`). Each node has its own retry budget
   (`RetryPolicy`); exhaustion routes the node to a DLQ status and
   flips the run to `dead_letter`.
3. **State store** (`IWorkflowStateStore`). Memory backend is used by
   tests and ephemeral runs. SQLite backend wraps every mutating write
   in `BEGIN IMMEDIATE` and enforces `CHECK (status IN ...)` on both
   the run and node-run tables (SR-NEW3 invariant in the schema, not
   the application).
4. **Human approval seam** (`HumanApprovalNodeHandler` +
   `WorkflowApprovalApplier`). The handler builds an
   `ApprovalProposal` with `source = AutonomySource::Workflow`,
   scrubs every payload / decision-trace string through `PIIFilter`
   (SR-NEW4), and submits via `AutonomyApprovalWorkflow::propose`. The
   applier is registered for `AutonomySource::Workflow`; once a
   proposal is approved + applied, it calls `engine.resume(run_id)`.
5. **GatewayRuntime wiring** owns engine, state store, and applier.
   Destruction order respects A2 symmetry: applier resets before
   engine before state store.

---

## 3. YAML DSL

A workflow is one YAML document with id, version, and a `nodes`
sequence:

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
    tool_id: security_admin     # reviewer role (not a ToolRegistry tool)
    depends_on: [enrich]
    timeout_ms: 600000
  - id: dispatch
    type: tool
    tool_id: dispatch_response
    depends_on: [review]
```

Five `NodeType` variants are accepted: `tool`, `human_approval`,
`conditional`, `fan_out`, `fan_in`. v1 implements the first two as
executable nodes; the remaining three are accepted by the parser but
behave as topology-only passes (skipped) so future v2 work can fill
them in without changing the wire format.

Both parsers reject:

- Duplicate node ids.
- A `depends_on` that names a missing node.
- A cycle (self-loop or longer) — surfaced via `validateNoCycle()`.
- An empty `tool_id` on a `tool` / `human_approval` node (SR3 invariant
  #1, surfaced via `validateNoSandboxBypass()`).

---

## 4. Engine state machine

| WorkflowRunStatus | Trigger |
|-------------------|---------|
| `pending`              | persisted before first ready-queue tick |
| `running`              | first ready node about to dispatch |
| `waiting_for_approval` | engine reached a HumanApproval node, paused |
| `succeeded`            | all nodes reached `succeeded` or `skipped` |
| `failed`               | pre-flight refused (cycle / sandbox bypass) |
| `cancelled`            | SR17 short-circuit before any side effect |
| `dead_letter`          | any node exhausted retries (when `stop_on_first_failure`) |

Per-node statuses mirror the run set plus `running`, `waiting_for_approval`,
`succeeded`, `failed`, `dead_letter`, `skipped`. The CHECK constraints
on both columns are enforced at the schema level so a forgotten
`toString()` call cannot insert a bogus value.

`WorkflowEngine::resume(run_id)`:

1. Loads the persisted DSL + run record.
2. Verifies `canonicalHash(replayed_dsl) == run.dsl_hash` (T01).
3. Marks the `WaitingForApproval` node `Succeeded`.
4. Re-enters `execute()` with a trimmed DSL containing only unrun
   nodes (their `depends_on` filtered to the trimmed set), reusing the
   same `run_id`.

---

## 5. Human approval seam

`HumanApprovalNodeHandler` is what the engine callback calls. It:

- builds an `ApprovalProposal` with all fields the applier needs to
  resume() unaided: `run_id`, `workflow_id`, `node_id`, `dsl_hash`,
  `tool_id`, `arguments`, `context`.
- writes `decision_trace.{source_id, algorithm_name, input_hash_sha256,
  proposed_at_ms}` so the proposal passes the strict validator inside
  `AutonomyApprovalWorkflow::propose()`.
- runs every string value recursively through `PIIFilter::mask`
  (SR-NEW4). DSL-supplied phone numbers / emails / id cards never
  reach the approval queue or audit log.

`WorkflowApprovalApplier::isLowRisk()` is the 4-rule predicate (SR2):

1. `tool_id` must be in the curated safelist (`read_only_metrics_lookup`,
   `audit_log_query`, `shadow_inference`, `tracing_lookup`).
2. `arguments.size() < 5`.
3. `timeout_ms <= 10000`.
4. `tags` must contain `"low_risk_audited"` — explicit operator opt-in.

All four must pass; any single failure drops back to manual approval.

---

## 6. Persistence

`MemoryWorkflowStateStore` is the default in tests and the v1
`GatewayRuntime` wiring. It stores runs + node runs in
`std::unordered_map` guarded by `std::mutex` (lock layer 3 — see
`docs/LOCK_ORDERING.md`).

`SQLiteWorkflowStateStore` is the durable backend. The schema:

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

WAL is enabled and every mutating method opens its statement inside
`BEGIN IMMEDIATE; ... ; COMMIT;` so concurrent engine + applier
transitions on the same row serialise without write-write deadlock.

`pruneOldRuns(cutoff_ms)` deletes runs whose `updated_at_ms < cutoff_ms`
and lets the `ON DELETE CASCADE` clean up node rows.

---

## 7. Operational handles

| Concern | Handle |
|---------|--------|
| Disable all autonomy (kill switch) | `AEGISGATE_DISABLE_AUTONOMY=1` |
| Engine pool size | `WorkflowEngineConfig.worker_count` |
| Per-node retries | DSL `retry.{max_attempts, backoff_ms, exponential}` |
| Per-node timeout | DSL `timeout_ms` |
| Continue past first failure | `WorkflowEngineConfig.stop_on_first_failure = false` |
| State backend | swap `IWorkflowStateStore` impl at wiring time |
| SR drift CI | `bash tests/rules/test-phase11.3-sr-presence.sh` |

The kill switch is checked twice: at the engine boundary
(`WorkflowEngine::isAutonomyEnabled`, layer 1) and again at the
applier boundary
(`AutonomyApprovalWorkflow::isAutonomyEnabled`, layer 2). Either layer
firing returns the run to a safe terminal state without touching tool
side effects.

---

## 8. Walk-through: v1 lifecycle

The single integration test `tests/unit/workflow/test_workflow_e2e.cpp`
demonstrates the full lifecycle as an executable artefact:

1. `parseWorkflowDslYaml(text)` -> `WorkflowDsl`.
2. `engine.execute(dsl, "run-E2E", ctx)` runs the first Tool node
   (`enrich_request` through `ToolSandbox`).
3. Engine hits the HumanApproval node; calls the registered callback
   which invokes `HumanApprovalNodeHandler::enqueue(...)`. The
   proposal is now in `AutonomyApprovalWorkflow` queue with state
   `PROPOSED` and source `Workflow`. Run is `waiting_for_approval`.
4. Operator calls `approve_proposal(id, "alice")`. State -> `APPROVED`.
5. Operator calls `apply(id)`. The workflow dispatches to
   `WorkflowApprovalApplier::apply()`, which checks SR17 layer 2,
   verifies `dsl_hash`, then calls `engine.resume(run_id)`.
6. Engine resumes from the trimmed DAG, runs the downstream Tool
   node (`dispatch_response`), and transitions the run to
   `succeeded`.

Each step persists through the state store so a process restart between
step 3 and 5 still resumes correctly (SR-NEW3).
