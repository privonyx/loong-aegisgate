#include "storage/rollout_schema.h"
#include <nlohmann/json.hpp>

namespace aegisgate {

using nlohmann::json;

namespace {

json scopeToJson(const ScopeSelector& s) {
    return json{
        {"tenant_globs", s.tenant_globs},
        {"regions",      s.regions},
        {"percentage",   s.percentage},
    };
}

ScopeSelector scopeFromJson(const json& j) {
    ScopeSelector s;
    if (j.contains("tenant_globs") && j["tenant_globs"].is_array())
        s.tenant_globs = j["tenant_globs"].get<std::vector<std::string>>();
    if (j.contains("regions") && j["regions"].is_array())
        s.regions = j["regions"].get<std::vector<std::string>>();
    if (j.contains("percentage") && j["percentage"].is_number_integer())
        s.percentage = j["percentage"].get<int>();
    return s;
}

json observationToJson(const ObservationPolicy& o) {
    return json{
        {"min_duration_seconds", o.min_duration_seconds},
        {"min_sample_count",     o.min_sample_count},
    };
}

ObservationPolicy observationFromJson(const json& j) {
    ObservationPolicy o;
    if (j.contains("min_duration_seconds"))
        o.min_duration_seconds = j.value("min_duration_seconds", 0);
    if (j.contains("min_sample_count"))
        o.min_sample_count = j.value("min_sample_count", 0);
    return o;
}

json autoPauseToJson(const AutoPausePolicy& p) {
    return json{
        {"error_rate_gt",              p.error_rate_gt},
        {"p99_latency_ratio_gt",       p.p99_latency_ratio_gt},
        {"absolute_error_rate_gt",     p.absolute_error_rate_gt},
        {"absolute_p99_latency_ms_gt", p.absolute_p99_latency_ms_gt},
    };
}

AutoPausePolicy autoPauseFromJson(const json& j) {
    AutoPausePolicy p;
    p.error_rate_gt              = j.value("error_rate_gt",              0.0);
    p.p99_latency_ratio_gt       = j.value("p99_latency_ratio_gt",       0.0);
    p.absolute_error_rate_gt     = j.value("absolute_error_rate_gt",     0.0);
    p.absolute_p99_latency_ms_gt = j.value("absolute_p99_latency_ms_gt", 0.0);
    return p;
}

json stageToJson(const RolloutStageRecord& s) {
    return json{
        {"name",        s.name},
        {"scope",       scopeToJson(s.scope)},
        {"observation", observationToJson(s.observation)},
        {"auto_pause",  autoPauseToJson(s.auto_pause)},
    };
}

RolloutStageRecord stageFromJson(const json& j) {
    RolloutStageRecord s;
    s.name = j.value("name", std::string{});
    if (j.contains("scope"))       s.scope       = scopeFromJson(j["scope"]);
    if (j.contains("observation")) s.observation = observationFromJson(j["observation"]);
    if (j.contains("auto_pause"))  s.auto_pause  = autoPauseFromJson(j["auto_pause"]);
    return s;
}

} // namespace

std::string serializeRolloutSpec(const RolloutSpec& spec) {
    json stages = json::array();
    for (const auto& st : spec.stages) stages.push_back(stageToJson(st));
    json j = {
        {"target_version_id",            spec.target_version_id},
        {"stages",                       stages},
        {"sticky_key",                   spec.sticky_key},
        {"auto_rollback_on_pause",       spec.auto_rollback_on_pause},
        {"auto_rollback_grace_seconds",  spec.auto_rollback_grace_seconds},
        {"creator_comment",              spec.creator_comment},
    };
    return j.dump();
}

RolloutSpec parseRolloutSpec(const std::string& json_str) {
    RolloutSpec spec;
    if (json_str.empty()) return spec;
    json j;
    try {
        j = json::parse(json_str);
    } catch (const json::exception&) {
        return spec;
    }
    spec.target_version_id = j.value("target_version_id", std::string{});
    spec.sticky_key        = j.value("sticky_key", std::string{"tenant_id"});
    spec.auto_rollback_on_pause      = j.value("auto_rollback_on_pause", true);
    spec.auto_rollback_grace_seconds = j.value("auto_rollback_grace_seconds", 1800);
    spec.creator_comment   = j.value("creator_comment", std::string{});
    if (j.contains("stages") && j["stages"].is_array()) {
        for (const auto& st : j["stages"]) {
            spec.stages.push_back(stageFromJson(st));
        }
    }
    return spec;
}

} // namespace aegisgate
