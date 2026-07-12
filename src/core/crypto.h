#pragma once
#include <string>

namespace aegisgate::crypto {

std::string sha256(const std::string& input);
std::string sha256WithSalt(const std::string& input, const std::string& salt);
bool constantTimeEquals(const std::string& a, const std::string& b);

} // namespace aegisgate::crypto
