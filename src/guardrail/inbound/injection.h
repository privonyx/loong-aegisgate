#pragma once
#include "core/pipeline.h"
#include <re2/re2.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <memory>

namespace aegisgate {

class AuditLogger;

namespace guard {
class GuardAdminController;
}  // namespace guard

enum class InjectionSeverity { Low, Medium, High };

struct InjectionPattern {
    std::string name;
    std::unique_ptr<RE2> regex;
    InjectionSeverity severity;
};

struct InjectionResult {
    bool detected = false;
    std::string matched_pattern;
    InjectionSeverity severity = InjectionSeverity::Low;
    double confidence = 0.0;
    std::string layer;
};

struct HeuristicConfig {
    int nested_quotes_threshold = 3;
    double instruction_override_score = 0.7;
    double role_switch_score = 0.8;
    double encoding_attempt_score = 0.5;
};

class InjectionDetector : public PipelineStage {
public:
    InjectionDetector();

    void loadPatterns(const std::string& yaml_path);
    void reloadPatterns(const std::string& yaml_path);
    void setThreshold(double threshold);

    // P0-2: degradation policy when no rules are loaded (e.g. patterns YAML
    // missing/corrupt at startup). Default false = fail-closed (reject all),
    // preserving the secure posture. Set true to fail-open (pass requests
    // through) for availability-first deployments. Detection behaviour with
    // loaded rules is unaffected either way.
    void setFailOpen(bool fail_open) { fail_open_ = fail_open; }

    // P1-1: borrowed AuditLogger; when set, a reject decision writes a
    // "blocked" audit entry. Ownership stays with the caller (pipeline).
    void setAuditLogger(AuditLogger* logger) { audit_logger_ = logger; }

    // TASK-20260708-03 / REV20260707-C2: borrowed GuardAdminController; when
    // set, a reject decision also records a structured `GuardExplanation`
    // for `GET /admin/api/guard/explanation/{id}` lookup. Nullable = no-op.
    void setGuardAdminController(guard::GuardAdminController* controller) {
        guard_admin_controller_ = controller;
    }

    InjectionResult detect(const std::string& text) const;

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "InjectionDetector"; }

private:
    InjectionResult regexScan(const std::string& text) const;
    InjectionResult keywordScan(const std::string& text) const;
    InjectionResult heuristicScan(const std::string& text) const;

    std::vector<InjectionPattern> patterns_;
    std::vector<std::string> keywords_;
    HeuristicConfig heuristic_config_;
    double threshold_ = 0.5;
    bool loaded_ = false;
    bool fail_open_ = false;  // P0-2: see setFailOpen()
    AuditLogger* audit_logger_ = nullptr;  // P1-1: borrowed, may be null
    // TASK-20260708-03 / REV20260707-C2: borrowed, may be null.
    guard::GuardAdminController* guard_admin_controller_ = nullptr;
    mutable std::shared_mutex patterns_mutex_;  // Lock Layer 1 — see docs/LOCK_ORDERING.md
    mutable std::once_flag no_rules_log_flag_;  // P0-2: log "no rules" once, not per request
};

} // namespace aegisgate
