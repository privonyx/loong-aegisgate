#include "guardrail/rule_engine.h"
#include "guardrail/admin/guard_admin_controller.h"
#include "guardrail/audit.h"
#include "guardrail/guard_explanation_builder.h"
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace aegisgate {

RuleEngine::RuleEngine(const FeatureGate& gate) : gate_(gate) {}

void RuleEngine::addRule(const std::string& id, const std::string& description,
                          int priority, RuleAction action,
                          const std::string& action_param) {
    auto rule = std::make_unique<Rule>();
    rule->id = id;
    rule->description = description;
    rule->priority = priority;
    rule->action = action;
    rule->action_param = action_param;
    rules_.push_back(std::move(rule));

    std::sort(rules_.begin(), rules_.end(),
              [](const auto& a, const auto& b) {
                  return a->priority > b->priority;
              });
}

void RuleEngine::addConditionToRule(const std::string& rule_id,
                                     ConditionType type,
                                     const std::string& value) {
    for (auto& rule : rules_) {
        if (rule->id == rule_id) {
            RuleCondition cond;
            cond.type = type;
            cond.value = value;
            if (type == ConditionType::RegexMatch) {
                cond.compiled_regex = std::make_unique<RE2>(value);
                if (!cond.compiled_regex->ok()) {
                    spdlog::warn("Invalid rule regex '{}': {}",
                                 value, cond.compiled_regex->error());
                    return;
                }
            }
            rule->conditions.push_back(std::move(cond));
            return;
        }
    }
    spdlog::warn("Rule '{}' not found when adding condition", rule_id);
}

// --- YAML parsing (static, no lock needed) ---

bool RuleEngine::parseConditionType(const std::string& s, ConditionType& out) {
    if (s == "regex_match") { out = ConditionType::RegexMatch; return true; }
    if (s == "keyword_contains") { out = ConditionType::KeywordContains; return true; }
    if (s == "length_check") { out = ConditionType::LengthCheck; return true; }
    if (s == "model_match") { out = ConditionType::ModelMatch; return true; }
    return false;
}

bool RuleEngine::parseAction(const std::string& s, RuleAction& out) {
    if (s == "block") { out = RuleAction::Block; return true; }
    if (s == "warn") { out = RuleAction::Warn; return true; }
    if (s == "modify") { out = RuleAction::Modify; return true; }
    if (s == "allow") { out = RuleAction::Allow; return true; }
    return false;
}

bool RuleEngine::parseYaml(const std::string& path, RuleVec& out) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to load rules YAML '{}': {}", path, e.what());
        return false;
    }

    if (!root["rules"] || !root["rules"].IsSequence()) {
        spdlog::error("Rules YAML '{}' missing 'rules' sequence", path);
        return false;
    }

    RuleVec parsed;
    for (const auto& node : root["rules"]) {
        auto rule = std::make_unique<Rule>();
        rule->id = node["id"].as<std::string>("");
        rule->description = node["description"].as<std::string>("");
        rule->priority = node["priority"].as<int>(0);
        rule->enabled = node["enabled"].as<bool>(true);

        RuleAction action = RuleAction::Allow;
        if (!parseAction(node["action"].as<std::string>("allow"), action)) {
            spdlog::warn("Rule '{}': unknown action, defaulting to allow", rule->id);
        }
        rule->action = action;
        rule->action_param = node["action_param"].as<std::string>("");

        if (node["conditions"] && node["conditions"].IsSequence()) {
            for (const auto& cnode : node["conditions"]) {
                RuleCondition cond;
                std::string type_str = cnode["type"].as<std::string>("");
                if (!parseConditionType(type_str, cond.type)) {
                    spdlog::warn("Rule '{}': unknown condition type '{}', skipping",
                                 rule->id, type_str);
                    continue;
                }
                cond.value = cnode["value"].as<std::string>("");
                if (cond.type == ConditionType::RegexMatch) {
                    cond.compiled_regex = std::make_unique<RE2>(cond.value);
                    if (!cond.compiled_regex->ok()) {
                        spdlog::warn("Rule '{}': invalid regex '{}': {}",
                                     rule->id, cond.value,
                                     cond.compiled_regex->error());
                        continue;
                    }
                }
                rule->conditions.push_back(std::move(cond));
            }
        }

        parsed.push_back(std::move(rule));
    }

    std::sort(parsed.begin(), parsed.end(),
              [](const auto& a, const auto& b) {
                  return a->priority > b->priority;
              });

    out = std::move(parsed);
    return true;
}

bool RuleEngine::loadFromYaml(const std::string& path) {
    RuleVec parsed;
    if (!parseYaml(path, parsed)) return false;
    rules_ = std::move(parsed);
    spdlog::info("RuleEngine: loaded {} rules from {}", rules_.size(), path);
    return true;
}

bool RuleEngine::reloadRules(const std::string& path) {
    RuleVec parsed;
    if (!parseYaml(path, parsed)) {
        spdlog::warn("RuleEngine: reload failed for '{}', keeping current rules", path);
        return false;
    }

    {
        std::unique_lock lock(rules_mutex_);
        rules_ = std::move(parsed);
    }
    spdlog::info("RuleEngine: hot-reloaded {} rules from {}", ruleCount(), path);
    return true;
}

bool RuleEngine::parseRuleSetJson(const std::string& rules_json, RuleVec& out) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(rules_json);
    } catch (const std::exception& e) {
        spdlog::error("RuleEngine: failed to parse rules JSON: {}", e.what());
        return false;
    }

    RuleVec parsed;
    for (const auto& rj : j) {
        auto rule = std::make_unique<Rule>();
        rule->id = rj.value("id", "");
        rule->description = rj.value("description", "");
        rule->priority = rj.value("priority", 0);
        rule->enabled = rj.value("enabled", true);

        RuleAction action = RuleAction::Allow;
        parseAction(rj.value("action", "allow"), action);
        rule->action = action;
        rule->action_param = rj.value("action_param", "");

        if (rj.contains("conditions") && rj["conditions"].is_array()) {
            for (const auto& cj : rj["conditions"]) {
                RuleCondition cond;
                ConditionType ct;
                if (!parseConditionType(cj.value("type", ""), ct)) continue;
                cond.type = ct;
                cond.value = cj.value("value", "");
                if (cond.type == ConditionType::RegexMatch) {
                    cond.compiled_regex = std::make_unique<RE2>(cond.value);
                    if (!cond.compiled_regex->ok()) continue;
                }
                rule->conditions.push_back(std::move(cond));
            }
        }
        parsed.push_back(std::move(rule));
    }

    std::sort(parsed.begin(), parsed.end(),
              [](const auto& a, const auto& b) {
                  return a->priority > b->priority;
              });
    out = std::move(parsed);
    return true;
}

bool RuleEngine::loadFromStore(PersistentStore& store, const std::string& tenant_id) {
    auto active = store.getActiveRuleSet(tenant_id);
    if (!active) {
        spdlog::info("RuleEngine: no active rule set for tenant '{}'", tenant_id);
        return false;
    }

    RuleVec parsed;
    if (!parseRuleSetJson(active->rules_json, parsed)) return false;

    {
        std::unique_lock lock(rules_mutex_);
        rules_ = std::move(parsed);
    }
    spdlog::info("RuleEngine: loaded {} rules from store (tenant='{}', v={})",
                 ruleCount(), tenant_id, active->version);
    return true;
}

void RuleEngine::loadAllTenants(PersistentStore& store) {
    // SR-4：遍历各租户激活集建桶（无激活集不建桶 → 请求期回退全局）。
    auto tenants = store.listTenants(10000, 0);
    std::map<std::string, RuleVec> loaded;
    for (const auto& t : tenants) {
        if (t.id.empty()) continue;
        auto active = store.getActiveRuleSet(t.id);
        if (!active) continue;
        RuleVec parsed;
        if (!parseRuleSetJson(active->rules_json, parsed)) continue;
        loaded[t.id] = std::move(parsed);
    }
    size_t n = loaded.size();
    {
        std::unique_lock lock(rules_mutex_);
        tenant_rules_ = std::move(loaded);
    }
    spdlog::info("RuleEngine: loaded per-tenant rule sets for {} tenant(s)", n);
}

bool RuleEngine::reloadTenant(PersistentStore& store,
                              const std::string& tenant_id) {
    if (tenant_id.empty()) return false;  // 空 tenant = 全局，走 loadFromStore
    auto active = store.getActiveRuleSet(tenant_id);
    if (!active) {
        std::unique_lock lock(rules_mutex_);
        tenant_rules_.erase(tenant_id);  // 无激活集 → 回退全局
        return false;
    }
    RuleVec parsed;
    if (!parseRuleSetJson(active->rules_json, parsed)) return false;
    {
        std::unique_lock lock(rules_mutex_);
        tenant_rules_[tenant_id] = std::move(parsed);
    }
    spdlog::info("RuleEngine: refreshed tenant '{}' rule set (v={})",
                 tenant_id, active->version);
    return true;
}

bool RuleEngine::reloadFromStoreOrYaml(PersistentStore& store,
                                       const std::string& tenant_id,
                                       const std::string& yaml_path) {
    if (loadFromStore(store, tenant_id)) return true;
    reloadRules(yaml_path);
    return false;
}

bool RuleEngine::saveToStore(PersistentStore& store, const std::string& tenant_id) {
    nlohmann::json arr = nlohmann::json::array();
    {
        std::shared_lock lock(rules_mutex_);
        for (const auto& rule : rules_) {
            nlohmann::json rj;
            rj["id"] = rule->id;
            rj["description"] = rule->description;
            rj["priority"] = rule->priority;
            rj["enabled"] = rule->enabled;

            auto actionStr = [](RuleAction a) -> std::string {
                switch (a) {
                    case RuleAction::Block: return "block";
                    case RuleAction::Warn: return "warn";
                    case RuleAction::Modify: return "modify";
                    case RuleAction::Allow: return "allow";
                    default: return "allow";
                }
            };
            rj["action"] = actionStr(rule->action);
            rj["action_param"] = rule->action_param;

            nlohmann::json conds = nlohmann::json::array();
            for (const auto& cond : rule->conditions) {
                nlohmann::json cj;
                auto condStr = [](ConditionType ct) -> std::string {
                    switch (ct) {
                        case ConditionType::RegexMatch: return "regex_match";
                        case ConditionType::KeywordContains: return "keyword_contains";
                        case ConditionType::LengthCheck: return "length_check";
                        case ConditionType::ModelMatch: return "model_match";
                        default: return "";
                    }
                };
                cj["type"] = condStr(cond.type);
                cj["value"] = cond.value;
                conds.push_back(cj);
            }
            rj["conditions"] = conds;
            arr.push_back(rj);
        }
    }

    auto existing = store.listRuleSetVersions(tenant_id, 1);
    int64_t next_version = existing.empty() ? 1 : existing[0].version + 1;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    std::ostringstream oss;
    oss << std::put_time(&buf, "%Y-%m-%dT%H:%M:%SZ");

    PersistentStore::RuleSetRecord record;
    record.version = next_version;
    record.tenant_id = tenant_id;
    record.rules_json = arr.dump();
    record.created_at = oss.str();
    record.is_active = true;

    return store.insertRuleSet(tenant_id, record);
}

size_t RuleEngine::ruleCount() const {
    std::shared_lock lock(rules_mutex_);
    return rules_.size();
}

// --- Evaluation ---

std::string RuleEngine::extractText(const RequestContext& ctx) const {
    std::string text;
    for (size_t i = 0; i < ctx.chat_request.messages.size(); ++i) {
        const auto& msg = ctx.chat_request.messages[i];
        if (isToolMessage(msg)) continue;
        if (!text.empty()) text += "\x1E";
        text += ctx.scanText(i);
        // TASK-20260707-03 / REV20260707-N19: include the vision image reference
        // channel (image_url / data: URI text) so rules match content hidden in
        // image references.
        std::string image_ref = ctx.scanImageText(i);
        if (!image_ref.empty()) {
            text += "\x1E";
            text += image_ref;
        }
    }
    return text;
}

bool RuleEngine::evaluateCondition(const RuleCondition& cond,
                                    const RequestContext& ctx) const {
    std::string text = extractText(ctx);

    switch (cond.type) {
        case ConditionType::RegexMatch:
            if (cond.compiled_regex) {
                return RE2::PartialMatch(text, *cond.compiled_regex);
            }
            return false;

        case ConditionType::KeywordContains: {
            std::string lower_text = text;
            std::string lower_val = cond.value;
            std::transform(lower_text.begin(), lower_text.end(),
                           lower_text.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::transform(lower_val.begin(), lower_val.end(),
                           lower_val.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return lower_text.find(lower_val) != std::string::npos;
        }

        case ConditionType::LengthCheck: {
            try {
                size_t max_len = std::stoul(cond.value);
                return text.size() > max_len;
            } catch (const std::exception& e) {
                spdlog::warn("Invalid LengthCheck value '{}': {}",
                             cond.value, e.what());
                return false;
            }
        }

        case ConditionType::ModelMatch:
            return ctx.chat_request.model == cond.value ||
                   ctx.target_model == cond.value;

        default:
            return false;
    }
}

RuleResult RuleEngine::evaluateImpl(const RequestContext& ctx,
                                     const RuleVec& rules) const {
    if (!gate_.isEnabled(Feature::CustomRules)) {
        return {false, "", RuleAction::Allow, "Enterprise feature disabled", ""};
    }

    for (const auto& rule : rules) {
        if (!rule->enabled) continue;

        bool all_match = !rule->conditions.empty();
        for (const auto& cond : rule->conditions) {
            if (!evaluateCondition(cond, ctx)) {
                all_match = false;
                break;
            }
        }

        if (all_match) {
            return {true, rule->id, rule->action, rule->description,
                    rule->action_param};
        }
    }

    return {false, "", RuleAction::Allow, "", ""};
}

RuleResult RuleEngine::evaluate(const RequestContext& ctx) const {
    std::shared_lock lock(rules_mutex_);
    // SR-4：按 ctx.tenant_id 路由到租户桶；无桶回退全局 rules_。
    auto it = tenant_rules_.find(ctx.tenant_id);
    const RuleVec& rules = (it != tenant_rules_.end()) ? it->second : rules_;
    return evaluateImpl(ctx, rules);
}

StageResult RuleEngine::processImpl(RequestContext& ctx, const RuleVec& rules) {
    auto result = evaluateImpl(ctx, rules);
    if (!result.matched) return StageResult::Continue;

    spdlog::info("Rule matched: id={}, action={}, detail={}",
                 result.rule_id, static_cast<int>(result.action),
                 result.detail);

    switch (result.action) {
        case RuleAction::Block:
            // P1-C: audit the block (security-relevant decision).
            if (audit_logger_) {
                audit_logger_->logAction(
                    ctx.request_id, ctx.tenant_id, name(), "blocked",
                    "rule id='" + result.rule_id + "' " + result.detail);
            }
            // TASK-20260708-03 / REV20260707-C2: L2 rule-engine verdict
            // recorded as structured GuardExplanation. Nullable-safe (SR-3).
            if (guard_admin_controller_) {
                guard_admin_controller_->recordExplanation(
                    ctx.request_id,
                    guard::GuardExplanationBuilder::fromRule(result));
            }
            return StageResult::Reject;
        case RuleAction::Warn:
            return StageResult::Continue;
        case RuleAction::Modify: {
            for (auto& msg : ctx.chat_request.messages) {
                if (msg.role == "system") continue;
                for (const auto& rule : rules) {
                    if (rule->id != result.rule_id || !rule->enabled) continue;
                    for (const auto& cond : rule->conditions) {
                        if (cond.type == ConditionType::KeywordContains) {
                            std::string lower_val = cond.value;
                            std::transform(lower_val.begin(), lower_val.end(),
                                           lower_val.begin(),
                                           [](unsigned char c) {
                                               return std::tolower(c);
                                           });
                            for (;;) {
                                std::string lower_content = msg.content;
                                std::transform(
                                    lower_content.begin(), lower_content.end(),
                                    lower_content.begin(),
                                    [](unsigned char c) {
                                        return std::tolower(c);
                                    });
                                size_t pos = lower_content.find(lower_val);
                                if (pos == std::string::npos) {
                                    break;
                                }
                                msg.content.replace(pos, cond.value.size(),
                                                    result.action_param);
                            }
                        } else if (cond.type == ConditionType::RegexMatch &&
                                   cond.compiled_regex) {
                            RE2::GlobalReplace(&msg.content, *cond.compiled_regex,
                                               result.action_param);
                        }
                    }
                }
            }
            return StageResult::Continue;
        }
        case RuleAction::Allow:
            return StageResult::Continue;

        default:
            return StageResult::Continue;
    }
}

StageResult RuleEngine::process(RequestContext& ctx) {
    std::shared_lock lock(rules_mutex_);
    // SR-4：按 ctx.tenant_id 路由到租户桶；无桶回退全局 rules_。
    auto it = tenant_rules_.find(ctx.tenant_id);
    const RuleVec& rules = (it != tenant_rules_.end()) ? it->second : rules_;
    return processImpl(ctx, rules);
}

} // namespace aegisgate
