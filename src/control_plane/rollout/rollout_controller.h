#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.6.
//
// Orchestrates the complete rollout lifecycle:
//   create → start → (promote* | pause | resume | abort)* → complete | fail
//
// The controller implements RolloutTickHandler so a single RolloutTicker can
// drive evaluateStage + evaluateAutoRollback on every active rollout.
//
// Dependency surface (all pointer-injected, no singletons):
//   * PersistentStore        — persistence of RolloutRecord + events.
//   * ConfigServiceCore      — used to activate on last-stage promotion,
//                               and to rollback on auto-rollback / abort.
//   * RolloutMetricsProvider — per-version metrics window (CR1).
//   * RolloutAuditBridge     — 11-action audit chain (SR14).
//   * common::Clock          — steady_ms (deltas) + wall_ms (persisted).
//   * check_quota            — SR16 tenant quota hook (optional).
//   * auto_rollback_enabled  — SR17 env var switch (optional).
//
// Error code discipline: every mutating call returns a Result carrying an
// error_code string that maps 1:1 to the gRPC status code the adapter will
// translate to ("NOT_FOUND" / "FAILED_PRECONDITION" / "INVALID_ARGUMENT" /
// "ALREADY_EXISTS" / "RESOURCE_EXHAUSTED" / "INTERNAL").

#include "control_plane/rollout/rollout_record.h"
#include "control_plane/rollout/rollout_ticker.h"
#include "control_plane/ulid.h"
#include "common/clock.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

class PersistentStore;
class ConfigServiceCore;
class RolloutMetricsProvider;
class RolloutAuditBridge;

class RolloutController : public RolloutTickHandler {
public:
    struct Deps {
        PersistentStore*        store = nullptr;
        ConfigServiceCore*      config_core = nullptr;
        RolloutMetricsProvider* metrics = nullptr;
        RolloutAuditBridge*     audit = nullptr;
        common::Clock*          clock = nullptr;

        // SR16: returns true if tenant may create a new rollout. nullptr
        // means no quota gate (legacy / test harness behavior).
        std::function<bool(const std::string& /*tenant_id*/)> check_quota;
        // SR17: returns true when auto_rollback is permitted. nullptr means
        // auto_rollback remains enabled (default-on).
        std::function<bool()> auto_rollback_enabled;

        // Optional cooldown between auto-pause events for one rollout.
        // Default 5 minutes (creative doc §debounce).
        std::int64_t pause_cooldown_ms = 5 * 60 * 1000;

        // The virtual user recorded as activator on auto-rollback.
        std::string system_user_id = "system.autorollback";
    };

    struct Result {
        RolloutRecord record;
        std::string   error_code;    // "" on success
        std::string   error_message;
    };

    explicit RolloutController(Deps d);
    ~RolloutController() override = default;

    // --- Lifecycle (B.6.a + B.6.b) ----------------------------------------

    Result createRollout(const RolloutSpec& spec,
                          const std::string& creator);
    Result startRollout(const std::string& rollout_id,
                         const std::string& actor);
    Result promoteStage(const std::string& rollout_id,
                         const std::string& actor);
    Result pauseRollout(const std::string& rollout_id,
                         const std::string& actor,
                         const std::string& comment);
    Result resumeRollout(const std::string& rollout_id,
                          const std::string& actor);
    Result abortRollout(const std::string& rollout_id,
                         const std::string& actor);

    std::optional<RolloutRecord> getRollout(const std::string& rollout_id);
    std::vector<RolloutRecord>   listRollouts(const RolloutQuery& q);

    // --- Automatic evaluation (B.6.c + B.6.d) -----------------------------

    // Evaluate one rollout at the given steady/wall time. Used by onTick().
    // Exposed for deterministic testing via FakeClock + direct call.
    Result evaluateStage(const std::string& rollout_id,
                          std::int64_t steady_now_ms,
                          std::int64_t wall_now_ms);
    Result evaluateAutoRollback(const std::string& rollout_id,
                                 std::int64_t steady_now_ms,
                                 std::int64_t wall_now_ms);

    // RolloutTickHandler impl — enumerates active rollouts and runs both
    // evaluators with per-rollout exception isolation.
    void onTick(std::int64_t steady_now_ms,
                 std::int64_t wall_now_ms) override;

private:
    // Returns pointer-stable copy of the record, or sets error_code.
    bool loadRollout(const std::string& rollout_id, Result& out);

    // TASK-20260703-02 C7 (D1=C)：promote 非原子对账。config 活跃版本已达 rollout
    // 的 target 但 rollout 仍 PROGRESSING（activate 成功 / updateRollout 失败遗留）
    // → 幂等补齐 COMPLETED。返回 true 表示已对账（本 tick 跳过常规 evaluateStage）。
    bool reconcileActivatedRollout(const RolloutRecord& rec,
                                    std::int64_t wall_now_ms);

    Deps deps_;
    Ulid owned_id_gen_;

    // Internal helpers.
    std::int64_t wallNow() const;
    std::int64_t steadyNow() const;
};

} // namespace aegisgate
