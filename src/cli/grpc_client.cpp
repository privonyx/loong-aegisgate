// Phase 9.3 Epic 7 Task 7.1 — ControlPlaneClient implementation.

#include "cli/grpc_client.h"

#include <grpcpp/security/credentials.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace aegisgate::cli {

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to read file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

grpc::SslCredentialsOptions buildSslOptions(const ControlPlaneClient::ConnectOptions& o) {
    grpc::SslCredentialsOptions ssl;
    if (!o.ca_cert_path.empty()) {
        ssl.pem_root_certs = readFile(o.ca_cert_path);
    }
    if (!o.client_cert_path.empty() && !o.client_key_path.empty()) {
        ssl.pem_cert_chain = readFile(o.client_cert_path);
        ssl.pem_private_key = readFile(o.client_key_path);
    }
    return ssl;
}

}  // namespace

// --------------------------------------------------------------------------
// BearerTokenCredentialsPlugin
// --------------------------------------------------------------------------

BearerTokenCredentialsPlugin::BearerTokenCredentialsPlugin(std::string api_key)
    : api_key_(std::move(api_key)) {}

std::string BearerTokenCredentialsPlugin::authorizationValue(const std::string& api_key) {
    if (api_key.empty()) return {};
    return "Bearer " + api_key;
}

grpc::Status BearerTokenCredentialsPlugin::GetMetadata(
    grpc::string_ref /*service_url*/,
    grpc::string_ref /*method_name*/,
    const grpc::AuthContext& /*channel_auth_context*/,
    std::multimap<grpc::string, grpc::string>* metadata) {
    const auto value = authorizationValue(api_key_);
    if (value.empty()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "api_key is empty");
    }
    metadata->insert({"authorization", value});
    return grpc::Status::OK;
}

bool BearerTokenCredentialsPlugin::IsBlocking() const { return false; }

const char* BearerTokenCredentialsPlugin::GetType() const { return "BearerToken"; }

// --------------------------------------------------------------------------
// validateConnectOptions
// --------------------------------------------------------------------------

void validateConnectOptions(const ControlPlaneClient::ConnectOptions& o) {
    if (o.endpoint.empty()) {
        throw std::invalid_argument("ControlPlaneClient: endpoint must not be empty");
    }
    if (o.api_key.empty()) {
        throw std::invalid_argument("ControlPlaneClient: api_key must not be empty");
    }
    if (o.timeout_seconds <= 0) {
        throw std::invalid_argument(
            "ControlPlaneClient: timeout_seconds must be > 0");
    }
    const bool has_cert = !o.client_cert_path.empty();
    const bool has_key  = !o.client_key_path.empty();
    if (has_cert != has_key) {
        throw std::invalid_argument(
            "ControlPlaneClient: mTLS requires both client_cert_path and client_key_path");
    }
}

// --------------------------------------------------------------------------
// ControlPlaneClient
// --------------------------------------------------------------------------

ControlPlaneClient::ControlPlaneClient(ConnectOptions opts)
    : opts_(std::move(opts)) {
    validateConnectOptions(opts_);

    // Channel credentials: SSL (required) composed with Bearer call creds.
    auto ssl_opts = buildSslOptions(opts_);
    auto ssl_creds = grpc::SslCredentials(ssl_opts);
    auto call_creds = grpc::MetadataCredentialsFromPlugin(
        std::unique_ptr<grpc::MetadataCredentialsPlugin>(
            new BearerTokenCredentialsPlugin(opts_.api_key)));
    auto composite = grpc::CompositeChannelCredentials(ssl_creds, call_creds);

    grpc::ChannelArguments args;
    // SR2: client-side mirror of server cap — refuse anything above 1 MiB.
    args.SetMaxReceiveMessageSize(1 * 1024 * 1024);
    args.SetMaxSendMessageSize(1 * 1024 * 1024);

    channel_ = grpc::CreateCustomChannel(opts_.endpoint, composite, args);
}

std::unique_ptr<aegisgate::controlplane::v1::ConfigService::Stub>
ControlPlaneClient::stub() const {
    return aegisgate::controlplane::v1::ConfigService::NewStub(channel_);
}

void ControlPlaneClient::prepareContext(grpc::ClientContext& ctx) const {
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(opts_.timeout_seconds));
}

}  // namespace aegisgate::cli
