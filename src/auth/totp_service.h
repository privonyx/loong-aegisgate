#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace aegisgate {

class TotpService {
public:
    static std::string generateSecret();

    static std::string generateOtpAuthUri(const std::string& secret_base32,
                                           const std::string& user,
                                           const std::string& issuer);

    static bool verifyCode(const std::string& secret_base32, const std::string& code,
                           int64_t timestamp = 0);

    static std::vector<std::string> generateRecoveryCodes(int count = 8);

    static std::string hashRecoveryCode(const std::string& code);

    static bool verifyRecoveryCode(std::vector<std::string>& codes_hash, const std::string& code);

private:
    static std::string base32Encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> base32Decode(const std::string& encoded);
    static std::string generateCode(const std::vector<uint8_t>& secret, int64_t counter);
};

} // namespace aegisgate
