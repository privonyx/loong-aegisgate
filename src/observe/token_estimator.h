#pragma once
#include "aegisgate/types.h"
#include <string>
#include <vector>

namespace aegisgate {

class TokenEstimator {
public:
    static int estimateTokens(const std::string& text);

    static int estimateMessages(const std::vector<Message>& messages);

    static double chineseRatio(const std::string& text);

private:
    static constexpr double kEnglishCharsPerToken = 4.0;
    static constexpr double kChineseCharsPerToken = 1.5;
    static constexpr int kPerMessageOverhead = 4;
};

} // namespace aegisgate
