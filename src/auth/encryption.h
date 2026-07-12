#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace aegisgate {

class Encryption {
public:
    Encryption();

    static Encryption& instance();

    bool isAvailable() const;

    std::string encrypt(const std::string& plaintext, const std::string& purpose) const;
    std::optional<std::string> decrypt(const std::string& ciphertext_b64, const std::string& purpose) const;

private:
    std::vector<uint8_t> deriveKey(const std::string& purpose) const;

    std::vector<uint8_t> master_key_;
    bool available_ = false;
};

} // namespace aegisgate
