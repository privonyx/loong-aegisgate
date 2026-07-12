#pragma once
#include <string>

namespace aegisgate {

// RE2::GlobalReplace interprets '\' in replacement; escape so literals are preserved.
inline std::string escapeRe2Replacement(const std::string& replacement) {
    std::string out;
    out.reserve(replacement.size() * 2);
    for (char c : replacement) {
        if (c == '\\') {
            out += "\\\\";
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace aegisgate
