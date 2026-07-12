#include "core/rollout_resolve.h"

#include "common/scope_matcher.h"
#include "core/config.h"

namespace aegisgate {

std::string resolveActiveConfigId(const Config& cfg,
                                   const std::string& tenant_id,
                                   const std::string& region,
                                   const std::string& sticky_value) {
    auto rc = cfg.rolloutConfig();
    if (!rc) return cfg.activeVersionId();

    const auto& scope = rc->current_stage.scope;
    bool hit = common::matches(scope.tenant_globs,
                                scope.regions,
                                scope.percentage,
                                {tenant_id, region, sticky_value});
    return hit ? rc->target_version_id : cfg.activeVersionId();
}

} // namespace aegisgate
