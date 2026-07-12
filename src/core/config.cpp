#include "config.h"
#include "aegisgate/types.h"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <fstream>

namespace aegisgate {

bool Config::loadFromFile(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    try {
        root_ = YAML::LoadFile(path);
        file_path_ = path;
        loaded_ = true;
        spdlog::info("Config loaded from: {}", path);
        return true;
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to load config from {}: {}", path, e.what());
        return false;
    }
}

namespace {

RolloutConfigView parseRolloutSection(const YAML::Node& node) {
    RolloutConfigView rv;
    rv.rollout_id = node["rollout_id"].as<std::string>("");
    rv.target_version_id = node["target_version_id"].as<std::string>("");
    rv.sticky_key = node["sticky_key"].as<std::string>("tenant_id");
    if (auto cs = node["current_stage"]; cs && cs.IsMap()) {
        rv.current_stage.stage_index = cs["stage_index"].as<int>(0);
        rv.current_stage.name = cs["name"].as<std::string>("");
        if (auto sc = cs["scope"]; sc && sc.IsMap()) {
            if (auto tg = sc["tenant_globs"]; tg && tg.IsSequence()) {
                for (const auto& g : tg)
                    rv.current_stage.scope.tenant_globs.push_back(g.as<std::string>());
            }
            if (auto rg = sc["regions"]; rg && rg.IsSequence()) {
                for (const auto& r : rg)
                    rv.current_stage.scope.regions.push_back(r.as<std::string>());
            }
            rv.current_stage.scope.percentage = sc["percentage"].as<int>(0);
        }
    }
    return rv;
}

} // namespace

bool Config::loadFromString(const std::string& yaml_content) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    try {
        auto top = YAML::Load(yaml_content);
        file_path_.clear();

        // Phase 9.3.4: detect merged-yaml format by checking for
        // `active_version_id` top-level key.
        if (top["active_version_id"] && top["active_version_id"].IsScalar()) {
            active_version_id_ = top["active_version_id"].as<std::string>();
            rollout_config_.reset();
            configs_by_version_.clear();

            if (auto rn = top["rollout"]; rn && rn.IsMap()) {
                rollout_config_ = parseRolloutSection(rn);
            }

            // Parse sub-configs from the `configs` map. The active version's
            // sub-config becomes root_ so all existing getters work seamlessly.
            if (auto cfgs = top["configs"]; cfgs && cfgs.IsMap()) {
                for (auto it = cfgs.begin(); it != cfgs.end(); ++it) {
                    auto vid = it->first.as<std::string>();
                    if (vid == active_version_id_) {
                        root_ = it->second;
                    } else {
                        auto sub = std::make_unique<Config>();
                        sub->root_ = it->second;
                        sub->loaded_ = true;
                        configs_by_version_[vid] = std::move(sub);
                    }
                }
            } else {
                root_ = YAML::Load("{}");
            }
        } else {
            // Legacy format: entire yaml is the inline config.
            root_ = top;
            active_version_id_ = "inline";
            rollout_config_.reset();
            configs_by_version_.clear();
        }

        loaded_ = true;
        return true;
    } catch (const YAML::Exception& e) {
        spdlog::debug("Config::loadFromString failed: {}", e.what());
        return false;
    }
}

bool Config::reload() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (file_path_.empty()) return false;
    try {
        auto new_root = YAML::LoadFile(file_path_);
        root_ = new_root;
        spdlog::info("Config reloaded from: {}", file_path_);
        return true;
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to reload config: {}", e.what());
        return false;
    }
}

bool Config::isLoaded() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return loaded_;
}

const std::string& Config::filePath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return file_path_;
}

Edition Config::edition() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return Edition::Community;
    auto val = root_["edition"].as<std::string>("community");
    return (val == "enterprise") ? Edition::Enterprise : Edition::Community;
}

int Config::serverPort() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 8080;
    return safeGet<int>("server", "port", 8080);
}

std::string Config::serverHost() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "0.0.0.0";
    return safeGet<std::string>("server", "host", "0.0.0.0");
}

int Config::serverThreads() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0;
    return safeGet<int>("server", "threads", 0);
}

int Config::requestTimeoutSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 120;
    return safeGet<int>("server", "request_timeout_seconds", 120);
}

size_t Config::maxRequestBodySize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 65536;
    return safeGet<size_t>("limits", "max_request_body_size", 65536);
}

std::string Config::logLevel() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "info";
    return safeGet<std::string>("logging", "level", "info");
}

std::string Config::logFile() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("logging", "file", "");
}

bool Config::featureEnabled(const std::string& feature) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    try {
        return root_["features"][feature].as<bool>(false);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to read feature '{}': {}", feature, e.what());
        return false;
    }
}

std::string Config::modelsConfigPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "config/models.yaml";
    return root_["models_config"].as<std::string>("config/models.yaml");
}

bool Config::authEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("auth", "enabled", false);
}

std::vector<std::string> Config::authApiKeys() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> keys;
    if (!loaded_) return keys;
    auto node = safeNode("auth", "api_keys");
    if (node && node.IsSequence()) {
        for (const auto& k : node) {
            try {
                auto val = k.as<std::string>();
                if (val.size() > 3 && val.substr(0, 2) == "${" && val.back() == '}') {
                    auto env_name = val.substr(2, val.size() - 3);
                    const char* env_val = std::getenv(env_name.c_str());
                    if (env_val && std::string(env_val).size() > 0) {
                        keys.push_back(env_val);
                    }
                } else {
                    keys.push_back(val);
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to parse auth.api_keys: {}", e.what());
            }
        }
    }
    return keys;
}

std::string Config::adminApiKey() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    auto val = safeGet<std::string>("auth", "admin_key", "");
    if (val.size() > 3 && val.substr(0, 2) == "${" && val.back() == '}') {
        auto env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        return (env_val && std::string(env_val).size() > 0) ? std::string(env_val) : "";
    }
    return val;
}

bool Config::tlsEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("tls", "enabled", false);
}

int Config::tlsPort() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0;
    return safeGet<int>("tls", "port", 0);
}

std::string Config::tlsCertPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("tls", "cert_path", "");
}

std::string Config::tlsKeyPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("tls", "key_path", "");
}

std::string Config::auditLogPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("audit", "log_path", "");
}

std::string Config::licensePath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return root_["license_file"].as<std::string>("");
}

double Config::rateLimitMaxTokens() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 100.0;
    return safeGet<double>("rate_limit", "max_tokens", 100.0);
}

double Config::rateLimitRefillRate() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10.0;
    return safeGet<double>("rate_limit", "refill_rate", 10.0);
}

int Config::circuitFailureThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3;
    return safeGet<int>("circuit_breaker", "failure_threshold", 3);
}

int Config::circuitResetTimeoutSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 30;
    return safeGet<int>("circuit_breaker", "reset_timeout_seconds", 30);
}

int Config::circuitHalfOpenMaxCalls() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1;
    return safeGet<int>("circuit_breaker", "half_open_max_calls", 1);
}

int Config::circuitMaxCircuits() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 512;
    return safeGet<int>("circuit_breaker", "max_circuits", 512);
}

int Config::circuitIdleTtlSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0;
    return safeGet<int>("circuit_breaker", "circuit_idle_ttl_seconds", 0);
}

std::string Config::embeddingModelPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("embedding", "model_path", "");
}

std::string Config::embeddingVocabPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("embedding", "vocab_path", "");
}

int Config::auditRetentionDays() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0;
    return safeGet<int>("audit", "retention_days", 0);
}

std::string Config::cacheBackend() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "memory";
    return safeGet<std::string>("storage", "cache_backend", "memory");
}

std::string Config::persistentBackend() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "sqlite";
    return safeGet<std::string>("storage", "persistent_backend", "sqlite");
}

bool Config::strictBackends() const {
    // env 覆盖优先（部署期可临时切换，无需改 YAML）
    if (const char* v = std::getenv("AEGISGATE_STRICT_BACKENDS")) {
        std::string s(v);
        return (s == "1" || s == "true" || s == "on" || s == "yes");
    }
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;  // 默认 true（fail-closed）
    return safeGet<bool>("storage", "strict_backends", true);
}

std::string Config::sqlitePath() const {
    if (const char* env = std::getenv("AEGISGATE_SQLITE_PATH")) {
        std::string s(env);
        if (!s.empty()) return s;
    }
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "data/aegisgate.db";
    return safeGet<std::string>("storage", "sqlite", "path", "data/aegisgate.db");
}

bool Config::sqliteWalMode() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("storage", "sqlite", "wal_mode", true);
}

bool Config::dashboardPersistenceEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("dashboard", "persistence_enabled", true);
}

int Config::dashboardReloadDays() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 30;
    return safeGet<int>("dashboard", "reload_days", 30);
}

std::string Config::redisHost() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "127.0.0.1";
    return safeGet<std::string>("storage", "redis", "host", "127.0.0.1");
}

int Config::redisPort() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 6379;
    return safeGet<int>("storage", "redis", "port", 6379);
}

std::string Config::redisPassword() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    auto val = safeGet<std::string>("storage", "redis", "password", "");
    if (val.size() > 3 && val.substr(0, 2) == "${" && val.back() == '}') {
        auto env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        return (env_val) ? std::string(env_val) : "";
    }
    return val;
}

int Config::redisDb() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0;
    return safeGet<int>("storage", "redis", "db", 0);
}

int Config::redisPoolSize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 4;
    return safeGet<int>("storage", "redis", "pool_size", 4);
}

int Config::redisConnectTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3000;
    return safeGet<int>("storage", "redis", "connect_timeout_ms", 3000);
}

int Config::redisCommandTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1000;
    return safeGet<int>("storage", "redis", "command_timeout_ms", 1000);
}

std::string Config::pgUrl() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    auto val = safeGet<std::string>("storage", "postgres", "url", "");
    if (val.size() > 3 && val.substr(0, 2) == "${" && val.back() == '}') {
        auto env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        return (env_val) ? std::string(env_val) : "";
    }
    return val;
}

int Config::pgPoolSize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 4;
    return safeGet<int>("storage", "postgres", "pool_size", 4);
}

int Config::pgConnectTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5000;
    return safeGet<int>("storage", "postgres", "connect_timeout_ms", 5000);
}

// --- Phase 9.3: Control-plane server options ---
//
// All readers default-safe when loaded_ is false *or* when the relevant
// YAML node is missing: the control plane binary must be runnable from a
// minimal config (just `tls.cert_file` + `tls.key_file` + `storage.*`).

std::string Config::controlPlaneListen() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "0.0.0.0:9443";
    auto cp = root_["control_plane"];
    if (!cp || !cp.IsMap()) return "0.0.0.0:9443";
    auto srv = cp["server"];
    if (srv && srv.IsMap()) {
        auto addr = srv["listen_address"];
        if (addr && !addr.IsNull()) return addr.as<std::string>("0.0.0.0:9443");
    }
    // Fall back to top-level "server.listen_address" if operators shared
    // the data-plane YAML and simply appended a `control_plane` block.
    auto top = root_["server"];
    if (top && top.IsMap()) {
        auto addr = top["listen_address"];
        if (addr && !addr.IsNull()) return addr.as<std::string>("0.0.0.0:9443");
    }
    return "0.0.0.0:9443";
}

bool Config::controlPlaneMutualTls() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    auto tls = root_["tls"];
    if (!tls || !tls.IsMap()) return false;
    auto m = tls["mutual"];
    if (!m || m.IsNull()) return false;
    return m.as<bool>(false);
}

std::vector<std::string> Config::controlPlaneAllowedClientFingerprints() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> out;
    if (!loaded_) return out;
    auto tls = root_["tls"];
    if (!tls || !tls.IsMap()) return out;
    auto fps = tls["allowed_client_fingerprints_sha256"];
    if (!fps || !fps.IsSequence()) return out;
    out.reserve(fps.size());
    for (const auto& n : fps) {
        if (n.IsNull()) continue;
        out.push_back(n.as<std::string>(""));
    }
    return out;
}

int Config::controlPlaneSubmitRateLimitPerUserPerMin() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10;
    return safeGet<int>("control_plane", "submit_rate_limit_per_user_per_min", 10);
}

int Config::controlPlaneMaxYamlBytes() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    // 1 MiB cap mirrors SR2 and the ServerBootstrap transport guard so
    // operators cannot accidentally relax one without the other.
    if (!loaded_) return 1 * 1024 * 1024;
    return safeGet<int>("control_plane", "max_yaml_size_bytes", 1 * 1024 * 1024);
}

std::string Config::controlPlaneBootstrapYaml() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return {};
    return safeGet<std::string>("control_plane", "bootstrap_from_active_yaml",
                                 std::string{});
}

// --- Security: InputPreprocessor ---

bool Config::unicodeNormalizationEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("security", "unicode_normalization", true);
}

bool Config::encodingDetectionEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("security", "encoding_detection", true);
}

bool Config::injectionFailOpen() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;  // P0-2: default fail-closed
    return safeGet<bool>("security", "injection", "fail_open", false);
}

int Config::encodingMinBase64Length() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 20;
    return safeGet<int>("security", "encoding_min_base64_length", 20);
}

size_t Config::imageScanMaxDecodeBytes() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_)
        return kDefaultImageScanDecodeBytes;
    int v = safeGet<int>("security", "image_scan_max_decode_bytes",
                         static_cast<int>(kDefaultImageScanDecodeBytes));
    return v > 0 ? static_cast<size_t>(v) : kDefaultImageScanDecodeBytes;
}

// --- Security: GuardClassifier ---

bool Config::guardModelEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("security", "guard_model", "enabled", false);
}

std::string Config::guardModelPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("security", "guard_model", "model_path", "");
}

std::string Config::guardModelVocabPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("security", "guard_model", "vocab_path", "");
}

std::string Config::guardModelSpmPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("security", "guard_model", "spm_model_path", "");
}

std::string Config::guardModelFailPolicy() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "open";
    return safeGet<std::string>("security", "guard_model", "fail_policy", "open");
}

double Config::guardModelThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.5;
    return safeGet<double>("security", "guard_model", "threshold", 0.5);
}

// --- Security: External Safety APIs (L4) ---

bool Config::externalSafetyEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("security", "external_safety", "enabled", false);
}

std::string Config::externalSafetyMode() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "any";
    return safeGet<std::string>("security", "external_safety", "mode", "any");
}

std::string Config::externalSafetyFailPolicy() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "open";
    return safeGet<std::string>("security", "external_safety", "fail_policy", "open");
}

bool Config::externalSafetyParallel() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("security", "external_safety", "parallel", true);
}

// Phase 6.3 (Epic 4.2): shadow_mode + SR6 inflight + SR3 audit TTL.
// All three default to safe/off values so legacy deployments stay
// blocking-mode without any config change.
bool Config::externalSafetyShadowMode() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("security", "external_safety", "shadow_mode", false);
}

int Config::externalSafetyShadowMaxInflight() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1000;
    return safeGet<int>("security", "external_safety", "shadow_max_inflight",
                         1000);
}

int Config::externalSafetyShadowAuditTtlSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 86400;
    return safeGet<int>("security", "external_safety",
                         "shadow_audit_ttl_seconds", 86400);
}

bool Config::openaiModerationEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("security", "external_safety", "openai_moderation",
                         "enabled", false);
}

std::string Config::openaiModerationApiKey() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    auto val = safeGet<std::string>("security", "external_safety",
                                    "openai_moderation", "api_key", "");
    if (val.size() > 3 && val.substr(0, 2) == "${" && val.back() == '}') {
        auto env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        return (env_val && std::string(env_val).size() > 0) ? std::string(env_val) : "";
    }
    return val;
}

std::string Config::openaiModerationBaseUrl() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "https://api.openai.com";
    return safeGet<std::string>("security", "external_safety",
                                "openai_moderation", "base_url",
                                "https://api.openai.com");
}

std::string Config::openaiModerationModel() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "omni-moderation-latest";
    return safeGet<std::string>("security", "external_safety",
                                "openai_moderation", "model",
                                "omni-moderation-latest");
}

int Config::openaiModerationTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5000;
    return safeGet<int>("security", "external_safety",
                        "openai_moderation", "timeout_ms", 5000);
}

bool Config::perspectiveApiEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("security", "external_safety", "perspective",
                         "enabled", false);
}

std::string Config::perspectiveApiKey() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    auto val = safeGet<std::string>("security", "external_safety",
                                    "perspective", "api_key", "");
    if (val.size() > 3 && val.substr(0, 2) == "${" && val.back() == '}') {
        auto env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        return (env_val && std::string(env_val).size() > 0) ? std::string(env_val) : "";
    }
    return val;
}

std::string Config::perspectiveBaseUrl() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "https://commentanalyzer.googleapis.com";
    return safeGet<std::string>("security", "external_safety",
                                "perspective", "base_url",
                                "https://commentanalyzer.googleapis.com");
}

double Config::perspectiveThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.7;
    return safeGet<double>("security", "external_safety", "perspective",
                           "threshold", 0.7);
}

std::vector<std::string> Config::perspectiveAttributes() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> defaults = {
        "TOXICITY", "SEVERE_TOXICITY", "IDENTITY_ATTACK",
        "INSULT", "PROFANITY", "THREAT"
    };
    if (!loaded_) return defaults;
    auto node = safeNode("security", "external_safety", "perspective",
                         "attributes");
    if (node && node.IsSequence()) {
        std::vector<std::string> attrs;
        for (const auto& a : node) {
            try {
                attrs.push_back(a.as<std::string>());
            } catch (...) {
            }
        }
        if (!attrs.empty()) return attrs;
    }
    return defaults;
}

int Config::perspectiveTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5000;
    return safeGet<int>("security", "external_safety", "perspective",
                        "timeout_ms", 5000);
}

// --- Security: AbuseDetector ---

bool Config::abuseDetectionEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("security", "abuse_detection", "enabled", true);
}

int Config::abuseWindowSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 300;
    return safeGet<int>("security", "abuse_detection", "window_seconds", 300);
}

int Config::abuseWarnThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5;
    return safeGet<int>("security", "abuse_detection", "warn_threshold", 5);
}

int Config::abuseThrottleThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10;
    return safeGet<int>("security", "abuse_detection", "throttle_threshold", 10);
}

int Config::abuseBlockThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 20;
    return safeGet<int>("security", "abuse_detection", "block_threshold", 20);
}

int Config::abuseBlockDurationSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1800;
    return safeGet<int>("security", "abuse_detection",
                        "block_duration_seconds", 1800);
}

double Config::abuseThrottleFactor() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.5;
    return safeGet<double>("security", "abuse_detection", "throttle_factor", 0.5);
}

int Config::abuseMaxKeysPerShard() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1024;
    return safeGet<int>("security", "abuse_detection", "max_keys_per_shard", 1024);
}

bool Config::abuseSimilarityEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("security", "abuse_detection", "similarity_enabled", true);
}

int Config::abuseSimilarityHammingThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3;
    return safeGet<int>("security", "abuse_detection",
                        "similarity_hamming_threshold", 3);
}

int Config::abuseSimilarityMaxFingerprints() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 32;
    return safeGet<int>("security", "abuse_detection",
                        "similarity_max_fingerprints", 32);
}

int Config::abuseSimilarityMaxContentBytes() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 8192;
    return safeGet<int>("security", "abuse_detection",
                        "similarity_max_content_bytes", 8192);
}

// --- Vector Store ---

std::string Config::vectorStoreBackend() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "hnswlib";
    return safeGet<std::string>("vector_store", "backend", "hnswlib");
}

std::string Config::milvusHost() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "127.0.0.1";
    return safeGet<std::string>("vector_store", "milvus", "host", "127.0.0.1");
}
int Config::milvusPort() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 19530;
    return safeGet<int>("vector_store", "milvus", "port", 19530);
}
std::string Config::milvusCollectionPrefix() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "aegisgate_cache";
    return safeGet<std::string>("vector_store", "milvus", "collection_prefix", "aegisgate_cache");
}
std::string Config::milvusMetricType() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "IP";
    return safeGet<std::string>("vector_store", "milvus", "metric_type", "IP");
}
std::string Config::milvusToken() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("vector_store", "milvus", "token", "");
}
int Config::milvusConnectTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5000;
    return safeGet<int>("vector_store", "milvus", "connect_timeout_ms", 5000);
}
int Config::milvusRequestTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10000;
    return safeGet<int>("vector_store", "milvus", "request_timeout_ms", 10000);
}
bool Config::milvusAutoCreateCollection() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("vector_store", "milvus", "auto_create_collection", true);
}

std::string Config::qdrantHost() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "127.0.0.1";
    return safeGet<std::string>("vector_store", "qdrant", "host", "127.0.0.1");
}
int Config::qdrantPort() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 6333;
    return safeGet<int>("vector_store", "qdrant", "port", 6333);
}
std::string Config::qdrantCollectionPrefix() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "aegisgate_cache";
    return safeGet<std::string>("vector_store", "qdrant", "collection_prefix", "aegisgate_cache");
}
std::string Config::qdrantDistance() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "Cosine";
    return safeGet<std::string>("vector_store", "qdrant", "distance", "Cosine");
}
std::string Config::qdrantApiKey() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("vector_store", "qdrant", "api_key", "");
}
int Config::qdrantConnectTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5000;
    return safeGet<int>("vector_store", "qdrant", "connect_timeout_ms", 5000);
}
int Config::qdrantRequestTimeout() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10000;
    return safeGet<int>("vector_store", "qdrant", "request_timeout_ms", 10000);
}
bool Config::qdrantAutoCreateCollection() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("vector_store", "qdrant", "auto_create_collection", true);
}

// --- Cache ---

float Config::cacheThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.95f;
    return safeGet<float>("cache", "threshold", 0.95f);
}

int Config::cacheTtlSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3600;
    return safeGet<int>("cache", "ttl_seconds", 3600);
}

int Config::cacheMaxEntries() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10000;
    return safeGet<int>("cache", "max_entries", 10000);
}

int Config::cacheMaxPartitions() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 64;
    return safeGet<int>("cache", "max_partitions", 64);
}

bool Config::cacheContextAware() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("cache", "context_aware", true);
}

std::string Config::cacheWarmupFile() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("cache", "warmup_file", "");
}

std::string Config::cacheConversationHashMode() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "none";
    return safeGet<std::string>("cache", "conversation_hash", "mode", "none");
}

int Config::cacheConversationHashWindow() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 4;
    return safeGet<int>("cache", "conversation_hash", "window_size", 4);
}

// Phase 6.4 — Multi-turn conversation cache (TASK-20260513-01 Epic 5.1).
// All defaults are SAFE so legacy deployments observe zero behaviour change
// until cache.conversation_cache.enabled flips to true.
bool Config::conversationCacheEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("cache", "conversation_cache", "enabled", false);
}

std::string Config::conversationSummarizerType() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "rule_based";
    return safeGet<std::string>("cache", "conversation_cache", "summarizer",
                                "type", "rule_based");
}

std::string Config::conversationSummarizerOnnxModelPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("cache", "conversation_cache", "summarizer",
                                "onnx_model_path", "");
}

int Config::conversationSummarizerMaxSummaryMs() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 200;
    return safeGet<int>("cache", "conversation_cache", "summarizer",
                        "max_summary_ms", 200);
}

int Config::conversationSummarizerMaxInputTokens() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 4096;
    return safeGet<int>("cache", "conversation_cache", "summarizer",
                        "max_input_tokens", 4096);
}

bool Config::conversationIdResolverEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;  // resolver itself is harmless when cache is off
    return safeGet<bool>("cache", "conversation_cache", "id_resolver",
                         "enabled", true);
}

bool Config::cacheAdaptiveEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("cache", "adaptive_threshold", "enabled", false);
}

float Config::cacheAdaptiveMinThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.85f;
    return safeGet<float>("cache", "adaptive_threshold", "min_threshold", 0.85f);
}

float Config::cacheAdaptiveMaxThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.98f;
    return safeGet<float>("cache", "adaptive_threshold", "max_threshold", 0.98f);
}

float Config::cacheAdaptiveAdjustmentRate() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.01f;
    return safeGet<float>("cache", "adaptive_threshold", "adjustment_rate", 0.01f);
}

int Config::cacheAdaptiveWindowSize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 100;
    return safeGet<int>("cache", "adaptive_threshold", "window_size", 100);
}

bool Config::cachePolicyEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("cache", "policy", "enabled", true);
}

std::vector<std::string> Config::cachePolicySkipModels() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> models;
    if (!loaded_) return models;
    auto node = safeNode("cache", "policy", "skip_models");
    if (node && node.IsSequence()) {
        for (const auto& m : node) {
            try {
                models.push_back(m.as<std::string>());
            } catch (const std::exception&) {
            }
        }
    }
    return models;
}

double Config::cachePolicyMaxTemperature() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1.0;
    return safeGet<double>("cache", "policy", "max_temperature", 1.0);
}

bool Config::cachePolicySkipStreaming() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("cache", "policy", "skip_streaming", false);
}

std::vector<Config::AlertChannelConfig> Config::alertChannels() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<AlertChannelConfig> result;
    try {
        auto alerting = root_["alerting"];
        if (!alerting || !alerting.IsMap()) return result;
        auto channels = alerting["channels"];
        if (!channels || !channels.IsSequence()) return result;
        for (const auto& ch : channels) {
            AlertChannelConfig cfg;
            cfg.name = ch["name"].as<std::string>("");
            cfg.type = ch["type"].as<std::string>("");
            cfg.url = ch["url"].as<std::string>("");
            cfg.secret = ch["secret"].as<std::string>("");
            if (!cfg.type.empty() && !cfg.url.empty()) {
                result.push_back(std::move(cfg));
            }
        }
    } catch (...) {}
    return result;
}

std::vector<Config::AlertRuleConfig> Config::alertRules() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<AlertRuleConfig> result;
    try {
        auto alerting = root_["alerting"];
        if (!alerting || !alerting.IsMap()) return result;
        auto rules = alerting["rules"];
        if (!rules || !rules.IsSequence()) return result;
        for (const auto& r : rules) {
            AlertRuleConfig cfg;
            cfg.id = r["id"].as<std::string>("");
            cfg.description = r["description"].as<std::string>("");
            cfg.metric_name = r["metric"].as<std::string>("");
            cfg.threshold = r["threshold"].as<double>(0.0);
            cfg.severity = r["severity"].as<std::string>("warning");
            cfg.enabled = r["enabled"].as<bool>(true);
            cfg.cooldown_seconds = r["cooldown_seconds"].as<int>(0);
            // id and metric are mandatory; a rule without either cannot be
            // matched against any metric, so drop it rather than half-wiring.
            if (!cfg.id.empty() && !cfg.metric_name.empty()) {
                result.push_back(std::move(cfg));
            }
        }
    } catch (...) {}
    return result;
}

std::vector<Config::ValidationIssue> Config::validate() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<ValidationIssue> issues;

    if (!loaded_) {
        issues.push_back({ValidationIssue::Error, "file", "No configuration loaded"});
        return issues;
    }

    auto node = [this](const std::string& key) -> YAML::Node {
        try { return root_[key]; } catch (...) { return YAML::Node(); }
    };

    // L2: Structure — required fields
    auto server = node("server");
    if (!server || !server.IsMap()) {
        issues.push_back({ValidationIssue::Warning, "server", "Missing server section, using defaults"});
    } else {
        auto port = server["port"].as<int>(8080);
        if (port < 1 || port > 65535) {
            issues.push_back({ValidationIssue::Error, "server.port",
                "Port must be between 1 and 65535, got " + std::to_string(port)});
        }
        auto threads = server["threads"].as<int>(0);
        if (threads < 0) {
            issues.push_back({ValidationIssue::Error, "server.threads",
                "Thread count must be >= 0, got " + std::to_string(threads)});
        }
    }

    auto logging = node("logging");
    if (logging && logging.IsMap()) {
        auto log_level = logging["level"].as<std::string>("info");
        if (log_level != "trace" && log_level != "debug" && log_level != "info" &&
            log_level != "warn" && log_level != "error" && log_level != "critical" &&
            log_level != "off") {
            issues.push_back({ValidationIssue::Error, "logging.level",
                "Invalid log level: " + log_level});
        }
    }

    // L4: File references
    auto models_cfg = node("models_config");
    auto models_path = models_cfg ? models_cfg.as<std::string>("config/models.yaml") : std::string("config/models.yaml");
    {
        std::ifstream f(models_path);
        if (!f.good()) {
            issues.push_back({ValidationIssue::Warning, "models_config",
                "Models config file not found: " + models_path});
        }
    }

    // L5: Semantic — TLS enabled but missing cert/key
    auto tls = node("tls");
    if (tls && tls.IsMap() && tls["enabled"].as<bool>(false)) {
        auto cert = tls["cert"].as<std::string>("");
        auto key = tls["key"].as<std::string>("");
        if (cert.empty() || key.empty()) {
            issues.push_back({ValidationIssue::Error, "tls",
                "TLS is enabled but cert or key path is empty"});
        } else {
            std::ifstream cf(cert), kf(key);
            if (!cf.good()) {
                issues.push_back({ValidationIssue::Error, "tls.cert",
                    "TLS cert file not found: " + cert});
            }
            if (!kf.good()) {
                issues.push_back({ValidationIssue::Error, "tls.key",
                    "TLS key file not found: " + key});
            }
        }
    }

    // Auth warnings
    auto auth = node("auth");
    if (auth && auth.IsMap() && auth["enabled"].as<bool>(false)) {
        auto keys = auth["api_keys"];
        if (!keys || !keys.IsSequence() || keys.size() == 0) {
            issues.push_back({ValidationIssue::Warning, "auth",
                "Auth is enabled but no API keys configured"});
        }
    }

    // Rate limit
    auto rl = node("rate_limit");
    if (rl && rl.IsMap()) {
        auto max_tokens = rl["max_tokens"].as<double>(100.0);
        if (max_tokens <= 0) {
            issues.push_back({ValidationIssue::Error, "rate_limit.max_tokens",
                "Max tokens must be positive"});
        }
    }

    // Cache
    auto cache = node("cache");
    if (cache && cache.IsMap()) {
        auto threshold = cache["threshold"].as<float>(0.85f);
        if (threshold < 0.0f || threshold > 1.0f) {
            issues.push_back({ValidationIssue::Error, "cache.threshold",
                "Cache threshold must be between 0.0 and 1.0"});
        }
    }

    return issues;
}

TracingConfig Config::telemetryConfig() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    TracingConfig tc;
    if (!loaded_) return tc;
    try {
        auto node = root_["telemetry"];
        if (node) {
            tc.enabled = node["enabled"].as<bool>(false);
            tc.otlp_endpoint = node["otlp_endpoint"].as<std::string>(
                "http://localhost:4318");
            tc.service_name = node["service_name"].as<std::string>("aegisgate");
            tc.sample_ratio = node["sample_ratio"].as<double>(1.0);
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse telemetry config: {}", e.what());
    }
    if (const char* v = std::getenv("AEGISGATE_TELEMETRY_ENABLED")) {
        std::string s(v);
        tc.enabled = (s == "1" || s == "true" || s == "on");
    }
    if (const char* v = std::getenv("AEGISGATE_OTLP_ENDPOINT")) {
        std::string s(v);
        if (!s.empty()) tc.otlp_endpoint = s;
    }
    if (const char* v = std::getenv("AEGISGATE_TELEMETRY_SERVICE_NAME")) {
        std::string s(v);
        if (!s.empty()) tc.service_name = s;
    }
    if (const char* v = std::getenv("AEGISGATE_TELEMETRY_SAMPLE_RATIO")) {
        try {
            tc.sample_ratio = std::stod(v);
        } catch (...) {
            /* keep YAML value */
        }
    }
    return tc;
}

std::string Config::adminJwtSecret() const {
    if (const char* v = std::getenv("AEGISGATE_ADMIN_JWT_SECRET")) {
        std::string s(v);
        if (!s.empty()) return s;
    }
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    auto node = root_["admin"];
    if (!node || !node.IsMap()) return "";
    return node["jwt_secret"].as<std::string>("");
}

int Config::adminJwtExpireSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 28800;
    auto node = root_["admin"];
    if (!node || !node.IsMap()) return 28800;
    return node["jwt_expire_seconds"].as<int>(28800);
}

std::vector<std::string> Config::adminCorsOrigins() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> origins;
    if (!loaded_) return origins;
    auto node = root_["admin"];
    if (!node || !node.IsMap()) return origins;
    auto cors = node["cors_origins"];
    if (cors && cors.IsSequence()) {
        for (const auto& o : cors) {
            origins.push_back(o.as<std::string>());
        }
    }
    return origins;
}

std::vector<std::string> Config::adminAllowedIps() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> ips;
    if (!loaded_) return ips;
    auto node = root_["admin"];
    if (!node || !node.IsMap()) return ips;
    auto allowed = node["allowed_ips"];
    if (allowed && allowed.IsSequence()) {
        for (const auto& ip : allowed) {
            ips.push_back(ip.as<std::string>());
        }
    }
    return ips;
}

std::vector<std::string> Config::adminTrustedProxies() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> proxies;
    if (!loaded_) return proxies;
    auto node = root_["admin"];
    if (!node || !node.IsMap()) return proxies;
    auto tp = node["trusted_proxies"];
    if (tp && tp.IsSequence()) {
        for (const auto& p : tp) {
            proxies.push_back(p.as<std::string>());
        }
    }
    return proxies;
}

std::string Config::adminStaticDir() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "web/admin/dist";
    auto node = root_["admin"];
    if (!node || !node.IsMap()) return "web/admin/dist";
    return node["static_dir"].as<std::string>("web/admin/dist");
}

// --- Plugins ---

bool Config::pluginEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("plugins", "enabled", false);
}

std::string Config::pluginSearchPath() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "config/plugins";
    return safeGet<std::string>("plugins", "search_path", "config/plugins");
}

std::vector<Config::PluginStageConfig> Config::pluginStages() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<PluginStageConfig> result;
    if (!loaded_) return result;
    try {
        auto plugins = root_["plugins"];
        if (!plugins || !plugins.IsMap()) return result;
        auto stages = plugins["stages"];
        if (!stages || !stages.IsSequence()) return result;
        for (const auto& s : stages) {
            PluginStageConfig cfg;
            cfg.name = s["name"].as<std::string>("");
            cfg.path = s["path"].as<std::string>("");
            cfg.position = s["position"].as<std::string>("outbound");
            cfg.order = s["order"].as<int>(0);
            if (!cfg.name.empty() && !cfg.path.empty()) {
                result.push_back(std::move(cfg));
            }
        }
    } catch (...) {}
    return result;
}

// --- Deployment ---

std::string Config::deploymentMode() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    const char* env = std::getenv("AEGISGATE_DEPLOYMENT_MODE");
    if (env && env[0]) return env;
    if (!loaded_) return "standalone";
    return safeGet<std::string>("deployment", "mode", "standalone");
}

// --- Routing ---

std::string Config::routerType() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "cost_aware";
    return safeGet<std::string>("routing", "type", "cost_aware");
}

double Config::mlRouterCostWeight() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.4;
    return safeGet<double>("routing", "ml", "cost_weight", 0.4);
}

double Config::mlRouterQualityWeight() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.35;
    return safeGet<double>("routing", "ml", "quality_weight", 0.35);
}

double Config::mlRouterLatencyWeight() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.25;
    return safeGet<double>("routing", "ml", "latency_weight", 0.25);
}

bool Config::geoRoutingEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("routing", "geo", "enabled", false);
}

std::string Config::geoAffinity() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "prefer";
    return safeGet<std::string>("routing", "geo", "affinity", "prefer");
}

std::string Config::geoDefaultClientRegion() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "us-east";
    return safeGet<std::string>("routing", "geo", "default_client_region", "us-east");
}

std::vector<std::string> Config::geoHeaderNames() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<std::string> result;
    if (!loaded_) {
        result = {"X-AegisGate-Region", "X-Client-Region"};
        return result;
    }
    try {
        auto routing = root_["routing"];
        if (!routing || !routing.IsMap()) { return {"X-AegisGate-Region", "X-Client-Region"}; }
        auto geo = routing["geo"];
        if (!geo || !geo.IsMap()) { return {"X-AegisGate-Region", "X-Client-Region"}; }
        auto headers = geo["header_names"];
        if (!headers || !headers.IsSequence()) { return {"X-AegisGate-Region", "X-Client-Region"}; }
        for (const auto& h : headers) {
            auto name = h.as<std::string>("");
            if (!name.empty()) result.push_back(std::move(name));
        }
    } catch (...) {}
    if (result.empty()) { result = {"X-AegisGate-Region", "X-Client-Region"}; }
    return result;
}

std::vector<Config::GeoIpRange> Config::geoIpRegionMap() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<GeoIpRange> result;
    if (!loaded_) return result;
    try {
        auto routing = root_["routing"];
        if (!routing || !routing.IsMap()) return result;
        auto geo = routing["geo"];
        if (!geo || !geo.IsMap()) return result;
        auto map_node = geo["ip_region_map"];
        if (!map_node || !map_node.IsSequence()) return result;
        for (const auto& entry : map_node) {
            GeoIpRange r;
            r.cidr = entry["cidr"].as<std::string>("");
            r.region = entry["region"].as<std::string>("");
            if (!r.cidr.empty() && !r.region.empty()) {
                result.push_back(std::move(r));
            }
        }
    } catch (...) {}
    return result;
}

std::unordered_map<std::string, std::string> Config::geoRegionAliases() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::unordered_map<std::string, std::string> result;
    if (!loaded_) return result;
    try {
        auto routing = root_["routing"];
        if (!routing || !routing.IsMap()) return result;
        auto geo = routing["geo"];
        if (!geo || !geo.IsMap()) return result;
        auto aliases = geo["region_aliases"];
        if (!aliases || !aliases.IsMap()) return result;
        for (const auto& kv : aliases) {
            auto k = kv.first.as<std::string>("");
            auto v = kv.second.as<std::string>("");
            if (!k.empty() && !v.empty()) {
                result[std::move(k)] = std::move(v);
            }
        }
    } catch (...) {}
    return result;
}

std::vector<Config::ABExperimentConfig> Config::abTestExperiments() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<ABExperimentConfig> result;
    if (!loaded_) return result;
    try {
        auto routing = root_["routing"];
        if (!routing || !routing.IsMap()) return result;
        auto ab_tests = routing["ab_tests"];
        if (!ab_tests || !ab_tests.IsSequence()) return result;
        for (const auto& exp : ab_tests) {
            ABExperimentConfig cfg;
            cfg.name = exp["name"].as<std::string>("");
            cfg.enabled = exp["enabled"].as<bool>(true);
            cfg.tenant_id = exp["tenant_id"].as<std::string>("");
            if (auto variants = exp["variants"]) {
                for (const auto& v : variants) {
                    ABVariantConfig vc;
                    vc.model = v["model"].as<std::string>("");
                    vc.weight = v["weight"].as<int>(1);
                    cfg.variants.push_back(std::move(vc));
                }
            }
            if (!cfg.name.empty() && !cfg.variants.empty()) {
                result.push_back(std::move(cfg));
            }
        }
    } catch (...) {}
    return result;
}

// --- Phase 6.1 Multimodal routing (TASK-20260513-01 Epic 5.1) ---

bool Config::multimodalEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("multimodal", "enabled", false);
}

std::string Config::multimodalRoutingPolicy() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "cheapest";
    return safeGet<std::string>("multimodal", "policy", "cheapest");
}

bool Config::multimodalCostAttributionEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("multimodal", "cost_attribution", "enabled", true);
}

bool Config::multimodalRateLimitEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("multimodal", "rate_limit", "enabled", false);
}

std::vector<Config::ModalityQuotaConfig>
Config::multimodalRateLimitQuotas() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<ModalityQuotaConfig> result;
    if (!loaded_) return result;
    auto quotas_node = safeNode("multimodal", "rate_limit", "quotas");
    if (!quotas_node || !quotas_node.IsSequence()) return result;
    result.reserve(quotas_node.size());
    for (const auto& q : quotas_node) {
        try {
            ModalityQuotaConfig cfg;
            cfg.modality = q["modality"].as<std::string>("");
            cfg.identity = q["identity"].as<std::string>("*");
            cfg.max_tokens = q["max_tokens"].as<double>(0.0);
            cfg.refill_rate = q["refill_rate"].as<double>(0.0);
            if (!cfg.modality.empty() && cfg.max_tokens > 0.0 &&
                cfg.refill_rate > 0.0) {
                result.push_back(std::move(cfg));
            }
        } catch (...) {
        }
    }
    return result;
}

// --- Autonomy / FeedbackBus (Phase 11.0) ---

bool Config::feedbackBusEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("autonomy", "feedback_bus", "enabled", false);
}

int Config::feedbackBusMaxQueueSize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10000;
    return safeGet<int>("autonomy", "feedback_bus", "max_queue_size", 10000);
}

std::string Config::feedbackBusDropPolicy() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "oldest";
    return safeGet<std::string>("autonomy", "feedback_bus", "drop_policy", "oldest");
}

// --- Phase 11.5 Autonomy / CostAutonomy / BudgetGuard (TASK-20260518-02) ---

bool Config::autonomyEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("autonomy", "enabled", false);
}

std::string Config::autonomyAutoApplyMode() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "manual_only";
    auto raw = safeGet<std::string>("autonomy", "auto_apply_mode", "manual_only");
    if (raw == "manual_only" || raw == "auto_low_risk" || raw == "auto_all") {
        return raw;
    }
    // Conservative fallback: unknown mode collapses to manual_only.
    spdlog::warn("Config::autonomyAutoApplyMode: unknown value '{}'; "
                 "falling back to manual_only", raw);
    return "manual_only";
}

int Config::autonomyProposalRetentionDays() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 90;
    auto v = safeGet<int>("autonomy", "proposal_retention_days", 90);
    return v < 0 ? 90 : v;
}

bool Config::costAutonomyEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("autonomy", "cost_optimizer", "enabled", false);
}

bool Config::budgetGuardEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("budget_guard", "enabled", false);
}

double Config::budgetGuardPerTenant24hUsd() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 100.0;
    auto v = safeGet<double>("budget_guard", "per_tenant_24h_usd", 100.0);
    return v <= 0.0 ? 100.0 : v;
}

double Config::budgetGuardPerRequestMaxUsd() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1.0;
    auto v = safeGet<double>("budget_guard", "per_request_max_usd", 1.0);
    return v <= 0.0 ? 1.0 : v;
}

bool Config::budgetGuardFailOpenOnError() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("budget_guard", "fail_open_on_error", true);
}

std::string Config::budgetGuardDowngradeTier() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "economy";
    auto raw = safeGet<std::string>("budget_guard", "downgrade_tier", "economy");
    return raw.empty() ? std::string("economy") : raw;
}

std::string Config::budgetGuardDowngradeHeaderName() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "X-AegisGate-Budget-Guard";
    return safeGet<std::string>("budget_guard", "downgrade_header_name",
                                "X-AegisGate-Budget-Guard");
}

std::string Config::budgetGuardDowngradeHeaderValue() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "triggered";
    return safeGet<std::string>("budget_guard", "downgrade_header_value",
                                "triggered");
}

// --- SSO ---

bool Config::ssoEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("sso", "enabled", false);
}

std::string Config::ssoDefaultProvider() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "";
    return safeGet<std::string>("sso", "default_provider", "");
}

// --- MFA ---

std::string Config::mfaEnforcement() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "disabled";
    return safeGet<std::string>("mfa", "enforcement", "disabled");
}

int Config::mfaTotpDigits() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 6;
    return safeGet<int>("mfa", "totp_digits", 6);
}

int Config::mfaTotpPeriod() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 30;
    return safeGet<int>("mfa", "totp_period", 30);
}

int Config::mfaRecoveryCodeCount() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 8;
    return safeGet<int>("mfa", "recovery_code_count", 8);
}

// TASK-20260702-02 P2-2（SR-2）：MFA 验证失败锁定策略。
int Config::mfaLockoutMaxFailures() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5;
    return safeGet<int>("mfa", "lockout_max_failures", 5);
}

int Config::mfaLockoutWindowSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 900;
    return safeGet<int>("mfa", "lockout_window_seconds", 900);
}

// --- Session ---

int Config::sessionAbsoluteTimeoutSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 28800;
    return safeGet<int>("session", "absolute_timeout", 28800);
}

int Config::sessionIdleTimeoutSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3600;
    return safeGet<int>("session", "idle_timeout", 3600);
}

int Config::sessionMaxConcurrent() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 5;
    return safeGet<int>("session", "max_concurrent", 5);
}

// --- Cluster / backends ---

std::string Config::rateLimiterBackend() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "memory";
    return safeGet<std::string>("rate_limiter", "backend", "memory");
}

// TASK-20260708-02 / REV20260707-C1: prefer `rate_limit.backend` (aligned
// with the documented `rate_limit.*` namespace); fall back to the legacy
// `rate_limiter.backend` key so existing configs keep working.
std::string Config::rateLimitBackend() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "memory";
    // Sentinel "" allows distinguishing "key missing" from "key set to memory".
    auto primary = safeGet<std::string>("rate_limit", "backend", "");
    if (!primary.empty()) return primary;
    return safeGet<std::string>("rate_limiter", "backend", "memory");
}

std::string Config::sessionBackend() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "sqlite";
    return safeGet<std::string>("session", "backend", "sqlite");
}

// --- Token optimization: prompt compression ---

bool Config::promptCompressionEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("token_optimization", "prompt_compression", "enabled", true);
}

int Config::promptCompressionMaxContextMessages() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 20;
    return safeGet<int>("token_optimization", "prompt_compression", "max_context_messages", 20);
}

bool Config::promptCompressionCompressWhitespace() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("token_optimization", "prompt_compression", "compress_whitespace", true);
}

bool Config::promptCompressionDedupSystem() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("token_optimization", "prompt_compression", "dedup_system_prompts", true);
}

// --- Token optimization: smart max tokens ---

bool Config::smartMaxTokensEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return true;
    return safeGet<bool>("token_optimization", "smart_max_tokens", "enabled", true);
}

int Config::smartMaxTokensDefaultOutput() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 2048;
    return safeGet<int>("token_optimization", "smart_max_tokens", "default_max_output", 2048);
}

double Config::smartMaxTokensOutputRatio() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 2.0;
    return safeGet<double>("token_optimization", "smart_max_tokens", "max_output_ratio", 2.0);
}

int Config::smartMaxTokensMinOutput() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 100;
    return safeGet<int>("token_optimization", "smart_max_tokens", "min_output_tokens", 100);
}

// --- Phase 8: Agent orchestration ---

bool Config::agentEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("agent", "enabled", false);
}

int Config::agentMaxSteps() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 10;
    return safeGet<int>("agent", "max_steps", 10);
}

int Config::agentMaxTotalTimeoutMs() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 120000;
    return safeGet<int>("agent", "max_total_timeout_ms", 120000);
}

int Config::agentToolDefaultTimeoutMs() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 30000;
    return safeGet<int>("agent", "tool_default_timeout_ms", 30000);
}

int Config::agentToolMaxOutputBytes() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 1048576;
    return safeGet<int>("agent", "tool_max_output_bytes", 1048576);
}

bool Config::agentEndpointEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("agent", "endpoint_enabled", false);
}

// --- TASK-20260703-04 D2: Workflow engine / endpoint ---

bool Config::workflowEndpointEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("workflow", "endpoint_enabled", false);
}

int Config::workflowWorkerCount() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 4;
    return safeGet<int>("workflow", "worker_count", 4);
}

int Config::workflowMaxConcurrentRuns() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 16;
    return safeGet<int>("workflow", "max_concurrent_runs", 16);
}

int Config::workflowNodeTimeoutMs() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 30000;
    return safeGet<int>("workflow", "node_timeout_ms", 30000);
}

// --- Phase 8: RAG ---

bool Config::ragEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("rag", "enabled", false);
}

int Config::ragTopK() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3;
    return safeGet<int>("rag", "top_k", 3);
}

float Config::ragMinRelevance() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.7f;
    return safeGet<float>("rag", "min_relevance", 0.7f);
}

int Config::ragMaxContextTokens() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 2000;
    return safeGet<int>("rag", "max_context_tokens", 2000);
}

int Config::ragChunkSize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 512;
    return safeGet<int>("rag", "chunk_size", 512);
}

int Config::ragChunkOverlap() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 64;
    return safeGet<int>("rag", "chunk_overlap", 64);
}

std::string Config::ragInjectionPosition() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return "before_user";
    return safeGet<std::string>("rag", "injection_position", std::string("before_user"));
}

// --- Phase 8: Cache 2.0 ---

bool Config::cacheFeedbackEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("cache", "feedback", "enabled", false);
}

bool Config::cachePredictiveWarmupEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("cache", "predictive_warmup", "enabled", false);
}

int Config::cachePredictiveWarmupIntervalSeconds() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 300;
    return safeGet<int>("cache", "predictive_warmup", "interval_seconds", 300);
}

int Config::cachePredictiveWarmupTopK() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 50;
    return safeGet<int>("cache", "predictive_warmup", "top_k", 50);
}

bool Config::cacheCrossTenantEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("cache", "cross_tenant", "enabled", false);
}

float Config::cacheCrossTenantMinSimilarity() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.95f;
    return safeGet<float>("cache", "cross_tenant", "min_similarity", 0.95f);
}

// --- Phase 8: Advanced observability ---

bool Config::costAttributionEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("observability", "cost_attribution", "enabled", false);
}

bool Config::anomalyDetectionEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("observability", "anomaly_detection", "enabled", false);
}

double Config::anomalyDetectionZScoreThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 3.0;
    return safeGet<double>("observability", "anomaly_detection", "z_score_threshold", 3.0);
}

int Config::anomalyDetectionWindowSize() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 100;
    return safeGet<int>("observability", "anomaly_detection", "window_size", 100);
}

bool Config::qualityMonitoringEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("observability", "quality_monitoring", "enabled", false);
}

double Config::qualityMonitoringAlertThreshold() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return 0.3;
    return safeGet<double>("observability", "quality_monitoring", "alert_threshold", 0.3);
}

bool Config::costOptimizationEnabled() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (!loaded_) return false;
    return safeGet<bool>("observability", "cost_optimization", "enabled", false);
}

// ---------------------------------------------------------------------------
// Phase 9.3.4 — merged-yaml getters
// ---------------------------------------------------------------------------

std::string Config::activeVersionId() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return active_version_id_;
}

std::optional<RolloutConfigView> Config::rolloutConfig() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return rollout_config_;
}

const Config* Config::configForVersion(const std::string& version_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = configs_by_version_.find(version_id);
    if (it == configs_by_version_.end()) return nullptr;
    return it->second.get();
}

} // namespace aegisgate
