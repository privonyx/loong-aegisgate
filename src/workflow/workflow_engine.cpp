#include "workflow/workflow_engine.h"

#include "agent/tool_sandbox.h"
#include "workflow/thread_pool.h"
#include "workflow/workflow_dsl_parser.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aegisgate::workflow {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

WorkflowEngine::WorkflowEngine(WorkflowEngineConfig         cfg,
                                aegisgate::ToolSandbox*      sandbox,
                                IWorkflowStateStore*         state_store)
    : cfg_(cfg), sandbox_(sandbox), state_store_(state_store) {}

WorkflowEngine::~WorkflowEngine() = default;

bool WorkflowEngine::isAutonomyEnabled() {
    const char* v = std::getenv("AEGISGATE_DISABLE_AUTONOMY");
    return !(v != nullptr && std::strcmp(v, "1") == 0);
}

bool WorkflowEngine::checkAutonomyOrCancel(const WorkflowDsl& dsl,
                                            const std::string& run_id,
                                            WorkflowExecutionResult& out) {
    bool enabled =
        autonomy_override_.has_value() ? *autonomy_override_ : isAutonomyEnabled();
    if (enabled) return true;
    spdlog::warn("[workflow-engine] execute refused for {} (SR17 layer 1)",
                  run_id);
    // Best-effort transition the run to Cancelled if it was already persisted.
    if (state_store_->getRun(run_id).has_value()) {
        state_store_->transitionRunStatus(run_id, WorkflowRunStatus::Cancelled,
                                            nowMs());
    }
    out.ok            = false;
    out.final_status  = WorkflowRunStatus::Cancelled;
    out.error_message = "autonomy_disabled";
    (void)dsl;
    return false;
}

WorkflowExecutionResult WorkflowEngine::execute(const WorkflowDsl&   dsl,
                                                 const std::string&   run_id,
                                                 const nlohmann::json& context) {
    WorkflowExecutionResult out;

    // --- SR-4 背压：run 级准入（fail-fast，无排队 → DoS 安全）。RAII 保证异常/
    // 提前返回也递减 active_runs_。---
    int cur = active_runs_.fetch_add(1) + 1;
    struct ActiveGuard {
        std::atomic<int>& c;
        ~ActiveGuard() { --c; }
    } active_guard{active_runs_};
    if (cur > cfg_.max_concurrent_runs) {
        spdlog::warn("[workflow-engine] backpressure reject {} ({}/{} runs)",
                     run_id, cur, cfg_.max_concurrent_runs);
        out.ok            = false;
        out.final_status  = WorkflowRunStatus::Failed;
        out.error_message = "backpressure_rejected";
        return out;
    }

    if (!checkAutonomyOrCancel(dsl, run_id, out)) return out;

    // Pre-flight validators (SR-NEW3 + SR3).
    std::string cycle_node;
    if (!validateNoCycle(dsl, &cycle_node)) {
        out.error_message = "cycle detected at node '" + cycle_node + "'";
        out.final_status  = WorkflowRunStatus::Failed;
        return out;
    }
    std::vector<std::string> bypass_nodes;
    if (!validateNoSandboxBypass(dsl, /*registry=*/nullptr, &bypass_nodes)) {
        out.error_message = "sandbox_bypass attempted at node '" +
                            (bypass_nodes.empty() ? "?" : bypass_nodes[0]) + "'";
        out.final_status  = WorkflowRunStatus::Failed;
        return out;
    }

    // Persist initial run.
    WorkflowRunRecord rr;
    rr.run_id        = run_id;
    rr.workflow_id   = dsl.id;
    rr.dsl_hash      = canonicalHash(dsl);
    rr.status        = WorkflowRunStatus::Running;
    rr.created_at_ms = nowMs();
    rr.updated_at_ms = rr.created_at_ms;
    rr.dsl_json      = toJson(dsl).dump();
    rr.context_json  = context.dump();
    state_store_->createRun(rr);
    state_store_->transitionRunStatus(run_id, WorkflowRunStatus::Running, nowMs());

    // Build adjacency + in-degree.
    std::unordered_map<std::string, const NodeSpec*> by_id;
    std::unordered_map<std::string, int>              in_degree;
    std::unordered_map<std::string, std::vector<std::string>> succ;
    for (const auto& n : dsl.nodes) {
        by_id[n.id]    = &n;
        in_degree[n.id] = 0;
    }
    for (const auto& n : dsl.nodes) {
        for (const auto& d : n.depends_on) {
            succ[d].push_back(n.id);
            in_degree[n.id]++;
        }
    }

    // --- D2=C 连续调度状态（同步安全是核心）---
    // sched_mu 保护 in_degree 递减/ready 判定/outstanding 计数/detached 收集。
    // 终止用 outstanding 计数 + cv（不用 pool.wait_all，避免 submit-in-callback
    // 死锁）。节点超时用 std::async（独立线程，不占 pool worker，规避两级 pool
    // 依赖）。context 只读，无需 ctx 锁；state store 各实现内部自带锁。
    std::mutex               sched_mu;
    std::condition_variable  done_cv;
    int                      outstanding = 0;   // 已提交未完成的 runNode 数
    std::atomic<bool>        cancelled{false};   // stop_on_first_failure / pause
    std::atomic<bool>        paused{false};      // HumanApproval
    std::atomic<int>         completed{0};
    std::atomic<int>         failed{0};
    std::atomic<bool>        any_failure{false};
    // 超时守卫的 straggler future 保留到 execute() 收尾统一析构（避免 runNode 内
    // ~future 阻塞；线程不可强杀，依赖 sandbox 内部限额兜底）。
    std::vector<std::future<aegisgate::ToolExecutionResult>> detached;

    ThreadPool pool(std::max<std::size_t>(1, cfg_.worker_count));

    std::function<void(const std::string&)> runNode;
    auto submitReady = [&](const std::string& id) {  // 调用方须持 sched_mu
        ++outstanding;
        pool.submitDetached([&runNode, id] { runNode(id); });
    };

    runNode = [&](const std::string& node_id) {
        const NodeSpec* node = by_id[node_id];
        bool advance = false;

        if (!cancelled.load()) {
            if (node->type == NodeType::HumanApproval) {
                WorkflowNodeRunRecord pending;
                pending.run_id        = run_id;
                pending.node_id       = node_id;
                pending.attempt       = 1;
                pending.status        = WorkflowNodeStatus::WaitingForApproval;
                pending.started_at_ms = nowMs();
                state_store_->upsertNodeRun(pending);
                bool do_pause = approval_cb_
                                    ? approval_cb_(run_id, *node, context)
                                    : true;
                if (do_pause) {
                    paused.store(true);
                    cancelled.store(true);  // 停止调度新节点，in-flight 自然结束
                } else {
                    pending.status      = WorkflowNodeStatus::Succeeded;
                    pending.ended_at_ms = nowMs();
                    state_store_->upsertNodeRun(pending);
                    ++completed;
                    advance = true;
                }
            } else if (node->type != NodeType::Tool) {
                // Conditional / FanOut / FanIn — v1 纯拓扑 no-op。
                WorkflowNodeRunRecord np;
                np.run_id      = run_id;
                np.node_id     = node_id;
                np.attempt     = 1;
                np.status      = WorkflowNodeStatus::Skipped;
                np.ended_at_ms = nowMs();
                state_store_->upsertNodeRun(np);
                advance = true;
            } else {
                // Tool node — retry loop + 每 attempt 超时守卫（SR-5）。
                int max_attempts = std::max(1, node->retry.max_attempts);
                int node_timeout = node->timeout_ms > 0
                                       ? node->timeout_ms
                                       : static_cast<int>(cfg_.default_node_timeout.count());
                bool node_ok = false;
                int  attempt = 0;
                std::string last_error;
                for (attempt = 1; attempt <= max_attempts && !cancelled.load();
                     ++attempt) {
                    WorkflowNodeRunRecord nr;
                    nr.run_id        = run_id;
                    nr.node_id       = node_id;
                    nr.attempt       = attempt;
                    nr.status        = WorkflowNodeStatus::Running;
                    nr.started_at_ms = nowMs();
                    state_store_->upsertNodeRun(nr);

                    nlohmann::json args = node->arguments;
                    args["__run_id"]    = run_id;
                    args["node_id"]     = node_id;

                    aegisgate::ToolExecutionResult sr;
                    if (sandbox_ == nullptr) {
                        // 生产未装配 sandbox → 优雅降级入 DLQ（不崩，见 C8）。
                        sr.status        = aegisgate::ToolExecutionStatus::Error;
                        sr.error_message = "no_tool_sandbox";
                    } else {
                        aegisgate::ToolSandbox* sb = sandbox_;
                        std::string tid = node->tool_id;
                        auto fut = std::async(std::launch::async,
                            [sb, tid, args] { return sb->execute(tid, args); });
                        if (fut.wait_for(std::chrono::milliseconds(node_timeout)) ==
                            std::future_status::timeout) {
                            sr.status        = aegisgate::ToolExecutionStatus::Timeout;
                            sr.error_message = "node_timeout";
                            std::lock_guard<std::mutex> g(sched_mu);
                            detached.push_back(std::move(fut));
                        } else {
                            sr = fut.get();
                        }
                    }

                    if (sr.status == aegisgate::ToolExecutionStatus::Success) {
                        nr.status      = WorkflowNodeStatus::Succeeded;
                        nr.ended_at_ms = nowMs();
                        nr.result_json = sr.output;
                        state_store_->upsertNodeRun(nr);
                        node_ok = true;
                        break;
                    }
                    last_error =
                        sr.error_message.empty() ? "tool_failed" : sr.error_message;
                    nr.status        = WorkflowNodeStatus::Failed;
                    nr.ended_at_ms   = nowMs();
                    nr.error_message = last_error;
                    state_store_->upsertNodeRun(nr);

                    if (attempt < max_attempts && node->retry.backoff_ms > 0) {
                        int delay = node->retry.backoff_ms;
                        if (node->retry.exponential) delay *= (1 << (attempt - 1));
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    }
                }

                if (node_ok) {
                    ++completed;
                    advance = true;
                } else {
                    WorkflowNodeRunRecord dlq;
                    dlq.run_id        = run_id;
                    dlq.node_id       = node_id;
                    dlq.attempt       = std::max(1, attempt - 1);
                    dlq.status        = WorkflowNodeStatus::DeadLetter;
                    dlq.ended_at_ms   = nowMs();
                    dlq.error_message = last_error;
                    state_store_->upsertNodeRun(dlq);
                    ++failed;
                    any_failure.store(true);
                    if (cfg_.stop_on_first_failure) cancelled.store(true);
                }
            }
        }

        // 完成回调：推进后继 + 递减 outstanding（持 sched_mu）。
        std::lock_guard<std::mutex> g(sched_mu);
        if (advance && !cancelled.load()) {
            for (const auto& s : succ[node_id]) {
                if (--in_degree[s] == 0) submitReady(s);
            }
        }
        if (--outstanding == 0) done_cv.notify_all();
    };

    // 初始 dispatch：所有 in_degree==0 节点。
    {
        std::lock_guard<std::mutex> g(sched_mu);
        for (const auto& kv : in_degree) {
            if (kv.second == 0) submitReady(kv.first);
        }
    }
    {
        std::unique_lock<std::mutex> lk(sched_mu);
        done_cv.wait(lk, [&] { return outstanding == 0; });
    }
    pool.shutdown();
    detached.clear();  // 阻塞等待超时 straggler 收敛（正确性优先，v2 优化）

    out.completed_nodes = completed.load();
    out.failed_nodes    = failed.load();

    if (paused.load()) {
        state_store_->transitionRunStatus(
            run_id, WorkflowRunStatus::WaitingForApproval, nowMs());
        out.ok           = false;
        out.final_status = WorkflowRunStatus::WaitingForApproval;
        return out;
    }
    if (any_failure.load()) {
        state_store_->transitionRunStatus(run_id, WorkflowRunStatus::DeadLetter,
                                            nowMs());
        out.ok            = false;
        out.final_status  = WorkflowRunStatus::DeadLetter;
        if (out.error_message.empty()) out.error_message = "node_failed";
        return out;
    }

    state_store_->transitionRunStatus(run_id, WorkflowRunStatus::Succeeded, nowMs());
    out.ok           = true;
    out.final_status = WorkflowRunStatus::Succeeded;
    return out;
}

WorkflowExecutionResult WorkflowEngine::resume(const std::string& run_id) {
    WorkflowExecutionResult out;
    auto run = state_store_->getRun(run_id);
    if (!run) {
        out.error_message = "run_not_found";
        return out;
    }
    if (run->status != WorkflowRunStatus::WaitingForApproval) {
        out.error_message = "run_not_paused";
        out.final_status  = run->status;
        return out;
    }

    // Replay DSL from persisted JSON.
    nlohmann::json dsl_json;
    try {
        dsl_json = nlohmann::json::parse(run->dsl_json);
    } catch (const nlohmann::json::parse_error&) {
        out.error_message = "dsl_corrupt";
        return out;
    }
    auto dsl_opt = fromJson(dsl_json);
    if (!dsl_opt) {
        out.error_message = "dsl_corrupt";
        return out;
    }
    // T01 — confirm canonicalHash matches what was committed at create time.
    if (canonicalHash(*dsl_opt) != run->dsl_hash) {
        out.error_message = "dsl_hash_mismatch";
        state_store_->transitionRunStatus(run_id, WorkflowRunStatus::Failed,
                                            nowMs());
        out.final_status = WorkflowRunStatus::Failed;
        return out;
    }

    // Mark approval node Succeeded and re-execute from the unrun frontier.
    auto nodes = state_store_->listNodeRuns(run_id);
    std::unordered_set<std::string> done;
    for (const auto& n : nodes) {
        if (n.status == WorkflowNodeStatus::Succeeded ||
            n.status == WorkflowNodeStatus::Skipped) {
            done.insert(n.node_id);
        }
        if (n.status == WorkflowNodeStatus::WaitingForApproval) {
            WorkflowNodeRunRecord upd = n;
            upd.status      = WorkflowNodeStatus::Succeeded;
            upd.ended_at_ms = nowMs();
            state_store_->upsertNodeRun(upd);
            done.insert(n.node_id);
        }
    }

    // Walk the DAG and dispatch any node whose deps are all done. We reuse
    // execute() with a synthetic "context" carrying done set via the
    // existing run + skipping already-succeeded nodes: rather than
    // duplicate the loop body we transition the run back to Running and
    // call execute() on a *trimmed* DSL — only nodes whose id is not in
    // `done` remain. Their depends_on filtered to the same shrunken set.
    WorkflowDsl trimmed;
    trimmed.id          = dsl_opt->id;
    trimmed.version     = dsl_opt->version;
    trimmed.metadata    = dsl_opt->metadata;
    for (const auto& n : dsl_opt->nodes) {
        if (done.count(n.id)) continue;
        NodeSpec copy = n;
        std::vector<std::string> kept;
        for (const auto& d : n.depends_on) {
            if (!done.count(d)) kept.push_back(d);
        }
        copy.depends_on = std::move(kept);
        trimmed.nodes.push_back(copy);
    }

    state_store_->transitionRunStatus(run_id, WorkflowRunStatus::Running, nowMs());

    // Re-enter execute() with the trimmed graph but reuse the same run_id.
    // execute() will try to createRun() and find it exists — that's fine,
    // the call returns false and we proceed. We pass an empty context: the
    // original context is already persisted on the run record.
    return execute(trimmed, run_id, nlohmann::json::parse(run->context_json));
}

} // namespace aegisgate::workflow
