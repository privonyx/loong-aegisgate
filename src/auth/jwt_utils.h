#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace aegisgate {

struct JwtPayload {
    std::string user_id;
    std::string tenant_id;
    std::string role;
};

class JwtUtils {
public:
    static std::string sign(const JwtPayload& payload,
                            const std::string& secret,
                            int expire_seconds = 28800);

    static std::optional<JwtPayload> verify(const std::string& token,
                                            const std::string& secret);

private:
    static std::string base64UrlEncode(const std::string& input);
    static std::string base64UrlDecode(const std::string& input);
    static std::string hmacSha256(const std::string& data, const std::string& key);
};

} // namespace aegisgate
