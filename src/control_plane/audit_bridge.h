#pragma once

// Phase 9.3 Epic 4 Task 4.2 — AuditBridge.
//
// Thin adapter that translates control-plane state transitions into the
// shared AuditLogger chain (C3 design decision: option A, single chain for
// gateway + control plane). Every record* call emits exactly one AuditEntry
// with:
//   * stage_name = "control_plane"
//   * tenant_id  = "system"
//   * request_id = fresh ULID (so log indexing stays unique even when several
//                  control-plane actions share the same gRPC call).
//   * detail     = nlohmann::json dumped (no yaml_content ever — SR11).
//
// AuditBridge is deliberately passive: it does NOT decide whether a state
// transition is legal. ConfigServiceCore already enforces SR2/3/4/5/9/10/11
// before invoking the bridge on the success path.

#include "control_plane/ulid.h"

#include <string>

namespace aegisgate {

class AuditLogger;
struct ConfigVersionRecord;

class AuditBridge {
public:
    // `audit` may be nullptr in which case every record* is a no-op. This
    // keeps synthetic harnesses (benches, unit tests that don't care about
    // audit) from needing to construct a real logger.
    explicit AuditBridge(AuditLogger* audit);

    // Called after ConfigServiceCore::submit persists a PENDING record.
    void recordSubmit(const ConfigVersionRecord& rec);

    // Called after a successful W3 review transition.
    void recordApprove(const std::string& version_id,
                       const std::string& reviewer,
                       const std::string& comment);
    void recordReject(const std::string& version_id,
                      const std::string& reviewer,
                      const std::string& comment);

    // Called after a successful atomic activation. `previous_active` is the
    // version_id that moved to SUPERSEDED, or "" if there was no prior active.
    void recordActivate(const std::string& version_id,
                        const std::string& activator,
                        const std::string& previous_active);

    // Called after a successful rollback (R2 path). `target_id` is the one
    // being activated; `previous_active` is what it supersedes.
    void recordRollback(const std::string& target_id,
                        const std::string& actor,
                        const std::string& previous_active,
                        const std::string& comment);

private:
    AuditLogger* audit_;
    Ulid         request_id_gen_;
};

} // namespace aegisgate
