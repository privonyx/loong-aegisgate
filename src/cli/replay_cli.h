#pragma once

// Phase 11.2 TASK-20260521-03 — aegisctl replay-routes CLI subcommand.
//
// Offline router-comparison replay: reads an audit-log JSONL file, masks
// PII (SR6), and runs each entry through baseline + new routers (dry-run)
// for comparison before promoting a routing strategy via
// BanditAutonomyApplier (Epic 5).
//
// This is DISTINCT from the existing `aegisctl replay` (live HTTP replay
// against the running gateway). The live replay sends raw bodies to the
// gateway which then runs PIIFilter as a pipeline stage; the offline
// path here MUST mask before invoking routers because no pipeline runs
// (SR6 fail-closed).

#include <iosfwd>
#include <string>
#include <vector>

namespace aegisgate::cli {

struct ReplayRoutesArgs {
    std::string audit_log_path;
    std::string baseline_strategy = "hybrid";       // default MultiObjective hybrid
    std::string new_strategy      = "cost-first";   // candidate to compare
    int         limit             = 100;
    bool        dry_run           = true;          // v1: always dry_run
    bool        json_output       = true;
};

// Parses argv (without the leading "replay-routes" token) into args.
// Throws std::invalid_argument on unrecognized flags.  Missing required
// flags are surfaced through runReplayRoutesCommand exit code 2.
ReplayRoutesArgs parseReplayRoutesArgs(const std::vector<std::string>& argv);

// Returns process exit code: 0 success / 1 runtime error / 2 usage error.
int runReplayRoutesCommand(const std::vector<std::string>& argv,
                            std::ostream& out, std::ostream& err);

}  // namespace aegisgate::cli
