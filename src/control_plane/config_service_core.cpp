#include "control_plane/config_service_core.h"

#include "control_plane/audit_bridge.h"
#include "control_plane/diff_engine.h"
#include "control_plane/sensitive_scanner.h"
#include "control_plane/state_machine.h"
#include "control_plane/ulid.h"
#include "core/crypto.h"
#include "guardrail/audit.h"
#include "storage/persistent_store.h"

#include <chrono>
#include <sstream>
#include <utility>

namespace aegisgate {

namespace {

std::int64_t wallClockMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace

ConfigServiceCore::ConfigServiceCore(Deps deps) : deps_(std::move(deps)) {
    if (!deps_.clock) {
        deps_.clock = &wallClockMillis;
    }
    if (deps_.audit) {
        bridge_ = std::make_unique<AuditBridge>(deps_.audit);
    }
}

std::int64_t ConfigServiceCore::now() const {
    return deps_.clock ? deps_.clock() : wallClockMillis();
}

ConfigServiceCore::SubmitResult ConfigServiceCore::submit(
    const std::string& yaml_content,
    const std::string& submitter_user_id,
    const std::string& submitter_comment,
    bool validate_only) {

    SubmitResult r;
    r.record.submitter = submitter_user_id;
    r.record.submitter_comment = submitter_comment;
    r.record.size_bytes = static_cast<std::int64_t>(yaml_content.size());

    // --- SR2: size cap ---
    if (yaml_content.size() > kMaxBundleBytes) {
        r.error_code = "PAYLOAD_TOO_LARGE";
        r.error_message = "yaml_content exceeds 1 MiB limit";
        return r;
    }

    // --- SR4: sensitive field scan ---
    SensitiveScanner scanner;
    auto findings = scanner.scan(yaml_content);
    if (!findings.empty()) {
        r.error_code = "SENSITIVE_FIELD_DETECTED";
        std::ostringstream msg;
        msg << "sensitive field(s) detected: ";
        for (std::size_t i = 0; i < findings.size(); ++i) {
            if (i > 0) msg << ", ";
            msg << findings[i].field_name
                << " (line " << findings[i].line << ")";
        }
        r.error_message = msg.str();
        return r;
    }

    // --- YAML syntax check (cheap gate before calling full validator) ---
    Config parse_only;
    if (!parse_only.loadFromString(yaml_content)) {
        r.error_code = "INVALID_YAML";
        r.error_message = "yaml_content is not a valid YAML document";
        return r;
    }

    // --- SR3: semantic validation ---
    if (deps_.validator) {
        auto issues = deps_.validator(yaml_content);
        bool has_error = false;
        std::ostringstream msg;
        for (const auto& is : issues) {
            if (is.severity == Config::ValidationIssue::Error) {
                if (has_error) msg << "; ";
                msg << is.field << ": " << is.message;
                has_error = true;
            }
        }
        if (has_error) {
            r.error_code = "CONFIG_VALIDATION_FAILED";
            r.error_message = msg.str();
            return r;
        }
    }

    // --- Compute content sha256 (after validation so callers cannot probe via sha alone) ---
    r.record.content_sha256 = crypto::sha256(yaml_content);

    // --- Dedupe: reject if a PENDING/APPROVED/ACTIVE version already has this sha256 ---
    if (deps_.store) {
        ConfigVersionQuery q;
        q.statuses = {ConfigStatus::PENDING, ConfigStatus::APPROVED,
                      ConfigStatus::ACTIVE};
        q.limit = 500;
        auto existing = deps_.store->listConfigVersions(q);
        for (const auto& rec : existing) {
            if (rec.content_sha256 == r.record.content_sha256) {
                r.error_code = "ALREADY_EXISTS";
                r.error_message = "a live version with this content already exists: " +
                                   rec.version_id;
                return r;
            }
        }
    }

    // --- SR10: rate limit (optional) ---
    if (deps_.rate_limit && !deps_.rate_limit(submitter_user_id)) {
        r.error_code = "RATE_LIMITED";
        r.error_message = "too many submissions from this user";
        return r;
    }

    const std::int64_t ts = now();
    r.record.submitted_at = ts;
    r.record.status = ConfigStatus::PENDING;
    r.record.yaml_content = yaml_content;

    if (validate_only) {
        // Leave version_id empty so callers don't mistake this for a live row.
        return r;
    }

    // --- Persist ---
    Ulid gen;
    r.record.version_id = gen.generate();

    if (deps_.store && !deps_.store->insertConfigVersion(r.record)) {
        r.error_code = "INTERNAL";
        r.error_message = "failed to persist config version";
        r.record.version_id.clear();
        return r;
    }

    if (bridge_) bridge_->recordSubmit(r.record);

    return r;
}

ConfigServiceCore::MutationResult ConfigServiceCore::approve(
    const std::string& version_id,
    const std::string& reviewer_user_id,
    const std::string& reviewer_comment) {
    return transitionReview(version_id, reviewer_user_id, reviewer_comment,
                             ConfigStatus::APPROVED, "config.approve");
}

ConfigServiceCore::MutationResult ConfigServiceCore::reject(
    const std::string& version_id,
    const std::string& reviewer_user_id,
    const std::string& reviewer_comment) {
    return transitionReview(version_id, reviewer_user_id, reviewer_comment,
                             ConfigStatus::REJECTED, "config.reject");
}

ConfigServiceCore::MutationResult ConfigServiceCore::transitionReview(
    const std::string& version_id,
    const std::string& reviewer_user_id,
    const std::string& reviewer_comment,
    ConfigStatus target_status,
    const std::string& audit_action) {
    MutationResult r;
    if (!deps_.store) {
        r.error_code = "INTERNAL";
        r.error_message = "no persistent store configured";
        return r;
    }

    auto existing = deps_.store->getConfigVersion(version_id);
    if (!existing.has_value()) {
        r.error_code = "NOT_FOUND";
        r.error_message = "unknown version_id: " + version_id;
        return r;
    }

    // SR5 (T14) — submitter must not be able to self-review.
    if (existing->submitter == reviewer_user_id) {
        r.error_code = "PERMISSION_DENIED";
        r.error_message = "reviewer must differ from submitter";
        return r;
    }

    ConfigAction action = target_status == ConfigStatus::APPROVED
                              ? ConfigAction::APPROVE
                              : ConfigAction::REJECT;
    auto maybe_next = StateMachine::next(existing->status, action);
    if (!maybe_next.has_value() || *maybe_next != target_status) {
        r.error_code = "FAILED_PRECONDITION";
        r.error_message = std::string("cannot transition from ") +
                          configStatusToString(existing->status) + " via " +
                          (action == ConfigAction::APPROVE ? "approve" : "reject");
        return r;
    }

    const std::int64_t ts = now();
    if (!deps_.store->updateConfigStatus(version_id, target_status,
                                          reviewer_user_id, reviewer_comment,
                                          ts)) {
        r.error_code = "INTERNAL";
        r.error_message = "failed to persist status transition";
        return r;
    }

    auto updated = deps_.store->getConfigVersion(version_id);
    if (!updated.has_value()) {
        r.error_code = "INTERNAL";
        r.error_message = "version disappeared after update";
        return r;
    }
    r.record = *updated;

    if (bridge_) {
        if (audit_action == "config.approve") {
            bridge_->recordApprove(version_id, reviewer_user_id, reviewer_comment);
        } else {
            bridge_->recordReject(version_id, reviewer_user_id, reviewer_comment);
        }
    }
    return r;
}

ConfigServiceCore::MutationResult ConfigServiceCore::activate(
    const std::string& version_id,
    const std::string& activator_user_id) {
    MutationResult r;
    if (!deps_.store) {
        r.error_code = "INTERNAL";
        r.error_message = "no persistent store configured";
        return r;
    }

    auto existing = deps_.store->getConfigVersion(version_id);
    if (!existing.has_value()) {
        r.error_code = "NOT_FOUND";
        r.error_message = "unknown version_id: " + version_id;
        return r;
    }

    auto maybe_next = StateMachine::next(existing->status, ConfigAction::ACTIVATE);
    if (!maybe_next.has_value() || *maybe_next != ConfigStatus::ACTIVE) {
        r.error_code = "FAILED_PRECONDITION";
        r.error_message = std::string("cannot activate from status ") +
                          configStatusToString(existing->status);
        return r;
    }

    auto prev = deps_.store->getActiveConfig();
    std::string previous_id = prev ? prev->version_id : std::string{};

    if (!deps_.store->activateConfig(version_id, activator_user_id, now())) {
        r.error_code = "INTERNAL";
        r.error_message = "activateConfig failed";
        return r;
    }

    auto updated = deps_.store->getConfigVersion(version_id);
    if (!updated.has_value()) {
        r.error_code = "INTERNAL";
        r.error_message = "version disappeared after activation";
        return r;
    }
    r.record = *updated;

    if (bridge_) {
        bridge_->recordActivate(version_id, activator_user_id, previous_id);
    }
    return r;
}

ConfigServiceCore::MutationResult ConfigServiceCore::rollback(
    const std::string& target_version_id,
    const std::string& activator_user_id,
    const std::string& comment,
    bool emergency) {
    MutationResult r;

    // R3 (SR9): emergency is reserved but not implemented in Phase 9.3.
    if (emergency) {
        r.error_code = "EMERGENCY_NOT_IMPLEMENTED";
        r.error_message = "emergency rollback is reserved for Phase 12";
        return r;
    }

    if (!deps_.store) {
        r.error_code = "INTERNAL";
        r.error_message = "no persistent store configured";
        return r;
    }

    auto target = deps_.store->getConfigVersion(target_version_id);
    if (!target.has_value()) {
        r.error_code = "NOT_FOUND";
        r.error_message = "unknown target version_id: " + target_version_id;
        return r;
    }

    auto maybe_next = StateMachine::next(target->status, ConfigAction::ROLLBACK_TO);
    if (!maybe_next.has_value()) {
        r.error_code = "FAILED_PRECONDITION";
        r.error_message = "target status " +
                          std::string(configStatusToString(target->status)) +
                          " cannot be rolled back to; use ActivateVersion for "
                          "approved-but-not-activated versions";
        return r;
    }

    auto prev = deps_.store->getActiveConfig();
    std::string previous_id = prev ? prev->version_id : std::string{};

    // Idempotent rollback to the current active — no store mutation needed.
    if (target->status == ConfigStatus::ACTIVE) {
        r.record = *target;
    } else {
        if (!deps_.store->activateConfig(target_version_id, activator_user_id,
                                          now())) {
            r.error_code = "INTERNAL";
            r.error_message = "activateConfig failed during rollback";
            return r;
        }
        auto updated = deps_.store->getConfigVersion(target_version_id);
        if (!updated.has_value()) {
            r.error_code = "INTERNAL";
            r.error_message = "version disappeared after rollback";
            return r;
        }
        r.record = *updated;
    }

    if (bridge_) {
        bridge_->recordRollback(target_version_id, activator_user_id,
                                 previous_id, comment);
    }
    return r;
}

std::vector<ConfigVersionRecord> ConfigServiceCore::listVersions(
    const ConfigVersionQuery& q) {
    std::vector<ConfigVersionRecord> out;
    if (!deps_.store) return out;
    out = deps_.store->listConfigVersions(q);
    // SR11 — list responses must never leak yaml_content. Clients wanting the
    // body call getVersion(id) explicitly which is audit-logged separately.
    for (auto& r : out) {
        r.yaml_content.clear();
    }
    return out;
}

std::optional<ConfigVersionRecord> ConfigServiceCore::getVersion(
    const std::string& version_id) {
    if (!deps_.store) return std::nullopt;
    return deps_.store->getConfigVersion(version_id);
}

std::optional<ConfigVersionRecord> ConfigServiceCore::getActive() {
    if (!deps_.store) return std::nullopt;
    return deps_.store->getActiveConfig();
}

ConfigServiceCore::DiffResult ConfigServiceCore::diffVersions(
    const std::string& from_version_id,
    const std::string& to_version_id) {
    DiffResult r;
    if (!deps_.store) {
        r.error_code = "INTERNAL";
        r.error_message = "no persistent store configured";
        return r;
    }
    auto from = deps_.store->getConfigVersion(from_version_id);
    auto to   = deps_.store->getConfigVersion(to_version_id);
    if (!from.has_value() || !to.has_value()) {
        r.error_code = "NOT_FOUND";
        r.error_message = !from.has_value()
            ? "unknown from version_id: " + from_version_id
            : "unknown to version_id: " + to_version_id;
        return r;
    }
    DiffEngine de;
    r.unified_diff = de.unifiedDiff(from->yaml_content, to->yaml_content);
    return r;
}

} // namespace aegisgate
