#include "gateway/routing_strategy_catalog.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>

namespace aegisgate {

RoutingStrategyCatalog::RoutingStrategyCatalog() {
    addDefaultStrategies();
}

void RoutingStrategyCatalog::addDefaultStrategies() {
    // 5 templates per design §3.1 + §4.3.

    RoutingStrategy cost_first;
    cost_first.name = "cost-first";
    cost_first.weights = {0.7, 0.2, 0.1};
    cost_first.bandit_algorithm = "thompson";
    cost_first.bandit_mode = "shadow";
    cost_first.canary_pct = 0.05;
    cost_first.description = "Minimize spend — prefers cheapest provider";
    insertOrReplace(cost_first);

    RoutingStrategy quality_first;
    quality_first.name = "quality-first";
    quality_first.weights = {0.2, 0.6, 0.2};
    quality_first.bandit_algorithm = "thompson";
    quality_first.bandit_mode = "shadow";
    quality_first.canary_pct = 0.05;
    quality_first.description = "Maximize success rate — prefers most reliable provider";
    insertOrReplace(quality_first);

    RoutingStrategy hybrid;
    hybrid.name = "hybrid";
    hybrid.weights = {};  // default {0.4, 0.35, 0.25}
    hybrid.bandit_algorithm = "thompson";
    hybrid.bandit_mode = "shadow";
    hybrid.canary_pct = 0.05;
    hybrid.description = "Balanced cost/quality/latency tradeoff";
    insertOrReplace(hybrid);

    RoutingStrategy canary;
    canary.name = "canary";
    canary.weights = {};
    canary.bandit_algorithm = "thompson";
    canary.bandit_mode = "live";   // canary is a live mode with low pct
    canary.canary_pct = 0.05;       // 5% live traffic
    canary.description = "Live-mode canary at 5% of traffic";
    insertOrReplace(canary);

    RoutingStrategy shadow;
    shadow.name = "shadow";
    shadow.weights = {};
    shadow.bandit_algorithm = "thompson";
    shadow.bandit_mode = "shadow";  // SR5: shadow mode default
    shadow.canary_pct = 0.0;        // shadow does not affect prod traffic
    shadow.description = "Shadow-only mode — observes without serving";
    insertOrReplace(shadow);
}

void RoutingStrategyCatalog::insertOrReplace(const RoutingStrategy& s) {
    strategies_[s.name] = s;
}

void RoutingStrategyCatalog::loadOverrides(const std::string& yaml_path) {
    if (!std::filesystem::exists(yaml_path)) {
        spdlog::warn("RoutingStrategyCatalog: override file {} not found, "
                     "keeping defaults",
                     yaml_path);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    try {
        auto root = YAML::LoadFile(yaml_path);
        auto seq = root["strategies"];
        if (!seq || !seq.IsSequence()) {
            spdlog::warn("RoutingStrategyCatalog: {} has no 'strategies' "
                         "sequence",
                         yaml_path);
            return;
        }
        int n_loaded = 0;
        for (const auto& node : seq) {
            RoutingStrategy s;
            s.name = node["name"].as<std::string>("");
            if (s.name.empty()) continue;

            auto wnode = node["weights"];
            if (wnode) {
                s.weights.cost = wnode["cost"].as<double>(0.4);
                s.weights.quality = wnode["quality"].as<double>(0.35);
                s.weights.latency = wnode["latency"].as<double>(0.25);
            }
            s.bandit_algorithm =
                node["bandit_algorithm"].as<std::string>("thompson");
            s.bandit_epsilon = node["bandit_epsilon"].as<double>(0.1);
            s.bandit_mode = node["bandit_mode"].as<std::string>("shadow");
            s.canary_pct = node["canary_pct"].as<double>(0.05);
            s.description = node["description"].as<std::string>("");

            insertOrReplace(s);
            n_loaded++;
        }
        spdlog::info("RoutingStrategyCatalog: loaded {} overrides from {}",
                     n_loaded, yaml_path);
    } catch (const YAML::Exception& e) {
        spdlog::error("RoutingStrategyCatalog: YAML error in {}: {}",
                      yaml_path, e.what());
    }
}

std::vector<RoutingStrategy> RoutingStrategyCatalog::list() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<RoutingStrategy> out;
    out.reserve(strategies_.size());
    for (const auto& kv : strategies_) out.push_back(kv.second);
    return out;
}

std::optional<RoutingStrategy> RoutingStrategyCatalog::get(
    const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = strategies_.find(name);
    if (it == strategies_.end()) return std::nullopt;
    return it->second;
}

}  // namespace aegisgate
