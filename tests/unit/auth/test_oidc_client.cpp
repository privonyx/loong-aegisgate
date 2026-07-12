#include <gtest/gtest.h>
#include "auth/oidc_client.h"
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <unordered_map>
#include <chrono>

using namespace aegisgate;

namespace {

// ---------------------------------------------------------------------------
// Mock HTTP fetch
// ---------------------------------------------------------------------------

class MockHttpFetch {
    std::unordered_map<std::string, std::string> responses_;
    int call_count_ = 0;
public:
    void addResponse(const std::string& url, const std::string& body) {
        responses_[url] = body;
    }
    int callCount() const { return call_count_; }

    std::optional<std::string> operator()(const std::string& url,
                                           const std::string& /*method*/,
                                           const std::string& /*body*/,
                                           const std::string& /*content_type*/) {
        ++call_count_;
        auto it = responses_.find(url);
        if (it != responses_.end()) return it->second;
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// RSA key helpers for test JWT signing
// ---------------------------------------------------------------------------

struct TestKeyPair {
    EvpPkeyPtr pkey;
    std::string n_b64url;
    std::string e_b64url;

    TestKeyPair() : pkey(nullptr, EVP_PKEY_free) {}
};

std::string testBase64UrlEncode(const std::string& input) {
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

TestKeyPair generateRsaKeyPair() {
    TestKeyPair result;

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY* raw_pkey = nullptr;
    EVP_PKEY_keygen(ctx, &raw_pkey);
    EVP_PKEY_CTX_free(ctx);

    result.pkey = EvpPkeyPtr(raw_pkey, EVP_PKEY_free);

    BIGNUM* n_bn = nullptr;
    BIGNUM* e_bn = nullptr;
    EVP_PKEY_get_bn_param(raw_pkey, "n", &n_bn);
    EVP_PKEY_get_bn_param(raw_pkey, "e", &e_bn);

    std::vector<unsigned char> n_bytes(BN_num_bytes(n_bn));
    BN_bn2bin(n_bn, n_bytes.data());
    result.n_b64url = testBase64UrlEncode(
        std::string(reinterpret_cast<char*>(n_bytes.data()), n_bytes.size()));

    std::vector<unsigned char> e_bytes(BN_num_bytes(e_bn));
    BN_bn2bin(e_bn, e_bytes.data());
    result.e_b64url = testBase64UrlEncode(
        std::string(reinterpret_cast<char*>(e_bytes.data()), e_bytes.size()));

    BN_free(n_bn);
    BN_free(e_bn);

    return result;
}

std::string signJwt(EVP_PKEY* pkey, const nlohmann::json& header,
                     const nlohmann::json& payload) {
    auto header_enc = testBase64UrlEncode(header.dump());
    auto payload_enc = testBase64UrlEncode(payload.dump());
    auto signed_input = header_enc + "." + payload_enc;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    size_t sig_len = 0;
    EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(md_ctx, signed_input.data(), signed_input.size());
    EVP_DigestSignFinal(md_ctx, nullptr, &sig_len);

    std::vector<unsigned char> sig(sig_len);
    EVP_DigestSignFinal(md_ctx, sig.data(), &sig_len);
    EVP_MD_CTX_free(md_ctx);

    auto sig_b64url = testBase64UrlEncode(
        std::string(reinterpret_cast<char*>(sig.data()), sig_len));

    return signed_input + "." + sig_b64url;
}

std::string makeDiscoveryJson(const std::string& issuer) {
    nlohmann::json j = {
        {"issuer", issuer},
        {"authorization_endpoint", issuer + "/authorize"},
        {"token_endpoint", issuer + "/token"},
        {"jwks_uri", issuer + "/jwks"},
        {"userinfo_endpoint", issuer + "/userinfo"},
        {"end_session_endpoint", issuer + "/logout"}
    };
    return j.dump();
}

std::string makeJwksJson(const std::string& kid, const std::string& n, const std::string& e) {
    nlohmann::json jwks = {
        {"keys", nlohmann::json::array({
            {{"kty", "RSA"}, {"use", "sig"}, {"kid", kid}, {"alg", "RS256"}, {"n", n}, {"e", e}}
        })}
    };
    return jwks.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class OidcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        key_pair_ = generateRsaKeyPair();
    }

    nlohmann::json makeValidPayload() {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return {
            {"iss", kIssuer},
            {"sub", "user-123"},
            {"aud", kClientId},
            {"exp", now + 3600},
            {"iat", now},
            {"nonce", "test-nonce"}
        };
    }

    std::string makeValidToken(const std::string& kid = "test-kid-1") {
        nlohmann::json header = {{"alg", "RS256"}, {"typ", "JWT"}, {"kid", kid}};
        return signJwt(key_pair_.pkey.get(), header, makeValidPayload());
    }

    void setupMockWithDiscoveryAndJwks(MockHttpFetch& mock,
                                        const std::string& kid = "test-kid-1") {
        mock.addResponse(std::string(kIssuer) + "/.well-known/openid-configuration",
                          makeDiscoveryJson(kIssuer));
        mock.addResponse(std::string(kIssuer) + "/jwks",
                          makeJwksJson(kid, key_pair_.n_b64url, key_pair_.e_b64url));
    }

    static constexpr const char* kIssuer = "https://idp.example.com";
    static constexpr const char* kClientId = "my-client-id";

    TestKeyPair key_pair_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(OidcClientTest, DiscoverValid) {
    MockHttpFetch mock;
    mock.addResponse(std::string(kIssuer) + "/.well-known/openid-configuration",
                      makeDiscoveryJson(kIssuer));

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });
    auto doc = client.discover(kIssuer);

    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->issuer, kIssuer);
    EXPECT_EQ(doc->authorization_endpoint, std::string(kIssuer) + "/authorize");
    EXPECT_EQ(doc->token_endpoint, std::string(kIssuer) + "/token");
    EXPECT_EQ(doc->jwks_uri, std::string(kIssuer) + "/jwks");
    EXPECT_EQ(doc->userinfo_endpoint, std::string(kIssuer) + "/userinfo");
    EXPECT_EQ(doc->end_session_endpoint, std::string(kIssuer) + "/logout");
}

TEST_F(OidcClientTest, DiscoverCached) {
    MockHttpFetch mock;
    mock.addResponse(std::string(kIssuer) + "/.well-known/openid-configuration",
                      makeDiscoveryJson(kIssuer));

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });

    auto doc1 = client.discover(kIssuer);
    ASSERT_TRUE(doc1.has_value());
    EXPECT_EQ(mock.callCount(), 1);

    auto doc2 = client.discover(kIssuer);
    ASSERT_TRUE(doc2.has_value());
    EXPECT_EQ(mock.callCount(), 1);

    EXPECT_EQ(doc2->issuer, kIssuer);
}

TEST_F(OidcClientTest, DiscoverEmptyUrl) {
    OidcClient client;
    auto doc = client.discover("");
    EXPECT_FALSE(doc.has_value());
}

TEST_F(OidcClientTest, FetchJwksValid) {
    MockHttpFetch mock;
    mock.addResponse(std::string(kIssuer) + "/jwks",
                      makeJwksJson("test-kid-1", key_pair_.n_b64url, key_pair_.e_b64url));

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });

    EXPECT_TRUE(client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer));
}

TEST_F(OidcClientTest, VerifyIdToken_Valid) {
    MockHttpFetch mock;
    setupMockWithDiscoveryAndJwks(mock);

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });

    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);

    auto token = makeValidToken();
    auto claims = client.verifyIdToken(token, kClientId, kIssuer);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ((*claims)["sub"].get<std::string>(), "user-123");
    EXPECT_EQ((*claims)["iss"].get<std::string>(), kIssuer);
    EXPECT_EQ((*claims)["aud"].get<std::string>(), kClientId);
}

TEST_F(OidcClientTest, VerifyIdToken_Expired) {
    MockHttpFetch mock;
    setupMockWithDiscoveryAndJwks(mock);

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });
    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json header = {{"alg", "RS256"}, {"typ", "JWT"}, {"kid", "test-kid-1"}};
    nlohmann::json payload = {
        {"iss", kIssuer}, {"sub", "user-123"}, {"aud", kClientId},
        {"exp", now - 100}, {"iat", now - 200}
    };
    auto token = signJwt(key_pair_.pkey.get(), header, payload);

    auto claims = client.verifyIdToken(token, kClientId, kIssuer);
    EXPECT_FALSE(claims.has_value());
}

TEST_F(OidcClientTest, VerifyIdToken_WrongAudience) {
    MockHttpFetch mock;
    setupMockWithDiscoveryAndJwks(mock);

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });
    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json header = {{"alg", "RS256"}, {"typ", "JWT"}, {"kid", "test-kid-1"}};
    nlohmann::json payload = {
        {"iss", kIssuer}, {"sub", "user-123"}, {"aud", "wrong-client"},
        {"exp", now + 3600}, {"iat", now}
    };
    auto token = signJwt(key_pair_.pkey.get(), header, payload);

    auto claims = client.verifyIdToken(token, kClientId, kIssuer);
    EXPECT_FALSE(claims.has_value());
}

TEST_F(OidcClientTest, VerifyIdToken_BadSignature) {
    MockHttpFetch mock;
    setupMockWithDiscoveryAndJwks(mock);

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });
    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);

    auto token = makeValidToken();
    auto last_dot = token.rfind('.');
    ASSERT_NE(last_dot, std::string::npos);
    auto tampered = token.substr(0, last_dot + 1) + "AAAA_bad_signature";

    auto claims = client.verifyIdToken(tampered, kClientId, kIssuer);
    EXPECT_FALSE(claims.has_value());
}

TEST_F(OidcClientTest, VerifyIdToken_MalformedToken) {
    OidcClient client;
    EXPECT_FALSE(client.verifyIdToken("", kClientId, kIssuer).has_value());
    EXPECT_FALSE(client.verifyIdToken("not-a-jwt", kClientId, kIssuer).has_value());
    EXPECT_FALSE(client.verifyIdToken("a.b", kClientId, kIssuer).has_value());
    EXPECT_FALSE(client.verifyIdToken("a.b.c.d", kClientId, kIssuer).has_value());
}

TEST_F(OidcClientTest, VerifyIdToken_AudArray) {
    MockHttpFetch mock;
    setupMockWithDiscoveryAndJwks(mock);

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });
    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json header = {{"alg", "RS256"}, {"typ", "JWT"}, {"kid", "test-kid-1"}};
    nlohmann::json payload = {
        {"iss", kIssuer}, {"sub", "user-123"},
        {"aud", nlohmann::json::array({"other-client", kClientId})},
        {"exp", now + 3600}, {"iat", now}
    };
    auto token = signJwt(key_pair_.pkey.get(), header, payload);

    auto claims = client.verifyIdToken(token, kClientId, kIssuer);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ((*claims)["sub"].get<std::string>(), "user-123");
}

TEST_F(OidcClientTest, GenerateAuthUrl) {
    OidcClient client;
    auto result = client.generateAuthUrl(
        "https://idp.example.com/authorize",
        "my-client",
        "https://app.example.com/callback",
        {"openid", "profile", "email"});

    EXPECT_FALSE(result.state.empty());
    EXPECT_FALSE(result.nonce.empty());
    EXPECT_FALSE(result.code_verifier.empty());

    EXPECT_NE(result.url.find("response_type=code"), std::string::npos);
    EXPECT_NE(result.url.find("client_id=my-client"), std::string::npos);
    EXPECT_NE(result.url.find("code_challenge="), std::string::npos);
    EXPECT_NE(result.url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(result.url.find("state="), std::string::npos);
    EXPECT_NE(result.url.find("nonce="), std::string::npos);
    EXPECT_NE(result.url.find("scope="), std::string::npos);

    EXPECT_NE(result.state, result.nonce);
    EXPECT_NE(result.state, result.code_verifier);
}

TEST_F(OidcClientTest, GenerateAuthUrl_Uniqueness) {
    OidcClient client;
    auto r1 = client.generateAuthUrl("https://idp/auth", "c", "https://app/cb", {"openid"});
    auto r2 = client.generateAuthUrl("https://idp/auth", "c", "https://app/cb", {"openid"});

    EXPECT_NE(r1.state, r2.state);
    EXPECT_NE(r1.nonce, r2.nonce);
    EXPECT_NE(r1.code_verifier, r2.code_verifier);
}

TEST_F(OidcClientTest, ExchangeCode) {
    MockHttpFetch mock;
    nlohmann::json token_resp = {
        {"id_token", "id.token.here"},
        {"access_token", "access-tok-123"},
        {"refresh_token", "refresh-tok-456"},
        {"token_type", "Bearer"},
        {"expires_in", 3600}
    };
    mock.addResponse("https://idp.example.com/token", token_resp.dump());

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });
    auto result = client.exchangeCode(
        "https://idp.example.com/token",
        "auth-code-xyz",
        "verifier-123",
        "my-client",
        "my-secret",
        "https://app.example.com/callback");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id_token, "id.token.here");
    EXPECT_EQ(result->access_token, "access-tok-123");
    EXPECT_EQ(result->refresh_token, "refresh-tok-456");
    EXPECT_EQ(result->token_type, "Bearer");
    EXPECT_EQ(result->expires_in, 3600);
}

TEST_F(OidcClientTest, ExchangeCode_NoFetchFn) {
    OidcClient client;
    auto result = client.exchangeCode(
        "https://idp.example.com/token", "code", "verifier",
        "client", "secret", "https://app/cb");
    EXPECT_FALSE(result.has_value());
}

TEST_F(OidcClientTest, VerifyIdToken_KidRefresh) {
    MockHttpFetch mock;
    mock.addResponse(std::string(kIssuer) + "/.well-known/openid-configuration",
                      makeDiscoveryJson(kIssuer));

    nlohmann::json empty_jwks = {{"keys", nlohmann::json::array()}};
    mock.addResponse(std::string(kIssuer) + "/jwks", empty_jwks.dump());

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });

    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);

    mock.addResponse(std::string(kIssuer) + "/jwks",
                      makeJwksJson("test-kid-1", key_pair_.n_b64url, key_pair_.e_b64url));

    auto token = makeValidToken();
    auto claims = client.verifyIdToken(token, kClientId, kIssuer);

    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ((*claims)["sub"].get<std::string>(), "user-123");
}

TEST_F(OidcClientTest, ClearCache) {
    MockHttpFetch mock;
    setupMockWithDiscoveryAndJwks(mock);

    OidcClient client([&mock](auto&&... args) { return mock(std::forward<decltype(args)>(args)...); });

    client.discover(kIssuer);
    client.fetchJwks(std::string(kIssuer) + "/jwks", kIssuer);
    EXPECT_EQ(mock.callCount(), 2);

    client.clearCache();

    client.discover(kIssuer);
    EXPECT_EQ(mock.callCount(), 3);
}
