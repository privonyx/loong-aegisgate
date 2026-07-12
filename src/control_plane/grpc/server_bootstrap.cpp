#include "control_plane/grpc/server_bootstrap.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <grpc/grpc_security_constants.h>
#include <grpc/impl/codegen/grpc_types.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

namespace aegisgate::control_plane::grpc_adapter {

namespace {

std::string toLowerHex(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Remove everything that's not [0-9a-fA-F] so callers can feed in "AA:BB:..."
// or "aa bb cc" style fingerprints that operators copy from openssl output.
std::string normalizeHex(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isxdigit(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return toLowerHex(std::move(out));
}

struct BioDeleter {
    void operator()(BIO* b) const noexcept { if (b) BIO_free(b); }
};
struct X509Deleter {
    void operator()(X509* x) const noexcept { if (x) X509_free(x); }
};

} // namespace

std::string computeCertFingerprintSha256(const std::string& cert_pem) {
    if (cert_pem.empty()) return {};

    std::unique_ptr<BIO, BioDeleter> bio(
        BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size())));
    if (!bio) return {};

    std::unique_ptr<X509, X509Deleter> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!cert) return {};

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!X509_digest(cert.get(), EVP_sha256(), digest, &digest_len)) {
        return {};
    }

    std::ostringstream oss;
    oss << std::hex;
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << (digest[i] < 0x10 ? "0" : "") << static_cast<int>(digest[i]);
    }
    return oss.str();
}

bool allowlistMatches(const std::string& fingerprint_hex,
                       const std::vector<std::string>& allowlist) {
    if (allowlist.empty()) {
        // Empty allowlist = "no fingerprint filtering" (CA trust still
        // enforced at the gRPC layer). Caller decides whether that's OK
        // for their deployment posture.
        return true;
    }
    auto normalized_fp = normalizeHex(fingerprint_hex);
    if (normalized_fp.empty()) return false;
    for (const auto& entry : allowlist) {
        if (normalizeHex(entry) == normalized_fp) return true;
    }
    return false;
}

std::unique_ptr<grpc::Server> bootstrapServer(
    const ServerBootstrapConfig& cfg,
    grpc::Service* service) {
    // SR7 — no insecure code path. Any misconfiguration throws so a
    // deployment can never fall back to plaintext silently.
    if (cfg.cert_pem.empty() || cfg.key_pem.empty()) {
        throw std::invalid_argument(
            "TLS is required: cert_pem and key_pem must be provided");
    }
    if (cfg.listen_address.empty()) {
        throw std::invalid_argument("listen_address must not be empty");
    }
    if (!service) {
        throw std::invalid_argument("service must not be null");
    }
    if (cfg.mutual_tls && cfg.client_ca_pem.empty()) {
        throw std::invalid_argument(
            "mutual_tls=true requires client_ca_pem to anchor client certs");
    }

    grpc::SslServerCredentialsOptions ssl_opts(
        cfg.mutual_tls
            ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
            : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    ssl_opts.pem_key_cert_pairs.push_back({cfg.key_pem, cfg.cert_pem});
    if (cfg.mutual_tls) {
        ssl_opts.pem_root_certs = cfg.client_ca_pem;
    }

    auto creds = grpc::SslServerCredentials(ssl_opts);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(cfg.listen_address, creds);
    builder.RegisterService(service);

    // T12: keepalive + concurrency caps so a single malicious client
    // cannot park thousands of idle streams or HTTP/2 PINGs.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, cfg.keepalive_ms);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                                cfg.keepalive_ms);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                                cfg.max_concurrent_streams);
    // SR2: 1 MiB ingress cap — mirrors the ConfigServiceCore guard so
    // clients learn the limit from the transport before the business
    // layer ever allocates the buffer.
    builder.SetMaxReceiveMessageSize(cfg.max_receive_message_bytes);

    auto server = builder.BuildAndStart();
    if (!server) {
        throw std::runtime_error(
            "grpc::ServerBuilder::BuildAndStart returned null");
    }
    return server;
}

std::unique_ptr<grpc::Server> bootstrapServer(
    const ServerBootstrapConfig& cfg,
    const std::vector<grpc::Service*>& services) {
    if (services.empty()) {
        throw std::invalid_argument("at least one service is required");
    }
    if (services.size() == 1) {
        return bootstrapServer(cfg, services[0]);
    }
    if (cfg.cert_pem.empty() || cfg.key_pem.empty()) {
        throw std::invalid_argument(
            "TLS is required: cert_pem and key_pem must be provided");
    }
    if (cfg.listen_address.empty()) {
        throw std::invalid_argument("listen_address must not be empty");
    }
    if (cfg.mutual_tls && cfg.client_ca_pem.empty()) {
        throw std::invalid_argument(
            "mutual_tls=true requires client_ca_pem to anchor client certs");
    }

    grpc::SslServerCredentialsOptions ssl_opts(
        cfg.mutual_tls
            ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
            : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    ssl_opts.pem_key_cert_pairs.push_back({cfg.key_pem, cfg.cert_pem});
    if (cfg.mutual_tls) {
        ssl_opts.pem_root_certs = cfg.client_ca_pem;
    }

    auto creds = grpc::SslServerCredentials(ssl_opts);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(cfg.listen_address, creds);
    for (auto* svc : services) {
        if (svc != nullptr) builder.RegisterService(svc);
    }
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, cfg.keepalive_ms);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                                cfg.keepalive_ms);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                                cfg.max_concurrent_streams);
    builder.SetMaxReceiveMessageSize(cfg.max_receive_message_bytes);

    auto server = builder.BuildAndStart();
    if (!server) {
        throw std::runtime_error(
            "grpc::ServerBuilder::BuildAndStart returned null");
    }
    return server;
}

} // namespace aegisgate::control_plane::grpc_adapter
