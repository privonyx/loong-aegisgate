#include "auth/oidc_client.h"
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace aegisgate {

OidcClient::OidcClient(HttpFetchFn fetch_fn) : fetch_fn_(std::move(fetch_fn)) {}

// ---------------------------------------------------------------------------
// base64url encode / decode (RFC 4648 §5)
// ---------------------------------------------------------------------------

std::string OidcClient::base64UrlEncode(const std::string& input) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    unsigned int val = 0;
    int bits = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(table[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        out.push_back(table[(val << (-bits)) & 0x3F]);
    }
    for (auto& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

std::string OidcClient::base64UrlDecode(const std::string& input) {
    std::string b64 = input;
    for (auto& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4 != 0) b64.push_back('=');

    static const int lookup[] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };

    std::string out;
    unsigned int val = 0;
    int bits = -8;
    for (unsigned char c : b64) {
        if (c == '=') break;
        if (c >= 128 || lookup[c] < 0) return "";
        val = (val << 6) + static_cast<unsigned>(lookup[c]);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JWT helpers
// ---------------------------------------------------------------------------

std::vector<std::string> OidcClient::splitJwt(const std::string& token) {
    std::vector<std::string> parts;
    if (token.empty()) return parts;

    size_t start = 0;
    for (int i = 0; i < 2; ++i) {
        auto pos = token.find('.', start);
        if (pos == std::string::npos) return {};
        parts.push_back(token.substr(start, pos - start));
        start = pos + 1;
    }
    if (token.find('.', start) != std::string::npos) return {};
    parts.push_back(token.substr(start));
    return parts;
}

// ---------------------------------------------------------------------------
// JWK → EVP_PKEY (RSA)
// ---------------------------------------------------------------------------

EvpPkeyPtr OidcClient::jwkToEvpPkey(const nlohmann::json& jwk) {
    EvpPkeyPtr null_key(nullptr, EVP_PKEY_free);

    if (!jwk.contains("kty") || jwk["kty"] != "RSA") return null_key;
    if (!jwk.contains("n") || !jwk.contains("e")) return null_key;

    auto n_bytes = base64UrlDecode(jwk["n"].get<std::string>());
    auto e_bytes = base64UrlDecode(jwk["e"].get<std::string>());
    if (n_bytes.empty() || e_bytes.empty()) return null_key;

    BIGNUM* n_bn = BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes.data()),
                              static_cast<int>(n_bytes.size()), nullptr);
    BIGNUM* e_bn = BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes.data()),
                              static_cast<int>(e_bytes.size()), nullptr);
    if (!n_bn || !e_bn) {
        BN_free(n_bn);
        BN_free(e_bn);
        return null_key;
    }

    // OpenSSL 3 非弃用路径：用 OSSL_PARAM_BLD + EVP_PKEY_fromdata 构造 RSA 公钥，
    // 取代已弃用的 RSA_new / RSA_set0_key / EVP_PKEY_assign_RSA（无需 pragma 抑制
    // -Wdeprecated-declarations，对 Apple Clang / GCC -Werror 同样干净）。
    // 安全不变量（SR-1/SR-2）：仅替换公钥构造方式，验签语义与拒绝路径不变；
    // 任一步失败 pkey 保持为空，绝不返回构造失败仍可用的 key。
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* ctx = nullptr;
    EVP_PKEY* pkey = nullptr;

    if (bld &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n_bn) == 1 &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e_bn) == 1 &&
        (params = OSSL_PARAM_BLD_to_param(bld)) != nullptr) {
        ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        if (ctx && EVP_PKEY_fromdata_init(ctx) == 1) {
            if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1) {
                pkey = nullptr;  // 失败时确保为空，绝不放宽校验
            }
        }
    }

    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(ctx);
    BN_free(n_bn);
    BN_free(e_bn);

    if (!pkey) return null_key;
    return EvpPkeyPtr(pkey, EVP_PKEY_free);
}

// ---------------------------------------------------------------------------
// RS256 signature verification
// ---------------------------------------------------------------------------

bool OidcClient::verifyRS256(const std::string& signed_data,
                              const std::string& signature_b64url,
                              EVP_PKEY* key) {
    if (!key) return false;

    auto sig = base64UrlDecode(signature_b64url);
    if (sig.empty()) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(ctx, signed_data.data(), signed_data.size()) == 1 &&
        EVP_DigestVerifyFinal(ctx,
                               reinterpret_cast<const unsigned char*>(sig.data()),
                               sig.size()) == 1) {
        ok = true;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

// ---------------------------------------------------------------------------
// PKCE helpers
// ---------------------------------------------------------------------------

std::string OidcClient::generateRandomBase64Url(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
        spdlog::error("OidcClient: RAND_bytes failed");
        return "";
    }
    return base64UrlEncode(std::string(reinterpret_cast<char*>(buf.data()), bytes));
}

std::string OidcClient::sha256Base64Url(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);
    return base64UrlEncode(std::string(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH));
}

// ---------------------------------------------------------------------------
// URL encoding helper
// ---------------------------------------------------------------------------

namespace {
std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << static_cast<int>(c);
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}
} // namespace

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

std::optional<OidcDiscoveryDoc> OidcClient::discover(const std::string& issuer_url) {
    if (issuer_url.empty()) return std::nullopt;

    {
        std::shared_lock lock(cache_mutex_);
        auto it = discovery_cache_.find(issuer_url);
        if (it != discovery_cache_.end()) {
            auto age = std::chrono::steady_clock::now() - it->second.fetched_at;
            if (age < kDiscoveryTtl) {
                return it->second.doc;
            }
        }
    }

    if (!fetch_fn_) {
        spdlog::warn("OidcClient: no HTTP fetch function configured");
        return std::nullopt;
    }

    std::string url = issuer_url;
    if (url.back() == '/') url.pop_back();
    url += "/.well-known/openid-configuration";

    auto resp = fetch_fn_(url, "GET", "", "");
    if (!resp) {
        spdlog::error("OidcClient: discovery fetch failed for {}", issuer_url);
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(*resp);
        OidcDiscoveryDoc doc;
        doc.issuer = j.value("issuer", "");
        doc.authorization_endpoint = j.value("authorization_endpoint", "");
        doc.token_endpoint = j.value("token_endpoint", "");
        doc.jwks_uri = j.value("jwks_uri", "");
        doc.userinfo_endpoint = j.value("userinfo_endpoint", "");
        doc.end_session_endpoint = j.value("end_session_endpoint", "");

        {
            std::unique_lock lock(cache_mutex_);
            discovery_cache_[issuer_url] = CachedDiscovery{doc, std::chrono::steady_clock::now()};
        }
        return doc;
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("OidcClient: discovery JSON parse error: {}", e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// JWKS fetch
// ---------------------------------------------------------------------------

bool OidcClient::fetchJwks(const std::string& jwks_uri, const std::string& issuer) {
    if (jwks_uri.empty() || issuer.empty()) return false;

    if (!fetch_fn_) {
        spdlog::warn("OidcClient: no HTTP fetch function configured");
        return false;
    }

    auto resp = fetch_fn_(jwks_uri, "GET", "", "");
    if (!resp) {
        spdlog::error("OidcClient: JWKS fetch failed for {}", jwks_uri);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(*resp);
        if (!j.contains("keys") || !j["keys"].is_array()) return false;

        std::unordered_map<std::string, EvpPkeyPtr> keys;
        for (const auto& key_json : j["keys"]) {
            if (key_json.value("kty", "") != "RSA") continue;
            if (key_json.contains("use") && key_json["use"] != "sig") continue;

            auto kid = key_json.value("kid", "");
            if (kid.empty()) continue;

            auto pkey = jwkToEvpPkey(key_json);
            if (pkey) {
                keys.emplace(kid, std::move(pkey));
            }
        }

        {
            std::unique_lock lock(cache_mutex_);
            auto& cached = jwks_cache_[issuer];
            cached.keys = std::move(keys);
            cached.fetched_at = std::chrono::steady_clock::now();
        }
        return true;
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("OidcClient: JWKS JSON parse error: {}", e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Key lookup with on-demand refresh
// ---------------------------------------------------------------------------

EVP_PKEY* OidcClient::findKey(const std::string& issuer, const std::string& kid) {
    {
        std::shared_lock lock(cache_mutex_);
        auto it = jwks_cache_.find(issuer);
        if (it != jwks_cache_.end()) {
            auto kit = it->second.keys.find(kid);
            if (kit != it->second.keys.end()) {
                return kit->second.get();
            }
        }
    }

    std::string jwks_uri;
    {
        std::shared_lock lock(cache_mutex_);
        auto dit = discovery_cache_.find(issuer);
        if (dit != discovery_cache_.end()) {
            jwks_uri = dit->second.doc.jwks_uri;
        }

        auto jit = jwks_cache_.find(issuer);
        if (jit != jwks_cache_.end()) {
            auto elapsed = std::chrono::steady_clock::now() - jit->second.last_refresh_attempt;
            if (elapsed < kJwksRefreshCooldown) {
                return nullptr;
            }
        }
    }

    if (jwks_uri.empty()) return nullptr;

    {
        std::unique_lock lock(cache_mutex_);
        auto jit = jwks_cache_.find(issuer);
        if (jit != jwks_cache_.end()) {
            jit->second.last_refresh_attempt = std::chrono::steady_clock::now();
        }
    }

    if (!fetchJwks(jwks_uri, issuer)) return nullptr;

    std::shared_lock lock(cache_mutex_);
    auto it = jwks_cache_.find(issuer);
    if (it != jwks_cache_.end()) {
        auto kit = it->second.keys.find(kid);
        if (kit != it->second.keys.end()) {
            return kit->second.get();
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ID Token verification
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> OidcClient::verifyIdToken(const std::string& token,
                                                          const std::string& expected_audience,
                                                          const std::string& expected_issuer) {
    auto parts = splitJwt(token);
    if (parts.size() != 3) return std::nullopt;

    nlohmann::json header;
    try {
        header = nlohmann::json::parse(base64UrlDecode(parts[0]));
    } catch (...) {
        return std::nullopt;
    }

    auto alg = header.value("alg", "");
    if (alg != "RS256") return std::nullopt;

    auto kid = header.value("kid", "");
    if (kid.empty()) return std::nullopt;

    auto* key = findKey(expected_issuer, kid);
    if (!key) return std::nullopt;

    auto signed_data = parts[0] + "." + parts[1];
    if (!verifyRS256(signed_data, parts[2], key)) return std::nullopt;

    nlohmann::json claims;
    try {
        claims = nlohmann::json::parse(base64UrlDecode(parts[1]));
    } catch (...) {
        return std::nullopt;
    }

    auto iss = claims.value("iss", "");
    if (iss != expected_issuer) return std::nullopt;

    if (claims.contains("aud")) {
        bool aud_match = false;
        if (claims["aud"].is_string()) {
            aud_match = (claims["aud"].get<std::string>() == expected_audience);
        } else if (claims["aud"].is_array()) {
            for (const auto& a : claims["aud"]) {
                if (a.is_string() && a.get<std::string>() == expected_audience) {
                    aud_match = true;
                    break;
                }
            }
        }
        if (!aud_match) return std::nullopt;
    } else {
        return std::nullopt;
    }

    if (claims.contains("exp")) {
        auto exp = claims["exp"].get<int64_t>();
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (exp <= now) return std::nullopt;
    } else {
        return std::nullopt;
    }

    return claims;
}

// ---------------------------------------------------------------------------
// Authorization URL generation with PKCE
// ---------------------------------------------------------------------------

OidcClient::AuthUrlResult OidcClient::generateAuthUrl(
    const std::string& authorization_endpoint,
    const std::string& client_id,
    const std::string& redirect_uri,
    const std::vector<std::string>& scopes) {

    auto state = generateRandomBase64Url(32);
    auto nonce = generateRandomBase64Url(32);
    auto code_verifier = generateRandomBase64Url(32);
    auto code_challenge = sha256Base64Url(code_verifier);

    std::string scope_str;
    for (size_t i = 0; i < scopes.size(); ++i) {
        if (i > 0) scope_str += ' ';
        scope_str += scopes[i];
    }

    std::ostringstream url;
    url << authorization_endpoint
        << "?response_type=code"
        << "&client_id=" << urlEncode(client_id)
        << "&redirect_uri=" << urlEncode(redirect_uri)
        << "&scope=" << urlEncode(scope_str)
        << "&state=" << urlEncode(state)
        << "&nonce=" << urlEncode(nonce)
        << "&code_challenge=" << urlEncode(code_challenge)
        << "&code_challenge_method=S256";

    return AuthUrlResult{url.str(), state, nonce, code_verifier};
}

// ---------------------------------------------------------------------------
// Token exchange
// ---------------------------------------------------------------------------

std::optional<OidcTokenResponse> OidcClient::exchangeCode(
    const std::string& token_endpoint,
    const std::string& code,
    const std::string& code_verifier,
    const std::string& client_id,
    const std::string& client_secret,
    const std::string& redirect_uri) {

    if (!fetch_fn_) {
        spdlog::warn("OidcClient: no HTTP fetch function configured");
        return std::nullopt;
    }

    std::ostringstream body;
    body << "grant_type=authorization_code"
         << "&code=" << urlEncode(code)
         << "&redirect_uri=" << urlEncode(redirect_uri)
         << "&client_id=" << urlEncode(client_id)
         << "&client_secret=" << urlEncode(client_secret)
         << "&code_verifier=" << urlEncode(code_verifier);

    auto resp = fetch_fn_(token_endpoint, "POST", body.str(),
                           "application/x-www-form-urlencoded");
    if (!resp) {
        spdlog::error("OidcClient: token exchange failed");
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(*resp);
        OidcTokenResponse result;
        result.id_token = j.value("id_token", "");
        result.access_token = j.value("access_token", "");
        result.refresh_token = j.value("refresh_token", "");
        result.token_type = j.value("token_type", "");
        result.expires_in = j.value("expires_in", 0);
        return result;
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("OidcClient: token response parse error: {}", e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void OidcClient::clearCache() {
    std::unique_lock lock(cache_mutex_);
    discovery_cache_.clear();
    jwks_cache_.clear();
}

} // namespace aegisgate
