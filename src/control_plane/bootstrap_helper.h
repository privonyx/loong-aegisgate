#pragma once

// Phase 9.3 Epic 6 Task 6.3 — Bootstrap helper (Q5).
//
// Extracted from main.cpp so we can unit-test the empty-table path without
// spinning up a gRPC server. The helper performs a guarded Submit + Approve
// + Activate triple using the `system_bootstrap` actor — this is the ONLY
// place in the system where submitter == reviewer is allowed, and it is
// hard-gated by a "table must be empty" precondition so the bypass can
// never be triggered twice or against a live deployment's history.

#include <string>

namespace aegisgate {

class ConfigServiceCore;
class PersistentStore;

namespace control_plane::bootstrap {

enum class Outcome {
    SkippedNotEmpty,    // config_versions already has rows — no-op
    SkippedNoBootstrap, // bootstrap_from_active_yaml key is empty
    FileReadFailed,     // path given but file missing / unreadable
    SubmitFailed,       // ConfigServiceCore.submit rejected (validation etc.)
    ApproveFailed,      // submit ok but approve rejected
    ActivateFailed,     // approve ok but activate rejected
    Bootstrapped,       // all three RPCs succeeded; config_versions now has 1 ACTIVE
};

struct Result {
    Outcome outcome = Outcome::SkippedNotEmpty;
    std::string version_id;   // populated on Bootstrapped
    std::string error_code;   // populated on any *Failed outcome
    std::string error_message;
};

// Runs the empty-table bootstrap sequence. Safe to call on every start —
// if the table is non-empty, returns `SkippedNotEmpty` without reading the
// filesystem. The file read is guarded by `core`'s 1 MiB SR2 cap: if the
// file is larger, the Submit path naturally returns PAYLOAD_TOO_LARGE.
//
// `store` and `core` must not be null. `bootstrap_yaml_path` may be empty
// (→ SkippedNoBootstrap).
Result bootstrapFromActiveYamlIfEmpty(
    PersistentStore* store,
    ConfigServiceCore* core,
    const std::string& bootstrap_yaml_path);

} // namespace control_plane::bootstrap
} // namespace aegisgate
