#include "observe/router_outcome_replayer.h"

#include "gateway/router.h"
#include "guardrail/inbound/pii_filter.h"
#include "core/context.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace aegisgate {

RouterOutcomeReplayer::RouterOutcomeReplayer(
    std::shared_ptr<Router> baseline_router,
    std::shared_ptr<Router> new_router,
    std::shared_ptr<PIIFilter> pii_filter)
    : baseline_router_(std::move(baseline_router)),
      new_router_(std::move(new_router)),
      pii_filter_(std::move(pii_filter)) {}

ReplayResult RouterOutcomeReplayer::replay(const ReplayConfig& cfg,
                                             const ConnectorRegistry& registry) {
    ReplayResult out;
    out.strategy_comparison = nlohmann::json::object();
    out.strategy_comparison["entries"] = nlohmann::json::array();

    if (!std::filesystem::exists(cfg.audit_log_path)) {
        spdlog::warn("RouterOutcomeReplayer: file {} not found",
                     cfg.audit_log_path);
        return out;
    }

    std::ifstream f(cfg.audit_log_path);
    if (!f.is_open()) {
        spdlog::warn("RouterOutcomeReplayer: cannot open {}",
                     cfg.audit_log_path);
        return out;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        out.total_entries_read++;

        if (out.total_replayed >= cfg.limit) break;

        nlohmann::json entry;
        try {
            entry = nlohmann::json::parse(line);
        } catch (const std::exception&) {
            out.total_skipped_invalid++;
            continue;
        }

        auto action = entry.value("action", "");
        if (action != "chat_request") continue;

        nlohmann::json detail;
        try {
            detail = nlohmann::json::parse(entry.value("detail", "{}"));
        } catch (const std::exception&) {
            out.total_skipped_invalid++;
            continue;
        }

        if (!detail.contains("model") || !detail.contains("messages") ||
            !detail["messages"].is_array()) {
            out.total_skipped_invalid++;
            continue;
        }

        // Build a request context with PII-masked messages (SR6).
        RequestContext ctx;
        ctx.request_id = entry.value("request_id", "");
        ctx.tenant_id = entry.value("tenant_id", "");
        ctx.chat_request.model = detail["model"].get<std::string>();

        for (const auto& m : detail["messages"]) {
            if (!m.is_object() || !m.contains("content")) continue;
            // SR6 invariant 1: mask PII BEFORE handing off to routers.
            const std::string raw = m["content"].get<std::string>();
            Message msg(m.value("role", "user"), pii_filter_->mask(raw));
            ctx.chat_request.messages.push_back(std::move(msg));
        }
        // SR6 invariant 2: counter advances EXACTLY once per masked replay.
        out.total_pii_masked++;

        std::string baseline_pick =
            baseline_router_->selectModel(ctx, registry);
        std::string new_pick = new_router_->selectModel(ctx, registry);
        out.total_replayed++;

        nlohmann::json comp;
        comp["request_id"] = ctx.request_id;
        comp["baseline"] = baseline_pick;
        comp["new"] = new_pick;
        comp["divergent"] = (baseline_pick != new_pick);
        out.strategy_comparison["entries"].push_back(std::move(comp));
    }

    out.strategy_comparison["summary"] = {
        {"total_replayed", out.total_replayed},
        {"total_pii_masked", out.total_pii_masked},
        {"total_skipped_invalid", out.total_skipped_invalid}};

    spdlog::info("RouterOutcomeReplayer: replayed {} / masked {} / skipped {}",
                  out.total_replayed, out.total_pii_masked,
                  out.total_skipped_invalid);
    return out;
}

}  // namespace aegisgate
