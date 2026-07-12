#include "control_plane/diff_engine.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace aegisgate {

namespace {

// Writes `content` to a freshly-created tmp file under the system tmp dir and
// returns the path. Paths embed a pid + random token so parallel tests do not
// collide.
std::filesystem::path writeTempFile(const std::string& content,
                                    const std::string& tag) {
    static thread_local std::mt19937_64 rng(
        static_cast<std::uint64_t>(::getpid()) ^
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&rng)));
    std::uint64_t token = rng();

    std::ostringstream name;
    name << "aegisgate_diff_" << ::getpid() << "_" << tag << "_" << std::hex
         << token << ".yaml";
    auto path = std::filesystem::temp_directory_path() / name.str();

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(content.data(),
              static_cast<std::streamsize>(content.size()));
    ofs.close();
    return path;
}

// Strips the per-file header lines that `diff -u` emits so callers see only
// the hunk body. Headers look like:
//   --- /tmp/aegisgate_diff_1234_from_....yaml   2026-04-20 ...
//   +++ /tmp/aegisgate_diff_1234_to_....yaml     2026-04-20 ...
// Leaving them in would leak host-level paths into the gRPC response.
std::string stripDiffHeaders(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t pos = 0;
    int stripped = 0;
    while (pos < raw.size()) {
        std::size_t eol = raw.find('\n', pos);
        std::string_view line(raw.data() + pos,
                               (eol == std::string::npos ? raw.size() : eol) - pos);
        bool is_header = stripped < 2 &&
                         ((line.rfind("--- ", 0) == 0 && stripped == 0) ||
                          (line.rfind("+++ ", 0) == 0 && stripped == 1));
        if (is_header) {
            ++stripped;
        } else {
            out.append(line.data(), line.size());
            if (eol != std::string::npos) out.push_back('\n');
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

} // namespace

std::string DiffEngine::unifiedDiff(const std::string& from,
                                    const std::string& to) const {
    if (from == to) return {};
    std::string system_out = runSystemDiff(from, to);
    // runSystemDiff returns empty string if it could not invoke `diff`. That
    // does NOT mean "no difference" (we already short-circuited equal inputs)
    // so fall back to the naive algorithm in that case.
    if (!system_out.empty()) return system_out;
    return naiveDiff(from, to);
}

std::string DiffEngine::runSystemDiff(const std::string& from,
                                      const std::string& to) const {
    auto left  = writeTempFile(from, "from");
    auto right = writeTempFile(to,   "to");

    // Build command with shell-safe quoting of the tmp paths. Our own tokens
    // are [a-z0-9_] so they can't break out, but we quote anyway.
    std::string cmd = "diff -u --label a --label b '" + left.string() +
                      "' '" + right.string() + "' 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    std::string raw;
    if (pipe) {
        std::array<char, 4096> buf;
        std::size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
            raw.append(buf.data(), n);
        }
        int rc = ::pclose(pipe);
        if (rc == -1) {
            spdlog::warn("DiffEngine: pclose failed errno={}", errno);
        }
        // diff exit codes: 0 = identical, 1 = differ, 2 = error.
        // We already short-circuited identical inputs so 0 here means `diff`
        // misfired. Treat 2 as failure too.
        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        if (exit_code != 1) {
            raw.clear();
        }
    }

    std::error_code ec;
    std::filesystem::remove(left, ec);
    std::filesystem::remove(right, ec);

    return stripDiffHeaders(raw);
}

std::string DiffEngine::naiveDiff(const std::string& from,
                                  const std::string& to) const {
    // Splits into lines, then emits:
    //   "-<line>\n" for each line in `from` not in `to`
    //   "+<line>\n" for each line in `to`   not in `from`
    // Preserves document order by walking both side-by-side and re-syncing on
    // equal lines. This is intentionally not a true LCS — fallback path only,
    // we expect `diff` to be available in every production image.
    auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                lines.emplace_back(s.data() + start, i - start);
                start = i + 1;
            }
        }
        if (start < s.size()) lines.emplace_back(s.data() + start, s.size() - start);
        return lines;
    };

    auto a = split(from);
    auto b = split(to);
    std::ostringstream out;
    std::size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (i < a.size() && j < b.size() && a[i] == b[j]) {
            out << " " << a[i] << "\n";
            ++i; ++j;
        } else if (i < a.size() && (j >= b.size() || a[i] != b[j])) {
            out << "-" << a[i] << "\n";
            ++i;
        } else {
            out << "+" << b[j] << "\n";
            ++j;
        }
    }
    return out.str();
}

} // namespace aegisgate
