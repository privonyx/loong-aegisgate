#pragma once

// Phase 9.3.4 Epic D.1 — RolloutConfigView.
//
// Lightweight, read-only view of the rollout section embedded in the
// merged-yaml format produced by the control plane for data-plane hot
// reload. Parsed once at Config::loadFromString; evaluated per-request
// via resolveActiveConfigId().

#include <string>
#include <vector>

namespace aegisgate {

struct RolloutScopeConfig {
    std::vector<std::string> tenant_globs;
    std::vector<std::string> regions;
    int percentage = 0;
};

struct RolloutStageConfig {
    int stage_index = 0;
    std::string name;
    RolloutScopeConfig scope;
};

struct RolloutConfigView {
    std::string rollout_id;
    std::string target_version_id;
    std::string sticky_key;  // "tenant_id" | "user_id"
    RolloutStageConfig current_stage;
};

} // namespace aegisgate
