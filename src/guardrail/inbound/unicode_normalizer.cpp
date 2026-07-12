#include "guardrail/inbound/unicode_normalizer.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

namespace {

inline uint32_t decodeUtf8(const char*& p, const char* end) {
    auto byte = static_cast<uint8_t>(*p);

    if (byte < 0x80) {
        ++p;
        return byte;
    }

    uint32_t cp;
    int remaining;

    if ((byte & 0xE0) == 0xC0) {
        cp = byte & 0x1F;
        remaining = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        cp = byte & 0x0F;
        remaining = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        cp = byte & 0x07;
        remaining = 3;
    } else {
        ++p;
        return 0xFFFD; // replacement character for invalid lead byte
    }

    ++p;
    for (int i = 0; i < remaining; ++i) {
        if (p >= end || (static_cast<uint8_t>(*p) & 0xC0) != 0x80) {
            return 0xFFFD;
        }
        cp = (cp << 6) | (static_cast<uint8_t>(*p) & 0x3F);
        ++p;
    }

    return cp;
}

inline void encodeUtf8(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // anonymous namespace

UnicodeNormalizer::UnicodeNormalizer() {
    // Fullwidth ASCII → halfwidth (U+FF01-U+FF5E → U+0021-U+007E)
    for (uint32_t cp = 0xFF01; cp <= 0xFF5E; ++cp) {
        mapping_[cp] = cp - 0xFF01 + 0x0021;
    }

    // Cyrillic confusables
    mapping_[0x0410] = 'A';  // А
    mapping_[0x0412] = 'B';  // В
    mapping_[0x0415] = 'E';  // Е
    mapping_[0x041A] = 'K';  // К
    mapping_[0x041C] = 'M';  // М
    mapping_[0x041D] = 'H';  // Н
    mapping_[0x041E] = 'O';  // О
    mapping_[0x0420] = 'P';  // Р
    mapping_[0x0421] = 'C';  // С
    mapping_[0x0422] = 'T';  // Т
    mapping_[0x0423] = 'Y';  // У
    mapping_[0x0425] = 'X';  // Х
    mapping_[0x0430] = 'a';  // а
    mapping_[0x0435] = 'e';  // е
    mapping_[0x043E] = 'o';  // о
    mapping_[0x0440] = 'p';  // р
    mapping_[0x0441] = 'c';  // с
    mapping_[0x0443] = 'y';  // у
    mapping_[0x0445] = 'x';  // х

    // Greek confusables
    mapping_[0x0391] = 'A';  // Α
    mapping_[0x0392] = 'B';  // Β
    mapping_[0x0395] = 'E';  // Ε
    mapping_[0x0397] = 'H';  // Η
    mapping_[0x0399] = 'I';  // Ι
    mapping_[0x039A] = 'K';  // Κ
    mapping_[0x039C] = 'M';  // Μ
    mapping_[0x039D] = 'N';  // Ν
    mapping_[0x039F] = 'O';  // Ο
    mapping_[0x03A1] = 'P';  // Ρ
    mapping_[0x03A4] = 'T';  // Τ
    mapping_[0x03A5] = 'Y';  // Υ
    mapping_[0x03A7] = 'X';  // Χ
    mapping_[0x03B1] = 'a';  // α
    mapping_[0x03BF] = 'o';  // ο

    // Mathematical bold A-Z, a-z
    for (uint32_t i = 0; i < 26; ++i) {
        mapping_[0x1D400 + i] = 'A' + i;
        mapping_[0x1D41A + i] = 'a' + i;
    }
}

bool UnicodeNormalizer::isZeroWidth(uint32_t cp) {
    switch (cp) {
        case 0x200B:  // ZWSP
        case 0x200C:  // ZWNJ
        case 0x200D:  // ZWJ
        case 0xFEFF:  // BOM
        case 0x00AD:  // SHY
        case 0x200E:  // LRM
        case 0x200F:  // RLM
        case 0x2060:  // WJ
        case 0x2061:  // Function Application
        case 0x2062:  // Invisible Times
        case 0x2063:  // Invisible Separator
        case 0x2064:  // Invisible Plus
            return true;
        default:
            return false;
    }
}

bool UnicodeNormalizer::isConfusable(uint32_t cp) {
    if (isZeroWidth(cp)) return true;

    // Fullwidth ASCII range
    if (cp >= 0xFF01 && cp <= 0xFF5E) return true;

    // Cyrillic confusables (uppercase)
    if (cp == 0x0410 || cp == 0x0412 || cp == 0x0415 ||
        cp == 0x041A || cp == 0x041C || cp == 0x041D ||
        cp == 0x041E || cp == 0x0420 || cp == 0x0421 ||
        cp == 0x0422 || cp == 0x0423 || cp == 0x0425)
        return true;

    // Cyrillic confusables (lowercase)
    if (cp == 0x0430 || cp == 0x0435 || cp == 0x043E ||
        cp == 0x0440 || cp == 0x0441 || cp == 0x0443 ||
        cp == 0x0445)
        return true;

    // Greek confusables (uppercase)
    if (cp == 0x0391 || cp == 0x0392 || cp == 0x0395 ||
        cp == 0x0397 || cp == 0x0399 || cp == 0x039A ||
        cp == 0x039C || cp == 0x039D || cp == 0x039F ||
        cp == 0x03A1 || cp == 0x03A4 || cp == 0x03A5 ||
        cp == 0x03A7)
        return true;

    // Greek confusables (lowercase)
    if (cp == 0x03B1 || cp == 0x03BF) return true;

    // Mathematical bold A-Z, a-z
    if (cp >= 0x1D400 && cp <= 0x1D433) return true;

    return false;
}

uint32_t UnicodeNormalizer::mapCodepoint(uint32_t cp) const {
    auto it = mapping_.find(cp);
    return (it != mapping_.end()) ? it->second : cp;
}

std::string UnicodeNormalizer::normalize(std::string_view input) const {
    std::string result;
    result.reserve(input.size());

    const char* p = input.data();
    const char* end = p + input.size();

    while (p < end) {
        uint32_t cp = decodeUtf8(p, end);
        if (isZeroWidth(cp)) continue;
        encodeUtf8(mapCodepoint(cp), result);
    }

    return result;
}

size_t UnicodeNormalizer::stripZeroWidth(std::string& text) const {
    size_t removed = 0;
    std::string result;
    result.reserve(text.size());

    const char* p = text.data();
    const char* end = p + text.size();

    while (p < end) {
        const char* start = p;
        uint32_t cp = decodeUtf8(p, end);
        if (isZeroWidth(cp)) {
            ++removed;
        } else {
            result.append(start, p);
        }
    }

    text = std::move(result);
    return removed;
}

bool UnicodeNormalizer::hasConfusables(std::string_view text) {
    const char* p = text.data();
    const char* end = p + text.size();

    while (p < end) {
        uint32_t cp = decodeUtf8(p, end);
        if (isConfusable(cp)) return true;
    }

    return false;
}

} // namespace aegisgate
