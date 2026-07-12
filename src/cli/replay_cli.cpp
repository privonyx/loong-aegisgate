#include "cli/replay_cli.h"

#include "observe/router_outcome_replayer.h"
#include "gateway/multi_objective_router.h"
#include "gateway/routing_strategy_catalog.h"
#include "gateway/connector/registry.h"
#include "guardrail/inbound/pii_filter.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace aegisgate::cli {

namespace {

std::shared_ptr<MultiObjectiveRouter> buildRouter(
    const std::string& strategy_name,
    const RoutingStrategyCatalog& catalog) {
    auto router = std::make_shared<MultiObjectiveRouter>();
    auto strat = catalog.get(strategy_name);
    if (strat.has_value() && strategy_name != "hybrid") {
        router->setActiveStrategy(strat->name, strat->weights);
    }
    return router;
}

}  // namespace

ReplayRoutesArgs parseReplayRoutesArgs(const std::vector<std::string>& argv) {
    ReplayRoutesArgs args;
    for (size_t i = 0; i < argv.size(); ++i) {
        const auto& t = argv[i];
        if (t == "--audit-log" && i + 1 < argv.size()) {
            args.audit_log_path = argv[++i];
        } else if (t == "--baseline" && i + 1 < argv.size()) {
            args.baseline_strategy = argv[++i];
        } else if (t == "--new" && i + 1 < argv.size()) {
            args.new_strategy = argv[++i];
        } else if (t == "--limit" && i + 1 < argv.size()) {
            args.limit = std::stoi(argv[++i]);
        } else if (t == "--dry-run") {
            args.dry_run = true;
        } else if (t == "--json") {
            args.json_output = true;
        } else if (t == "--help" || t == "-h") {
            // help flag tolerated; runReplayRoutesCommand renders usage.
        } else {
            throw std::invalid_argument(
                "replay-routes: unrecognized argument: " + t);
        }
    }
    return args;
}

int runReplayRoutesCommand(const std::vector<std::string>& argv,
                            std::ostream& out, std::ostream& err) {
    ReplayRoutesArgs args;
    try {
        args = parseReplayRoutesArgs(argv);
    } catch (const std::exception& e) {
        err << "Error: " << e.what() << "\n"
            << "Usage: aegisctl replay-routes --audit-log <path> "
               "[--baseline <strategy>] [--new <strategy>] [--limit N]\n";
        return 2;
    }

    if (args.audit_log_path.empty()) {
        err << "Error: --audit-log is required\n"
            << "Usage: aegisctl replay-routes --audit-log <path> "
               "[--baseline <strategy>] [--new <strategy>] [--limit N]\n";
        return 2;
    }

    RoutingStrategyCatalog catalog;
    auto baseline = buildRouter(args.baseline_strategy, catalog);
    auto candidate = buildRouter(args.new_strategy, catalog);
    auto pii = std::make_shared<PIIFilter>();

    RouterOutcomeReplayer replayer(baseline, candidate, pii);
    ReplayConfig cfg;
    cfg.audit_log_path = args.audit_log_path;
    cfg.limit = args.limit;
    cfg.dry_run = args.dry_run;

    ConnectorRegistry empty_registry;
    auto result = replayer.replay(cfg, empty_registry);

    nlohmann::json output;
    output["total_entries_read"] = result.total_entries_read;
    output["total_replayed"] = result.total_replayed;
    output["total_skipped_invalid"] = result.total_skipped_invalid;
    output["total_pii_masked"] = result.total_pii_masked;
    output["baseline_strategy"] = args.baseline_strategy;
    output["new_strategy"] = args.new_strategy;
    output["strategy_comparison"] = result.strategy_comparison;
    out << output.dump(2) << "\n";

    return 0;
}

}  // namespace aegisgate::cli
