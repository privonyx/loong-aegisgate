#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include "core/config.h"
#include "cli/bench_stats.h"
#include "cli/migrate.h"
#include "cli/cache_cli.h"
#include "cli/estimate_cli.h"
#include "cache/cache_migrator.h"
#include "cache/hnsw_vector_store.h"
#include "gateway/provider_spec/provider_manifest.h"
#include "plugin/rule_pack_manager.h"
#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"
#ifdef AEGISGATE_ENABLE_PG
#include "storage/pg_persistent_store.h"
#endif
#ifdef AEGISGATE_ENABLE_CONTROL_PLANE
#include "cli/config_cli.h"
#include "cli/rollout_cli.h"
#endif
#include "cli/replay_cli.h"  // Phase 11.2 — offline router comparison

namespace {

struct CliConfig {
    std::string base_url = "http://127.0.0.1:8080";
    std::string api_key;
};

void printUsage() {
    std::cout
        << "aegisctl — AegisGate CLI management tool\n\n"
        << "Usage: aegisctl [options] <command> [args]\n\n"
        << "Options:\n"
        << "  --url <url>       Gateway URL (default: http://127.0.0.1:8080)\n"
        << "  --api-key <key>   API key for authentication\n"
        << "  --help            Show this help\n\n"
        << "Commands:\n"
        << "  health            Check gateway health status\n"
        << "  models            List available models\n"
        << "  metrics           Show Prometheus metrics\n"
        << "  chat <message>    Send a chat completion request\n"
        << "  config validate   Validate configuration file\n"
        << "  cache stats       Show cache statistics\n"
        << "  cache import <f>  Import QA pairs from JSON file\n"
        << "  cache dump        Dump local vector cache to a binary snapshot (SR8)\n"
        << "  cache restore     Restore a snapshot into a target store (SR2+SR8)\n"
        << "  bench             Run benchmark against the gateway\n"
        << "  estimate          Pre-flight savings estimator (no gateway needed)\n"
        << "  logs tail         Stream real-time audit logs (SSE)\n"
        << "  migrate           Migrate data between persistent stores\n"
        << "  replay            Replay requests from audit log (live HTTP)\n"
        << "  replay-routes     Offline router comparison from audit log (Phase 11.2, PII-masked)\n"
        << "  tenant <action>   Manage tenants (list|create|get|update|delete)\n"
        << "  user <action>     Manage users (list|create|get|update|delete)\n"
        << "  key <action>      Manage API keys (list|create|revoke|rotate)\n"
        << "  rules <action>    Manage rule packs (list|install|remove|info|apply)\n"
        << "  conformance <a>   Validate Provider Manifest (check|check-all)\n\n"
        << "Config validate options:\n"
        << "  [path]              Config file (default: config/aegisgate.yaml)\n"
        << "  --strict            Treat warnings as errors\n\n"
        << "Bench options:\n"
        << "  --concurrency <n>   Concurrent requests (default: 10)\n"
        << "  --requests <n>      Total requests (default: 100)\n"
        << "  --model <name>      Model to use (default: empty)\n"
        << "  --prompt <text>     Prompt text (default: \"Say hello\")\n\n"
        << "Estimate options (pre-flight, no gateway HTTP):\n"
        << "  --model <id>                    Model id from config/models.yaml (required)\n"
        << "  --monthly-calls <n>             Estimated monthly request count (required)\n"
        << "  --avg-input-tokens <n>          Average prompt tokens per call (required)\n"
        << "  --avg-output-tokens <n>         Average completion tokens per call (required)\n"
        << "  --scenario <name>               conservative | balanced | aggressive (default: balanced)\n"
        << "  --cache-hit-rate <0-1>          Override scenario cache hit rate\n"
        << "  --routing-savings-rate <0-1>    Override scenario routing rate\n"
        << "  --compression-rate <0-1>        Override scenario compression rate\n"
        << "  --target-model <id>             Cheaper model for routing (auto if omitted)\n"
        << "  --models-config <path>          Pricing yaml (default: config/models.yaml)\n"
        << "  --output <fmt>                  table | json (default: table)\n"
        << "  --explain                       Append assumptions footnotes\n\n"
        << "Logs tail options:\n"
        << "  --level <level>     Filter by log level\n\n"
        << "Migrate options:\n"
        << "  --from <type>       Source type: sqlite|memory\n"
        << "  --from-path <path>  Source path (for sqlite)\n"
        << "  --to <type>         Target type: sqlite|postgres\n"
        << "  --to-path <path>    Target path (for sqlite)\n"
        << "  --to-url <url>      Target URL (for postgres)\n\n"
        << "Replay options:\n"
        << "  --file <path>       Audit log file (JSONL)\n"
        << "  --limit <n>         Max requests to replay (default: all)\n"
        << "  --dry-run           Print requests without sending\n\n"
        << "Tenant commands:\n"
        << "  tenant list\n"
        << "  tenant create <name> [--model-whitelist \"m1,m2\"] [--daily-cost-limit N] [--monthly-cost-limit N]\n"
        << "  tenant get <id>\n"
        << "  tenant update <id> [--name <n>] [--status <s>] [--model-whitelist \"m1,m2\"]\n"
        << "  tenant delete <id>\n\n"
        << "User commands:\n"
        << "  user list [--tenant <id>]\n"
        << "  user create <username> --tenant <id> --role <role>\n"
        << "  user get <id>\n"
        << "  user update <id> [--role <role>] [--status <s>] [--display-name <n>]\n"
        << "  user delete <id>\n\n"
        << "Key commands:\n"
        << "  key list [--tenant <id>]\n"
        << "  key create --user <id> --name <name> [--role <role>]\n"
        << "  key revoke <id>\n"
        << "  key rotate <id>\n\n"
        << "Conformance commands:\n"
        << "  conformance check <file.yaml>\n"
        << "  conformance check-all <dir>\n\n"
        << "Examples:\n"
        << "  aegisctl health\n"
        << "  aegisctl --api-key sk-xxx models\n"
        << "  aegisctl chat \"What is the capital of France?\"\n"
        << "  aegisctl config validate config/aegisgate.yaml\n"
        << "  aegisctl bench --concurrency 20 --requests 200\n"
        << "  aegisctl logs tail --level warn\n"
        << "  aegisctl migrate --from sqlite --from-path data/aegisgate.db "
        << "--to sqlite --to-path data/new.db\n"
        << "  aegisctl replay --file audit.jsonl --limit 10\n"
        << "  aegisctl tenant list\n"
        << "  aegisctl tenant create \"Acme Corp\"\n"
        << "  aegisctl user create admin --tenant T1 --role tenant_admin\n"
        << "  aegisctl conformance check api/providers/definitions/openai.yaml\n"
        << "  aegisctl conformance check-all api/providers/definitions/\n"
        << "  aegisctl key create --user U1 --name dev-key\n"
        << "  aegisctl key revoke KEY-ID\n";
}

std::pair<drogon::ReqResult, drogon::HttpResponsePtr>
httpGet(const std::string& base_url, const std::string& path,
        const std::string& api_key) {
    auto client = drogon::HttpClient::newHttpClient(base_url);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(path);
    if (!api_key.empty()) {
        req->addHeader("Authorization", "Bearer " + api_key);
    }
    return client->sendRequest(req, 10.0);
}

std::pair<drogon::ReqResult, drogon::HttpResponsePtr>
httpPost(const std::string& base_url, const std::string& path,
         const std::string& body, const std::string& api_key) {
    auto client = drogon::HttpClient::newHttpClient(base_url);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    if (!api_key.empty()) {
        req->addHeader("Authorization", "Bearer " + api_key);
    }
    return client->sendRequest(req, 60.0);
}

std::pair<drogon::ReqResult, drogon::HttpResponsePtr>
httpPut(const std::string& base_url, const std::string& path,
        const std::string& body, const std::string& api_key) {
    auto client = drogon::HttpClient::newHttpClient(base_url);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Put);
    req->setPath(path);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    if (!api_key.empty()) {
        req->addHeader("Authorization", "Bearer " + api_key);
    }
    return client->sendRequest(req, 60.0);
}

std::pair<drogon::ReqResult, drogon::HttpResponsePtr>
httpDelete(const std::string& base_url, const std::string& path,
           const std::string& api_key) {
    auto client = drogon::HttpClient::newHttpClient(base_url);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Delete);
    req->setPath(path);
    if (!api_key.empty()) {
        req->addHeader("Authorization", "Bearer " + api_key);
    }
    return client->sendRequest(req, 10.0);
}

int cmdHealth(const CliConfig& cfg) {
    try {
        auto [result, resp] = httpGet(cfg.base_url, "/health", "");
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot reach gateway at " << cfg.base_url << "\n";
            return 1;
        }
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Status:      " << j.value("status", "unknown") << "\n"
                  << "Version:     " << j.value("version", "unknown") << "\n"
                  << "Initialized: " << (j.value("initialized", false) ? "yes" : "no") << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int cmdModels(const CliConfig& cfg) {
    try {
        auto [result, resp] = httpGet(cfg.base_url, "/v1/models", cfg.api_key);
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot reach gateway\n";
            return 1;
        }
        if (resp->statusCode() != drogon::k200OK) {
            std::cerr << "Error " << static_cast<int>(resp->statusCode())
                      << ": " << resp->body() << "\n";
            return 1;
        }
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Available models:\n";
        for (const auto& m : j["data"]) {
            std::cout << "  " << m.value("id", "?")
                      << "  (provider: " << m.value("owned_by", "?") << ")\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int cmdMetrics(const CliConfig& cfg) {
    try {
        auto [result, resp] = httpGet(cfg.base_url, "/metrics", "");
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot reach gateway\n";
            return 1;
        }
        std::cout << resp->body() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int cmdChat(const CliConfig& cfg, const std::string& message) {
    nlohmann::json body;
    body["model"] = "";
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "user"}, {"content", message}});

    try {
        auto [result, resp] = httpPost(
            cfg.base_url, "/v1/chat/completions", body.dump(), cfg.api_key);
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot reach gateway\n";
            return 1;
        }
        if (resp->statusCode() != drogon::k200OK) {
            auto err = nlohmann::json::parse(resp->body());
            std::cerr << "Error: " << err["error"].value("message", resp->body()) << "\n";
            return 1;
        }
        auto j = nlohmann::json::parse(resp->body());
        if (j.contains("choices") && !j["choices"].empty()) {
            std::cout << j["choices"][0]["message"].value("content", "") << "\n";
        }
        auto& usage = j["usage"];
        std::cout << "\n--- tokens: prompt=" << usage.value("prompt_tokens", 0)
                  << " completion=" << usage.value("completion_tokens", 0)
                  << " total=" << usage.value("total_tokens", 0)
                  << " model=" << j.value("model", "?") << " ---\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

std::unique_ptr<aegisgate::PersistentStore> makeStore(
    const std::string& type, const std::string& path,
    [[maybe_unused]] const std::string& url) {
    if (type == "sqlite") {
        if (path.empty()) {
            std::cerr << "Error: --from-path or --to-path required for sqlite\n";
            return nullptr;
        }
        if (path.find("..") != std::string::npos) {
            std::cerr << "Error: path must not contain '..'\n";
            return nullptr;
        }
        return std::make_unique<aegisgate::SQLitePersistentStore>(path);
    }
#ifdef AEGISGATE_ENABLE_PG
    if (type == "postgres" || type == "pg") {
        if (url.empty()) {
            std::cerr << "Error: --to-url required for postgres\n";
            return nullptr;
        }
        aegisgate::PgConfig cfg;
        cfg.url = url;
        cfg.pool_size = 2;
        return std::make_unique<aegisgate::PgPersistentStore>(cfg);
    }
#endif
    if (type == "memory") {
        return std::make_unique<aegisgate::MemoryPersistentStore>();
    }
    std::cerr << "Error: unknown store type: " << type << "\n";
    return nullptr;
}

int cmdCacheStats(const CliConfig& cfg) {
    try {
        auto [result, resp] = httpGet(cfg.base_url, "/cache/stats", cfg.api_key);
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot reach gateway\n";
            return 1;
        }
        if (resp->statusCode() != drogon::k200OK) {
            std::cerr << "Error " << static_cast<int>(resp->statusCode())
                      << ": " << resp->body() << "\n";
            return 1;
        }
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Cache Statistics:\n"
                  << "  Entries:     " << j.value("entry_count", 0) << "\n"
                  << "  Hits:        " << j.value("hit_count", 0) << "\n"
                  << "  Misses:      " << j.value("miss_count", 0) << "\n"
                  << "  Puts:        " << j.value("put_count", 0) << "\n"
                  << "  Hit Rate:    " << std::fixed << std::setprecision(2)
                  << (j.value("hit_rate", 0.0) * 100.0) << "%\n"
                  << "  Threshold:   " << std::setprecision(4)
                  << j.value("current_threshold", 0.0) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int cmdCacheImport(const CliConfig& cfg, const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open file: " << filepath << "\n";
        return 1;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    try {
        auto [result, resp] = httpPost(
            cfg.base_url, "/admin/cache/import", content, cfg.api_key);
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot reach gateway\n";
            return 1;
        }
        if (resp->statusCode() != drogon::k200OK) {
            std::cerr << "Error " << static_cast<int>(resp->statusCode())
                      << ": " << resp->body() << "\n";
            return 1;
        }
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Imported " << j.value("imported", 0)
                  << " entries from " << filepath << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

// Phase 6.2 (Epic 3.4 / D5=B): cache dump subcommand. SR8 enforces
// AEGISGATE_CP_API_KEY; absence returns exit code 2.
int cmdCacheDump(const std::vector<std::string>& args) {
    using namespace aegisgate::cache_cli;
    DumpArgs parsed;
    if (!parseDumpArgs(args, parsed, std::cerr)) return kExitFail;
    if (parsed.help) {
        std::cout << "aegisctl cache dump --output <file> "
                     "[--backend <hnswlib>] [--config <path>]\n"
                     "  Requires AEGISGATE_CP_API_KEY env var (SR8).\n";
        return kExitOk;
    }
    EnvLookup env = [](const char* n) { return std::getenv(n); };
    if (!requireApiKey(env)) {
        std::cerr << "Error: " << kApiKeyEnvVar
                  << " environment variable is required (SR8)\n";
        return kExitFail;
    }

    // For now only hnswlib is wired locally (Milvus/Qdrant enumerate is
    // explicitly unsupported by D8=A). Construct a fresh store so the tool
    // remains a pure offline utility — production callers will use the
    // gateway's existing store via the future control-plane endpoint.
    aegisgate::HnswVectorStore store(128, 100000, 64);
    store.initialize();
    aegisgate::CacheMigrator m;
    try {
        auto stats = m.dump(store, parsed.output);
        emitJsonProgress(std::cout, "dump", stats.entries_written);
        std::cout << "{\"sha256\":\"" << stats.sha256_hex << "\",\"bytes\":"
                  << stats.bytes_written << "}\n";
        return kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "Error: dump failed: " << e.what() << "\n";
        return kExitFail;
    }
}

// Phase 6.2 (Epic 3.4 / D5=B): cache restore. Maps RestoreStats to exit
// codes 0/1/2 = OK / PARTIAL / FAIL per design §5.3.2.
int cmdCacheRestore(const std::vector<std::string>& args) {
    using namespace aegisgate::cache_cli;
    RestoreArgs parsed;
    if (!parseRestoreArgs(args, parsed, std::cerr)) return kExitFail;
    if (parsed.help) {
        std::cout << "aegisctl cache restore --input <file> --target <uri> "
                     "[--tenant-allowlist t1,t2,...]\n"
                     "  Requires AEGISGATE_CP_API_KEY env var (SR8).\n";
        return kExitOk;
    }
    EnvLookup env = [](const char* n) { return std::getenv(n); };
    if (!requireApiKey(env)) {
        std::cerr << "Error: " << kApiKeyEnvVar
                  << " environment variable is required (SR8)\n";
        return kExitFail;
    }

    aegisgate::HnswVectorStore target(128, 100000, 64);
    target.initialize();
    aegisgate::CacheMigrator m;
    auto stats = m.restore(parsed.input, target, parsed.tenant_allowlist);
    emitJsonProgress(std::cout, "restore", stats.entries_restored);
    if (!stats.failure_reason.empty()) {
        std::cerr << "{\"error\":\"" << stats.failure_reason << "\"}\n";
        return kExitFail;
    }
    if (stats.entries_skipped_tenant > 0 || stats.entries_corrupted > 0) {
        std::cout << "{\"skipped_tenant\":" << stats.entries_skipped_tenant
                  << ",\"corrupted\":" << stats.entries_corrupted << "}\n";
        return kExitPartial;
    }
    return kExitOk;
}

int cmdMigrate(const std::vector<std::string>& args) {
    std::string from_type, from_path, to_type, to_path, to_url;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--from" && i + 1 < args.size()) from_type = args[++i];
        else if (args[i] == "--from-path" && i + 1 < args.size()) from_path = args[++i];
        else if (args[i] == "--to" && i + 1 < args.size()) to_type = args[++i];
        else if (args[i] == "--to-path" && i + 1 < args.size()) to_path = args[++i];
        else if (args[i] == "--to-url" && i + 1 < args.size()) to_url = args[++i];
    }

    if (from_type.empty() || to_type.empty()) {
        std::cerr << "Error: --from and --to are required\n";
        return 1;
    }

    auto source = makeStore(from_type, from_path, "");
    auto target = makeStore(to_type, to_path, to_url);
    if (!source || !target) return 1;

    if (!source->initialize()) {
        std::cerr << "Error: failed to initialize source store\n";
        return 1;
    }
    if (!target->initialize()) {
        std::cerr << "Error: failed to initialize target store\n";
        return 1;
    }

    std::cout << "Migrating from " << source->backendName()
              << " to " << target->backendName() << "...\n";

    aegisgate::MigrationTool tool;
    auto result = tool.migrate(*source, *target,
        [](const std::string& phase, int64_t done, int64_t total) {
            std::cout << "  " << phase << ": " << done << "/" << total << "\n";
        });

    if (result.success) {
        std::cout << "Migration complete:\n"
                  << "  Audits: " << result.audits_migrated << " migrated, "
                  << result.audits_skipped << " skipped\n"
                  << "  Costs:  " << result.costs_migrated << " migrated, "
                  << result.costs_skipped << " skipped\n";
        return 0;
    }

    std::cerr << "Migration failed: " << result.error << "\n";
    return 1;
}

int cmdConfigValidate(const std::vector<std::string>& args) {
    std::string config_path = "config/aegisgate.yaml";
    bool strict = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--strict") {
            strict = true;
        } else {
            config_path = args[i];
        }
    }

    aegisgate::Config config;
    if (!config.loadFromFile(config_path)) {
        std::cerr << "ERROR: Cannot parse YAML file: " << config_path << "\n";
        return 1;
    }

    auto issues = config.validate();
    int errors = 0, warnings = 0;
    for (const auto& issue : issues) {
        bool is_err = (issue.severity == aegisgate::Config::ValidationIssue::Error);
        if (strict || is_err) {
            std::cerr << (is_err ? "ERROR" : "WARN ") << "  [" << issue.field << "] "
                      << issue.message << "\n";
        } else {
            std::cout << "WARN   [" << issue.field << "] " << issue.message << "\n";
        }
        if (is_err) errors++;
        else warnings++;
    }

    if (errors == 0 && (!strict || warnings == 0)) {
        std::cout << "Configuration valid: " << config_path;
        if (warnings > 0) std::cout << " (" << warnings << " warnings)";
        std::cout << "\n";
        return 0;
    }
    std::cerr << "Validation failed: " << errors << " errors, " << warnings << " warnings\n";
    return 1;
}

int cmdBench(const CliConfig& cfg, const std::vector<std::string>& args) {
    int concurrency = 10;
    int total_requests = 100;
    std::string model;
    std::string prompt = "Say hello";

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--concurrency" && i + 1 < args.size())
            concurrency = std::stoi(args[++i]);
        else if (args[i] == "--requests" && i + 1 < args.size())
            total_requests = std::stoi(args[++i]);
        else if (args[i] == "--model" && i + 1 < args.size())
            model = args[++i];
        else if (args[i] == "--prompt" && i + 1 < args.size())
            prompt = args[++i];
    }

    nlohmann::json body;
    body["model"] = model;
    body["messages"] = nlohmann::json::array();
    body["messages"].push_back({{"role", "user"}, {"content", prompt}});

    std::cout << "Benchmark: " << total_requests << " requests, "
              << concurrency << " concurrent\n";

    std::vector<double> latencies;
    std::atomic<int> completed{0};
    std::atomic<int> error_count{0};
    std::mutex lat_mutex;

    auto start = std::chrono::steady_clock::now();

    auto work = aegisgate::cli::distributeWork(total_requests, concurrency);
    std::vector<std::thread> threads;

    for (int t = 0; t < concurrency; ++t) {
        int count = work[t];
        threads.emplace_back([&, count]() {
            auto client = drogon::HttpClient::newHttpClient(cfg.base_url);
            for (int i = 0; i < count; ++i) {
                auto req = drogon::HttpRequest::newHttpRequest();
                req->setMethod(drogon::Post);
                req->setPath("/v1/chat/completions");
                req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                req->setBody(body.dump());
                if (!cfg.api_key.empty()) {
                    req->addHeader("Authorization", "Bearer " + cfg.api_key);
                }
                auto req_start = std::chrono::steady_clock::now();
                auto [result, resp] = client->sendRequest(req, 60.0);
                auto req_end = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(
                    req_end - req_start).count();

                if (result != drogon::ReqResult::Ok || !resp ||
                    resp->statusCode() != drogon::k200OK) {
                    error_count.fetch_add(1);
                }
                {
                    std::lock_guard<std::mutex> lock(lat_mutex);
                    latencies.push_back(ms);
                }
                completed.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) th.join();

    auto end = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(end - start).count();

    auto stats = aegisgate::cli::computeBenchStats(
        latencies, error_count.load(), total_sec);

    std::cout << std::fixed << std::setprecision(1)
              << "  Total time:    " << stats.total_sec << "s\n"
              << "  RPS:           " << stats.rps << "\n"
              << "  Latency (ms):  p50=" << stats.p50
              << "  p90=" << stats.p90
              << "  p99=" << stats.p99 << "\n"
              << "  Errors:        " << stats.errors
              << " (" << std::setprecision(1)
              << (stats.completed > 0 ? 100.0 * stats.errors / stats.completed : 0.0) << "%)\n";
    return 0;
}

int cmdLogsTail(const CliConfig& cfg, const std::vector<std::string>& args) {
    std::string level, filter_key;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--level" && i + 1 < args.size()) level = args[++i];
        else if (args[i] == "--api-key" && i + 1 < args.size()) filter_key = args[++i];
    }

    std::string path = "/admin/logs/stream";
    std::string sep = "?";
    if (!level.empty()) { path += sep + "level=" + level; sep = "&"; }
    if (!filter_key.empty()) { path += sep + "api_key=" + filter_key; }

    try {
        auto client = drogon::HttpClient::newHttpClient(cfg.base_url);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath(path);
        if (!cfg.api_key.empty()) {
            req->addHeader("Authorization", "Bearer " + cfg.api_key);
        }

        auto [result, resp] = client->sendRequest(req, 0);
        if (result != drogon::ReqResult::Ok || !resp) {
            std::cerr << "Error: cannot connect to log stream\n";
            return 1;
        }
        if (resp->statusCode() != drogon::k200OK) {
            std::cerr << "Error " << static_cast<int>(resp->statusCode())
                      << ": " << resp->body() << "\n";
            return 1;
        }
        std::cout << resp->body();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int cmdReplay(const CliConfig& cfg, const std::vector<std::string>& args) {
    std::string file_path;
    int limit = 0;
    bool dry_run = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--file" && i + 1 < args.size()) file_path = args[++i];
        else if (args[i] == "--limit" && i + 1 < args.size()) limit = std::stoi(args[++i]);
        else if (args[i] == "--dry-run") dry_run = true;
    }

    if (file_path.empty()) {
        std::cerr << "Error: --file is required for replay\n";
        return 1;
    }

    std::ifstream infile(file_path);
    if (!infile.is_open()) {
        std::cerr << "Error: cannot open file: " << file_path << "\n";
        return 1;
    }

    int sent = 0, succeeded = 0, failed = 0, skipped = 0;
    std::string line;

    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        if (limit > 0 && sent >= limit) break;

        try {
            auto entry = nlohmann::json::parse(line);

            auto action = entry.value("action", "");
            if (action != "request_received" && action != "chat_request") {
                ++skipped;
                continue;
            }

            auto detail_str = entry.value("detail", "");
            if (detail_str.empty()) {
                ++skipped;
                continue;
            }

            nlohmann::json detail;
            try {
                detail = nlohmann::json::parse(detail_str);
            } catch (...) {
                ++skipped;
                continue;
            }

            if (!detail.contains("model") || !detail.contains("messages")) {
                ++skipped;
                continue;
            }

            nlohmann::json body;
            body["model"] = detail["model"];
            body["messages"] = detail["messages"];
            if (detail.contains("stream")) body["stream"] = detail["stream"];

            auto orig_id = entry.value("request_id", "?");

            if (dry_run) {
                std::cout << "[DRY-RUN] " << orig_id << " → "
                          << body["model"].get<std::string>()
                          << " (" << body["messages"].size() << " messages)\n";
                ++sent;
                ++succeeded;
                continue;
            }

            auto start = std::chrono::steady_clock::now();
            auto [result, resp] = httpPost(
                cfg.base_url, "/v1/chat/completions", body.dump(), cfg.api_key);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            ++sent;
            if (result == drogon::ReqResult::Ok && resp &&
                resp->statusCode() == drogon::k200OK) {
                ++succeeded;
                std::cout << "[OK] " << orig_id << " → "
                          << elapsed.count() << "ms\n";
            } else {
                ++failed;
                int status = resp ? static_cast<int>(resp->statusCode()) : 0;
                std::cerr << "[FAIL] " << orig_id << " → HTTP "
                          << status << " (" << elapsed.count() << "ms)\n";
            }
        } catch (const std::exception&) {
            ++skipped;
        }
    }

    std::cout << "\nReplay summary:\n"
              << "  Sent:      " << sent << "\n"
              << "  Succeeded: " << succeeded << "\n"
              << "  Failed:    " << failed << "\n"
              << "  Skipped:   " << skipped << "\n";
    return failed > 0 ? 1 : 0;
}

bool handleApiResponse(const drogon::ReqResult& result,
                       const drogon::HttpResponsePtr& resp,
                       const std::string& context) {
    if (result != drogon::ReqResult::Ok || !resp) {
        std::cerr << "Error: cannot reach gateway (" << context << ")\n";
        return false;
    }
    int code = static_cast<int>(resp->statusCode());
    if (code >= 400) {
        try {
            auto j = nlohmann::json::parse(resp->body());
            std::cerr << "Error " << code << ": "
                      << j["error"].value("message", resp->body()) << "\n";
        } catch (...) {
            std::cerr << "Error " << code << ": " << resp->body() << "\n";
        }
        return false;
    }
    return true;
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(' ');
        if (start != std::string::npos)
            result.push_back(item.substr(start));
    }
    return result;
}

int cmdTenant(const CliConfig& cfg, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: aegisctl tenant <list|create|get|update|delete> [args]\n";
        return 1;
    }
    const auto& action = args[0];

    if (action == "list") {
        auto [result, resp] = httpGet(cfg.base_url, "/admin/tenants", cfg.api_key);
        if (!handleApiResponse(result, resp, "tenant list")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        auto tenants = j.value("tenants", nlohmann::json::array());
        std::cout << "Tenants (" << tenants.size() << "):\n";
        for (const auto& t : tenants) {
            std::cout << "  " << t.value("id", "?")
                      << "  " << t.value("name", "?")
                      << "  [" << t.value("status", "?") << "]\n";
        }
        return 0;
    }

    if (action == "create") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl tenant create <name> [options]\n";
            return 1;
        }
        nlohmann::json body;
        body["name"] = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--model-whitelist" && i + 1 < args.size())
                body["model_whitelist"] = splitCsv(args[++i]);
            else if (args[i] == "--daily-cost-limit" && i + 1 < args.size())
                body["daily_cost_limit"] = std::stod(args[++i]);
            else if (args[i] == "--monthly-cost-limit" && i + 1 < args.size())
                body["monthly_cost_limit"] = std::stod(args[++i]);
        }
        auto [result, resp] = httpPost(cfg.base_url, "/admin/tenants", body.dump(), cfg.api_key);
        if (!handleApiResponse(result, resp, "tenant create")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Created tenant: " << j.value("id", "?")
                  << " (" << j.value("name", "?") << ")\n";
        return 0;
    }

    if (action == "get") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl tenant get <id>\n";
            return 1;
        }
        auto [result, resp] = httpGet(cfg.base_url, "/admin/tenants/" + args[1], cfg.api_key);
        if (!handleApiResponse(result, resp, "tenant get")) return 1;
        std::cout << nlohmann::json::parse(resp->body()).dump(2) << "\n";
        return 0;
    }

    if (action == "update") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl tenant update <id> [options]\n";
            return 1;
        }
        nlohmann::json body;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--name" && i + 1 < args.size())
                body["name"] = args[++i];
            else if (args[i] == "--status" && i + 1 < args.size())
                body["status"] = args[++i];
            else if (args[i] == "--model-whitelist" && i + 1 < args.size())
                body["model_whitelist"] = splitCsv(args[++i]);
            else if (args[i] == "--daily-cost-limit" && i + 1 < args.size())
                body["daily_cost_limit"] = std::stod(args[++i]);
            else if (args[i] == "--monthly-cost-limit" && i + 1 < args.size())
                body["monthly_cost_limit"] = std::stod(args[++i]);
        }
        auto [result, resp] = httpPut(cfg.base_url, "/admin/tenants/" + args[1], body.dump(), cfg.api_key);
        if (!handleApiResponse(result, resp, "tenant update")) return 1;
        std::cout << "Tenant updated.\n";
        return 0;
    }

    if (action == "delete") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl tenant delete <id>\n";
            return 1;
        }
        auto [result, resp] = httpDelete(cfg.base_url, "/admin/tenants/" + args[1], cfg.api_key);
        if (!handleApiResponse(result, resp, "tenant delete")) return 1;
        std::cout << "Tenant deleted.\n";
        return 0;
    }

    std::cerr << "Unknown tenant action: " << action << "\n";
    return 1;
}

int cmdUser(const CliConfig& cfg, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: aegisctl user <list|create|get|update|delete> [args]\n";
        return 1;
    }
    const auto& action = args[0];

    if (action == "list") {
        std::string path = "/admin/users";
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--tenant" && i + 1 < args.size())
                path += "?tenant_id=" + args[++i];
        }
        auto [result, resp] = httpGet(cfg.base_url, path, cfg.api_key);
        if (!handleApiResponse(result, resp, "user list")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        auto users = j.value("users", nlohmann::json::array());
        std::cout << "Users (" << users.size() << "):\n";
        for (const auto& u : users) {
            std::cout << "  " << u.value("id", "?")
                      << "  " << u.value("username", "?")
                      << "  role=" << u.value("role", "?")
                      << "  [" << u.value("status", "?") << "]\n";
        }
        return 0;
    }

    if (action == "create") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl user create <username> --tenant <id> --role <role>\n";
            return 1;
        }
        nlohmann::json body;
        body["username"] = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--tenant" && i + 1 < args.size())
                body["tenant_id"] = args[++i];
            else if (args[i] == "--role" && i + 1 < args.size())
                body["role"] = args[++i];
            else if (args[i] == "--display-name" && i + 1 < args.size())
                body["display_name"] = args[++i];
        }
        auto [result, resp] = httpPost(cfg.base_url, "/admin/users", body.dump(), cfg.api_key);
        if (!handleApiResponse(result, resp, "user create")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Created user: " << j.value("id", "?")
                  << " (" << j.value("username", "?") << ")\n";
        return 0;
    }

    if (action == "get") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl user get <id>\n";
            return 1;
        }
        auto [result, resp] = httpGet(cfg.base_url, "/admin/users/" + args[1], cfg.api_key);
        if (!handleApiResponse(result, resp, "user get")) return 1;
        std::cout << nlohmann::json::parse(resp->body()).dump(2) << "\n";
        return 0;
    }

    if (action == "update") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl user update <id> [options]\n";
            return 1;
        }
        nlohmann::json body;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--role" && i + 1 < args.size())
                body["role"] = args[++i];
            else if (args[i] == "--status" && i + 1 < args.size())
                body["status"] = args[++i];
            else if (args[i] == "--display-name" && i + 1 < args.size())
                body["display_name"] = args[++i];
        }
        auto [result, resp] = httpPut(cfg.base_url, "/admin/users/" + args[1], body.dump(), cfg.api_key);
        if (!handleApiResponse(result, resp, "user update")) return 1;
        std::cout << "User updated.\n";
        return 0;
    }

    if (action == "delete") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl user delete <id>\n";
            return 1;
        }
        auto [result, resp] = httpDelete(cfg.base_url, "/admin/users/" + args[1], cfg.api_key);
        if (!handleApiResponse(result, resp, "user delete")) return 1;
        std::cout << "User deleted.\n";
        return 0;
    }

    std::cerr << "Unknown user action: " << action << "\n";
    return 1;
}

int cmdKey(const CliConfig& cfg, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: aegisctl key <list|create|revoke|rotate> [args]\n";
        return 1;
    }
    const auto& action = args[0];

    if (action == "list") {
        std::string path = "/admin/keys";
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--tenant" && i + 1 < args.size())
                path += "?tenant_id=" + args[++i];
        }
        auto [result, resp] = httpGet(cfg.base_url, path, cfg.api_key);
        if (!handleApiResponse(result, resp, "key list")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        auto keys = j.value("keys", nlohmann::json::array());
        std::cout << "API Keys (" << keys.size() << "):\n";
        for (const auto& k : keys) {
            std::cout << "  " << k.value("id", "?")
                      << "  " << k.value("key_prefix", "?") << "..."
                      << "  " << k.value("name", "?")
                      << "  [" << k.value("status", "?") << "]\n";
        }
        return 0;
    }

    if (action == "create") {
        nlohmann::json body;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--user" && i + 1 < args.size())
                body["user_id"] = args[++i];
            else if (args[i] == "--name" && i + 1 < args.size())
                body["name"] = args[++i];
            else if (args[i] == "--role" && i + 1 < args.size())
                body["role"] = args[++i];
        }
        if (!body.contains("user_id") || !body.contains("name")) {
            std::cerr << "Usage: aegisctl key create --user <id> --name <name> [--role <role>]\n";
            return 1;
        }
        auto [result, resp] = httpPost(cfg.base_url, "/admin/keys", body.dump(), cfg.api_key);
        if (!handleApiResponse(result, resp, "key create")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Created API key:\n"
                  << "  ID:     " << j.value("id", "?") << "\n"
                  << "  Key:    " << j.value("key", "?") << "\n"
                  << "  Prefix: " << j.value("key_prefix", "?") << "\n"
                  << "  NOTE: Save the key now — it will not be shown again.\n";
        return 0;
    }

    if (action == "revoke") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl key revoke <id>\n";
            return 1;
        }
        auto [result, resp] = httpDelete(cfg.base_url, "/admin/keys/" + args[1], cfg.api_key);
        if (!handleApiResponse(result, resp, "key revoke")) return 1;
        std::cout << "API key revoked.\n";
        return 0;
    }

    if (action == "rotate") {
        if (args.size() < 2) {
            std::cerr << "Usage: aegisctl key rotate <id>\n";
            return 1;
        }
        auto [result, resp] = httpPost(
            cfg.base_url, "/admin/keys/" + args[1] + "/rotate", "{}", cfg.api_key);
        if (!handleApiResponse(result, resp, "key rotate")) return 1;
        auto j = nlohmann::json::parse(resp->body());
        std::cout << "Rotated API key:\n"
                  << "  New ID:  " << j.value("id", "?") << "\n"
                  << "  New Key: " << j.value("key", "?") << "\n"
                  << "  NOTE: Save the key now — it will not be shown again.\n";
        return 0;
    }

    std::cerr << "Unknown key action: " << action << "\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    CliConfig cfg;

    // Check env vars
    if (const char* url = std::getenv("AEGISGATE_URL")) {
        cfg.base_url = url;
    }
    if (const char* key = std::getenv("AEGISGATE_API_KEY")) {
        cfg.api_key = key;
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        args.emplace_back(argv[i]);
    }

    // Parse global options
    std::string command;
    std::vector<std::string> cmd_args;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--url" && i + 1 < args.size()) {
            cfg.base_url = args[++i];
        } else if (args[i] == "--api-key" && i + 1 < args.size()) {
            cfg.api_key = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            printUsage();
            return 0;
        } else if (command.empty()) {
            command = args[i];
        } else {
            cmd_args.push_back(args[i]);
        }
    }

    if (command.empty()) {
        printUsage();
        return 0;
    }

    if (command == "health") return cmdHealth(cfg);
    if (command == "models") return cmdModels(cfg);
    if (command == "metrics") return cmdMetrics(cfg);
    if (command == "migrate") return cmdMigrate(cmd_args);
    if (command == "bench") return cmdBench(cfg, cmd_args);
    if (command == "estimate") return aegisgate::cli::runEstimate(cmd_args);
    if (command == "replay") return cmdReplay(cfg, cmd_args);
    if (command == "replay-routes") {
        return aegisgate::cli::runReplayRoutesCommand(
            cmd_args, std::cout, std::cerr);
    }
    if (command == "config") {
        if (!cmd_args.empty() && cmd_args[0] == "validate") {
            std::vector<std::string> sub_args(cmd_args.begin() + 1, cmd_args.end());
            return cmdConfigValidate(sub_args);
        }
#ifdef AEGISGATE_ENABLE_CONTROL_PLANE
        // Phase 9.3 Epic 7 — route control-plane subcommands to the gRPC
        // dispatcher. Unknown subcommands fall through to the usage help
        // below so the error surface stays consistent with the OFF path.
        if (!cmd_args.empty()) {
            static const std::vector<std::string> kControlPlaneSubs = {
                "apply", "approve", "reject", "activate", "rollback",
                "list", "get", "show", "current", "diff",
            };
            // Also accept global flags *before* the subcommand (operators
            // often write `aegisctl config --endpoint ... apply file.yaml`).
            auto starts_with_global = [](const std::string& t) {
                return t == "--endpoint" || t == "--tls-ca" ||
                       t == "--tls-cert" || t == "--tls-key" ||
                       t == "--timeout"  || t == "--output";
            };
            bool is_cp = starts_with_global(cmd_args[0]);
            for (const auto& s : kControlPlaneSubs) {
                if (cmd_args[0] == s) { is_cp = true; break; }
            }
            if (is_cp) {
                return aegisgate::cli::runConfigCommand(cmd_args,
                                                        std::cout,
                                                        std::cerr);
            }
        }
#endif
        std::cerr << "Usage: aegisctl config validate [path] [--strict]\n";
#ifdef AEGISGATE_ENABLE_CONTROL_PLANE
        std::cerr <<
            "       aegisctl config <apply|approve|reject|activate|rollback|\n"
            "                        list|get|show|current|diff> [flags]\n"
            "       (requires AEGISGATE_CP_API_KEY; see docs/ops for details)\n";
#endif
        return 1;
    }
#ifdef AEGISGATE_ENABLE_CONTROL_PLANE
    if (command == "rollout") {
        if (cmd_args.empty()) {
            std::cerr << "Usage: aegisctl rollout <create|start|pause|resume|promote|abort|status|list> [flags]\n"
                      << "       (requires AEGISGATE_CP_API_KEY; see docs/ops for details)\n";
            return 1;
        }
        return aegisgate::cli::runRolloutCommand(cmd_args, std::cout, std::cerr);
    }
#endif
    if (command == "logs") {
        if (!cmd_args.empty() && cmd_args[0] == "tail") {
            std::vector<std::string> sub_args(cmd_args.begin() + 1, cmd_args.end());
            return cmdLogsTail(cfg, sub_args);
        }
        std::cerr << "Usage: aegisctl logs tail [--level <level>]\n";
        return 1;
    }
    if (command == "cache") {
        if (cmd_args.empty()) {
            std::cerr << "Usage: aegisctl cache <stats|import|dump|restore> "
                         "[args]\n";
            return 1;
        }
        if (cmd_args[0] == "stats") return cmdCacheStats(cfg);
        if (cmd_args[0] == "import") {
            if (cmd_args.size() < 2) {
                std::cerr << "Usage: aegisctl cache import <file.json>\n";
                return 1;
            }
            return cmdCacheImport(cfg, cmd_args[1]);
        }
        // Phase 6.2 (Epic 3.4): offline dump/restore. SR8 enforced inside.
        if (cmd_args[0] == "dump") {
            std::vector<std::string> sub(cmd_args.begin() + 1, cmd_args.end());
            return cmdCacheDump(sub);
        }
        if (cmd_args[0] == "restore") {
            std::vector<std::string> sub(cmd_args.begin() + 1, cmd_args.end());
            return cmdCacheRestore(sub);
        }
        std::cerr << "Unknown cache subcommand: " << cmd_args[0] << "\n";
        return 1;
    }
    if (command == "chat") {
        if (cmd_args.empty()) {
            std::cerr << "Usage: aegisctl chat <message>\n";
            return 1;
        }
        std::string msg;
        for (const auto& a : cmd_args) {
            if (!msg.empty()) msg += " ";
            msg += a;
        }
        return cmdChat(cfg, msg);
    }
    if (command == "tenant") return cmdTenant(cfg, cmd_args);
    if (command == "user") return cmdUser(cfg, cmd_args);
    if (command == "key") return cmdKey(cfg, cmd_args);
    if (command == "rules") {
        if (cmd_args.empty()) {
            std::cerr << "Usage: aegisctl rules <list|install|remove|info|apply>\n";
            return 1;
        }
        std::string install_dir = "data/rulepacks";
        std::string rules_dir = "config/rules";
        aegisgate::RulePackManager mgr(install_dir);

        if (cmd_args[0] == "list") {
            auto packs = mgr.list();
            if (packs.empty()) {
                std::cout << "No rule packs installed.\n";
                return 0;
            }
            std::cout << std::left << std::setw(20) << "NAME"
                      << std::setw(12) << "VERSION"
                      << "DESCRIPTION\n";
            for (const auto& p : packs) {
                std::cout << std::setw(20) << p.name
                          << std::setw(12) << p.version
                          << p.description << "\n";
            }
            return 0;
        }
        if (cmd_args[0] == "install") {
            if (cmd_args.size() < 2) {
                std::cerr << "Usage: aegisctl rules install <path>\n";
                return 1;
            }
            return mgr.install(cmd_args[1]) ? 0 : 1;
        }
        if (cmd_args[0] == "remove") {
            if (cmd_args.size() < 2) {
                std::cerr << "Usage: aegisctl rules remove <name>\n";
                return 1;
            }
            return mgr.remove(cmd_args[1]) ? 0 : 1;
        }
        if (cmd_args[0] == "info") {
            if (cmd_args.size() < 2) {
                std::cerr << "Usage: aegisctl rules info <name>\n";
                return 1;
            }
            auto p = mgr.info(cmd_args[1]);
            if (!p) {
                std::cerr << "Rule pack '" << cmd_args[1] << "' not found.\n";
                return 1;
            }
            std::cout << "Name: " << p->name << "\n"
                      << "Version: " << p->version << "\n"
                      << "Description: " << p->description << "\n";
            if (!p->tags.empty()) {
                std::cout << "Tags: ";
                for (size_t i = 0; i < p->tags.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << p->tags[i];
                }
                std::cout << "\n";
            }
            return 0;
        }
        if (cmd_args[0] == "apply") {
            return mgr.applyAll(rules_dir) ? 0 : 1;
        }
        std::cerr << "Unknown rules subcommand: " << cmd_args[0] << "\n";
        return 1;
    }

    if (command == "conformance") {
        if (cmd_args.empty()) {
            std::cerr << "Usage: aegisctl conformance <check|check-all> <path>\n";
            return 1;
        }

        auto print_issues = [](const aegisgate::ValidationReport& r) {
            for (const auto& issue : r.issues) {
                std::cout << "    "
                          << (issue.severity == aegisgate::ValidationIssue::Severity::Error
                                  ? "[E]" : "[W]")
                          << " " << issue.code << " :: " << issue.message;
                if (!issue.field_path.empty()) {
                    std::cout << "  (" << issue.field_path << ")";
                }
                std::cout << "\n";
            }
        };

        auto check_one = [&](const std::string& path) -> int {
            aegisgate::ValidationReport parse_report;
            auto manifest = aegisgate::loadManifestFromFile(path, parse_report);
            aegisgate::ValidationReport combined = parse_report;
            if (manifest.has_value()) {
                combined.merge(aegisgate::validateManifest(*manifest));
                combined.merge(aegisgate::runConformanceChecks(*manifest));
            }
            const bool ok = combined.ok();
            std::cout << (ok ? "PASS" : "FAIL") << "  " << path
                      << "  (errors=" << combined.errorCount()
                      << ", warnings=" << combined.warningCount() << ")\n";
            print_issues(combined);
            return ok ? 0 : 1;
        };

        if (cmd_args[0] == "check") {
            if (cmd_args.size() < 2) {
                std::cerr << "Usage: aegisctl conformance check <manifest.yaml>\n";
                return 1;
            }
            return check_one(cmd_args[1]);
        }

        if (cmd_args[0] == "check-all") {
            if (cmd_args.size() < 2) {
                std::cerr << "Usage: aegisctl conformance check-all <dir>\n";
                return 1;
            }
            int failures = 0;
            int files = 0;
            namespace fs = std::filesystem;
            try {
                for (const auto& entry : fs::directory_iterator(cmd_args[1])) {
                    if (!entry.is_regular_file()) continue;
                    if (entry.path().extension() != ".yaml" &&
                        entry.path().extension() != ".yml") {
                        continue;
                    }
                    ++files;
                    if (check_one(entry.path().string()) != 0) ++failures;
                }
            } catch (const std::exception& e) {
                std::cerr << "Failed to scan directory: " << e.what() << "\n";
                return 1;
            }
            std::cout << "\n" << (files - failures) << "/" << files << " PASS, "
                      << failures << " FAIL\n";
            return failures == 0 ? 0 : 1;
        }

        std::cerr << "Unknown conformance subcommand: " << cmd_args[0] << "\n";
        return 1;
    }

    std::cerr << "Unknown command: " << command << "\n";
    printUsage();
    return 1;
}
