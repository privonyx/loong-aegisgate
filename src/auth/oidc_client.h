#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <shared_mutex>
#include <memory>
#include <functional>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace aegisgate {

struct OidcDiscoveryDoc {
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string jwks_uri;
    std::string userinfo_endpoint;
    std::string end_session_endpoint;
    std::string issuer;
};

struct OidcTokenResponse {
    std::string id_token;
    std::string access_token;
    std::string refresh_token;
    std::string token_type;
    int expires_in = 0;
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

class OidcClient {
public:
    using HttpFetchFn = std::function<std::optional<std::string>(const std::string& url,
                                                                  const std::string& method,
                                                                  const std::string& body,
                                                                  const std::string& content_type)>;

    explicit OidcClient(HttpFetchFn fetch_fn = nullptr);

    std::optional<OidcDiscoveryDoc> discover(const std::string& issuer_url);

    bool fetchJwks(const std::string& jwks_uri, const std::string& issuer);

    std::optional<nlohmann::json> verifyIdToken(const std::string& token,
                                                 const std::string& expected_audience,
                                                 const std::string& expected_issuer);

    struct AuthUrlResult {
        std::string url;
        std::string state;
        std::string nonce;
        std::string code_verifier;
    };
    AuthUrlResult generateAuthUrl(const std::string& authorization_endpoint,
                                   const std::string& client_id,
                                   const std::string& redirect_uri,
                                   const std::vector<std::string>& scopes);

    std::optional<OidcTokenResponse> exchangeCode(const std::string& token_endpoint,
                                                    const std::string& code,
                                                    const std::string& code_verifier,
                                                    const std::string& client_id,
                                                    const std::string& client_secret,
                                                    const std::string& redirect_uri);

    void clearCache();

private:
    static std::string base64UrlDecode(const std::string& input);
    static std::string base64UrlEncode(const std::string& input);
    static std::vector<std::string> splitJwt(const std::string& token);

    static EvpPkeyPtr jwkToEvpPkey(const nlohmann::json& jwk);

    static bool verifyRS256(const std::string& signed_data,
                             const std::string& signature_b64url,
                             EVP_PKEY* key);

    static std::string generateRandomBase64Url(size_t bytes);
    static std::string sha256Base64Url(const std::string& input);

    struct CachedDiscovery {
        OidcDiscoveryDoc doc;
        std::chrono::steady_clock::time_point fetched_at;
    };
    struct CachedJwks {
        std::unordered_map<std::string, EvpPkeyPtr> keys;
        std::chrono::steady_clock::time_point fetched_at;
        std::chrono::steady_clock::time_point last_refresh_attempt;
    };

    std::unordered_map<std::string, CachedDiscovery> discovery_cache_;
    std::unordered_map<std::string, CachedJwks> jwks_cache_;
    mutable std::shared_mutex cache_mutex_;

    static constexpr auto kDiscoveryTtl = std::chrono::hours(24);
    static constexpr auto kJwksTtl = std::chrono::hours(1);
    static constexpr auto kJwksRefreshCooldown = std::chrono::seconds(60);

    HttpFetchFn fetch_fn_;

    EVP_PKEY* findKey(const std::string& issuer, const std::string& kid);
};

} // namespace aegisgate
