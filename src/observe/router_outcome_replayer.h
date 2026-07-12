#pragma once

// Phase 11.2 TASK-20260521-03 — RouterOutcomeReplayer.
//
// Reads an audit-log JSONL file (output of AuditLogger), pulls out each
// "chat_request" entry, masks PII via PIIFilter (SR6), and runs the entry
// through both baseline + new routers (dry-run) so callers can compare
// routing decisions before promoting a candidate strategy.
//
// Threading: single-threaded by design (offline tool). Callers MUST NOT
// share an instance across threads.
//
// SR6 invariants:
//   1. Every entry's request body flows through PIIFilter::mask() BEFORE
//      being passed to either router.
//   2. ReplayResult.total_pii_masked == ReplayResult.total_replayed
//      (counter is incremented EXACTLY once per masked replay).

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace aegisgate {

class Router;
class PIIFilter;
class ConnectorRegistry;

struct ReplayConfig {
    std::string audit_log_path;
    int limit = 100;
    bool dry_run = true;
    // v2 留作业: --include-raw flag for SuperAdmin (skip masking) is
    // intentionally NOT supported in v1 (SR6 fail-closed).
};

struct ReplayResult {
    int total_entries_read = 0;
    int total_replayed = 0;
    int total_skipped_invalid = 0;
    int total_pii_masked = 0;  // SR6: must equal total_replayed
    nlohmann::json strategy_comparison;  // baseline vs new outcome map
};

class RouterOutcomeReplayer {
public:
    RouterOutcomeReplayer(std::shared_ptr<Router> baseline_router,
                           std::shared_ptr<Router> new_router,
                           std::shared_ptr<PIIFilter> pii_filter);

    ReplayResult replay(const ReplayConfig& cfg,
                          const ConnectorRegistry& registry);

private:
    std::shared_ptr<Router> baseline_router_;
    std::shared_ptr<Router> new_router_;
    std::shared_ptr<PIIFilter> pii_filter_;
};

}  // namespace aegisgate
