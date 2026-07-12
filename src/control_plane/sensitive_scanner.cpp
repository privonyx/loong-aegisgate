#include "control_plane/sensitive_scanner.h"

#include <re2/re2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace aegisgate {

namespace {

// Pattern set — each entry is a 4-tuple:
//   * rule_id  — stable identifier we surface to callers / metrics
//   * field_regex — captures the yaml key, e.g. `api_key` or `api-key`.
//                   We intentionally do NOT capture the value so it never
//                   leaves the scanner.
//   * full_regex  — matches `<field>: <value>` with value pattern + anchoring
//                   on start-of-line to skip comments and indented descendants.
struct Pattern {
    std::string_view rule_id;
    std::string_view regex;   // captures (field_name) in group 1
};

// NOTE on anchoring: we use `(?m)^[ \t]*` so indented leaf nodes still match
// but `#` comments do not. We disallow leading `#` explicitly.
constexpr std::array<Pattern, 4> kPatterns = {{
    // api_key / api-key / apikey : sk-xxx | sess-xxx | raw 32+ token
    {"api_key",
     R"((?im)^[ \t]*(api[_-]?key)\s*:\s*["']?(?:sk-[A-Za-z0-9_-]{10,}|sess-[A-Za-z0-9]{10,}|[A-Za-z0-9]{32,})["']?\s*$)"},

    // license_key / license-key : 20+ char base64-like
    {"license_key",
     R"((?im)^[ \t]*(license[_-]?key)\s*:\s*["']?[A-Za-z0-9+/=]{20,}["']?\s*$)"},

    // jwt_secret / jwt-secret : 16+ non-whitespace. First char must not be
    // `$` so `${...}` env refs are excluded. RE2 has no lookaround so we rely
    // on a character class instead of `(?!\$\{)`.
    {"jwt_secret",
     R"((?im)^[ \t]*(jwt[_-]?secret)\s*:\s*["']?[^\s"'$][^\s"']{15,}["']?\s*$)"},

    // password : 3+ non-whitespace. First-char class `[^\s"'#${]` rejects empty
    // values, comments and env-var placeholders without needing lookaround.
    {"password",
     R"((?im)^[ \t]*(password)\s*:\s*["']?[^\s"'#${][^\s"'#]{2,}["']?\s*$)"},
}};

} // namespace

std::vector<SensitiveFinding> SensitiveScanner::scan(
    const std::string& yaml_text) const {
    std::vector<SensitiveFinding> out;

    for (const auto& pat : kPatterns) {
        RE2 re(std::string(pat.regex));
        if (!re.ok()) {
            // Conservative: if a rule fails to compile we silently skip it so
            // a malformed pattern can't DoS the whole submit path. Tests will
            // catch compile-time regressions by exercising positive cases.
            continue;
        }

        re2::StringPiece field;
        std::size_t search_pos = 0;

        while (true) {
            re2::StringPiece rest(yaml_text.data() + search_pos,
                                   yaml_text.size() - search_pos);
            if (!RE2::PartialMatch(rest, re, &field)) break;

            // Compute absolute offset of the field capture inside yaml_text so
            // we can derive (line, column).
            const char* field_begin = field.data();
            std::size_t abs_off = static_cast<std::size_t>(
                field_begin - yaml_text.data());

            int line = 1;
            int col = 1;
            for (std::size_t i = 0; i < abs_off; ++i) {
                if (yaml_text[i] == '\n') { ++line; col = 1; }
                else                      { ++col; }
            }

            SensitiveFinding f;
            f.line = line;
            f.column = col;
            f.field_name = std::string(field.data(), field.size());
            f.rule_id = std::string(pat.rule_id);
            out.push_back(std::move(f));

            // Advance past this match to the next newline so we never double-
            // report the same line even with overlapping patterns.
            std::size_t next_nl = yaml_text.find('\n', abs_off);
            if (next_nl == std::string::npos) break;
            search_pos = next_nl + 1;
        }
    }

    // Stable ordering by (line, column) for deterministic test output.
    std::sort(out.begin(), out.end(),
        [](const SensitiveFinding& a, const SensitiveFinding& b) {
            if (a.line != b.line) return a.line < b.line;
            return a.column < b.column;
        });

    return out;
}

} // namespace aegisgate
