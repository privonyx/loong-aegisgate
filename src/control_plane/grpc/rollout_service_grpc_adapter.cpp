#include "control_plane/grpc/rollout_service_grpc_adapter.h"

#include "control_plane/grpc/config_service_grpc_adapter.h"  // toGrpcStatus()
#include "control_plane/rollout/rollout_controller.h"

#include <string>
#include <utility>

namespace aegisgate::control_plane::grpc_adapter {

namespace pb = aegisgate::controlplane::v1;

// ---------------------------------------------------------------------------
// Pure converters
// ---------------------------------------------------------------------------

pb::RolloutStatus rolloutStatusToProto(RolloutStatus s) {
    switch (s) {
        case RolloutStatus::PENDING:     return pb::ROLLOUT_STATUS_PENDING;
        case RolloutStatus::PROGRESSING: return pb::ROLLOUT_STATUS_PROGRESSING;
        case RolloutStatus::PAUSED:      return pb::ROLLOUT_STATUS_PAUSED;
        case RolloutStatus::COMPLETED:   return pb::ROLLOUT_STATUS_COMPLETED;
        case RolloutStatus::FAILED:      return pb::ROLLOUT_STATUS_FAILED;
        case RolloutStatus::ABORTED:     return pb::ROLLOUT_STATUS_ABORTED;
    }
    return pb::ROLLOUT_STATUS_UNSPECIFIED;
}

std::optional<RolloutStatus> rolloutStatusFromProto(pb::RolloutStatus s) {
    switch (s) {
        case pb::ROLLOUT_STATUS_PENDING:     return RolloutStatus::PENDING;
        case pb::ROLLOUT_STATUS_PROGRESSING: return RolloutStatus::PROGRESSING;
        case pb::ROLLOUT_STATUS_PAUSED:      return RolloutStatus::PAUSED;
        case pb::ROLLOUT_STATUS_COMPLETED:   return RolloutStatus::COMPLETED;
        case pb::ROLLOUT_STATUS_FAILED:      return RolloutStatus::FAILED;
        case pb::ROLLOUT_STATUS_ABORTED:     return RolloutStatus::ABORTED;
        case pb::ROLLOUT_STATUS_UNSPECIFIED:
        default:
            return std::nullopt;
    }
}

pb::PauseReason pauseReasonToProto(PauseReason r) {
    switch (r) {
        case PauseReason::UNSPECIFIED:   return pb::PAUSE_REASON_UNSPECIFIED;
        case PauseReason::MANUAL:        return pb::PAUSE_REASON_MANUAL;
        case PauseReason::ERROR_RATE:    return pb::PAUSE_REASON_ERROR_RATE;
        case PauseReason::LATENCY_RATIO: return pb::PAUSE_REASON_LATENCY_RATIO;
        case PauseReason::AUTO_ROLLBACK: return pb::PAUSE_REASON_AUTO_ROLLBACK;
    }
    return pb::PAUSE_REASON_UNSPECIFIED;
}

std::optional<PauseReason> pauseReasonFromProto(pb::PauseReason r) {
    switch (r) {
        case pb::PAUSE_REASON_MANUAL:        return PauseReason::MANUAL;
        case pb::PAUSE_REASON_ERROR_RATE:    return PauseReason::ERROR_RATE;
        case pb::PAUSE_REASON_LATENCY_RATIO: return PauseReason::LATENCY_RATIO;
        case pb::PAUSE_REASON_AUTO_ROLLBACK: return PauseReason::AUTO_ROLLBACK;
        case pb::PAUSE_REASON_UNSPECIFIED:
        default:
            return PauseReason::UNSPECIFIED;
    }
}

// --- Spec converters -------------------------------------------------------

pb::RolloutSpec rolloutSpecToProto(const RolloutSpec& spec) {
    pb::RolloutSpec out;
    out.set_target_version_id(spec.target_version_id);
    out.set_sticky_key(spec.sticky_key);
    out.set_auto_rollback_on_pause(spec.auto_rollback_on_pause);
    out.set_auto_rollback_grace_seconds(spec.auto_rollback_grace_seconds);
    out.set_creator_comment(spec.creator_comment);
    for (const auto& stg : spec.stages) {
        auto* ps = out.add_stages();
        ps->set_name(stg.name);
        auto* sc = ps->mutable_scope();
        for (const auto& g : stg.scope.tenant_globs) sc->add_tenant_globs(g);
        for (const auto& r : stg.scope.regions) sc->add_regions(r);
        sc->set_percentage(stg.scope.percentage);
        auto* obs = ps->mutable_observation();
        obs->set_min_duration_seconds(stg.observation.min_duration_seconds);
        obs->set_min_sample_count(stg.observation.min_sample_count);
        auto* ap = ps->mutable_auto_pause();
        ap->set_error_rate_gt(stg.auto_pause.error_rate_gt);
        ap->set_p99_latency_ratio_gt(stg.auto_pause.p99_latency_ratio_gt);
        ap->set_absolute_error_rate_gt(stg.auto_pause.absolute_error_rate_gt);
        ap->set_absolute_p99_latency_ms_gt(stg.auto_pause.absolute_p99_latency_ms_gt);
    }
    return out;
}

RolloutSpec rolloutSpecFromProto(const pb::RolloutSpec& msg) {
    RolloutSpec spec;
    spec.target_version_id = msg.target_version_id();
    spec.sticky_key = msg.sticky_key();
    spec.auto_rollback_on_pause = msg.auto_rollback_on_pause();
    spec.auto_rollback_grace_seconds = msg.auto_rollback_grace_seconds();
    spec.creator_comment = msg.creator_comment();
    for (int i = 0; i < msg.stages_size(); ++i) {
        const auto& ps = msg.stages(i);
        RolloutStageRecord stg;
        stg.name = ps.name();
        for (int j = 0; j < ps.scope().tenant_globs_size(); ++j)
            stg.scope.tenant_globs.push_back(ps.scope().tenant_globs(j));
        for (int j = 0; j < ps.scope().regions_size(); ++j)
            stg.scope.regions.push_back(ps.scope().regions(j));
        stg.scope.percentage = ps.scope().percentage();
        stg.observation.min_duration_seconds = ps.observation().min_duration_seconds();
        stg.observation.min_sample_count = ps.observation().min_sample_count();
        stg.auto_pause.error_rate_gt = ps.auto_pause().error_rate_gt();
        stg.auto_pause.p99_latency_ratio_gt = ps.auto_pause().p99_latency_ratio_gt();
        stg.auto_pause.absolute_error_rate_gt = ps.auto_pause().absolute_error_rate_gt();
        stg.auto_pause.absolute_p99_latency_ms_gt = ps.auto_pause().absolute_p99_latency_ms_gt();
        spec.stages.push_back(std::move(stg));
    }
    return spec;
}

// --- Rollout record converters ---------------------------------------------

pb::Rollout rolloutToProto(const RolloutRecord& rec) {
    pb::Rollout out;
    out.set_rollout_id(rec.rollout_id);
    out.set_target_version_id(rec.target_version_id);
    out.set_previous_active_version_id(rec.previous_active_version_id);
    *out.mutable_spec() = rolloutSpecToProto(rec.spec);
    out.set_status(rolloutStatusToProto(rec.status));
    out.set_current_stage_index(rec.current_stage_index);
    out.set_started_at(rec.started_at);
    out.set_stage_started_at(rec.stage_started_at);
    out.set_paused_at(rec.paused_at);
    out.set_pause_reason(pauseReasonToProto(rec.pause_reason));
    out.set_pause_detail(rec.pause_detail);
    out.set_creator(rec.creator);
    out.set_last_actor(rec.last_actor);
    out.set_completed_at(rec.completed_at);
    out.set_chain_hash(rec.chain_hash);
    return out;
}

RolloutRecord rolloutFromProto(const pb::Rollout& msg) {
    RolloutRecord rec;
    rec.rollout_id = msg.rollout_id();
    rec.target_version_id = msg.target_version_id();
    rec.previous_active_version_id = msg.previous_active_version_id();
    rec.spec = rolloutSpecFromProto(msg.spec());
    rec.status = rolloutStatusFromProto(msg.status()).value_or(RolloutStatus::PENDING);
    rec.current_stage_index = msg.current_stage_index();
    rec.started_at = msg.started_at();
    rec.stage_started_at = msg.stage_started_at();
    rec.paused_at = msg.paused_at();
    rec.pause_reason = pauseReasonFromProto(msg.pause_reason()).value_or(PauseReason::UNSPECIFIED);
    rec.pause_detail = msg.pause_detail();
    rec.creator = msg.creator();
    rec.last_actor = msg.last_actor();
    rec.completed_at = msg.completed_at();
    rec.chain_hash = msg.chain_hash();
    return rec;
}

// ---------------------------------------------------------------------------
// RolloutServiceImpl
// ---------------------------------------------------------------------------

namespace {
grpc::Status kUnauthenticated{grpc::UNAUTHENTICATED, "missing or invalid authentication"};
} // namespace

RolloutServiceImpl::RolloutServiceImpl(RolloutController* ctrl,
                                        UserExtractor extractor)
    : ctrl_(ctrl), extract_user_(std::move(extractor)) {}

grpc::Status RolloutServiceImpl::GetRollout(
    grpc::ServerContext* ctx,
    const pb::GetRolloutRequest* req,
    pb::Rollout* resp) {
    if (extract_user_(ctx).empty()) return kUnauthenticated;
    if (req->rollout_id().empty()) {
        return {grpc::INVALID_ARGUMENT, "rollout_id is required"};
    }
    auto maybe = ctrl_->getRollout(req->rollout_id());
    if (!maybe) return {grpc::NOT_FOUND, "rollout not found"};
    *resp = rolloutToProto(*maybe);
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::ListRollouts(
    grpc::ServerContext* ctx,
    const pb::ListRolloutsRequest* req,
    pb::ListRolloutsResponse* resp) {
    if (extract_user_(ctx).empty()) return kUnauthenticated;
    RolloutQuery q;
    for (int i = 0; i < req->statuses_size(); ++i) {
        auto mapped = rolloutStatusFromProto(req->statuses(i));
        if (!mapped) {
            return {grpc::INVALID_ARGUMENT, "unknown RolloutStatus in request"};
        }
        q.statuses.push_back(*mapped);
    }
    q.limit = req->page_size() > 0 ? req->page_size() : 50;
    q.page_token = req->page_token();
    auto records = ctrl_->listRollouts(q);
    for (const auto& rec : records) {
        *resp->add_rollouts() = rolloutToProto(rec);
    }
    resp->set_next_page_token("");
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::CreateRollout(
    grpc::ServerContext* ctx,
    const pb::CreateRolloutRequest* req,
    pb::Rollout* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto spec = rolloutSpecFromProto(req->spec());
    auto r = ctrl_->createRollout(spec, user);
    if (!r.error_code.empty()) return toGrpcStatus(r.error_code, r.error_message);
    *resp = rolloutToProto(r.record);
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::StartRollout(
    grpc::ServerContext* ctx,
    const pb::StartRolloutRequest* req,
    pb::Rollout* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = ctrl_->startRollout(req->rollout_id(), user);
    if (!r.error_code.empty()) return toGrpcStatus(r.error_code, r.error_message);
    *resp = rolloutToProto(r.record);
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::PauseRollout(
    grpc::ServerContext* ctx,
    const pb::PauseRolloutRequest* req,
    pb::Rollout* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = ctrl_->pauseRollout(req->rollout_id(), user, req->comment());
    if (!r.error_code.empty()) return toGrpcStatus(r.error_code, r.error_message);
    *resp = rolloutToProto(r.record);
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::ResumeRollout(
    grpc::ServerContext* ctx,
    const pb::ResumeRolloutRequest* req,
    pb::Rollout* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = ctrl_->resumeRollout(req->rollout_id(), user);
    if (!r.error_code.empty()) return toGrpcStatus(r.error_code, r.error_message);
    *resp = rolloutToProto(r.record);
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::PromoteRollout(
    grpc::ServerContext* ctx,
    const pb::PromoteRolloutRequest* req,
    pb::Rollout* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = ctrl_->promoteStage(req->rollout_id(), user);
    if (!r.error_code.empty()) return toGrpcStatus(r.error_code, r.error_message);
    *resp = rolloutToProto(r.record);
    return grpc::Status::OK;
}

grpc::Status RolloutServiceImpl::AbortRollout(
    grpc::ServerContext* ctx,
    const pb::AbortRolloutRequest* req,
    pb::Rollout* resp) {
    std::string user = extract_user_(ctx);
    if (user.empty()) return kUnauthenticated;
    auto r = ctrl_->abortRollout(req->rollout_id(), user);
    if (!r.error_code.empty()) return toGrpcStatus(r.error_code, r.error_message);
    *resp = rolloutToProto(r.record);
    return grpc::Status::OK;
}

} // namespace aegisgate::control_plane::grpc_adapter
