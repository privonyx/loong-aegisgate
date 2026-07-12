#include "token_estimator.h"
#include <algorithm>

namespace aegisgate {

double TokenEstimator::chineseRatio(const std::string& text) {
    if (text.empty()) return 0.0;

    size_t cjk_chars = 0;
    size_t total_chars = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = p + text.size();

    while (p < end) {
        uint32_t cp = 0;
        int len = 1;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
            cp = (*p & 0x1F) << 6 | (*(p + 1) & 0x3F);
            len = 2;
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
            cp = (*p & 0x0F) << 12 | (*(p + 1) & 0x3F) << 6 | (*(p + 2) & 0x3F);
            len = 3;
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
            cp = (*p & 0x07) << 18 | (*(p + 1) & 0x3F) << 12 |
                 (*(p + 2) & 0x3F) << 6 | (*(p + 3) & 0x3F);
            len = 4;
        }
        p += len;
        ++total_chars;

        if ((cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x20000 && cp <= 0x2A6DF) ||
            (cp >= 0x3000 && cp <= 0x303F) ||
            (cp >= 0xFF00 && cp <= 0xFFEF) ||
            (cp >= 0xAC00 && cp <= 0xD7AF) ||
            (cp >= 0x3040 && cp <= 0x309F) ||
            (cp >= 0x30A0 && cp <= 0x30FF)) {
            ++cjk_chars;
        }
    }

    return total_chars > 0 ? static_cast<double>(cjk_chars) / static_cast<double>(total_chars) : 0.0;
}

int TokenEstimator::estimateTokens(const std::string& text) {
    if (text.empty()) return 0;

    double cjk_ratio = chineseRatio(text);
    double avg_chars_per_token =
        cjk_ratio * kChineseCharsPerToken + (1.0 - cjk_ratio) * kEnglishCharsPerToken;

    auto estimated = static_cast<int>(static_cast<double>(text.size()) / avg_chars_per_token);
    return std::max(1, estimated);
}

int TokenEstimator::estimateMessages(const std::vector<Message>& messages) {
    int total = 0;
    for (const auto& msg : messages) {
        total += estimateTokens(msg.content) + kPerMessageOverhead;
        total += estimateTokens(msg.role);
    }
    total += 3; // reply priming
    return total;
}

} // namespace aegisgate
