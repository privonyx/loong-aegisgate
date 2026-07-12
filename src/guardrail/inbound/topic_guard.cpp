#include "guardrail/inbound/topic_guard.h"
#include "guardrail/audit.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <shared_mutex>

namespace aegisgate {

TopicGuard::TopicGuard() = default;

void TopicGuard::loadConfig(const std::string& yaml_path) {
    std::unique_lock<std::shared_mutex> lock(config_mutex_);
    try {
        auto root = YAML::LoadFile(yaml_path);

        auto mode_str = root["mode"].as<std::string>("blacklist");
        if (mode_str == "whitelist") mode_ = TopicMode::Whitelist;
        else if (mode_str == "both") mode_ = TopicMode::Both;
        else mode_ = TopicMode::Blacklist;

        if (auto bl = root["blacklist"]) {
            if (auto kws = bl["keywords"]) {
                for (const auto& kw : kws) {
                    auto s = kw.as<std::string>();
                    if (!s.empty()) {
                        blacklist_keywords_.push_back(std::move(s));
                    }
                }
            }
            if (auto pats = bl["patterns"]) {
                for (const auto& p : pats) {
                    addBlacklistPattern(p.as<std::string>());
                }
            }
        }

        if (auto wl = root["whitelist"]) {
            if (auto topics = wl["topics"]) {
                for (const auto& t : topics) {
                    whitelist_topics_.push_back(t.as<std::string>());
                }
            }
        }

        spdlog::info("TopicGuard loaded: {} blacklist keywords, {} patterns, {} whitelist topics",
                      blacklist_keywords_.size(), blacklist_patterns_.size(),
                      whitelist_topics_.size());
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to load topic config: {}", e.what());
    }
}

void TopicGuard::reloadConfig(const std::string& yaml_path) {
    TopicMode new_mode = TopicMode::Blacklist;
    std::vector<std::string> new_keywords;
    std::vector<std::unique_ptr<RE2>> new_patterns;
    std::vector<std::string> new_whitelist;

    try {
        auto root = YAML::LoadFile(yaml_path);

        auto mode_str = root["mode"].as<std::string>("blacklist");
        if (mode_str == "whitelist") new_mode = TopicMode::Whitelist;
        else if (mode_str == "both") new_mode = TopicMode::Both;
        else new_mode = TopicMode::Blacklist;

        if (auto bl = root["blacklist"]) {
            if (auto kws = bl["keywords"]) {
                for (const auto& kw : kws) {
                    auto s = kw.as<std::string>();
                    if (!s.empty()) {
                        new_keywords.push_back(std::move(s));
                    }
                }
            }
            if (auto pats = bl["patterns"]) {
                for (const auto& p : pats) {
                    auto re = std::make_unique<RE2>(p.as<std::string>());
                    if (re->ok()) {
                        new_patterns.push_back(std::move(re));
                    } else {
                        spdlog::warn("Invalid topic pattern: {}", re->error());
                    }
                }
            }
        }

        if (auto wl = root["whitelist"]) {
            if (auto topics = wl["topics"]) {
                for (const auto& t : topics) {
                    new_whitelist.push_back(t.as<std::string>());
                }
            }
        }

        std::unique_lock<std::shared_mutex> lock(config_mutex_);
        mode_ = new_mode;
        blacklist_keywords_ = std::move(new_keywords);
        blacklist_patterns_ = std::move(new_patterns);
        whitelist_topics_ = std::move(new_whitelist);
        spdlog::info("Reloaded TopicGuard: {} blacklist keywords, {} patterns, {} whitelist topics",
                      blacklist_keywords_.size(), blacklist_patterns_.size(),
                      whitelist_topics_.size());
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to reload topic config: {}", e.what());
    }
}

void TopicGuard::setMode(TopicMode mode) { mode_ = mode; }

void TopicGuard::addBlacklistKeyword(const std::string& keyword) {
    if (!keyword.empty()) {
        blacklist_keywords_.push_back(keyword);
    }
}

void TopicGuard::addBlacklistPattern(const std::string& pattern) {
    auto re = std::make_unique<RE2>(pattern);
    if (re->ok()) {
        blacklist_patterns_.push_back(std::move(re));
    } else {
        spdlog::warn("Invalid topic pattern: {}", re->error());
    }
}

void TopicGuard::addWhitelistTopic(const std::string& topic) {
    whitelist_topics_.push_back(topic);
}

TopicResult TopicGuard::check(const std::string& text) const {
    std::shared_lock<std::shared_mutex> lock(config_mutex_);
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Blacklist keyword check
    if (mode_ == TopicMode::Blacklist || mode_ == TopicMode::Both) {
        for (const auto& kw : blacklist_keywords_) {
            std::string lower_kw = kw;
            std::transform(lower_kw.begin(), lower_kw.end(), lower_kw.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower.find(lower_kw) != std::string::npos) {
                return {true, kw, "Blocked by blacklist keyword"};
            }
        }

        for (const auto& pat : blacklist_patterns_) {
            if (RE2::PartialMatch(lower, *pat)) {
                return {true, pat->pattern(), "Blocked by blacklist pattern"};
            }
        }
    }

    // Whitelist check
    if (mode_ == TopicMode::Whitelist || mode_ == TopicMode::Both) {
        if (whitelist_topics_.empty()) {
            return {true, "whitelist", "Whitelist is empty; no topic allowed"};
        }
        bool on_topic = false;
        for (const auto& topic : whitelist_topics_) {
            std::string lower_topic = topic;
            std::transform(lower_topic.begin(), lower_topic.end(),
                           lower_topic.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower.find(lower_topic) != std::string::npos) {
                on_topic = true;
                break;
            }
        }
        if (!on_topic) {
            return {true, "whitelist", "Topic not in whitelist"};
        }
    }

    return {false, "", ""};
}

StageResult TopicGuard::process(RequestContext& ctx) {
    for (size_t i = 0; i < ctx.chat_request.messages.size(); ++i) {
        const auto& msg = ctx.chat_request.messages[i];
        if (msg.role == "system" || isToolMessage(msg)) continue;
        auto result = check(ctx.scanText(i));
        // TASK-20260707-03 / REV20260707-N19: also scan the vision image
        // reference channel (image_url / data: URI text) so off-topic content
        // can't bypass the guard by hiding in an image reference.
        if (!result.blocked) {
            std::string image_ref = ctx.scanImageText(i);
            if (!image_ref.empty()) result = check(image_ref);
        }
        if (result.blocked) {
            spdlog::warn("Topic blocked in request {}: rule='{}', reason='{}'",
                         ctx.request_id, result.matched_rule, result.reason);
            if (audit_logger_) {
                audit_logger_->logAction(
                    ctx.request_id, ctx.tenant_id, name(), "blocked",
                    "topic rule='" + result.matched_rule +
                        "' reason=" + result.reason);
            }
            return StageResult::Reject;
        }
    }
    return StageResult::Continue;
}

} // namespace aegisgate
