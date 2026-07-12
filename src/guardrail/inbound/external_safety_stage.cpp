#include "guardrail/inbound/external_safety_stage.h"
#include "guardrail/admin/guard_admin_controller.h"
#include "guardrail/audit.h"
#include "guardrail/guard_explanation_builder.h"
#include <spdlog/spdlog.h>
#include <future>
#include <sstream>
#include <thread>

namespace aegisgate {

ExternalSafetyStage::ExternalSafetyStage(const ExternalSafetyStageConfig& config)
    : config_(config) {}

ExternalSafetyStage::~ExternalSafetyStage() {
    // SR6: avoid use-after-free of `this` from detached shadow workers.
    waitForShadowDrain(std::chrono::milliseconds{5000});
}

void ExternalSafetyStage::setGuardAdminController(
    guard::GuardAdminController* controller) {
    guard_admin_controller_ = controller;
}

void ExternalSafetyStage::setAuditLogger(AuditLogger* logger) {
    audit_logger_ = logger;
}

bool ExternalSafetyStage::waitForShadowDrain(
    std::chrono::milliseconds timeout) const {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (shadow_inflight_.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

void ExternalSafetyStage::addProvider(std::unique_ptr<ExternalSafetyApi> provider) {
    providers_.push_back(std::move(provider));
}

size_t ExternalSafetyStage::providerCount() const {
    return providers_.size();
}

std::string ExternalSafetyStage::extractTextFromRequest(const RequestContext& ctx) const {
    std::ostringstream oss;
    for (size_t i = 0; i < ctx.chat_request.messages.size(); ++i) {
        const auto& msg = ctx.chat_request.messages[i];
        if (isToolMessage(msg)) continue;
        if (!oss.str().empty()) oss << "\n";
        oss << ctx.scanText(i);
        // TASK-20260707-03 / REV20260707-N19: include the vision image reference
        // channel (image_url / data: URI text) so external safety providers see
        // content hidden in image references.
        std::string image_ref = ctx.scanImageText(i);
        if (!image_ref.empty()) oss << "\n" << image_ref;
    }
    return oss.str();
}

StageResult ExternalSafetyStage::process(RequestContext& ctx) {
    if (providers_.empty()) {
        return StageResult::Continue;
    }

    std::string text = extractTextFromRequest(ctx);
    if (text.empty()) {
        return StageResult::Continue;
    }

    // Phase 6.3 (SR3+SR6): shadow mode short-circuits the synchronous block
    // path. We bump inflight under the cap, kick off a detached worker that
    // runs providers + writes audit, and return Continue immediately so the
    // hot path never waits on external HTTP.
    if (config_.shadow_mode) {
        size_t now = shadow_inflight_.load(std::memory_order_acquire);
        while (now < config_.shadow_max_inflight) {
            if (shadow_inflight_.compare_exchange_weak(
                    now, now + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                ++shadow_dispatched_;
                RequestContext snapshot = ctx;
                std::thread([this, snapshot = std::move(snapshot)]() mutable {
                    shadowDispatch(snapshot);
                    shadow_inflight_.fetch_sub(1, std::memory_order_acq_rel);
                }).detach();
                return StageResult::Continue;
            }
        }
        ++shadow_skipped_;
        spdlog::warn(
            "ExternalSafetyStage: shadow skip due to backpressure "
            "(inflight={}, cap={})",
            now, config_.shadow_max_inflight);
        return StageResult::Continue;
    }

    last_results_.clear();

    if (config_.async_parallel && providers_.size() > 1) {
        std::vector<std::future<SafetyResult>> futures;
        futures.reserve(providers_.size());
        for (auto& provider : providers_) {
            auto* p = provider.get();
            futures.push_back(
                std::async(std::launch::async, [p, &text]() {
                    return p->check(text);
                }));
        }
        for (auto& f : futures) {
            last_results_.push_back(f.get());
        }
    } else {
        for (auto& provider : providers_) {
            last_results_.push_back(provider->check(text));
        }
    }

    for (const auto& r : last_results_) {
        if (!r.success) {
            spdlog::warn("ExternalSafetyStage: {} returned error: {}",
                         r.provider, r.error);
        } else {
            spdlog::debug("ExternalSafetyStage: {} completed in {}ms, flagged={}",
                          r.provider, r.latency.count(), r.flagged);
        }
    }

    ctx.external_safety_checked = true;

    if (shouldBlock(last_results_)) {
        ctx.external_safety_flagged = true;
        std::string details;
        for (const auto& r : last_results_) {
            if (r.flagged) {
                details += r.provider + " ";
                for (const auto& c : r.categories) {
                    if (c.flagged) {
                        details += "[" + c.name + ":" +
                                   std::to_string(c.score).substr(0, 5) + "] ";
                    }
                }
            }
        }
        spdlog::warn("ExternalSafetyStage: content blocked by L4. request={}, providers={}",
                     ctx.request_id, details);
        // P1-C: a synchronous block is a security-relevant decision — audit it
        // (previously only shadow-mode scans wrote an audit entry).
        if (audit_logger_) {
            audit_logger_->logAction(ctx.request_id, ctx.tenant_id,
                                      name(), "blocked",
                                      "external_safety " + details);
        }
        // TASK-20260708-03 / REV20260707-C2: L4 external provider verdict
        // recorded as structured GuardExplanation. Nullable-safe (SR-3).
        // Uses the first flagged provider + its first flagged category as
        // the canonical verdict; the details string above still captures
        // the aggregate picture in audit.
        if (guard_admin_controller_) {
            std::string provider_name;
            std::string verdict;
            float confidence = 0.0f;
            for (const auto& r : last_results_) {
                if (!r.flagged) continue;
                provider_name = r.provider;
                for (const auto& c : r.categories) {
                    if (c.flagged) {
                        verdict = c.name;
                        confidence = c.score;
                        break;
                    }
                }
                break;
            }
            guard_admin_controller_->recordExplanation(
                ctx.request_id,
                guard::GuardExplanationBuilder::fromExternalProvider(
                    provider_name, verdict, confidence));
        }
        return StageResult::Reject;
    }

    return StageResult::Continue;
}

void ExternalSafetyStage::shadowDispatch(const RequestContext& ctx) {
    try {
        std::string text = extractTextFromRequest(ctx);
        if (text.empty()) return;

        std::vector<SafetyResult> results;
        results.reserve(providers_.size());
        if (config_.async_parallel && providers_.size() > 1) {
            std::vector<std::future<SafetyResult>> futures;
            futures.reserve(providers_.size());
            for (auto& provider : providers_) {
                auto* p = provider.get();
                futures.push_back(std::async(std::launch::async,
                    [p, &text]() { return p->check(text); }));
            }
            for (auto& f : futures) results.push_back(f.get());
        } else {
            for (auto& provider : providers_) {
                results.push_back(provider->check(text));
            }
        }

        // SR3: every shadow scan MUST write an audit entry tagged shadow=true.
        if (audit_logger_) {
            std::ostringstream detail;
            detail << "shadow=true providers=" << results.size();
            for (const auto& r : results) {
                detail << "; " << r.provider << ":";
                if (!r.success) detail << "error";
                else if (r.flagged) detail << "flagged";
                else detail << "ok";
            }
            audit_logger_->logAction(ctx.request_id, ctx.tenant_id,
                                      name(), "external_safety_shadow",
                                      detail.str());
            ++shadow_audit_writes_;
        }
    } catch (const std::exception& e) {
        spdlog::warn(
            "ExternalSafetyStage: shadow worker swallowed exception: {}",
            e.what());
    } catch (...) {
        spdlog::warn(
            "ExternalSafetyStage: shadow worker swallowed unknown exception");
    }
}

bool ExternalSafetyStage::shouldBlock(const std::vector<SafetyResult>& results) const {
    int flagged_count = 0;
    int success_count = 0;
    int error_count = 0;

    for (const auto& r : results) {
        if (!r.success) {
            error_count++;
            continue;
        }
        success_count++;
        if (r.flagged) {
            flagged_count++;
        }
    }

    if (error_count > 0 && success_count == 0) {
        return config_.fail_policy == ExternalSafetyFailPolicy::Closed;
    }

    if (error_count > 0 && config_.fail_policy == ExternalSafetyFailPolicy::Closed) {
        return true;
    }

    switch (config_.mode) {
        case ExternalSafetyMode::Any:
            return flagged_count > 0;
        case ExternalSafetyMode::All:
            return success_count > 0 && flagged_count == success_count;
        case ExternalSafetyMode::Majority:
            return success_count > 0 &&
                   flagged_count > success_count / 2;
    }
    return false;
}

} // namespace aegisgate
