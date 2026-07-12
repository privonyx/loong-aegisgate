#pragma once

// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.5.
//
// Audit adapter for rollout lifecycle events. Mirrors the existing
// AuditBridge pattern (Phase 9.3 Epic 4): takes an AuditLogger*, writes
// one AuditEntry per call into the shared control-plane audit chain,
// with stage_name="control_plane", tenant_id="system", and a fresh
// ULID request_id. yaml_content / RolloutSpec bodies are never written
// (SR14) — only the minimal identifying fields needed for traceability.
//
// Action strings (stable, wire-level identifiers):
//   rollout.created
//   rollout.started
//   rollout.stage_promoted
//   rollout.paused_manual
//   rollout.paused_auto
//   rollout.resumed
//   rollout.auto_rollback_triggered
//   rollout.auto_rollback_completed
//   rollout.aborted
//   rollout.completed
//   rollout.failed

#include "control_plane/rollout/rollout_record.h"
#include "control_plane/ulid.h"

#include <string>

namespace aegisgate {

class AuditLogger;

class RolloutAuditBridge {
public:
    // `audit` may be nullptr; every record* becomes a no-op in that case.
    explicit RolloutAuditBridge(AuditLogger* audit);

    void recordCreated(const RolloutRecord& r, const std::string& actor);
    void recordStarted(const RolloutRecord& r, const std::string& actor);
    void recordStagePromoted(const RolloutRecord& r,
                              int from_stage_index,
                              int to_stage_index,
                              const std::string& actor);
    void recordPausedManual(const RolloutRecord& r,
                             const std::string& actor,
                             const std::string& comment);
    void recordPausedAuto(const RolloutRecord& r,
                           PauseReason reason,
                           const std::string& detail);
    void recordResumed(const RolloutRecord& r, const std::string& actor);
    void recordAutoRollbackTriggered(const RolloutRecord& r,
                                      const std::string& detail);
    void recordAutoRollbackCompleted(const RolloutRecord& r,
                                      const std::string& new_active_version_id);
    void recordAborted(const RolloutRecord& r, const std::string& actor);
    void recordCompleted(const RolloutRecord& r,
                          const std::string& new_active_version_id);
    void recordFailed(const RolloutRecord& r, const std::string& reason);

private:
    AuditLogger* audit_;
    Ulid         request_id_gen_;
};

} // namespace aegisgate
