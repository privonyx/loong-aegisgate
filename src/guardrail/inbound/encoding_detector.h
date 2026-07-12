#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <memory>

namespace re2 { class RE2; }

namespace aegisgate {

struct EncodedSegment {
    size_t offset;
    size_t length;
    std::string encoding_type;  // "base64" | "hex" | "url" | "unicode_escape"
    std::string decoded_text;
};

class EncodingDetector {
public:
    explicit EncodingDetector(int min_base64_length = 20);
    ~EncodingDetector();
    EncodingDetector(EncodingDetector&&) noexcept;
    EncodingDetector& operator=(EncodingDetector&&) noexcept;

    std::vector<EncodedSegment> detect(std::string_view input) const;

    static std::string decodeBase64(std::string_view encoded);
    static std::string decodeHex(std::string_view encoded);
    static std::string decodeUrl(std::string_view encoded);
    static std::string decodeUnicodeEscape(std::string_view encoded);

private:
    std::vector<EncodedSegment> detectBase64(std::string_view input) const;
    std::vector<EncodedSegment> detectHex(std::string_view input) const;
    std::vector<EncodedSegment> detectUrl(std::string_view input) const;
    std::vector<EncodedSegment> detectUnicodeEscape(std::string_view input) const;

    static bool isPrintableText(std::string_view text);

    std::unique_ptr<re2::RE2> base64_re_;
};

} // namespace aegisgate
