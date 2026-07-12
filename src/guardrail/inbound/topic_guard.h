#pragma once
#include "core/pipeline.h"
#include <re2/re2.h>
#include <shared_mutex>
#include <string>
#include <vector>
#include <memory>

namespace aegisgate {

class AuditLogger;

enum class TopicMode { Whitelist, Blacklist, Both };

struct TopicResult {
    bool blocked = false;
    std::string matched_rule;
    std::string reason;
};

class TopicGuard : public PipelineStage {
public:
    TopicGuard();

    void loadConfig(const std::string& yaml_path);
    void reloadConfig(const std::string& yaml_path);
    void setMode(TopicMode mode);
    void addBlacklistKeyword(const std::string& keyword);
    void addBlacklistPattern(const std::string& pattern);
    void addWhitelistTopic(const std::string& topic);

    // P1-1: borrowed AuditLogger; reject writes a "blocked" audit entry.
    void setAuditLogger(AuditLogger* logger) { audit_logger_ = logger; }

    TopicResult check(const std::string& text) const;

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "TopicGuard"; }

private:
    TopicMode mode_ = TopicMode::Blacklist;
    std::vector<std::string> blacklist_keywords_;
    std::vector<std::unique_ptr<RE2>> blacklist_patterns_;
    std::vector<std::string> whitelist_topics_;
    AuditLogger* audit_logger_ = nullptr;  // P1-1: borrowed, may be null
    mutable std::shared_mutex config_mutex_;  // Lock Layer 1
};

} // namespace aegisgate
