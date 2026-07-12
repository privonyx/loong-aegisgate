// Phase 9.3 Epic 7 — aegisctl config subcommand implementation.

#include "cli/config_cli.h"
#include "cli/grpc_client.h"
#include "control_plane/sensitive_scanner.h"

#include <nlohmann/json.hpp>

#include <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace aegisgate::cli {

namespace pb = ::aegisgate::controlplane::v1;

namespace {

// ---- argv parsing helpers -----------------------------------------------

[[noreturn]] void throwUsage(const std::string& msg) {
    throw std::invalid_argument(msg);
}

// Consume "--flag <value>" starting at index i (points at the flag). Advances
// i past the value on success. Throws if the flag has no accompanying value.
std::string consumeFlagValue(const std::vector<std::string>& argv,
                             std::size_t& i,
                             const std::string& flag) {
    if (i + 1 >= argv.size()) {
        throwUsage("missing value for " + flag);
    }
    ++i;
    return argv[i];
}

// ---- file I/O -----------------------------------------------------------

struct FileReadResult {
    bool        ok = false;
    std::string content;
    std::string error;
};

FileReadResult readFileBinary(const std::string& path) {
    FileReadResult r;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        r.error = "cannot open " + path + ": " + std::strerror(errno);
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    r.content = ss.str();
    r.ok = true;
    return r;
}

// ---- rendering ----------------------------------------------------------

const char* statusCodeName(grpc::StatusCode code) {
    switch (code) {
        case grpc::StatusCode::OK:                  return "OK";
        case grpc::StatusCode::CANCELLED:           return "CANCELLED";
        case grpc::StatusCode::UNKNOWN:             return "UNKNOWN";
        case grpc::StatusCode::INVALID_ARGUMENT:    return "INVALID_ARGUMENT";
        case grpc::StatusCode::DEADLINE_EXCEEDED:   return "DEADLINE_EXCEEDED";
        case grpc::StatusCode::NOT_FOUND:           return "NOT_FOUND";
        case grpc::StatusCode::ALREADY_EXISTS:      return "ALREADY_EXISTS";
        case grpc::StatusCode::PERMISSION_DENIED:   return "PERMISSION_DENIED";
        case grpc::StatusCode::UNAUTHENTICATED:     return "UNAUTHENTICATED";
        case grpc::StatusCode::RESOURCE_EXHAUSTED:  return "RESOURCE_EXHAUSTED";
        case grpc::StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
        case grpc::StatusCode::ABORTED:             return "ABORTED";
        case grpc::StatusCode::OUT_OF_RANGE:        return "OUT_OF_RANGE";
        case grpc::StatusCode::UNIMPLEMENTED:       return "UNIMPLEMENTED";
        case grpc::StatusCode::INTERNAL:            return "INTERNAL";
        case grpc::StatusCode::UNAVAILABLE:         return "UNAVAILABLE";
        case grpc::StatusCode::DATA_LOSS:           return "DATA_LOSS";
        default:                                    return "UNSPECIFIED";
    }
}

void reportStatus(std::ostream& err, const std::string& ctx,
                  const grpc::Status& status) {
    err << "Error: " << ctx << ": "
        << statusCodeName(status.error_code());
    if (!status.error_message().empty()) {
        err << " — " << status.error_message();
    }
    err << "\n";
}

void renderVersionTable(std::ostream& out, const pb::ConfigVersion& v) {
    out << "version_id: "  << v.version_id()      << "\n"
        << "status:     "  << statusToString(v.status()) << "\n"
        << "sha256:     "  << v.content_sha256()  << "\n"
        << "size_bytes: "  << v.size_bytes()      << "\n";
    if (!v.submitter().empty()) {
        out << "submitter:  " << v.submitter() << "\n";
    }
    if (!v.reviewer().empty()) {
        out << "reviewer:   " << v.reviewer() << "\n";
    }
}

}  // namespace

// --------------------------------------------------------------------------
// Public helpers
// --------------------------------------------------------------------------

std::string statusToString(pb::ConfigStatus s) {
    switch (s) {
        case pb::CONFIG_STATUS_PENDING:    return "PENDING";
        case pb::CONFIG_STATUS_APPROVED:   return "APPROVED";
        case pb::CONFIG_STATUS_REJECTED:   return "REJECTED";
        case pb::CONFIG_STATUS_ACTIVE:     return "ACTIVE";
        case pb::CONFIG_STATUS_SUPERSEDED: return "SUPERSEDED";
        case pb::CONFIG_STATUS_UNSPECIFIED:
        default:
            return "UNKNOWN";
    }
}

// --------------------------------------------------------------------------
// Parsers
// --------------------------------------------------------------------------

ApplyArgs parseApplyArgs(const std::vector<std::string>& argv) {
    ApplyArgs args;
    bool saw_comment = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (tok == "--comment") {
            args.comment = consumeFlagValue(argv, i, "--comment");
            saw_comment = true;
        } else if (tok == "--dry-run") {
            args.dry_run = true;
        } else if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        } else {
            if (!args.file_path.empty()) {
                throwUsage("unexpected positional argument: " + tok);
            }
            args.file_path = tok;
        }
    }
    if (args.file_path.empty()) {
        throwUsage("config apply: missing <file.yaml>");
    }
    if (!saw_comment) {
        throwUsage("config apply: --comment is required");
    }
    return args;
}

namespace {
ReviewArgs parseReviewArgsImpl(const std::vector<std::string>& argv,
                                const char* subcommand) {
    ReviewArgs args;
    bool saw_comment = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (tok == "--comment") {
            args.comment = consumeFlagValue(argv, i, "--comment");
            saw_comment = true;
        } else if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        } else {
            if (!args.version_id.empty()) {
                throwUsage("unexpected positional argument: " + tok);
            }
            args.version_id = tok;
        }
    }
    if (args.version_id.empty()) {
        throwUsage(std::string("config ") + subcommand +
                   ": missing <version_id>");
    }
    if (!saw_comment) {
        throwUsage(std::string("config ") + subcommand +
                   ": --comment is required");
    }
    return args;
}
}  // namespace

ReviewArgs parseApproveArgs(const std::vector<std::string>& argv) {
    return parseReviewArgsImpl(argv, "approve");
}

ReviewArgs parseRejectArgs(const std::vector<std::string>& argv) {
    return parseReviewArgsImpl(argv, "reject");
}

// --------------------------------------------------------------------------
// Runners
// --------------------------------------------------------------------------

int runApply(ConfigServiceClient& client, const ApplyArgs& args,
             std::ostream& out, std::ostream& err, OutputFormat /*fmt*/) {
    const auto file = readFileBinary(args.file_path);
    if (!file.ok) {
        err << "Error: " << file.error << "\n";
        return 1;
    }

    pb::SubmitVersionRequest req;
    req.set_yaml_content(file.content);
    req.set_submitter_comment(args.comment);
    req.set_validate_only(args.dry_run);

    pb::ConfigVersion resp;
    const auto status = client.Submit(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config apply failed", status);
        return 1;
    }
    out << (args.dry_run ? "Validation succeeded:\n" : "Submitted:\n");
    renderVersionTable(out, resp);
    return 0;
}

int runApprove(ConfigServiceClient& client, const ReviewArgs& args,
               std::ostream& out, std::ostream& err, OutputFormat /*fmt*/) {
    pb::ApproveVersionRequest req;
    req.set_version_id(args.version_id);
    req.set_reviewer_comment(args.comment);

    pb::ConfigVersion resp;
    const auto status = client.Approve(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config approve failed", status);
        return 1;
    }
    out << "Approved:\n";
    renderVersionTable(out, resp);
    return 0;
}

int runReject(ConfigServiceClient& client, const ReviewArgs& args,
              std::ostream& out, std::ostream& err, OutputFormat /*fmt*/) {
    pb::RejectVersionRequest req;
    req.set_version_id(args.version_id);
    req.set_reviewer_comment(args.comment);

    pb::ConfigVersion resp;
    const auto status = client.Reject(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config reject failed", status);
        return 1;
    }
    out << "Rejected:\n";
    renderVersionTable(out, resp);
    return 0;
}

// --------------------------------------------------------------------------
// Task 7.3 — activate (RPC + atomic data-plane file write + SIGHUP)
// --------------------------------------------------------------------------

ActivateArgs parseActivateArgs(const std::vector<std::string>& argv) {
    ActivateArgs args;
    bool saw_comment = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (tok == "--comment") {
            args.comment = consumeFlagValue(argv, i, "--comment");
            saw_comment = true;
        } else if (tok == "--data-plane-config-path") {
            args.data_plane_config_path =
                consumeFlagValue(argv, i, "--data-plane-config-path");
        } else if (tok == "--signal-pid") {
            const auto raw = consumeFlagValue(argv, i, "--signal-pid");
            // strtol is used rather than std::stoi so trailing garbage is
            // rejected explicitly (stoi would silently accept "42abc").
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(raw.c_str(), &end, 10);
            if (errno != 0 || end == raw.c_str() || *end != '\0') {
                throwUsage("--signal-pid must be an integer");
            }
            if (parsed <= 0) {
                throwUsage("--signal-pid must be a positive integer");
            }
            args.signal_pid = static_cast<int>(parsed);
        } else if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        } else {
            if (!args.version_id.empty()) {
                throwUsage("unexpected positional argument: " + tok);
            }
            args.version_id = tok;
        }
    }
    if (args.version_id.empty()) {
        throwUsage("config activate: missing <version_id>");
    }
    if (!saw_comment) {
        throwUsage("config activate: --comment is required");
    }
    return args;
}

int defaultSignalSender(int pid, int sig) {
    return ::kill(static_cast<pid_t>(pid), sig);
}

void writeYamlAtomic(const std::string& path, const std::string& content) {
    if (path.empty()) {
        throw std::runtime_error("writeYamlAtomic: empty path");
    }
    // Temp file co-located with target (same filesystem → rename is atomic
    // per POSIX). Suffix carries pid + time to avoid collisions between
    // concurrent aegisctl invocations.
    std::ostringstream tmp;
    tmp << path << ".tmp." << ::getpid();
    const std::string tmp_path = tmp.str();

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(
                "failed to open " + tmp_path + ": " + std::strerror(errno));
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            // best-effort cleanup; ignore errors from the cleanup itself.
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            throw std::runtime_error(
                "failed to write " + tmp_path + ": " + std::strerror(errno));
        }
    }

    // ::rename is atomic on POSIX when src + dst live on the same fs.
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        throw std::runtime_error(
            "failed to rename " + tmp_path + " -> " + path +
            ": " + std::strerror(errno));
    }
}

namespace {

// Shared between activate + rollback. Returns 0 on success, non-zero + writes
// to err otherwise. `label` is the human word shown in messages ("wrote",
// "signaled").
int applyDataPlaneSideEffects(const std::string& data_plane_config_path,
                               int signal_pid,
                               const std::string& yaml_content,
                               SignalSenderFn send_signal,
                               std::ostream& out, std::ostream& err) {
    if (!data_plane_config_path.empty()) {
        try {
            writeYamlAtomic(data_plane_config_path, yaml_content);
        } catch (const std::exception& e) {
            err << "Error: failed to write data-plane config to "
                << data_plane_config_path << ": " << e.what() << "\n";
            return 1;
        }
        out << "Wrote " << yaml_content.size()
            << " bytes to " << data_plane_config_path << "\n";
    }
    if (signal_pid > 0) {
        const int rc = send_signal ? send_signal(signal_pid, SIGHUP)
                                   : defaultSignalSender(signal_pid, SIGHUP);
        if (rc != 0) {
            err << "Error: failed to send signal to pid " << signal_pid
                << ": " << std::strerror(errno) << "\n";
            return 1;
        }
        out << "Sent SIGHUP to pid " << signal_pid << "\n";
    }
    return 0;
}

}  // namespace

int runActivate(ConfigServiceClient& client, const ActivateArgs& args,
                std::ostream& out, std::ostream& err,
                SignalSenderFn send_signal, OutputFormat /*fmt*/) {
    pb::ActivateVersionRequest req;
    req.set_version_id(args.version_id);
    req.set_activation_comment(args.comment);

    pb::ConfigVersion resp;
    const auto status = client.Activate(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config activate failed", status);
        return 1;
    }

    out << "Activated:\n";
    renderVersionTable(out, resp);

    return applyDataPlaneSideEffects(args.data_plane_config_path,
                                      args.signal_pid,
                                      resp.yaml_content(),
                                      send_signal, out, err);
}

// --------------------------------------------------------------------------
// Task 7.4 — rollback (SR9 / T15)
// --------------------------------------------------------------------------

RollbackArgs parseRollbackArgs(const std::vector<std::string>& argv) {
    RollbackArgs args;
    bool saw_comment = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (tok == "--comment") {
            args.comment = consumeFlagValue(argv, i, "--comment");
            saw_comment = true;
        } else if (tok == "--data-plane-config-path") {
            args.data_plane_config_path =
                consumeFlagValue(argv, i, "--data-plane-config-path");
        } else if (tok == "--signal-pid") {
            const auto raw = consumeFlagValue(argv, i, "--signal-pid");
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(raw.c_str(), &end, 10);
            if (errno != 0 || end == raw.c_str() || *end != '\0') {
                throwUsage("--signal-pid must be an integer");
            }
            if (parsed <= 0) {
                throwUsage("--signal-pid must be a positive integer");
            }
            args.signal_pid = static_cast<int>(parsed);
        } else if (tok == "--emergency") {
            args.emergency = true;
        } else if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        } else {
            if (!args.target_version_id.empty()) {
                throwUsage("unexpected positional argument: " + tok);
            }
            args.target_version_id = tok;
        }
    }
    if (args.target_version_id.empty()) {
        throwUsage("config rollback: missing <target_version_id>");
    }
    if (!saw_comment) {
        throwUsage("config rollback: --comment is required");
    }
    return args;
}

// --------------------------------------------------------------------------
// Task 7.5 — read subcommands
// --------------------------------------------------------------------------

pb::ConfigStatus statusFromString(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (s == "PENDING")    return pb::CONFIG_STATUS_PENDING;
    if (s == "APPROVED")   return pb::CONFIG_STATUS_APPROVED;
    if (s == "REJECTED")   return pb::CONFIG_STATUS_REJECTED;
    if (s == "ACTIVE")     return pb::CONFIG_STATUS_ACTIVE;
    if (s == "SUPERSEDED") return pb::CONFIG_STATUS_SUPERSEDED;
    throw std::invalid_argument("unknown status: " + raw);
}

namespace {

// Split "PENDING,APPROVED" into ["PENDING", "APPROVED"]. Empty items are
// rejected so callers catch typos like "PENDING,,APPROVED".
std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (cur.empty()) throw std::invalid_argument("empty status token in --statuses");
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (cur.empty()) throw std::invalid_argument("empty status token in --statuses");
    out.push_back(std::move(cur));
    return out;
}

long parseLongOrThrow(const std::string& raw, const std::string& flag) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (errno != 0 || end == raw.c_str() || *end != '\0') {
        throwUsage(flag + " must be an integer");
    }
    return parsed;
}

}  // namespace

ListArgs parseListArgs(const std::vector<std::string>& argv) {
    ListArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (tok == "--statuses") {
            const auto raw = consumeFlagValue(argv, i, "--statuses");
            for (const auto& part : splitCsv(raw)) {
                args.statuses.push_back(statusFromString(part));
            }
        } else if (tok == "--since") {
            args.since_millis = parseLongOrThrow(
                consumeFlagValue(argv, i, "--since"), "--since");
        } else if (tok == "--page-size") {
            const auto v = parseLongOrThrow(
                consumeFlagValue(argv, i, "--page-size"), "--page-size");
            if (v <= 0) throwUsage("--page-size must be a positive integer");
            args.page_size = static_cast<int32_t>(v);
        } else if (tok == "--page-token") {
            args.page_token = consumeFlagValue(argv, i, "--page-token");
        } else if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        } else {
            throwUsage("config list: unexpected positional argument: " + tok);
        }
    }
    return args;
}

GetArgs parseGetArgs(const std::vector<std::string>& argv) {
    GetArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        }
        if (!args.version_id.empty()) {
            throwUsage("unexpected positional argument: " + tok);
        }
        args.version_id = tok;
    }
    if (args.version_id.empty()) {
        throwUsage("config get: missing <version_id>");
    }
    return args;
}

ShowArgs parseShowArgs(const std::vector<std::string>& argv) {
    ShowArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (tok == "--redact") {
            args.redact = true;
        } else if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        } else {
            if (!args.version_id.empty()) {
                throwUsage("unexpected positional argument: " + tok);
            }
            args.version_id = tok;
        }
    }
    if (args.version_id.empty()) {
        throwUsage("config show: missing <version_id>");
    }
    return args;
}

DiffArgs parseDiffArgs(const std::vector<std::string>& argv) {
    DiffArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (!tok.empty() && tok[0] == '-') {
            throwUsage("unknown flag: " + tok);
        }
        if (args.from_version_id.empty()) {
            args.from_version_id = tok;
        } else if (args.to_version_id.empty()) {
            args.to_version_id = tok;
        } else {
            throwUsage("config diff: expected <from> [<to>]");
        }
    }
    if (args.from_version_id.empty()) {
        throwUsage("config diff: missing <from_version_id>");
    }
    return args;
}

// --------------------------------------------------------------------------
// Redaction helper
// --------------------------------------------------------------------------

std::string redactYaml(const std::string& yaml_text) {
    aegisgate::SensitiveScanner scanner;
    const auto findings = scanner.scan(yaml_text);
    if (findings.empty()) return yaml_text;

    // Collect the 1-indexed line numbers that need replacement.
    std::unordered_set<int> hit_lines;
    for (const auto& f : findings) hit_lines.insert(f.line);

    // Regex: capture indentation + key + ':' prefix; we replace the value.
    // Same anchoring style as SensitiveScanner so comment lines are left alone.
    static const std::regex kKvLine(R"(^([ \t]*[A-Za-z0-9_-]+[ \t]*:)[ \t]*.*$)");

    std::ostringstream out;
    int line_no = 1;
    std::size_t i = 0;
    while (i <= yaml_text.size()) {
        const auto nl = yaml_text.find('\n', i);
        const std::string line = (nl == std::string::npos)
            ? yaml_text.substr(i)
            : yaml_text.substr(i, nl - i);
        if (hit_lines.count(line_no)) {
            std::smatch m;
            if (std::regex_match(line, m, kKvLine)) {
                out << m[1].str() << " <redacted>";
            } else {
                out << line;  // conservative — keep line untouched
            }
        } else {
            out << line;
        }
        if (nl == std::string::npos) break;
        out << '\n';
        i = nl + 1;
        ++line_no;
    }
    return out.str();
}

// --------------------------------------------------------------------------
// SR-NEW1 (TASK-20260515-01 C1): JSON output + always-on redact
// --------------------------------------------------------------------------
//
// `--output json` is the operator opt-in for machine-readable output. It
// MUST never echo raw secrets even when the operator explicitly asked for
// JSON, hence:
//   1. Every string value whose key matches one of the SENSITIVE_KEY
//      substrings (case-insensitive) is replaced with "<redacted>"
//   2. The yaml_content field is additionally run through redactYaml()
//      so SR4 patterns inside the YAML body are scrubbed line-by-line
//   3. The redactor runs unconditionally for OutputFormat::Json — there
//      is no `--no-redact` flag (defense in depth + audit clarity)
//
// SensitiveScanner remains the source of truth for in-YAML secret
// detection; this layer adds field-name based protection for non-YAML
// strings (e.g. submitter_comment that an operator might paste a token
// into by accident).

namespace {

// Substring match; covers field names like "api_key", "password",
// "client_secret", "bearer_token", "submitter_comment", etc.
constexpr std::string_view kSensitiveKeySubstrings[] = {
    "password", "secret", "token", "credential",
    "api_key", "apikey", "auth", "_key",
    "comment",  // *_comment fields can carry pasted secrets
};

bool keyLooksSensitive(std::string_view key) {
    std::string lower(key.size(), '\0');
    std::transform(key.begin(), key.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (auto needle : kSensitiveKeySubstrings) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Scrub `j` in place; returns the count of redactions performed.
size_t redactSensitiveJsonImpl(nlohmann::json& j) {
    size_t count = 0;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string() && keyLooksSensitive(it.key())) {
                if (it.value().get<std::string>() != "<redacted>") {
                    it.value() = "<redacted>";
                    ++count;
                }
            } else {
                count += redactSensitiveJsonImpl(it.value());
            }
        }
    } else if (j.is_array()) {
        for (auto& item : j) count += redactSensitiveJsonImpl(item);
    }
    return count;
}

nlohmann::json versionToJson(const pb::ConfigVersion& v,
                              bool include_yaml_content = false) {
    nlohmann::json j;
    j["version_id"]      = v.version_id();
    j["status"]          = statusToString(v.status());
    j["sha256"]          = v.content_sha256();
    j["size_bytes"]      = v.size_bytes();
    j["submitter"]       = v.submitter();
    j["reviewer"]        = v.reviewer();
    j["submitter_comment"] = v.submitter_comment();
    j["reviewer_comment"]  = v.reviewer_comment();
    if (include_yaml_content) {
        // yaml_content always goes through redactYaml() first for SR4.
        j["yaml_content"] = redactYaml(v.yaml_content());
    }
    return j;
}

void emitJson(std::ostream& out, nlohmann::json& j) {
    redactSensitiveJsonImpl(j);
    out << j.dump(2) << "\n";
}

}  // namespace

// Public wrapper for tests (covers the same code path as emitJson).
size_t redactSensitiveJsonForTest(std::string& json_text) {
    auto j = nlohmann::json::parse(json_text);
    auto n = redactSensitiveJsonImpl(j);
    json_text = j.dump(2);
    return n;
}

// --------------------------------------------------------------------------
// Runners
// --------------------------------------------------------------------------

int runList(ConfigServiceClient& client, const ListArgs& args,
            std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::ListVersionsRequest req;
    for (auto s : args.statuses) req.add_statuses(s);
    req.set_since_millis(args.since_millis);
    req.set_page_size(args.page_size);
    req.set_page_token(args.page_token);

    pb::ListVersionsResponse resp;
    const auto status = client.List(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config list failed", status);
        return 1;
    }
    if (fmt == OutputFormat::Json) {
        nlohmann::json j;
        j["versions"] = nlohmann::json::array();
        for (const auto& v : resp.versions()) {
            j["versions"].push_back(versionToJson(v));
        }
        if (!resp.next_page_token().empty()) {
            j["next_page_token"] = resp.next_page_token();
        }
        emitJson(out, j);
        return 0;
    }
    if (resp.versions_size() == 0) {
        out << "(no versions)\n";
        return 0;
    }
    // Simple, wide-enough columns. Operator-facing; numbers and ULIDs fit.
    out << std::left
        << std::setw(28) << "VERSION_ID"
        << std::setw(12) << "STATUS"
        << std::setw(12) << "SIZE"
        << "SUBMITTER\n";
    for (const auto& v : resp.versions()) {
        out << std::setw(28) << v.version_id()
            << std::setw(12) << statusToString(v.status())
            << std::setw(12) << v.size_bytes()
            << v.submitter() << "\n";
    }
    if (!resp.next_page_token().empty()) {
        out << "\nnext_page_token: " << resp.next_page_token() << "\n";
    }
    return 0;
}

int runGet(ConfigServiceClient& client, const GetArgs& args,
           std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::GetVersionRequest req;
    req.set_version_id(args.version_id);
    pb::ConfigVersion resp;
    const auto status = client.GetVersion(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config get failed", status);
        return 1;
    }
    if (fmt == OutputFormat::Json) {
        // SR14 + SR-NEW1: metadata only; yaml_content NOT included even
        // in JSON. Operators must opt-in via `show [--redact]`.
        auto j = versionToJson(resp, /*include_yaml_content=*/false);
        emitJson(out, j);
        return 0;
    }
    renderVersionTable(out, resp);
    return 0;
}

int runCurrent(ConfigServiceClient& client,
               std::ostream& out, std::ostream& err, OutputFormat fmt) {
    pb::GetActiveRequest req;
    pb::ConfigVersion resp;
    const auto status = client.GetActive(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config current failed", status);
        return 1;
    }
    if (fmt == OutputFormat::Json) {
        auto j = versionToJson(resp, /*include_yaml_content=*/false);
        emitJson(out, j);
        return 0;
    }
    renderVersionTable(out, resp);
    return 0;
}

int runShow(ConfigServiceClient& client, const ShowArgs& args,
            std::ostream& out, std::ostream& err) {
    pb::GetVersionRequest req;
    req.set_version_id(args.version_id);
    pb::ConfigVersion resp;
    const auto status = client.GetVersion(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config show failed", status);
        return 1;
    }
    if (args.redact) {
        out << redactYaml(resp.yaml_content());
    } else {
        out << resp.yaml_content();
    }
    if (!resp.yaml_content().empty() &&
        resp.yaml_content().back() != '\n') {
        out << '\n';
    }
    return 0;
}

int runDiff(ConfigServiceClient& client, const DiffArgs& args,
            std::ostream& out, std::ostream& err) {
    std::string to_id = args.to_version_id;
    if (to_id.empty()) {
        pb::GetActiveRequest req;
        pb::ConfigVersion active;
        const auto status = client.GetActive(req, &active);
        if (!status.ok()) {
            reportStatus(err, "config diff: failed to resolve current ACTIVE",
                          status);
            return 1;
        }
        to_id = active.version_id();
    }

    pb::DiffVersionsRequest req;
    req.set_from_version_id(args.from_version_id);
    req.set_to_version_id(to_id);
    req.set_format(pb::DIFF_FORMAT_UNIFIED);

    pb::DiffVersionsResponse resp;
    const auto status = client.Diff(req, &resp);
    if (!status.ok()) {
        reportStatus(err, "config diff failed", status);
        return 1;
    }
    out << resp.unified_diff();
    if (!resp.unified_diff().empty() && resp.unified_diff().back() != '\n') {
        out << '\n';
    }
    return 0;
}

int runRollback(ConfigServiceClient& client, const RollbackArgs& args,
                std::ostream& out, std::ostream& err,
                SignalSenderFn send_signal, OutputFormat /*fmt*/) {
    pb::RollbackVersionRequest req;
    req.set_target_version_id(args.target_version_id);
    req.set_rollback_comment(args.comment);
    req.set_emergency(args.emergency);

    pb::ConfigVersion resp;
    const auto status = client.Rollback(req, &resp);
    if (!status.ok()) {
        // SR9 / T15 — stitch a friendly explanation when the operator used
        // --emergency and the server rejected it. The client still sent the
        // request so the server-side audit entry fires.
        if (args.emergency &&
            status.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
            err << "ERROR: emergency bypass is reserved for Phase 12 and "
                   "not implemented.\n"
                   "       rollback was rejected by control plane.\n";
        } else {
            reportStatus(err, "config rollback failed", status);
        }
        return 1;
    }

    out << "Rolled back:\n";
    renderVersionTable(out, resp);

    return applyDataPlaneSideEffects(args.data_plane_config_path,
                                      args.signal_pid,
                                      resp.yaml_content(),
                                      send_signal, out, err);
}

// --------------------------------------------------------------------------
// Production ConfigServiceClient impl
// --------------------------------------------------------------------------

namespace {

class GrpcConfigServiceClientImpl final : public ConfigServiceClient {
public:
    explicit GrpcConfigServiceClientImpl(ControlPlaneClient& base)
        : base_(base), stub_(base.stub()) {}

    grpc::Status Submit(const pb::SubmitVersionRequest& req,
                        pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->SubmitVersion(&ctx, req, out);
    }
    grpc::Status Approve(const pb::ApproveVersionRequest& req,
                         pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->ApproveVersion(&ctx, req, out);
    }
    grpc::Status Reject(const pb::RejectVersionRequest& req,
                        pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->RejectVersion(&ctx, req, out);
    }
    grpc::Status Activate(const pb::ActivateVersionRequest& req,
                          pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->ActivateVersion(&ctx, req, out);
    }
    grpc::Status Rollback(const pb::RollbackVersionRequest& req,
                          pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->RollbackVersion(&ctx, req, out);
    }
    grpc::Status List(const pb::ListVersionsRequest& req,
                      pb::ListVersionsResponse* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->ListVersions(&ctx, req, out);
    }
    grpc::Status GetVersion(const pb::GetVersionRequest& req,
                             pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->GetVersion(&ctx, req, out);
    }
    grpc::Status GetActive(const pb::GetActiveRequest& req,
                            pb::ConfigVersion* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->GetActive(&ctx, req, out);
    }
    grpc::Status Diff(const pb::DiffVersionsRequest& req,
                      pb::DiffVersionsResponse* out) override {
        grpc::ClientContext ctx;
        base_.prepareContext(ctx);
        return stub_->DiffVersions(&ctx, req, out);
    }

private:
    ControlPlaneClient& base_;
    std::unique_ptr<pb::ConfigService::Stub> stub_;
};

}  // namespace

std::unique_ptr<ConfigServiceClient> makeGrpcConfigServiceClient(
    ControlPlaneClient& base) {
    return std::make_unique<GrpcConfigServiceClientImpl>(base);
}

// --------------------------------------------------------------------------
// Task 7.6 — common flags + dispatcher
// --------------------------------------------------------------------------

OutputFormat outputFormatFromString(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "table") return OutputFormat::Table;
    if (lower == "json")  return OutputFormat::Json;
    if (lower == "yaml")  return OutputFormat::Yaml;
    throw std::invalid_argument("unknown --output format: " + s);
}

namespace {

std::string envOr(const EnvLookupFn& lookup, const std::string& key,
                   const std::string& fallback = {}) {
    if (lookup) {
        auto v = lookup(key);
        if (!v.empty()) return v;
    }
    return fallback;
}

EnvLookupFn defaultEnvLookup() {
    return [](const std::string& k) -> std::string {
        const char* v = std::getenv(k.c_str());
        return v ? std::string(v) : std::string();
    };
}

bool isKnownGlobalFlag(const std::string& tok) {
    return tok == "--endpoint" || tok == "--tls-ca" ||
           tok == "--tls-cert" || tok == "--tls-key" ||
           tok == "--timeout"  || tok == "--output";
}

}  // namespace

GlobalFlags parseGlobalFlags(const std::vector<std::string>& argv,
                              EnvLookupFn lookup) {
    if (!lookup) lookup = defaultEnvLookup();
    GlobalFlags flags;

    // Populate from env first so explicit flags can override.
    flags.connect.endpoint =
        envOr(lookup, "AEGISGATE_CP_ENDPOINT", "127.0.0.1:9443");
    flags.connect.ca_cert_path     = envOr(lookup, "AEGISGATE_CP_TLS_CA");
    flags.connect.client_cert_path = envOr(lookup, "AEGISGATE_CP_TLS_CERT");
    flags.connect.client_key_path  = envOr(lookup, "AEGISGATE_CP_TLS_KEY");
    flags.connect.api_key          = envOr(lookup, "AEGISGATE_CP_API_KEY");
    {
        const auto t = envOr(lookup, "AEGISGATE_CP_TIMEOUT");
        if (!t.empty()) {
            const long v = parseLongOrThrow(t, "AEGISGATE_CP_TIMEOUT");
            if (v <= 0) {
                throw std::invalid_argument(
                    "AEGISGATE_CP_TIMEOUT must be a positive integer");
            }
            flags.connect.timeout_seconds = static_cast<int>(v);
        }
    }

    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& tok = argv[i];
        if (flags.subcommand.empty() && isKnownGlobalFlag(tok)) {
            // global flag before subcommand
        } else if (!flags.subcommand.empty() && isKnownGlobalFlag(tok)) {
            // global flag after subcommand: still consumed here, not passed
            // through to the subcommand parser.
        } else if (flags.subcommand.empty() && !tok.empty() && tok[0] != '-') {
            flags.subcommand = tok;
            continue;
        } else {
            flags.subcommand_args.push_back(tok);
            continue;
        }

        // Handle the recognised global flag.
        if (tok == "--endpoint") {
            flags.connect.endpoint = consumeFlagValue(argv, i, "--endpoint");
        } else if (tok == "--tls-ca") {
            flags.connect.ca_cert_path = consumeFlagValue(argv, i, "--tls-ca");
        } else if (tok == "--tls-cert") {
            flags.connect.client_cert_path = consumeFlagValue(argv, i, "--tls-cert");
        } else if (tok == "--tls-key") {
            flags.connect.client_key_path = consumeFlagValue(argv, i, "--tls-key");
        } else if (tok == "--timeout") {
            const auto v = parseLongOrThrow(
                consumeFlagValue(argv, i, "--timeout"), "--timeout");
            if (v <= 0) throwUsage("--timeout must be a positive integer");
            flags.connect.timeout_seconds = static_cast<int>(v);
        } else if (tok == "--output") {
            flags.output = outputFormatFromString(
                consumeFlagValue(argv, i, "--output"));
        }
    }

    if (flags.subcommand.empty()) {
        throw std::invalid_argument("config: missing subcommand");
    }
    if (flags.connect.api_key.empty()) {
        throw std::invalid_argument(
            "config: AEGISGATE_CP_API_KEY environment variable is required "
            "(do not pass API keys on the command line)");
    }
    return flags;
}

int runConfigCommand(const std::vector<std::string>& argv,
                     std::ostream& out, std::ostream& err) {
    GlobalFlags flags;
    try {
        flags = parseGlobalFlags(argv, nullptr);
    } catch (const std::exception& e) {
        err << "Error: " << e.what() << "\n";
        return 2;
    }

    std::unique_ptr<ControlPlaneClient> base;
    std::unique_ptr<ConfigServiceClient> client;
    try {
        base = std::make_unique<ControlPlaneClient>(flags.connect);
        client = makeGrpcConfigServiceClient(*base);
    } catch (const std::exception& e) {
        err << "Error: failed to connect to control plane: " << e.what() << "\n";
        return 2;
    }

    try {
        const auto& sub = flags.subcommand;
        if (sub == "apply") {
            return runApply(*client, parseApplyArgs(flags.subcommand_args),
                             out, err, flags.output);
        }
        if (sub == "approve") {
            return runApprove(*client, parseApproveArgs(flags.subcommand_args),
                               out, err, flags.output);
        }
        if (sub == "reject") {
            return runReject(*client, parseRejectArgs(flags.subcommand_args),
                              out, err, flags.output);
        }
        if (sub == "activate") {
            return runActivate(*client, parseActivateArgs(flags.subcommand_args),
                                out, err, &defaultSignalSender, flags.output);
        }
        if (sub == "rollback") {
            return runRollback(*client, parseRollbackArgs(flags.subcommand_args),
                                out, err, &defaultSignalSender, flags.output);
        }
        if (sub == "list") {
            return runList(*client, parseListArgs(flags.subcommand_args),
                            out, err, flags.output);
        }
        if (sub == "get") {
            return runGet(*client, parseGetArgs(flags.subcommand_args),
                           out, err, flags.output);
        }
        if (sub == "show") {
            return runShow(*client, parseShowArgs(flags.subcommand_args),
                            out, err);
        }
        if (sub == "current") {
            if (!flags.subcommand_args.empty()) {
                err << "Error: config current takes no arguments\n";
                return 2;
            }
            return runCurrent(*client, out, err, flags.output);
        }
        if (sub == "diff") {
            return runDiff(*client, parseDiffArgs(flags.subcommand_args),
                            out, err);
        }
        err << "Error: unknown subcommand: " << sub << "\n";
        return 2;
    } catch (const std::invalid_argument& e) {
        err << "Error: " << e.what() << "\n";
        return 2;
    }
}

}  // namespace aegisgate::cli
