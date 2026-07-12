#pragma once

// Phase 9.3.4 Epic D.2 — per-request rollout scope resolution.
//
// Pure function that returns which config version_id a given request should
// use. Called on the data-plane hot path for every proxied request when
// a rollout is active.

#include <string>

namespace aegisgate {

class Config;

// Returns the version_id to use for this request.
// - tenant_id: the requesting tenant
// - region: inferred client region (may be empty)
// - sticky_value: value used for percentage hashing (tenant_id or user_id
//   depending on the rollout's sticky_key)
std::string resolveActiveConfigId(const Config& cfg,
                                   const std::string& tenant_id,
                                   const std::string& region,
                                   const std::string& sticky_value);

} // namespace aegisgate
