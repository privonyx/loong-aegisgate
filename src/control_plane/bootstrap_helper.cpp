#include "control_plane/bootstrap_helper.h"

#include "control_plane/config_service_core.h"
#include "storage/persistent_store.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace aegisgate::control_plane::bootstrap {

namespace {

// Actor string used exclusively by the bootstrap path. Hard-coded so that
// audit logs unambiguously mark the pre-existing ACTIVE entry as
// system-provisioned rather than originating from a human.
constexpr const char* kBootstrapActor = "system_bootstrap";

bool storeHasAnyVersion(PersistentStore* store) {
    ConfigVersionQuery q;
    q.limit = 1;
    // No status filter — we want to detect a row in any state, including
    // REJECTED/SUPERSEDED so we never overwrite historical data on an
    // operational deployment that happens to have zero ACTIVE rows.
    auto rows = store->listConfigVersions(q);
    return !rows.empty();
}

bool readFileToString(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    if (!oss) return false;
    out = oss.str();
    return true;
}

} // namespace

Result bootstrapFromActiveYamlIfEmpty(
    PersistentStore* store,
    ConfigServiceCore* core,
    const std::string& bootstrap_yaml_path) {
    Result r;

    if (!store || !core) {
        // Defensive — callers should never pass null, but if they do we
        // fail closed rather than dereferencing.
        r.outcome = Outcome::SkippedNoBootstrap;
        r.error_code = "INVALID_ARGUMENT";
        r.error_message = "store or core is null";
        return r;
    }

    if (bootstrap_yaml_path.empty()) {
        r.outcome = Outcome::SkippedNoBootstrap;
        return r;
    }

    if (storeHasAnyVersion(store)) {
        r.outcome = Outcome::SkippedNotEmpty;
        return r;
    }

    std::string yaml;
    if (!readFileToString(bootstrap_yaml_path, yaml)) {
        r.outcome = Outcome::FileReadFailed;
        r.error_code = "BOOTSTRAP_FILE_READ_FAILED";
        r.error_message = "cannot read bootstrap yaml: " + bootstrap_yaml_path;
        return r;
    }

    // Step 1 — Submit through the real core so SR2 (size), SR3 (validator)
    // and SR4 (sensitive scanner) run exactly as they would for a human
    // submission. A malformed bootstrap file must fail with the same
    // error_code the aegisctl caller would have seen.
    auto sub = core->submit(yaml, kBootstrapActor,
                             "bootstrap from " + bootstrap_yaml_path,
                             /*validate_only=*/false);
    if (!sub.error_code.empty()) {
        r.outcome = Outcome::SubmitFailed;
        r.error_code = sub.error_code;
        r.error_message = sub.error_message;
        spdlog::error("control-plane bootstrap: submit failed ({}): {}",
                      sub.error_code, sub.error_message);
        return r;
    }
    r.version_id = sub.record.version_id;

    // Step 2 — Elevate directly via the store, skipping core->approve /
    // core->activate. SR5 (`submitter != reviewer`) intentionally blocks
    // any path in core where the same actor tries to self-approve; we
    // can bypass that invariant here because `storeHasAnyVersion` just
    // proved the table was empty and this branch runs at most once per
    // install. Going through store->updateConfigStatus + activateConfig
    // preserves the audit chain hash (AuditBridge already logged the
    // submit) and leaves SR5 fully enforced for every other path.
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (!store->updateConfigStatus(sub.record.version_id,
                                    ConfigStatus::APPROVED,
                                    kBootstrapActor,
                                    "bootstrap auto-approve",
                                    now_ms)) {
        r.outcome = Outcome::ApproveFailed;
        r.error_code = "BOOTSTRAP_APPROVE_FAILED";
        r.error_message = "store->updateConfigStatus returned false";
        spdlog::error("control-plane bootstrap: approve failed for {}",
                      sub.record.version_id);
        return r;
    }

    if (!store->activateConfig(sub.record.version_id, kBootstrapActor,
                                 now_ms)) {
        r.outcome = Outcome::ActivateFailed;
        r.error_code = "BOOTSTRAP_ACTIVATE_FAILED";
        r.error_message = "store->activateConfig returned false";
        spdlog::error("control-plane bootstrap: activate failed for {}",
                      sub.record.version_id);
        return r;
    }

    r.outcome = Outcome::Bootstrapped;
    spdlog::warn("control-plane bootstrap: seeded config_versions from {} "
                 "(version_id={})",
                 bootstrap_yaml_path, r.version_id);
    return r;
}

} // namespace aegisgate::control_plane::bootstrap
