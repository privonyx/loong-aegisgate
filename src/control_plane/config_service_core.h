#pragma once

// Phase 9.3 Epic 3 — ConfigServiceCore (pure C++ business logic layer).
//
// Sits between the gRPC handler layer (src/control_plane/grpc/, built only
// when ENABLE_CONTROL_PLANE=ON) and the persistence / audit / validation
// collaborators. Kept as ENABLE_CONTROL_PLANE-agnostic so the OFF build path
// exercises the same state-machine and SR* enforcement as ON.
//
// Threat-model enforcement is spread across the methods of this class:
//   * SR2  — 1 MiB cap on yaml_content in submit().
//   * SR3  — semantic Config validation via the injected validator callback.
//   * SR4  — SensitiveScanner run before persistence.
//   * SR5  — submitter != reviewer check in approve() / reject() (Task 3.6).
//   * SR9  — emergency rollback gated off in rollback() (Task 3.9).
//   * SR10 — optional per-user rate limit hook.
//   * SR11 — list responses clear yaml_content (Task 3.10).

#include "control_plane/audit_bridge.h"
#include "control_plane/config_version_record.h"
#include "core/config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

class PersistentStore;
class AuditLogger;

class ConfigServiceCore {
public:
    // Callback that validates a YAML string and returns issues. Empty result
    // (or Warning-only) means the submission is acceptable for persistence.
    // Errors (`Severity::Error`) block the submit with CONFIG_VALIDATION_FAILED.
    // Tests inject a stub; production wires `Config::loadFromString` + validate.
    using Validator = std::function<std::vector<Config::ValidationIssue>(
        const std::string&)>;

    // Optional pre-persist rate-limit hook — returns true to allow, false to
    // reject with RATE_LIMITED. Left unset in tests that don't exercise SR10.
    using RateLimitHook = std::function<bool(const std::string& /*user_id*/)>;

    using Clock = std::function<std::int64_t()>;

    struct Deps {
        PersistentStore* store = nullptr;
        AuditLogger*     audit = nullptr;
        Validator        validator;
        RateLimitHook    rate_limit;      // optional
        Clock            clock;           // optional; defaults to wall clock
    };

    struct SubmitResult {
        ConfigVersionRecord record;
        std::string error_code;    // "" on success
        std::string error_message;
    };

    explicit ConfigServiceCore(Deps deps);

    // Shared result type for mutating operations that return the updated
    // record; error_code == "" means success.
    struct MutationResult {
        ConfigVersionRecord record;
        std::string error_code;
        std::string error_message;
    };

    // Read-side result for diffVersions.
    struct DiffResult {
        std::string unified_diff;
        std::string error_code;
        std::string error_message;
    };

    // SR2/SR3/SR4-gated write. When `validate_only` is true, does not persist
    // and leaves record.version_id empty while populating derived fields.
    SubmitResult submit(const std::string& yaml_content,
                        const std::string& submitter_user_id,
                        const std::string& submitter_comment,
                        bool validate_only);

    // W3 approve (Task 3.6). Enforces SR5: reviewer_user_id != submitter.
    MutationResult approve(const std::string& version_id,
                           const std::string& reviewer_user_id,
                           const std::string& reviewer_comment);

    // W3 reject (Task 3.7). Same SR5 guard; allowed from PENDING or APPROVED.
    MutationResult reject(const std::string& version_id,
                          const std::string& reviewer_user_id,
                          const std::string& reviewer_comment);

    // Atomic activation (Task 3.8). Delegates the transaction to the
    // PersistentStore; validates that the target is APPROVED first.
    MutationResult activate(const std::string& version_id,
                            const std::string& activator_user_id);

    // R2 rollback (Task 3.9). `emergency=true` returns
    // EMERGENCY_NOT_IMPLEMENTED (R3 reservation, SR9).
    MutationResult rollback(const std::string& target_version_id,
                            const std::string& activator_user_id,
                            const std::string& comment,
                            bool emergency);

    // Read helpers (Task 3.10). listVersions strips yaml_content (SR11).
    std::vector<ConfigVersionRecord> listVersions(const ConfigVersionQuery& q);
    std::optional<ConfigVersionRecord> getVersion(const std::string& version_id);
    std::optional<ConfigVersionRecord> getActive();
    DiffResult diffVersions(const std::string& from_version_id,
                            const std::string& to_version_id);

private:
    // Shared implementation for approve/reject since both differ only in
    // target state.
    MutationResult transitionReview(const std::string& version_id,
                                    const std::string& reviewer_user_id,
                                    const std::string& reviewer_comment,
                                    ConfigStatus target_status,
                                    const std::string& audit_action);

    // Owned AuditBridge built from deps_.audit (if non-null). Keeping it on
    // the core rather than in Deps avoids forcing every caller to construct
    // the bridge separately; the tests that inspect audit entries still
    // observe the exact same side-effects.
    std::unique_ptr<AuditBridge> bridge_;
    // Cap on yaml_content size (SR2). 1 MiB matches the proto/gRPC default
    // max-receive-message-size we keep for the control-plane server so the
    // server never silently drops oversized payloads.
    static constexpr std::size_t kMaxBundleBytes = 1024 * 1024;

    std::int64_t now() const;

    Deps deps_;
};

} // namespace aegisgate
