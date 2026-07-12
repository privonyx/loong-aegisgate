#include "control_plane/grpc/auth_interceptor.h"

#include "auth/auth_service.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace aegisgate::control_plane::grpc_adapter {

namespace {

// gRPC metadata keys are canonically lowercase. We keep this explicit to
// avoid a case-mismatch bug if a future refactor accidentally compares
// against "Authorization".
constexpr const char* kAuthorizationKey = "authorization";

grpc::Status kMissingAuth{grpc::UNAUTHENTICATED, "missing authorization"};
grpc::Status kInvalidAuth{grpc::UNAUTHENTICATED, "invalid authorization"};
grpc::Status kForbidden{grpc::PERMISSION_DENIED,
                         "SuperAdmin role required"};

std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string stripAsciiSpaces(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

} // namespace

AuthInterceptor::AuthInterceptor(AuthService* auth) : auth_(auth) {}

std::string AuthInterceptor::parseBearer(const std::string& header_value) {
    // Expected format: "Bearer <token>". Scheme is case-insensitive per
    // RFC 6750 §2.1; token itself is opaque to us.
    auto trimmed = stripAsciiSpaces(header_value);
    if (trimmed.size() < 7) return {};  // "Bearer " + at least 1 char
    std::string scheme = trimmed.substr(0, 6);
    if (toLowerCopy(scheme) != "bearer") return {};
    if (!std::isspace(static_cast<unsigned char>(trimmed[6]))) return {};
    auto token = stripAsciiSpaces(trimmed.substr(7));
    // Reject obvious garbage early so we don't hit the store for them.
    if (token.empty()) return {};
    return token;
}

AuthInterceptor::Result AuthInterceptor::authorizeHeaders(
    const std::multimap<std::string, std::string>& headers) const {
    Result out;

    auto it = headers.find(kAuthorizationKey);
    if (it == headers.end() || it->second.empty()) {
        out.status = kMissingAuth;
        return out;
    }

    auto token = parseBearer(it->second);
    if (token.empty()) {
        out.status = kInvalidAuth;
        return out;
    }

    if (!auth_) {
        // No resolver wired up — treat as misauth not a server error so a
        // misconfigured deployment fails closed rather than leaking RPCs.
        out.status = kInvalidAuth;
        return out;
    }

    auto ctx = auth_->resolve(token);
    if (!ctx.has_value() || ctx->user_id.empty()) {
        out.status = kInvalidAuth;
        return out;
    }

    // SR1: control plane is SuperAdmin-only. Lower roles are authenticated
    // but not authorized, so we surface PERMISSION_DENIED (distinct from
    // UNAUTHENTICATED above) to give clients actionable feedback.
    if (ctx->role != Role::SuperAdmin) {
        out.status = kForbidden;
        return out;
    }

    out.context = *ctx;
    return out;
}

AuthInterceptor::Result AuthInterceptor::authorize(
    grpc::ServerContext* ctx) const {
    if (!ctx) {
        Result r;
        r.status = kMissingAuth;
        return r;
    }
    std::multimap<std::string, std::string> headers;
    for (const auto& kv : ctx->client_metadata()) {
        // client_metadata returns grpc::string_ref; copy into std::string.
        std::string key(kv.first.data(), kv.first.size());
        std::string val(kv.second.data(), kv.second.size());
        headers.emplace(toLowerCopy(std::move(key)), std::move(val));
    }
    return authorizeHeaders(headers);
}

UserExtractor AuthInterceptor::makeUserExtractor(
    const AuthInterceptor* interceptor) {
    // Capture by pointer so the returned closure stays as light as a
    // std::function can be; interceptor must outlive the closure.
    return [interceptor](grpc::ServerContext* ctx) -> std::string {
        if (!interceptor) return {};
        auto r = interceptor->authorize(ctx);
        if (!r.status.ok()) return {};
        return r.context.user_id;
    };
}

} // namespace aegisgate::control_plane::grpc_adapter
