#pragma once
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace aegisgate {

// Phase 6.2 (D5=B + SR8): aegisctl cache dump/restore subcommands.
// Separated from aegisctl.cpp so unit tests can exercise arg parsing and
// SR8 (AEGISGATE_CP_API_KEY) enforcement without spinning up Drogon.
namespace cache_cli {

// SR8: environment variable name used to authorize dangerous cache ops.
constexpr const char kApiKeyEnvVar[] = "AEGISGATE_CP_API_KEY";

// Exit codes for CLI dispatch (also returned to main).
constexpr int kExitOk             = 0;
constexpr int kExitPartial        = 1;  // partial restore (some skipped)
constexpr int kExitFail           = 2;  // hard failure (sha256 / missing args / missing key)

struct DumpArgs {
    std::string backend = "hnswlib";
    std::string output;
    std::string config_path;
    bool help = false;
};

struct RestoreArgs {
    std::string input;
    std::string target;
    std::vector<std::string> tenant_allowlist;
    bool help = false;
};

// Indirection seam so tests can inject a fake getenv without depending on the
// real process environment.
using EnvLookup = std::function<const char*(const char*)>;

// Parses arguments for `cache dump [--backend B] --output F [--config C]`.
// Returns false on bad arguments (and prints usage to err).
bool parseDumpArgs(const std::vector<std::string>& args, DumpArgs& out,
                   std::ostream& err);

// Parses arguments for `cache restore --input F --target T
//                                       [--tenant-allowlist a,b,c]`.
bool parseRestoreArgs(const std::vector<std::string>& args, RestoreArgs& out,
                      std::ostream& err);

// SR8 guard. Returns true iff a non-empty AEGISGATE_CP_API_KEY is present.
bool requireApiKey(const EnvLookup& env);

// JSON progress emitter shared by dump/restore.
void emitJsonProgress(std::ostream& out, const std::string& phase,
                      size_t count);

} // namespace cache_cli
} // namespace aegisgate
