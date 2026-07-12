#include "control_plane/rollout/rollout_controller.h"

#include "control_plane/config_service_core.h"
#include "control_plane/config_version_record.h"
#include "control_plane/rollout/rollout_audit_bridge.h"
#include "control_plane/rollout/rollout_metrics_provider.h"
#include "control_plane/rollout/rollout_state_machine.h"
#include "storage/persistent_store.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <exception>

namespace aegisgate {

namespace {

constexpr const char* kErrInvalidArgument     = "INVALID_ARGUMENT";
constexpr const char* kErrNotFound            = "NOT_FOUND";
constexpr const char* kErrFailedPrecondition  = "FAILED_PRECONDITION";
constexpr const char* kErrResourceExhausted   = "RESOURCE_EXHAUSTED";
constexpr const char* kErrAlreadyExists       = "ALREADY_EXISTS";
constexpr const char* kErrInternal            = "INTERNAL";

RolloutController::Result makeError(const std::string& code,
                                     const std::string& msg) {
    RolloutController::Result r;
    r.error_code = code;
    r.error_message = msg;
    return r;
}

} // namespace

RolloutController::RolloutController(Deps d) : deps_(std::move(d)) {}

std::int64_t RolloutController::wallNow() const {
    if (deps_.clock != nullptr) return deps_.clock->wallClockMillis();
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::int64_t RolloutController::steadyNow() const {
    if (deps_.clock != nullptr) return deps_.clock->nowMillis();
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

bool RolloutController::loadRollout(const std::string& rollout_id, Result& out) {
    if (rollout_id.empty()) {
        out = makeError(kErrInvalidArgument, "rollout_id is required");
        return false;
    }
    if (deps_.store == nullptr) {
        out = makeError(kErrInternal, "store dep is null");
        return false;
    }
    auto maybe = deps_.store->getRollout(rollout_id);
    if (!maybe.has_value()) {
        out = makeError(kErrNotFound, "rollout not found: " + rollout_id);
        return false;
    }
    out.record = std::move(*maybe);
    return true;
}

// --- B.6.a: create / start / get / list ------------------------------------

RolloutController::Result
RolloutController::createRollout(const RolloutSpec& spec,
                                  const std::string& creator) {
    if (deps_.store == nullptr || deps_.config_core == nullptr) {
        return makeError(kErrInternal, "store/config_core deps required");
    }
    if (spec.target_version_id.empty()) {
        return makeError(kErrInvalidArgument, "target_version_id is required");
    }
    if (spec.stages.empty()) {
        return makeError(kErrInvalidArgument, "at least one stage is required");
    }
    for (std::size_t i = 0; i < spec.stages.size(); ++i) {
        int p = spec.stages[i].scope.percentage;
        if (p < 0 || p > 100) {
            return makeError(kErrInvalidArgument,
                              "stage " + std::to_string(i) +
                                  ": percentage must be in [0,100]");
        }
    }
    if (creator.empty()) {
        return makeError(kErrInvalidArgument, "creator is required");
    }

    // SR16: tenant quota.
    if (deps_.check_quota && !deps_.check_quota(creator)) {
        return makeError(kErrResourceExhausted,
                          "tenant rollout quota exhausted: " + creator);
    }

    // Verify target version exists and is APPROVED.
    auto v = deps_.config_core->getVersion(spec.target_version_id);
    if (!v.has_value()) {
        return makeError(kErrNotFound,
                          "target_version_id not found: " + spec.target_version_id);
    }
    if (v->status != ConfigStatus::APPROVED) {
        return makeError(kErrFailedPrecondition,
                          "target_version_id is not APPROVED");
    }

    // Enforce "at most one active rollout per target" here (store will also
    // enforce at schema level, but we return a friendlier code).
    auto existing_active = deps_.store->findActiveRolloutByTarget(spec.target_version_id);
    if (existing_active.has_value()) {
        return makeError(kErrAlreadyExists,
                          "active rollout already exists for target: " +
                              spec.target_version_id);
    }

    RolloutRecord rec;
    rec.rollout_id = owned_id_gen_.generate();
    rec.target_version_id = spec.target_version_id;
    rec.spec = spec;
    rec.creator = creator;
    rec.status = RolloutStatus::PENDING;
    rec.current_stage_index = 0;
    rec.last_actor = creator;

    if (!deps_.store->insertRollout(rec)) {
        return makeError(kErrInternal, "insertRollout failed");
    }
    if (deps_.audit != nullptr) {
        deps_.audit->recordCreated(rec, creator);
    }

    Result r;
    r.record = std::move(rec);
    return r;
}

RolloutController::Result
RolloutController::startRollout(const std::string& rollout_id,
                                 const std::string& actor) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;

    auto next = attemptRolloutTransition({r.record.status, RolloutAction::Start, false});
    if (!next.has_value()) {
        return makeError(kErrFailedPrecondition,
                          "cannot start rollout in state " +
                              std::string(rolloutStatusToString(r.record.status)));
    }

    // Capture current active version for later rollback/abort paths.
    auto active = deps_.config_core != nullptr ? deps_.config_core->getActive()
                                                : std::optional<ConfigVersionRecord>{};
    const std::int64_t now = wallNow();
    r.record.status = *next;
    r.record.started_at = now;
    r.record.stage_started_at = now;
    r.record.previous_active_version_id = active ? active->version_id : std::string{};
    r.record.last_actor = actor;

    if (!deps_.store->updateRollout(r.record)) {
        return makeError(kErrInternal, "updateRollout failed");
    }
    if (deps_.audit != nullptr) deps_.audit->recordStarted(r.record, actor);
    return r;
}

std::optional<RolloutRecord>
RolloutController::getRollout(const std::string& rollout_id) {
    if (deps_.store == nullptr) return std::nullopt;
    return deps_.store->getRollout(rollout_id);
}

std::vector<RolloutRecord>
RolloutController::listRollouts(const RolloutQuery& q) {
    if (deps_.store == nullptr) return {};
    return deps_.store->listRollouts(q);
}

// --- B.6.b: promote / pause / resume / abort -------------------------------

RolloutController::Result
RolloutController::promoteStage(const std::string& rollout_id,
                                 const std::string& actor) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;

    const int stage_count = static_cast<int>(r.record.spec.stages.size());
    const bool is_last = (r.record.current_stage_index + 1) >= stage_count;

    auto next = attemptRolloutTransition(
        {r.record.status, RolloutAction::Promote, is_last});
    if (!next.has_value()) {
        return makeError(kErrFailedPrecondition,
                          "cannot promote in state " +
                              std::string(rolloutStatusToString(r.record.status)));
    }

    const std::int64_t now = wallNow();
    const int from_idx = r.record.current_stage_index;

    if (is_last) {
        // Final stage — activate target version via ConfigServiceCore.
        if (deps_.config_core != nullptr) {
            auto mut = deps_.config_core->activate(r.record.target_version_id, actor);
            if (!mut.error_code.empty()) {
                // Activation failed; transition rollout to FAILED via state machine.
                auto fail_next = attemptRolloutTransition(
                    {r.record.status, RolloutAction::Fail, false});
                if (fail_next.has_value()) {
                    r.record.status = *fail_next;
                    r.record.completed_at = now;
                    deps_.store->updateRollout(r.record);
                    if (deps_.audit != nullptr) {
                        deps_.audit->recordFailed(
                            r.record,
                            "activate_failed: " + mut.error_code + "/" + mut.error_message);
                    }
                }
                return makeError(kErrInternal,
                                  "activate failed: " + mut.error_message);
            }
        }
        r.record.status = *next;  // COMPLETED
        r.record.completed_at = now;
        r.record.last_actor = actor;
        if (!deps_.store->updateRollout(r.record)) {
            // C7 (D1=C)：activate 已成功（config 已生效），仅 rollout 元数据未落库。
            // 不谎报 COMPLETED；onTick 将幂等对账补齐终态。
            return makeError(kErrInternal,
                             "config activated but rollout status persist failed; "
                             "pending onTick reconcile");
        }
        if (deps_.audit != nullptr) {
            deps_.audit->recordCompleted(r.record, r.record.target_version_id);
        }
    } else {
        r.record.status = *next;  // PROGRESSING
        r.record.current_stage_index = from_idx + 1;
        r.record.stage_started_at = now;
        r.record.last_actor = actor;
        if (!deps_.store->updateRollout(r.record)) {
            return makeError(kErrInternal, "updateRollout failed");
        }
        if (deps_.audit != nullptr) {
            deps_.audit->recordStagePromoted(r.record, from_idx,
                                              r.record.current_stage_index, actor);
        }
    }
    return r;
}

RolloutController::Result
RolloutController::pauseRollout(const std::string& rollout_id,
                                 const std::string& actor,
                                 const std::string& comment) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;

    auto next = attemptRolloutTransition(
        {r.record.status, RolloutAction::PauseManual, false});
    if (!next.has_value()) {
        return makeError(kErrFailedPrecondition,
                          "cannot pause in state " +
                              std::string(rolloutStatusToString(r.record.status)));
    }
    const std::int64_t now = wallNow();
    r.record.status = *next;
    r.record.paused_at = now;
    r.record.pause_reason = PauseReason::MANUAL;
    r.record.pause_detail = comment;
    r.record.last_actor = actor;
    if (!deps_.store->updateRollout(r.record)) {
        return makeError(kErrInternal, "updateRollout failed");
    }
    if (deps_.audit != nullptr) {
        deps_.audit->recordPausedManual(r.record, actor, comment);
    }
    return r;
}

RolloutController::Result
RolloutController::resumeRollout(const std::string& rollout_id,
                                  const std::string& actor) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;

    auto next = attemptRolloutTransition(
        {r.record.status, RolloutAction::Resume, false});
    if (!next.has_value()) {
        return makeError(kErrFailedPrecondition,
                          "cannot resume in state " +
                              std::string(rolloutStatusToString(r.record.status)));
    }
    const std::int64_t now = wallNow();
    r.record.status = *next;
    // NOTE: paused_at is intentionally NOT cleared — it doubles as the
    // cooldown anchor for evaluateStage's auto-pause debounce.
    r.record.pause_reason = PauseReason::UNSPECIFIED;
    r.record.pause_detail.clear();
    r.record.stage_started_at = now;  // fresh observation window
    r.record.last_actor = actor;
    if (!deps_.store->updateRollout(r.record)) {
        return makeError(kErrInternal, "updateRollout failed");
    }
    if (deps_.audit != nullptr) deps_.audit->recordResumed(r.record, actor);
    return r;
}

RolloutController::Result
RolloutController::abortRollout(const std::string& rollout_id,
                                 const std::string& actor) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;

    const bool was_live = r.record.status == RolloutStatus::PROGRESSING ||
                          r.record.status == RolloutStatus::PAUSED;

    auto next = attemptRolloutTransition(
        {r.record.status, RolloutAction::Abort, false});
    if (!next.has_value()) {
        return makeError(kErrFailedPrecondition,
                          "cannot abort in state " +
                              std::string(rolloutStatusToString(r.record.status)));
    }
    r.record.status = *next;
    r.record.completed_at = wallNow();
    r.record.last_actor = actor;
    if (!deps_.store->updateRollout(r.record)) {
        return makeError(kErrInternal, "updateRollout failed");
    }
    if (deps_.audit != nullptr) deps_.audit->recordAborted(r.record, actor);

    // Restore previous active version if we had started the rollout.
    if (was_live && deps_.config_core != nullptr &&
        !r.record.previous_active_version_id.empty()) {
        auto mut = deps_.config_core->rollback(
            r.record.previous_active_version_id, actor,
            "rollout aborted: " + r.record.rollout_id,
            /*emergency=*/false);
        if (!mut.error_code.empty() && deps_.audit != nullptr) {
            deps_.audit->recordFailed(
                r.record,
                "abort_rollback_failed: " + mut.error_code + "/" + mut.error_message);
        }
    }
    return r;
}

// --- B.6.c + d: evaluate stage / auto-rollback -----------------------------

RolloutController::Result
RolloutController::evaluateStage(const std::string& rollout_id,
                                  std::int64_t /*steady_now_ms*/,
                                  std::int64_t wall_now_ms) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;
    if (r.record.status != RolloutStatus::PROGRESSING) return r;  // NOP
    if (r.record.spec.stages.empty()) return r;
    if (r.record.current_stage_index < 0 ||
        r.record.current_stage_index >= static_cast<int>(r.record.spec.stages.size())) {
        return r;
    }

    // Cooldown: if we recently auto-paused, back off.
    if (r.record.paused_at > 0 &&
        (wall_now_ms - r.record.paused_at) < deps_.pause_cooldown_ms) {
        return r;  // cooldown in effect
    }

    const auto& stage = r.record.spec.stages[r.record.current_stage_index];
    const auto& obs = stage.observation;
    const auto& ap  = stage.auto_pause;

    const std::int64_t elapsed_ms = wall_now_ms - r.record.stage_started_at;
    if (elapsed_ms < std::int64_t(obs.min_duration_seconds) * 1000) return r;

    if (deps_.metrics == nullptr) return r;
    const auto window = std::chrono::seconds(
        std::max(1, obs.min_duration_seconds > 0 ? obs.min_duration_seconds : 60));
    auto target_metrics = deps_.metrics->forVersion(r.record.target_version_id, window);
    if (target_metrics.sample_count < obs.min_sample_count) return r;

    // Baseline metrics (previous_active) for relative thresholds.
    VersionMetrics baseline;
    if (!r.record.previous_active_version_id.empty()) {
        baseline = deps_.metrics->forVersion(
            r.record.previous_active_version_id, window);
    }

    PauseReason breach_reason = PauseReason::UNSPECIFIED;
    std::string detail;

    // Absolute safety-net first (if baseline is itself broken, relative
    // ratios become meaningless — fall back to absolute).
    const bool baseline_unhealthy =
        baseline.sample_count > 0 && baseline.error_rate > 0.10;

    if (ap.absolute_error_rate_gt > 0.0 &&
        target_metrics.error_rate > ap.absolute_error_rate_gt) {
        breach_reason = PauseReason::ERROR_RATE;
        detail = "absolute_error_rate=" + std::to_string(target_metrics.error_rate) +
                 " > " + std::to_string(ap.absolute_error_rate_gt);
    } else if (ap.absolute_p99_latency_ms_gt > 0.0 &&
               target_metrics.p99_latency_ms > ap.absolute_p99_latency_ms_gt) {
        breach_reason = PauseReason::LATENCY_RATIO;
        detail = "absolute_p99=" + std::to_string(target_metrics.p99_latency_ms) +
                 " > " + std::to_string(ap.absolute_p99_latency_ms_gt);
    } else if (!baseline_unhealthy && ap.error_rate_gt > 0.0 &&
               baseline.sample_count > 0 &&
               (target_metrics.error_rate - baseline.error_rate) > ap.error_rate_gt) {
        breach_reason = PauseReason::ERROR_RATE;
        detail = "error_rate_delta=" +
                 std::to_string(target_metrics.error_rate - baseline.error_rate) +
                 " > " + std::to_string(ap.error_rate_gt);
    } else if (!baseline_unhealthy && ap.p99_latency_ratio_gt > 0.0 &&
               baseline.p99_latency_ms > 0.0 &&
               (target_metrics.p99_latency_ms / baseline.p99_latency_ms) >
                   ap.p99_latency_ratio_gt) {
        breach_reason = PauseReason::LATENCY_RATIO;
        detail = "latency_ratio=" +
                 std::to_string(target_metrics.p99_latency_ms / baseline.p99_latency_ms) +
                 " > " + std::to_string(ap.p99_latency_ratio_gt);
    }

    if (breach_reason == PauseReason::UNSPECIFIED) return r;  // all clear

    auto next = attemptRolloutTransition(
        {r.record.status, RolloutAction::PauseAuto, false});
    if (!next.has_value()) return r;

    r.record.status = *next;
    r.record.paused_at = wall_now_ms;
    r.record.pause_reason = breach_reason;
    r.record.pause_detail = detail;
    if (!deps_.store->updateRollout(r.record)) {
        return makeError(kErrInternal, "updateRollout failed");
    }
    if (deps_.audit != nullptr) {
        deps_.audit->recordPausedAuto(r.record, breach_reason, detail);
    }
    return r;
}

RolloutController::Result
RolloutController::evaluateAutoRollback(const std::string& rollout_id,
                                         std::int64_t /*steady_now_ms*/,
                                         std::int64_t wall_now_ms) {
    Result r;
    if (!loadRollout(rollout_id, r)) return r;
    if (r.record.status != RolloutStatus::PAUSED) return r;  // NOP

    // SR17: env-var / caller-provided kill switch.
    if (deps_.auto_rollback_enabled && !deps_.auto_rollback_enabled()) return r;

    // spec opts out of auto rollback.
    if (!r.record.spec.auto_rollback_on_pause) return r;

    const std::int64_t grace_ms =
        std::int64_t(r.record.spec.auto_rollback_grace_seconds) * 1000;
    if ((wall_now_ms - r.record.paused_at) < grace_ms) return r;  // grace pending

    // Audit the trigger before the state flip so operators see intent.
    if (deps_.audit != nullptr) {
        deps_.audit->recordAutoRollbackTriggered(
            r.record, "grace_seconds=" +
                           std::to_string(r.record.spec.auto_rollback_grace_seconds) +
                           " exceeded; reason=" +
                           std::string(pauseReasonToString(r.record.pause_reason)));
    }

    // State machine: PAUSED + AutoRollback → FAILED.
    auto next = attemptRolloutTransition(
        {r.record.status, RolloutAction::AutoRollback, false});
    if (!next.has_value()) return r;

    r.record.status = *next;  // FAILED
    r.record.pause_reason = PauseReason::AUTO_ROLLBACK;
    r.record.completed_at = wall_now_ms;

    // Attempt rollback via ConfigServiceCore, impersonating the reserved
    // system user so audit reflects machine-initiated action (SR13).
    bool rollback_ok = true;
    std::string rollback_err;
    if (deps_.config_core != nullptr &&
        !r.record.previous_active_version_id.empty()) {
        auto mut = deps_.config_core->rollback(
            r.record.previous_active_version_id,
            deps_.system_user_id,
            "auto_rollback: grace_expired",
            /*emergency=*/false);
        if (!mut.error_code.empty()) {
            rollback_ok = false;
            rollback_err = mut.error_code + "/" + mut.error_message;
        }
    }

    if (!deps_.store->updateRollout(r.record)) {
        return makeError(kErrInternal, "updateRollout failed");
    }
    if (deps_.audit != nullptr) {
        if (rollback_ok) {
            deps_.audit->recordAutoRollbackCompleted(
                r.record, r.record.previous_active_version_id);
        } else {
            deps_.audit->recordFailed(
                r.record, "auto_rollback_failed: " + rollback_err);
        }
    }
    return r;
}

bool RolloutController::reconcileActivatedRollout(const RolloutRecord& rec,
                                                   std::int64_t wall_now_ms) {
    if (deps_.config_core == nullptr) return false;
    auto active = deps_.config_core->getActive();
    if (!active.has_value() || active->version_id != rec.target_version_id) {
        return false;
    }
    // config 活跃版本已是 target → promote 的 activate 已生效，rollout 未终态是
    // updateRollout 失败遗留。通过状态机补齐 COMPLETED（等价 last-stage promote）。
    auto next = attemptRolloutTransition(
        {rec.status, RolloutAction::Promote, /*is_last_stage=*/true});
    if (!next.has_value()) return false;

    RolloutRecord updated = rec;
    updated.status = *next;  // COMPLETED
    updated.completed_at = wall_now_ms;
    if (!deps_.store->updateRollout(updated)) return false;
    if (deps_.audit != nullptr) {
        deps_.audit->recordCompleted(updated, updated.target_version_id);
    }
    spdlog::info("rollout-controller: reconciled {} to COMPLETED "
                 "(config already active at target {})",
                 rec.rollout_id, rec.target_version_id);
    return true;
}

void RolloutController::onTick(std::int64_t steady_now_ms,
                                std::int64_t wall_now_ms) {
    if (deps_.store == nullptr) return;
    RolloutQuery q;
    q.statuses = {RolloutStatus::PROGRESSING, RolloutStatus::PAUSED};
    q.limit = 500;
    auto rollouts = deps_.store->listRollouts(q);
    for (const auto& rec : rollouts) {
        try {
            if (rec.status == RolloutStatus::PROGRESSING) {
                // C7 (TASK-20260703-02 / D1=C)：promote 的 activate 成功但
                // updateRollout 失败 → config 已达 target 而 rollout 仍 PROGRESSING。
                // 幂等对账：config 活跃版本 == target_version_id 时补齐 COMPLETED。
                // activate 幂等、config 已实际生效，此处只补元数据终态。
                if (reconcileActivatedRollout(rec, wall_now_ms)) {
                    continue;
                }
                evaluateStage(rec.rollout_id, steady_now_ms, wall_now_ms);
            } else if (rec.status == RolloutStatus::PAUSED) {
                evaluateAutoRollback(rec.rollout_id, steady_now_ms, wall_now_ms);
            }
        } catch (const std::exception& e) {
            spdlog::error("rollout-controller: evaluate {} failed: {}",
                            rec.rollout_id, e.what());
        } catch (...) {
            spdlog::error("rollout-controller: evaluate {} unknown failure",
                            rec.rollout_id);
        }
    }
}

} // namespace aegisgate
