#include "auth/jwt_utils.h"
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <chrono>
#include <sstream>

namespace aegisgate {

std::string JwtUtils::base64UrlEncode(const std::string& input) {
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
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::string JwtUtils::base64UrlDecode(const std::string& input) {
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
        val = (val << 6) + lookup[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string JwtUtils::hmacSha256(const std::string& data, const std::string& key) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &len);
    return std::string(reinterpret_cast<char*>(result), len);
}

std::string JwtUtils::sign(const JwtPayload& payload,
                           const std::string& secret,
                           int expire_seconds) {
    nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    auto header_enc = base64UrlEncode(header.dump());

    auto now = std::chrono::system_clock::now();
    auto iat = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json claims = {
        {"sub", payload.user_id},
        {"tid", payload.tenant_id},
        {"role", payload.role},
        {"iat", iat},
        {"exp", iat + expire_seconds}
    };
    auto payload_enc = base64UrlEncode(claims.dump());

    auto signing_input = header_enc + "." + payload_enc;
    auto sig = hmacSha256(signing_input, secret);
    return signing_input + "." + base64UrlEncode(sig);
}

std::optional<JwtPayload> JwtUtils::verify(const std::string& token,
                                           const std::string& secret) {
    if (token.empty()) return std::nullopt;

    auto dot1 = token.find('.');
    if (dot1 == std::string::npos) return std::nullopt;
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return std::nullopt;
    if (token.find('.', dot2 + 1) != std::string::npos) return std::nullopt;

    auto signing_input = token.substr(0, dot2);
    auto sig_encoded = token.substr(dot2 + 1);
    auto sig_decoded = base64UrlDecode(sig_encoded);

    auto expected_sig = hmacSha256(signing_input, secret);

    if (expected_sig.size() != sig_decoded.size() ||
        CRYPTO_memcmp(expected_sig.data(), sig_decoded.data(), expected_sig.size()) != 0) {
        return std::nullopt;
    }

    auto payload_str = base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1));
    try {
        auto claims = nlohmann::json::parse(payload_str);

        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (claims.contains("exp") && claims["exp"].get<int64_t>() < now) {
            return std::nullopt;
        }

        JwtPayload p;
        p.user_id = claims.value("sub", "");
        p.tenant_id = claims.value("tid", "");
        p.role = claims.value("role", "");
        return p;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace aegisgate
