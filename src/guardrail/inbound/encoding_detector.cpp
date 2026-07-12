#include "guardrail/inbound/encoding_detector.h"
#include <re2/re2.h>
#include <algorithm>
#include <array>
#include <cstdint>

namespace aegisgate {

namespace {

constexpr std::array<uint8_t, 256> buildBase64Table() {
    std::array<uint8_t, 256> t{};
    for (auto& v : t) v = 255;
    for (int i = 0; i < 26; ++i) {
        t[static_cast<size_t>('A' + i)] = static_cast<uint8_t>(i);
        t[static_cast<size_t>('a' + i)] = static_cast<uint8_t>(i + 26);
    }
    for (int i = 0; i < 10; ++i)
        t[static_cast<size_t>('0' + i)] = static_cast<uint8_t>(i + 52);
    t[static_cast<size_t>('+')] = 62;
    t[static_cast<size_t>('/')] = 63;
    return t;
}

constexpr auto kBase64Table = buildBase64Table();

uint8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0;
}

void encodeUtf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // anonymous namespace

EncodingDetector::EncodingDetector(int min_base64_length)
    : base64_re_(std::make_unique<RE2>(
          "([A-Za-z0-9+/]{" + std::to_string(min_base64_length) + ",}={0,2})")) {}

EncodingDetector::~EncodingDetector() = default;
EncodingDetector::EncodingDetector(EncodingDetector&&) noexcept = default;
EncodingDetector& EncodingDetector::operator=(EncodingDetector&&) noexcept = default;

std::string EncodingDetector::decodeBase64(std::string_view encoded) {
    std::string result;
    result.reserve(encoded.size() * 3 / 4);

    uint32_t buf = 0;
    int bits = 0;

    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ')
            continue;
        uint8_t val = kBase64Table[static_cast<uint8_t>(c)];
        if (val == 255)
            continue;
        buf = (buf << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return result;
}

std::string EncodingDetector::decodeHex(std::string_view encoded) {
    std::string result;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (i + 3 < encoded.size() && encoded[i] == '\\' && encoded[i + 1] == 'x') {
            uint8_t byte = static_cast<uint8_t>(
                (hexNibble(encoded[i + 2]) << 4) | hexNibble(encoded[i + 3]));
            result.push_back(static_cast<char>(byte));
            i += 3;
        } else {
            result.push_back(encoded[i]);
        }
    }
    return result;
}

std::string EncodingDetector::decodeUrl(std::string_view encoded) {
    std::string result;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (i + 2 < encoded.size() && encoded[i] == '%') {
            uint8_t byte = static_cast<uint8_t>(
                (hexNibble(encoded[i + 1]) << 4) | hexNibble(encoded[i + 2]));
            result.push_back(static_cast<char>(byte));
            i += 2;
        } else {
            result.push_back(encoded[i]);
        }
    }
    return result;
}

std::string EncodingDetector::decodeUnicodeEscape(std::string_view encoded) {
    std::string result;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (i + 5 < encoded.size() && encoded[i] == '\\' && encoded[i + 1] == 'u') {
            uint32_t cp = 0;
            for (int j = 0; j < 4; ++j)
                cp = (cp << 4) | hexNibble(encoded[i + 2 + j]);
            encodeUtf8(cp, result);
            i += 5;
        } else {
            result.push_back(encoded[i]);
        }
    }
    return result;
}

bool EncodingDetector::isPrintableText(std::string_view text) {
    if (text.empty()) return false;

    size_t printable = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        auto byte = static_cast<uint8_t>(text[i]);
        if ((byte >= 0x20 && byte <= 0x7E) ||
            byte == '\t' || byte == '\n' || byte == '\r') {
            ++printable;
        } else if ((byte & 0xE0) == 0xC0 && i + 1 < text.size() &&
                   (static_cast<uint8_t>(text[i + 1]) & 0xC0) == 0x80) {
            printable += 2;
            i += 1;
        } else if ((byte & 0xF0) == 0xE0 && i + 2 < text.size() &&
                   (static_cast<uint8_t>(text[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<uint8_t>(text[i + 2]) & 0xC0) == 0x80) {
            printable += 3;
            i += 2;
        } else if ((byte & 0xF8) == 0xF0 && i + 3 < text.size() &&
                   (static_cast<uint8_t>(text[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<uint8_t>(text[i + 2]) & 0xC0) == 0x80 &&
                   (static_cast<uint8_t>(text[i + 3]) & 0xC0) == 0x80) {
            printable += 4;
            i += 3;
        }
    }
    return static_cast<double>(printable) / static_cast<double>(text.size()) > 0.80;
}

std::vector<EncodedSegment> EncodingDetector::detectBase64(std::string_view input) const {
    std::vector<EncodedSegment> results;

    re2::StringPiece sp(input.data(), input.size());
    re2::StringPiece match;
    while (RE2::FindAndConsume(&sp, *base64_re_, &match)) {
        std::string decoded = decodeBase64({match.data(), match.size()});
        if (!decoded.empty() && isPrintableText(decoded)) {
            auto offset = static_cast<size_t>(match.data() - input.data());
            results.push_back({offset, match.size(), "base64", std::move(decoded)});
        }
    }
    return results;
}

std::vector<EncodedSegment> EncodingDetector::detectHex(std::string_view input) const {
    std::vector<EncodedSegment> results;
    static const RE2 re(R"(((?:\\x[0-9a-fA-F]{2}){3,}))");

    re2::StringPiece sp(input.data(), input.size());
    re2::StringPiece match;
    while (RE2::FindAndConsume(&sp, re, &match)) {
        std::string decoded = decodeHex({match.data(), match.size()});
        if (!decoded.empty() && isPrintableText(decoded)) {
            auto offset = static_cast<size_t>(match.data() - input.data());
            results.push_back({offset, match.size(), "hex", std::move(decoded)});
        }
    }
    return results;
}

std::vector<EncodedSegment> EncodingDetector::detectUrl(std::string_view input) const {
    std::vector<EncodedSegment> results;
    static const RE2 re(R"(((?:%[0-9a-fA-F]{2}){3,}))");

    re2::StringPiece sp(input.data(), input.size());
    re2::StringPiece match;
    while (RE2::FindAndConsume(&sp, re, &match)) {
        std::string decoded = decodeUrl({match.data(), match.size()});
        if (!decoded.empty() && isPrintableText(decoded)) {
            auto offset = static_cast<size_t>(match.data() - input.data());
            results.push_back({offset, match.size(), "url", std::move(decoded)});
        }
    }
    return results;
}

std::vector<EncodedSegment> EncodingDetector::detectUnicodeEscape(
    std::string_view input) const {
    std::vector<EncodedSegment> results;
    static const RE2 re(R"(((?:\\u[0-9a-fA-F]{4}){3,}))");

    re2::StringPiece sp(input.data(), input.size());
    re2::StringPiece match;
    while (RE2::FindAndConsume(&sp, re, &match)) {
        std::string decoded = decodeUnicodeEscape({match.data(), match.size()});
        if (!decoded.empty() && isPrintableText(decoded)) {
            auto offset = static_cast<size_t>(match.data() - input.data());
            results.push_back({offset, match.size(), "unicode_escape", std::move(decoded)});
        }
    }
    return results;
}

std::vector<EncodedSegment> EncodingDetector::detect(std::string_view input) const {
    if (input.empty()) return {};

    auto results = detectBase64(input);

    auto hex = detectHex(input);
    results.insert(results.end(),
                   std::make_move_iterator(hex.begin()),
                   std::make_move_iterator(hex.end()));

    auto url = detectUrl(input);
    results.insert(results.end(),
                   std::make_move_iterator(url.begin()),
                   std::make_move_iterator(url.end()));

    auto uni = detectUnicodeEscape(input);
    results.insert(results.end(),
                   std::make_move_iterator(uni.begin()),
                   std::make_move_iterator(uni.end()));

    std::sort(results.begin(), results.end(),
              [](const EncodedSegment& a, const EncodedSegment& b) {
                  return a.offset < b.offset;
              });
    return results;
}

} // namespace aegisgate
