#include "cli/rollout_cli.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace aegisgate::cli {

namespace pb = ::aegisgate::controlplane::v1;

// ---------------------------------------------------------------------------
// gRPC production client
// ---------------------------------------------------------------------------

class GrpcRolloutServiceClient final : public RolloutServiceClient {
public:
    explicit GrpcRolloutServiceClient(ControlPlaneClient& base)
        : base_(base),
          stub_(pb::RolloutService::NewStub(base.channel())) {}

    grpc::Status Create(const pb::CreateRolloutRequest& req,
                        pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->CreateRollout(&ctx, req, out);
    }
    grpc::Status Get(const pb::GetRolloutRequest& req,
                     pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->GetRollout(&ctx, req, out);
    }
    grpc::Status List(const pb::ListRolloutsRequest& req,
                      pb::ListRolloutsResponse* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->ListRollouts(&ctx, req, out);
    }
    grpc::Status Start(const pb::StartRolloutRequest& req,
                       pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->StartRollout(&ctx, req, out);
    }
    grpc::Status Pause(const pb::PauseRolloutRequest& req,
                       pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->PauseRollout(&ctx, req, out);
    }
    grpc::Status Resume(const pb::ResumeRolloutRequest& req,
                        pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->ResumeRollout(&ctx, req, out);
    }
    grpc::Status Promote(const pb::PromoteRolloutRequest& req,
                         pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->PromoteRollout(&ctx, req, out);
    }
    grpc::Status Abort(const pb::AbortRolloutRequest& req,
                       pb::Rollout* out) override {
        grpc::ClientContext ctx; base_.prepareContext(ctx);
        return stub_->AbortRollout(&ctx, req, out);
    }

private:
    ControlPlaneClient& base_;
    std::unique_ptr<pb::RolloutService::Stub> stub_;
};

std::unique_ptr<RolloutServiceClient> makeGrpcRolloutServiceClient(
    ControlPlaneClient& base) {
    return std::make_unique<GrpcRolloutServiceClient>(base);
}

// ---------------------------------------------------------------------------
// Arg parsers
// ---------------------------------------------------------------------------

RolloutCreateArgs parseRolloutCreateArgs(const std::vector<std::string>& argv) {
    RolloutCreateArgs a;
    for (size_t i = 0; i < argv.size(); ++i) {
        if ((argv[i] == "--spec" || argv[i] == "-f") && i + 1 < argv.size()) {
            a.spec_file = argv[++i];
        }
    }
    if (a.spec_file.empty())
        throw std::invalid_argument("--spec FILE is required");
    return a;
}

RolloutIdArgs parseRolloutIdArgs(const std::vector<std::string>& argv) {
    RolloutIdArgs a;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (argv[i] == "--rollout-id" && i + 1 < argv.size()) {
            a.rollout_id = argv[++i];
        } else if (argv[i] == "--comment" && i + 1 < argv.size()) {
            a.comment = argv[++i];
        }
    }
    if (a.rollout_id.empty())
        throw std::invalid_argument("--rollout-id ID is required");
    return a;
}

RolloutListArgs parseRolloutListArgs(const std::vector<std::string>& argv) {
    RolloutListArgs a;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (argv[i] == "--page-size" && i + 1 < argv.size()) {
            a.page_size = std::stoi(argv[++i]);
        } else if (argv[i] == "--page-token" && i + 1 < argv.size()) {
            a.page_token = argv[++i];
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

std::string rolloutStatusToString(pb::RolloutStatus s) {
    switch (s) {
        case pb::ROLLOUT_STATUS_PENDING:     return "PENDING";
        case pb::ROLLOUT_STATUS_PROGRESSING: return "PROGRESSING";
        case pb::ROLLOUT_STATUS_PAUSED:      return "PAUSED";
        case pb::ROLLOUT_STATUS_COMPLETED:   return "COMPLETED";
        case pb::ROLLOUT_STATUS_FAILED:      return "FAILED";
        case pb::ROLLOUT_STATUS_ABORTED:     return "ABORTED";
        default: return "UNKNOWN";
    }
}

std::string pauseReasonToString(pb::PauseReason r) {
    switch (r) {
        case pb::PAUSE_REASON_MANUAL:        return "MANUAL";
        case pb::PAUSE_REASON_ERROR_RATE:    return "ERROR_RATE";
        case pb::PAUSE_REASON_LATENCY_RATIO: return "LATENCY_RATIO";
        case pb::PAUSE_REASON_AUTO_ROLLBACK: return "AUTO_ROLLBACK";
        default: return "";
    }
}

namespace {

void renderRolloutTable(std::ostream& out, const pb::Rollout& r) {
    out << "rollout_id:       " << r.rollout_id() << "\n"
        << "target_version:   " << r.target_version_id() << "\n"
        << "status:           " << rolloutStatusToString(r.status()) << "\n"
        << "stage_index:      " << r.current_stage_index() << "\n";
    if (r.pause_reason() != pb::PAUSE_REASON_UNSPECIFIED) {
        out << "pause_reason:     " << pauseReasonToString(r.pause_reason()) << "\n";
    }
    if (!r.pause_detail().empty()) {
        out << "pause_detail:     " << r.pause_detail() << "\n";
    }
    out << "creator:          " << r.creator() << "\n"
        << "last_actor:       " << r.last_actor() << "\n";
}

void renderRolloutJson(std::ostream& out, const pb::Rollout& r) {
    nlohmann::json j;
    j["rollout_id"] = r.rollout_id();
    j["target_version_id"] = r.target_version_id();
    j["previous_active_version_id"] = r.previous_active_version_id();
    j["status"] = rolloutStatusToString(r.status());
    j["current_stage_index"] = r.current_stage_index();
    j["started_at"] = r.started_at();
    j["paused_at"] = r.paused_at();
    j["pause_reason"] = pauseReasonToString(r.pause_reason());
    j["pause_detail"] = r.pause_detail();
    j["creator"] = r.creator();
    j["last_actor"] = r.last_actor();
    j["completed_at"] = r.completed_at();
    out << j.dump(2) << "\n";
}

void renderRollout(std::ostream& out, const pb::Rollout& r, OutputFormat fmt) {
    if (fmt == OutputFormat::Json) renderRolloutJson(out, r);
    else renderRolloutTable(out, r);
}

int handleError(std::ostream& err, const grpc::Status& st) {
    err << "error: " << st.error_message() << "\n";
    return 1;
}

pb::RolloutSpec parseSpecFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) throw std::runtime_error("cannot open spec file: " + path);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    auto content = oss.str();

    pb::RolloutSpec spec;
    auto node = YAML::Load(content);
    spec.set_target_version_id(node["target_version_id"].as<std::string>(""));
    spec.set_sticky_key(node["sticky_key"].as<std::string>("tenant_id"));
    spec.set_auto_rollback_on_pause(node["auto_rollback_on_pause"].as<bool>(false));
    spec.set_auto_rollback_grace_seconds(node["auto_rollback_grace_seconds"].as<int>(1800));
    spec.set_creator_comment(node["creator_comment"].as<std::string>(""));

    if (auto stages = node["stages"]; stages && stages.IsSequence()) {
        for (const auto& sn : stages) {
            auto* ps = spec.add_stages();
            ps->set_name(sn["name"].as<std::string>(""));
            if (auto sc = sn["scope"]; sc && sc.IsMap()) {
                if (auto tg = sc["tenant_globs"]; tg && tg.IsSequence())
                    for (const auto& g : tg)
                        ps->mutable_scope()->add_tenant_globs(g.as<std::string>());
                if (auto rg = sc["regions"]; rg && rg.IsSequence())
                    for (const auto& r : rg)
                        ps->mutable_scope()->add_regions(r.as<std::string>());
                ps->mutable_scope()->set_percentage(sc["percentage"].as<int>(0));
            }
            if (auto obs = sn["observation"]; obs && obs.IsMap()) {
                ps->mutable_observation()->set_min_duration_seconds(
                    obs["min_duration_seconds"].as<int>(0));
                ps->mutable_observation()->set_min_sample_count(
                    obs["min_sample_count"].as<int>(0));
            }
            if (auto ap = sn["auto_pause"]; ap && ap.IsMap()) {
                ps->mutable_auto_pause()->set_error_rate_gt(
                    ap["error_rate_gt"].as<double>(0));
                ps->mutable_auto_pause()->set_p99_latency_ratio_gt(
                    ap["p99_latency_ratio_gt"].as<double>(0));
                ps->mutable_auto_pause()->set_absolute_error_rate_gt(
                    ap["absolute_error_rate_gt"].as<double>(0));
                ps->mutable_auto_pause()->set_absolute_p99_latency_ms_gt(
                    ap["absolute_p99_latency_ms_gt"].as<double>(0));
            }
        }
    }
    return spec;
}

} // namespace

// ---------------------------------------------------------------------------
// Runners
// ---------------------------------------------------------------------------

int runRolloutCreate(RolloutServiceClient& client, const RolloutCreateArgs& args,
                     std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::RolloutSpec spec;
    try { spec = parseSpecFile(args.spec_file); }
    catch (const std::exception& e) { err << "error: " << e.what() << "\n"; return 1; }

    pb::CreateRolloutRequest req;
    *req.mutable_spec() = spec;
    pb::Rollout resp;
    auto st = client.Create(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutStart(RolloutServiceClient& client, const RolloutIdArgs& args,
                    std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::StartRolloutRequest req;
    req.set_rollout_id(args.rollout_id);
    req.set_comment(args.comment);
    pb::Rollout resp;
    auto st = client.Start(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutPause(RolloutServiceClient& client, const RolloutIdArgs& args,
                    std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::PauseRolloutRequest req;
    req.set_rollout_id(args.rollout_id);
    req.set_comment(args.comment);
    pb::Rollout resp;
    auto st = client.Pause(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutResume(RolloutServiceClient& client, const RolloutIdArgs& args,
                     std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::ResumeRolloutRequest req;
    req.set_rollout_id(args.rollout_id);
    req.set_comment(args.comment);
    pb::Rollout resp;
    auto st = client.Resume(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutPromote(RolloutServiceClient& client, const RolloutIdArgs& args,
                      std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::PromoteRolloutRequest req;
    req.set_rollout_id(args.rollout_id);
    req.set_comment(args.comment);
    pb::Rollout resp;
    auto st = client.Promote(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutAbort(RolloutServiceClient& client, const RolloutIdArgs& args,
                    std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::AbortRolloutRequest req;
    req.set_rollout_id(args.rollout_id);
    req.set_comment(args.comment);
    pb::Rollout resp;
    auto st = client.Abort(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutStatus(RolloutServiceClient& client, const RolloutIdArgs& args,
                     std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::GetRolloutRequest req;
    req.set_rollout_id(args.rollout_id);
    pb::Rollout resp;
    auto st = client.Get(req, &resp);
    if (!st.ok()) return handleError(err, st);
    renderRollout(out, resp, fmt);
    return 0;
}

int runRolloutList(RolloutServiceClient& client, const RolloutListArgs& args,
                   std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::ListRolloutsRequest req;
    req.set_page_size(args.page_size);
    req.set_page_token(args.page_token);
    pb::ListRolloutsResponse resp;
    auto st = client.List(req, &resp);
    if (!st.ok()) return handleError(err, st);

    if (fmt == OutputFormat::Json) {
        nlohmann::json jarr = nlohmann::json::array();
        for (const auto& r : resp.rollouts()) {
            nlohmann::json j;
            j["rollout_id"] = r.rollout_id();
            j["target_version_id"] = r.target_version_id();
            j["status"] = rolloutStatusToString(r.status());
            j["current_stage_index"] = r.current_stage_index();
            j["creator"] = r.creator();
            jarr.push_back(j);
        }
        out << jarr.dump(2) << "\n";
    } else {
        if (resp.rollouts_size() == 0) {
            out << "(no rollouts)\n";
        }
        for (const auto& r : resp.rollouts()) {
            out << r.rollout_id() << "  "
                << rolloutStatusToString(r.status()) << "  "
                << r.target_version_id() << "  "
                << r.creator() << "\n";
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Top-level dispatcher
// ---------------------------------------------------------------------------

int runRolloutCommand(const std::vector<std::string>& argv,
                      std::ostream& out, std::ostream& err) {
    if (argv.empty()) {
        err << "Usage: aegisctl rollout <create|start|pause|resume|promote|abort|status|list> [flags]\n";
        return 1;
    }

    GlobalFlags flags;
    try {
        flags = parseGlobalFlags(argv, nullptr);
    } catch (const std::exception& e) {
        err << "Error: " << e.what() << "\n";
        return 2;
    }

    std::unique_ptr<ControlPlaneClient> base;
    std::unique_ptr<RolloutServiceClient> client;
    try {
        base = std::make_unique<ControlPlaneClient>(flags.connect);
        client = makeGrpcRolloutServiceClient(*base);
    } catch (const std::exception& e) {
        err << "Error: failed to connect to control plane: " << e.what() << "\n";
        return 2;
    }

    const auto& sub = flags.subcommand;

    try {
        if (sub == "create") {
            return runRolloutCreate(*client, parseRolloutCreateArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "start") {
            return runRolloutStart(*client, parseRolloutIdArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "pause") {
            return runRolloutPause(*client, parseRolloutIdArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "resume") {
            return runRolloutResume(*client, parseRolloutIdArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "promote") {
            return runRolloutPromote(*client, parseRolloutIdArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "abort") {
            return runRolloutAbort(*client, parseRolloutIdArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "status") {
            return runRolloutStatus(*client, parseRolloutIdArgs(flags.subcommand_args), out, err, flags.output);
        }
        if (sub == "list") {
            return runRolloutList(*client, parseRolloutListArgs(flags.subcommand_args), out, err, flags.output);
        }
    } catch (const std::invalid_argument& e) {
        err << "error: " << e.what() << "\n";
        return 2;
    }

    err << "unknown rollout subcommand: " << sub << "\n";
    return 1;
}

} // namespace aegisgate::cli
