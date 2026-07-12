#pragma once
#include <string>
#include <unordered_map>

namespace aegisgate {

// P1-8 / P1-9: pipeline stages accumulate client-facing response headers in
// RequestContext::response_headers (e.g. budget downgrade marker, A/B variant).
// Those headers were written but never read by the transport. This helper
// applies them onto an HTTP response, skipping empty keys.
//
// Templated on the response type so it is unit-testable with a lightweight fake
// (drogon::HttpResponse is only available at link time and awkward to fabricate
// offline). Any type exposing addHeader(const std::string&, const std::string&)
// works.
template <typename ResponseLike>
inline void applyResponseHeaders(
    ResponseLike& resp,
    const std::unordered_map<std::string, std::string>& headers) {
    for (const auto& [key, value] : headers) {
        if (key.empty()) {
            continue;
        }
        resp.addHeader(key, value);
    }
}

} // namespace aegisgate
