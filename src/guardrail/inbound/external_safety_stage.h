#pragma once
#include "core/pipeline.h"
#include "external_safety_api.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <string>

namespace aegisgate {

class AuditLogger;

namespace guard {
class GuardAdminController;
}  // namespace guard

enum class ExternalSafetyMode {
    Any,  // Block if ANY provider flags content
    All,  // Block only if ALL providers flag content
    Majority  // Block if majority of providers flag content
};

enum class ExternalSafetyFailPolicy {
    Open,   // On API error, allow request through (fail-open)
    Closed  // On API error, block request (fail-closed)
};

struct ExternalSafetyStageConfig {
    ExternalSafetyMode mode = ExternalSafetyMode::Any;
    ExternalSafetyFailPolicy fail_policy = ExternalSafetyFailPolicy::Open;
    bool async_parallel = true;

    // Phase 6.3 (SR3+SR6): when true, process() returns Continue immediately
    // and fires the upstream scan asynchronously, writing the audit trail
    // with `shadow=true`. shadow_max_inflight caps concurrent shadow workers
    // (SR6 backpressure); when reached, the dispatch is skipped with a warn.
    bool shadow_mode = false;
    std::chrono::seconds shadow_audit_ttl{86400};
    size_t shadow_max_inflight = 1000;
};

class ExternalSafetyStage : public PipelineStage {
public:
    explicit ExternalSafetyStage(const ExternalSafetyStageConfig& config = {});
    ~ExternalSafetyStage();

    void addProvider(std::unique_ptr<ExternalSafetyApi> provider);
    size_t providerCount() const;

    // Phase 6.3: AuditLogger is borrowed; ownership stays with the caller.
    // Passing nullptr disables shadow logging but does NOT disable shadow
    // dispatch (provider side-effects still run).
    void setAuditLogger(AuditLogger* logger);

    // TASK-20260708-03 / REV20260707-C2: borrowed GuardAdminController; when
    // set, a synchronous L4 block records a structured `GuardExplanation`
    // for admin lookup. Nullable = no-op. Only applies to sync block path,
    // NOT shadow-mode dispatch (shadow uses audit_logger with shadow=true).
    void setGuardAdminController(guard::GuardAdminController* controller);

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "ExternalSafetyStage"; }

    const std::vector<SafetyResult>& lastResults() const { return last_results_; }

    // Shadow telemetry — useful for tests, dashboards, and SR6 dashboards.
    size_t shadowInflight() const { return shadow_inflight_.load(); }
    size_t shadowDispatched() const { return shadow_dispatched_.load(); }
    size_t shadowSkipped() const { return shadow_skipped_.load(); }
    size_t shadowAuditWrites() const { return shadow_audit_writes_.load(); }

    // Block until all in-flight shadow workers complete; bounded by `timeout`.
    // Returns true iff inflight drops to zero in time.
    bool waitForShadowDrain(std::chrono::milliseconds timeout) const;

private:
    bool shouldBlock(const std::vector<SafetyResult>& results) const;
    std::string extractTextFromRequest(const RequestContext& ctx) const;
    void shadowDispatch(const RequestContext& ctx);

    ExternalSafetyStageConfig config_;
    std::vector<std::unique_ptr<ExternalSafetyApi>> providers_;
    std::vector<SafetyResult> last_results_;

    AuditLogger* audit_logger_ = nullptr;
    // TASK-20260708-03 / REV20260707-C2: borrowed, may be null.
    guard::GuardAdminController* guard_admin_controller_ = nullptr;
    mutable std::atomic<size_t> shadow_inflight_{0};
    std::atomic<size_t> shadow_dispatched_{0};
    std::atomic<size_t> shadow_skipped_{0};
    std::atomic<size_t> shadow_audit_writes_{0};
};

} // namespace aegisgate
