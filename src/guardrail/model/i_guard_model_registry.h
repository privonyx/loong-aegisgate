#pragma once

// Phase 11.1 TASK-20260523-01 — Epic 1.2 IGuardModelRegistry.
//
// D1=C "registry + ABI 双层" decision: model metadata is owned by this
// registry (insert/get/list/promote/revert) while the actual ONNX runtime
// load happens via a separate ABI (kept out of scope here so MemoryGuard /
// SQLiteGuard can be tested without the GUARD_MODEL build flag).
//
// Status state-machine (D1 / SR-NEW1):
//
//     Shadow ──promote──> Live ──revert──> Retired
//                              └──promote─other───> demote to Retired
//     Shadow ──revert────────────────────> ILLEGAL  (no live to revert)
//     Retired ──promote──────────────────> ILLEGAL  (one-way terminal)
//
// Promote enforces the at-most-one-Live invariant per model_id (demotes the
// previous Live to Retired in the same atomic step).
//
// Design reference: docs/specs/2026-05-23-phase11.1-adaptive-guard-design.md §4.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegisgate::guard {

enum class GuardModelStatus {
    Shadow = 0,
    Live = 1,
    Retired = 2,
};

inline std::string_view statusToString(GuardModelStatus s) {
    switch (s) {
        case GuardModelStatus::Shadow: return "shadow";
        case GuardModelStatus::Live: return "live";
        case GuardModelStatus::Retired: return "retired";
    }
    return "unknown";
}

inline std::optional<GuardModelStatus> statusFromString(std::string_view s) {
    if (s == "shadow") return GuardModelStatus::Shadow;
    if (s == "live") return GuardModelStatus::Live;
    if (s == "retired") return GuardModelStatus::Retired;
    return std::nullopt;
}

struct ModelRegistryRecord {
    std::string model_id;
    std::string version;
    std::string path;
    float classifier_threshold = 0.5f;
    GuardModelStatus status = GuardModelStatus::Shadow;
    std::int64_t promoted_at_ms = 0;
    // SHA-256 of the artifact bytes. Frozen at insert(); promote/revert
    // MUST NOT mutate this field (T01 tamper defense).
    std::string artifact_sha256;
    // JSON blob of shadow-period metrics summary (win_rate, fp_rate, etc.).
    std::string metrics_summary;
};

struct RegistryOpResult {
    bool ok = false;
    // Stable, machine-readable error code. Empty on success.
    std::string error_code;
    // Human-readable detail; safe to surface to admins.
    std::string detail;

    static RegistryOpResult success() { return {true, {}, {}}; }
    static RegistryOpResult fail(std::string code, std::string detail = {}) {
        return {false, std::move(code), std::move(detail)};
    }
};

class IGuardModelRegistry {
public:
    virtual ~IGuardModelRegistry() = default;

    virtual RegistryOpResult insert(const ModelRegistryRecord& record) = 0;

    virtual std::optional<ModelRegistryRecord> get(
        const std::string& model_id, const std::string& version) const = 0;

    virtual std::vector<ModelRegistryRecord> list(
        const std::string& model_id) const = 0;

    virtual std::vector<ModelRegistryRecord> listByStatus(
        const std::string& model_id, GuardModelStatus status) const = 0;

    // Atomically transition the given version to Live. If another version is
    // currently Live for the same model_id, it is demoted to Retired in the
    // same operation. Returns illegal_transition if the target is already
    // Retired.
    virtual RegistryOpResult promote(const std::string& model_id,
                                     const std::string& version,
                                     std::int64_t promoted_at_ms) = 0;

    // Mark a Live version as Retired (instant rollback). Shadow -> Retired
    // is illegal (you only revert what was promoted).
    virtual RegistryOpResult revert(const std::string& model_id,
                                    const std::string& version) = 0;
};

}  // namespace aegisgate::guard
