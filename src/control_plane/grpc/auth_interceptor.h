#pragma once

// Phase 9.3 Epic 5 Task 5.3 — AuthInterceptor.
//
// Guards the control-plane gRPC surface with Bearer API-Key authentication
// (SR1/M3). The class is intentionally *not* a grpc::experimental::Interceptor
// because synchronous services in gRPC 1.51 cannot short-circuit to a non-OK
// Status from the interceptor call-chain. Instead it exposes a pure
// `authorizeHeaders()` used from every ConfigServiceImpl handler via a
// UserExtractor closure (see `makeUserExtractor`). Consequences:
//
//   * Unit tests exercise the full decision tree without a running gRPC
//     server — we feed a plain metadata multimap.
//   * The ConfigServiceImpl stays framework-neutral: if Task 5.4 later
//     switches to async services or a true Interceptor, only
//     `makeUserExtractor` changes.
//
// Security contract:
//   * missing / malformed `authorization` header       -> UNAUTHENTICATED
//   * API-Key not resolvable or inactive               -> UNAUTHENTICATED
//   * resolved principal with Role < SuperAdmin        -> PERMISSION_DENIED
//   * resolved SuperAdmin                              -> OK + user_id
//
// The status message is deliberately minimal (SR8): never echo the raw
// Authorization header value or any internal resolver diagnostics.

#include "auth/auth_models.h"
#include "control_plane/grpc/config_service_grpc_adapter.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/string_ref.h>

#include <functional>
#include <map>
#include <string>

namespace aegisgate {

class AuthService;

namespace control_plane::grpc_adapter {

class AuthInterceptor {
public:
    // `auth` must outlive the interceptor. nullptr is legal and means every
    // request is rejected as UNAUTHENTICATED (used by failure-mode tests).
    explicit AuthInterceptor(AuthService* auth);

    struct Result {
        grpc::Status status = grpc::Status::OK;  // OK == authorized
        AuthContext  context;                     // populated iff status.ok()
    };

    // Decision function — the only place business logic lives. Accepts a
    // lowercase-keyed metadata view so tests can synthesize requests
    // without building a real ServerContext.
    Result authorizeHeaders(
        const std::multimap<std::string, std::string>& headers) const;

    // Adapter for the live server path: pulls `authorization` from the
    // ServerContext client_metadata and delegates to authorizeHeaders.
    Result authorize(grpc::ServerContext* ctx) const;

    // Helper so ConfigServiceImpl can be constructed with a plain
    // UserExtractor (see Task 5.2). If authorization fails the extractor
    // returns an empty string — ConfigServiceImpl already maps that to
    // UNAUTHENTICATED, but it loses the PERMISSION_DENIED distinction,
    // so production code should use `authorize()` directly. This helper
    // exists primarily for binding in tests.
    static UserExtractor makeUserExtractor(const AuthInterceptor* interceptor);

    // Pure helper exposed for tests: splits "Bearer <token>" (case-insensitive
    // scheme) into the token, or returns empty on malformed input.
    static std::string parseBearer(const std::string& header_value);

private:
    AuthService* auth_;
};

} // namespace control_plane::grpc_adapter
} // namespace aegisgate
