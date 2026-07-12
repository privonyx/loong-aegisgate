#pragma once

// ConfigVersionRecord — POCO mirror of control_plane.v1.ConfigVersion.
//
// Lives in src/control_plane/ but is intentionally free of any proto/gRPC
// dependency so that the ConfigBundle persistence layer and business-logic
// core can be built regardless of ENABLE_CONTROL_PLANE. Only the gRPC
// adaptation layer (src/control_plane/grpc/) requires the proto generated
// code.
//
// Phase 9.3 MVP (TASK-20260420-01). Kept stable; any field reordering or
// semantic change must be reflected in proto + storage schema + audit.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

// Lifecycle of an aegisgate.yaml ConfigBundle. Mirrors proto ConfigStatus.
//
// Allowed transitions (W3 dual-approval, submitter != reviewer):
//   PENDING    --approve-->  APPROVED
//   PENDING    --reject-->   REJECTED          (terminal)
//   APPROVED   --reject-->   REJECTED          (terminal)
//   APPROVED   --activate--> ACTIVE            (previous ACTIVE -> SUPERSEDED)
//   ACTIVE     --rollback--> ACTIVE            (idempotent to self)
//   SUPERSEDED --rollback--> ACTIVE            (R2 exemption)
enum class ConfigStatus {
    PENDING,
    APPROVED,
    REJECTED,
    ACTIVE,
    SUPERSEDED,
};

// In-process representation of a single versioned bundle. Matches the
// config_versions table schema and proto ConfigVersion field for field.
struct ConfigVersionRecord {
    std::string  version_id;          // ULID (26 chars)
    std::string  content_sha256;      // hex-encoded SHA-256 of yaml_content
    std::string  yaml_content;        // raw bytes (storage uses BLOB/BYTEA)
    std::int64_t size_bytes = 0;      // redundant, convenient for list views
    ConfigStatus status = ConfigStatus::PENDING;

    // W3 submitter / reviewer
    std::string  submitter;
    std::string  submitter_comment;
    std::int64_t submitted_at = 0;    // Unix epoch milliseconds

    std::string  reviewer;
    std::string  reviewer_comment;
    std::int64_t reviewed_at = 0;

    // Activation
    std::string  activator;
    std::int64_t activated_at = 0;
    std::int64_t deactivated_at = 0;  // set when transitioning to SUPERSEDED

    // Audit chain (shared with AuditLogger chain hash)
    std::string  chain_hash;
};

// Query parameters for listConfigVersions. Empty fields mean "unbounded".
struct ConfigVersionQuery {
    std::vector<ConfigStatus> statuses;     // empty = every status
    std::int64_t              since_millis = 0;  // 0 = no lower bound
    int                       limit = 50;        // server-side caps at 500
    std::string               page_token;        // opaque cursor (typically version_id)
};

inline const char* configStatusToString(ConfigStatus s) {
    switch (s) {
        case ConfigStatus::PENDING:    return "PENDING";
        case ConfigStatus::APPROVED:   return "APPROVED";
        case ConfigStatus::REJECTED:   return "REJECTED";
        case ConfigStatus::ACTIVE:     return "ACTIVE";
        case ConfigStatus::SUPERSEDED: return "SUPERSEDED";
    }
    return "UNKNOWN";
}

inline std::optional<ConfigStatus> configStatusFromString(const std::string& s) {
    if (s == "PENDING")    return ConfigStatus::PENDING;
    if (s == "APPROVED")   return ConfigStatus::APPROVED;
    if (s == "REJECTED")   return ConfigStatus::REJECTED;
    if (s == "ACTIVE")     return ConfigStatus::ACTIVE;
    if (s == "SUPERSEDED") return ConfigStatus::SUPERSEDED;
    return std::nullopt;
}

} // namespace aegisgate
