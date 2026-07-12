#include "control_plane/grpc/config_service_grpc_adapter.h"

#include "control_plane/config_service_core.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace aegisgate::control_plane::grpc_adapter {

namespace pb = aegisgate::controlplane::v1;

// ---------------------------------------------------------------------------
// Pure converters (Task 5.1)
// ---------------------------------------------------------------------------

pb::ConfigStatus statusToProto(ConfigStatus s) {
    switch (s) {
        case ConfigStatus::PENDING:    return pb::CONFIG_STATUS_PENDING;
        case ConfigStatus::APPROVED:   return pb::CONFIG_STATUS_APPROVED;
        case ConfigStatus::REJECTED:   return pb::CONFIG_STATUS_REJECTED;
        case ConfigStatus::ACTIVE:     return pb::CONFIG_STATUS_ACTIVE;
        case ConfigStatus::SUPERSEDED: return pb::CONFIG_STATUS_SUPERSEDED;
    }
    // enum is exhaustive in C++ scope; keeping UNSPECIFIED as a silent
    // fallback would mask programmer errors, so deliberately return it
    // only when explicitly unreachable.
    return pb::CONFIG_STATUS_UNSPECIFIED;
}

std::optional<ConfigStatus> statusFromProto(pb::ConfigStatus s) {
    switch (s) {
        case pb::CONFIG_STATUS_PENDING:    return ConfigStatus::PENDING;
        case pb::CONFIG_STATUS_APPROVED:   return ConfigStatus::APPROVED;
        case pb::CONFIG_STATUS_REJECTED:   return ConfigStatus::REJECTED;
        case pb::CONFIG_STATUS_ACTIVE:     return ConfigStatus::ACTIVE;
        case pb::CONFIG_STATUS_SUPERSEDED: return ConfigStatus::SUPERSEDED;
        case pb::CONFIG_STATUS_UNSPECIFIED:
        default:
            return std::nullopt;
    }
}

pb::ConfigVersion toProto(const ConfigVersionRecord& rec) {
    pb::ConfigVersion v;
    v.set_version_id(rec.version_id);
    v.set_submitted_at(rec.submitted_at);
    v.set_submitter(rec.submitter);
    v.set_submitter_comment(rec.submitter_comment);
    v.set_yaml_content(rec.yaml_content);              // may be empty (SR11)
    v.set_content_sha256(rec.content_sha256);
    v.set_size_bytes(rec.size_bytes);
    v.set_status(statusToProto(rec.status));
    v.set_reviewer(rec.reviewer);
    v.set_reviewed_at(rec.reviewed_at);
    v.set_reviewer_comment(rec.reviewer_comment);
    v.set_activator(rec.activator);
    v.set_activated_at(rec.activated_at);
    v.set_deactivated_at(rec.deactivated_at);
    v.set_chain_hash(rec.chain_hash);
    return v;
}

ConfigVersionRecord fromProto(const pb::ConfigVersion& msg) {
    ConfigVersionRecord r;
    r.version_id = msg.version_id();
    r.submitted_at = msg.submitted_at();
    r.submitter = msg.submitter();
    r.submitter_comment = msg.submitter_comment();
    r.yaml_content = msg.yaml_content();
    r.content_sha256 = msg.content_sha256();
    r.size_bytes = msg.size_bytes();
    // Unknown proto status -> PENDING keeps this function total; upstream
    // handlers that care about validity should use statusFromProto() first.
    r.status = statusFromProto(msg.status()).value_or(ConfigStatus::PENDING);
    r.reviewer = msg.reviewer();
    r.reviewed_at = msg.reviewed_at();
    r.reviewer_comment = msg.reviewer_comment();
    r.activator = msg.activator();
    r.activated_at = msg.activated_at();
    r.deactivated_at = msg.deactivated_at();
    r.chain_hash = msg.chain_hash();
    return r;
}

// ---------------------------------------------------------------------------
// Error mapping (Task 5.2)
// ---------------------------------------------------------------------------

grpc::Status toGrpcStatus(const std::string& code, const std::string& msg) {
    // Safe-to-surface user-facing codes — the core generates these
    // deliberately and the strings are already sanitised (no host paths,
    // no YAML content echoes).
    if (code.empty()) return grpc::Status::OK;

    if (code == "PAYLOAD_TOO_LARGE")          return {grpc::INVALID_ARGUMENT, msg};
    if (code == "SENSITIVE_FIELD_DETECTED")   return {grpc::INVALID_ARGUMENT, msg};
    if (code == "INVALID_YAML")               return {grpc::INVALID_ARGUMENT, msg};
    if (code == "CONFIG_VALIDATION_FAILED")   return {grpc::INVALID_ARGUMENT, msg};
    if (code == "INVALID_ARGUMENT")           return {grpc::INVALID_ARGUMENT, msg};

    if (code == "ALREADY_EXISTS")             return {grpc::ALREADY_EXISTS, msg};
    if (code == "NOT_FOUND")                  return {grpc::NOT_FOUND, msg};
    if (code == "PERMISSION_DENIED")          return {grpc::PERMISSION_DENIED, msg};
    if (code == "UNAUTHENTICATED")            return {grpc::UNAUTHENTICATED, msg};
    if (code == "FAILED_PRECONDITION")        return {grpc::FAILED_PRECONDITION, msg};
    if (code == "RATE_LIMITED")               return {grpc::RESOURCE_EXHAUSTED, msg};
    if (code == "EMERGENCY_NOT_IMPLEMENTED")  return {grpc::UNIMPLEMENTED, msg};

    // Anything else is treated as a server-side failure we don't want to
    // advertise. SR8: collapse to a generic "internal error" so stack
    // traces / database driver messages never leak on the wire.
    return {grpc::INTERNAL, "internal error"};
}

// ---------------------------------------------------------------------------
// ConfigServiceImpl (Task 5.2)
// ---------------------------------------------------------------------------

namespace {

grpc::Status kUnauthenticated{
    grpc::UNAUTHENTICATED, "missing or invalid authentication"};

// Clones the core record but drops the yaml_content field. Shared by
// ListVersions so operators cannot accidentally exfil bundles through the
// list API (SR11 defence-in-depth — ConfigServiceCore::listVersions
// already strips, but duplicating here keeps the invariant local to the
// proto surface).
ConfigVersionRecord stripYaml(ConfigVersionRecord r) {
    r.yaml_content.clear();
    return r;
}

} // namespace

ConfigServiceImpl::ConfigServiceImpl(ConfigServiceCore* core,
                                      UserExtractor extractor)
    : core_(core), extract_user_(std::move(extractor)) {}

// --- Read RPCs (authenticated SuperAdmin still required per SR1) ---

grpc::Status ConfigServiceImpl::ListVersions(
    grpc::ServerContext* ctx,
    const pb::ListVersionsRequest* req,
    pb::ListVersionsResponse* resp) {
    if (extract_user_(ctx).empty()) return kUnauthenticated;

    ConfigVersionQuery q;
    q.since_millis = req->since_millis();
    q.limit = req->page_size() > 0 ? req->page_size() : 50;
    q.page_token = req->page_token();
    for (int i = 0; i < req->statuses_size(); ++i) {
        auto mapped = statusFromProto(req->statuses(i));
        if (!mapped) {
            return {grpc::INVALID_ARGUMENT, "unknown ConfigStatus in request"};
        }
        q.statuses.push_back(*mapped);
    }

    auto records = core_->listVersions(q);
    for (const auto& rec : records) {
        *resp->add_versions() = toProto(stripYaml(rec));
    }
    // next_page_token is reserved for future cursor support; MVP leaves it
    // empty so clients treat the page as terminal.
    resp->set_next_page_token("");
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::GetVersion(
    grpc::ServerContext* ctx,
    const pb::GetVersionRequest* req,
    pb::ConfigVersion* resp) {
    if (extract_user_(ctx).empty()) return kUnauthenticated;
    if (req->version_id().empty()) {
        return {grpc::INVALID_ARGUMENT, "version_id is required"};
    }
    auto maybe = core_->getVersion(req->version_id());
    if (!maybe) return {grpc::NOT_FOUND, "version not found"};
    *resp = toProto(*maybe);
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::GetActive(
    grpc::ServerContext* ctx,
    const pb::GetActiveRequest* /*req*/,
    pb::ConfigVersion* resp) {
    if (extract_user_(ctx).empty()) return kUnauthenticated;
    auto maybe = core_->getActive();
    if (!maybe) return {grpc::NOT_FOUND, "no active version"};
    *resp = toProto(*maybe);
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::DiffVersions(
    grpc::ServerContext* ctx,
    const pb::DiffVersionsRequest* req,
    pb::DiffVersionsResponse* resp) {
    if (extract_user_(ctx).empty()) return kUnauthenticated;
    if (req->from_version_id().empty() || req->to_version_id().empty()) {
        return {grpc::INVALID_ARGUMENT,
                "both from_version_id and to_version_id are required"};
    }
    auto r = core_->diffVersions(req->from_version_id(), req->to_version_id());
    if (!r.error_code.empty()) {
        return toGrpcStatus(r.error_code, r.error_message);
    }
    resp->set_unified_diff(r.unified_diff);
    // SemanticChange is reserved for a future release (proto DiffFormat).
    return grpc::Status::OK;
}

// --- Write RPCs (SuperAdmin; W3 dual-approval enforced in core) ---

grpc::Status ConfigServiceImpl::SubmitVersion(
    grpc::ServerContext* ctx,
    const pb::SubmitVersionRequest* req,
    pb::ConfigVersion* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = core_->submit(req->yaml_content(), user,
                            req->submitter_comment(),
                            req->validate_only());
    if (!r.error_code.empty()) {
        return toGrpcStatus(r.error_code, r.error_message);
    }
    *resp = toProto(r.record);
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::ApproveVersion(
    grpc::ServerContext* ctx,
    const pb::ApproveVersionRequest* req,
    pb::ConfigVersion* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = core_->approve(req->version_id(), user, req->reviewer_comment());
    if (!r.error_code.empty()) {
        return toGrpcStatus(r.error_code, r.error_message);
    }
    *resp = toProto(r.record);
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::RejectVersion(
    grpc::ServerContext* ctx,
    const pb::RejectVersionRequest* req,
    pb::ConfigVersion* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = core_->reject(req->version_id(), user, req->reviewer_comment());
    if (!r.error_code.empty()) {
        return toGrpcStatus(r.error_code, r.error_message);
    }
    *resp = toProto(r.record);
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::ActivateVersion(
    grpc::ServerContext* ctx,
    const pb::ActivateVersionRequest* req,
    pb::ConfigVersion* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = core_->activate(req->version_id(), user);
    if (!r.error_code.empty()) {
        return toGrpcStatus(r.error_code, r.error_message);
    }
    *resp = toProto(r.record);
    return grpc::Status::OK;
}

grpc::Status ConfigServiceImpl::RollbackVersion(
    grpc::ServerContext* ctx,
    const pb::RollbackVersionRequest* req,
    pb::ConfigVersion* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = core_->rollback(req->target_version_id(), user,
                              req->rollback_comment(),
                              req->emergency());
    if (!r.error_code.empty()) {
        return toGrpcStatus(r.error_code, r.error_message);
    }
    *resp = toProto(r.record);
    return grpc::Status::OK;
}

} // namespace aegisgate::control_plane::grpc_adapter
