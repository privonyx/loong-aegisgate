#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>

namespace aegisgate {

class UnicodeNormalizer {
public:
    UnicodeNormalizer();

    std::string normalize(std::string_view input) const;
    size_t stripZeroWidth(std::string& text) const;
    static bool hasConfusables(std::string_view text);

private:
    uint32_t mapCodepoint(uint32_t cp) const;
    static bool isZeroWidth(uint32_t cp);
    static bool isConfusable(uint32_t cp);

    std::unordered_map<uint32_t, uint32_t> mapping_;
};

} // namespace aegisgate
