#include "cli/cache_cli.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace aegisgate::cache_cli {

namespace {

void printDumpUsage(std::ostream& err) {
    err << "Usage: aegisctl cache dump --output <file>"
        << " [--backend <hnswlib>] [--config <path>]\n";
}

void printRestoreUsage(std::ostream& err) {
    err << "Usage: aegisctl cache restore --input <file> --target <uri>"
        << " [--tenant-allowlist t1,t2,...]\n";
}

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back())) s.pop_back();
    return s;
}

} // namespace

bool parseDumpArgs(const std::vector<std::string>& args, DumpArgs& out,
                   std::ostream& err) {
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--help" || a == "-h") {
            out.help = true;
            return true;
        }
        auto need = [&](const char* opt) -> bool {
            if (i + 1 >= args.size()) {
                err << "Missing value for " << opt << "\n";
                printDumpUsage(err);
                return false;
            }
            return true;
        };
        if (a == "--backend") {
            if (!need("--backend")) return false;
            out.backend = args[++i];
        } else if (a == "--output") {
            if (!need("--output")) return false;
            out.output = args[++i];
        } else if (a == "--config") {
            if (!need("--config")) return false;
            out.config_path = args[++i];
        } else {
            err << "Unknown option: " << a << "\n";
            printDumpUsage(err);
            return false;
        }
    }
    if (out.output.empty()) {
        err << "Missing required --output argument\n";
        printDumpUsage(err);
        return false;
    }
    return true;
}

bool parseRestoreArgs(const std::vector<std::string>& args, RestoreArgs& out,
                      std::ostream& err) {
    std::string allowlist_raw;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--help" || a == "-h") {
            out.help = true;
            return true;
        }
        auto need = [&](const char* opt) -> bool {
            if (i + 1 >= args.size()) {
                err << "Missing value for " << opt << "\n";
                printRestoreUsage(err);
                return false;
            }
            return true;
        };
        if (a == "--input") {
            if (!need("--input")) return false;
            out.input = args[++i];
        } else if (a == "--target") {
            if (!need("--target")) return false;
            out.target = args[++i];
        } else if (a == "--tenant-allowlist") {
            if (!need("--tenant-allowlist")) return false;
            allowlist_raw = args[++i];
        } else {
            err << "Unknown option: " << a << "\n";
            printRestoreUsage(err);
            return false;
        }
    }
    if (out.input.empty()) {
        err << "Missing required --input argument\n";
        printRestoreUsage(err);
        return false;
    }
    if (out.target.empty()) {
        err << "Missing required --target argument\n";
        printRestoreUsage(err);
        return false;
    }
    if (!allowlist_raw.empty()) {
        std::stringstream ss(allowlist_raw);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            tok = trim(std::move(tok));
            if (!tok.empty()) out.tenant_allowlist.push_back(tok);
        }
    }
    return true;
}

bool requireApiKey(const EnvLookup& env) {
    if (!env) return false;
    const char* val = env(kApiKeyEnvVar);
    if (!val || *val == '\0') return false;
    return true;
}

void emitJsonProgress(std::ostream& out, const std::string& phase,
                      size_t count) {
    out << "{\"phase\":\"" << phase << "\",\"count\":" << count << "}\n";
}

} // namespace aegisgate::cache_cli
