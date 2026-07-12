// Phase 9.3 Epic 6 Task 6.1 — aegisgate-control-plane binary entry point.
//
// Minimal by design. The data-plane binary (`aegisgate`) lives behind
// GatewayRuntime + Drogon; the control plane has no HTTP surface, no
// pipeline stages, no routing — it is just:
//
//   * load config
//   * stand up PersistentStore + AuditLogger + AuthService + FeatureGate
//   * build ConfigServiceCore (SR2/3/4/5/9/10/11 enforcement)
//   * wrap it in ConfigServiceImpl + AuthInterceptor (SR1) + ServerBootstrap (SR7)
//   * optionally bootstrap an empty table from a seed yaml (Q5)
//   * run until SIGINT/SIGTERM, then graceful shutdown
//
// Every layer was designed for library-first testing in Epics 1-5, so
// main.cpp itself contains almost no logic — just wiring and argv parsing.

#include "control_plane/bootstrap_helper.h"
#include "control_plane/config_service_core.h"
#include "control_plane/grpc/auth_interceptor.h"
#include "control_plane/grpc/config_service_grpc_adapter.h"
#include "control_plane/grpc/rollout_service_grpc_adapter.h"
#include "control_plane/grpc/server_bootstrap.h"
#include "control_plane/rollout/rollout_audit_bridge.h"
#include "control_plane/rollout/rollout_controller.h"
#include "control_plane/rollout/rollout_metrics_provider.h"
#include "control_plane/rollout/rollout_ticker.h"
#include "control_plane/rollout/rollout_wiring.h"
#include "common/clock.h"

#include "auth/auth_service.h"
#include "core/config.h"
#include "core/feature_gate.h"
#include "gateway/rate_limiter.h"
#include "guardrail/audit.h"
#include "observe/feedback_bus.h"

#include "storage/memory_persistent_store.h"
#include "storage/sqlite_persistent_store.h"

#ifdef AEGISGATE_ENABLE_PG
#include "storage/pg_persistent_store.h"
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>

namespace {

void printUsage(const char* argv0) {
    std::cout <<
        "Usage: " << argv0 << " [--config PATH] [--help]\n"
        "\n"
        "Phase 9.3 control-plane gRPC server (aegisgate.controlplane.v1).\n"
        "\n"
        "Options:\n"
        "  --config PATH   Path to YAML configuration "
        "(default: /etc/aegisgate/aegisgate-control-plane.yaml)\n"
        "  --help          Show this message and exit\n"
        "\n"
        "Signals:\n"
        "  SIGINT/SIGTERM  graceful shutdown (5s drain)\n";
}

std::string readFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::unique_ptr<aegisgate::PersistentStore> makeStore(
    const aegisgate::Config& config) {
    using namespace aegisgate;
    auto backend = config.persistentBackend();

#ifdef AEGISGATE_ENABLE_PG
    if (backend == "postgres") {
        PgConfig pcfg;
        pcfg.url = config.pgUrl();
        pcfg.pool_size = static_cast<size_t>(config.pgPoolSize());
        pcfg.connect_timeout_ms = config.pgConnectTimeout();
        return std::make_unique<PgPersistentStore>(pcfg);
    }
#endif

    if (backend == "sqlite") {
        auto db_path = config.sqlitePath();
        auto parent = std::filesystem::path(db_path).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }
        return std::make_unique<SQLitePersistentStore>(
            db_path, config.sqliteWalMode());
    }

    spdlog::warn("control-plane: falling back to in-memory persistent store "
                 "(backend='{}' not supported or compiled out)", backend);
    return std::make_unique<MemoryPersistentStore>();
}

} // namespace

int main(int argc, char* argv[]) {
    // ---- signal mask (must run before any thread is spawned!) --------------
    // Block SIGINT/SIGTERM in every thread that inherits this mask so only
    // the dedicated watcher thread (below) receives them. Doing it first
    // keeps components like AuditLogger from inheriting an unmasked set
    // and terminating the process under its own worker thread when the
    // user SIGTERM-s us.
    sigset_t shutdown_mask;
    sigemptyset(&shutdown_mask);
    sigaddset(&shutdown_mask, SIGINT);
    sigaddset(&shutdown_mask, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &shutdown_mask, nullptr) != 0) {
        std::fprintf(stderr,
            "pthread_sigmask failed; refusing to start without a working "
            "shutdown path\n");
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);

    // ---- argv parsing ------------------------------------------------------
    std::string config_path = "/etc/aegisgate/aegisgate-control-plane.yaml";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            printUsage(argv[0]);
            return 0;
        }
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }
        std::cerr << "unknown argument: " << argv[i] << "\n";
        printUsage(argv[0]);
        return 2;
    }

    spdlog::set_default_logger(
        spdlog::stdout_color_mt("aegisgate-control-plane"));
    spdlog::info("aegisgate-control-plane starting (config={})", config_path);

    // ---- config ------------------------------------------------------------
    aegisgate::Config config;
    if (!config.loadFromFile(config_path)) {
        spdlog::critical("failed to load config from {}", config_path);
        return 1;
    }

    // ---- persistent store --------------------------------------------------
    auto store = makeStore(config);
    if (!store->initialize()) {
        spdlog::critical("failed to initialize persistent store '{}'",
                         config.persistentBackend());
        return 1;
    }
    spdlog::info("persistent store: {}", store->backendName());

    // ---- audit logger ------------------------------------------------------
    aegisgate::AuditLogger audit;
    audit.setPersistentStore(store.get());

    // ---- auth --------------------------------------------------------------
    aegisgate::FeatureGate gate(
        aegisgate::FeatureGate::createUnlocked(config.edition()));
    aegisgate::AuthService auth_svc(store.get(), &config, &gate);
    if (!gate.isEnabled(aegisgate::Feature::RBAC)) {
        // SR1 requires role-based authorization. Legacy config-key lists
        // have no concept of SuperAdmin and would bypass the role gate.
        spdlog::critical("control plane requires RBAC (edition='enterprise'); "
                         "current edition does not enable Feature::RBAC");
        return 1;
    }

    // ---- rate limiter (SR10) ------------------------------------------------
    aegisgate::RateLimiter::Config rl_cfg;
    auto per_min = config.controlPlaneSubmitRateLimitPerUserPerMin();
    if (per_min <= 0) per_min = 10;
    rl_cfg.max_tokens = static_cast<double>(per_min);
    // Tokens refill evenly across the minute so a client cannot stockpile
    // budget by going silent for an hour and then bursting.
    rl_cfg.refill_rate = static_cast<double>(per_min) / 60.0;
    aegisgate::RateLimiter limiter(rl_cfg);

    // ---- core --------------------------------------------------------------
    aegisgate::ConfigServiceCore::Deps core_deps;
    core_deps.store = store.get();
    core_deps.audit = &audit;
    core_deps.validator = [](const std::string& yaml_content) {
        aegisgate::Config tmp;
        if (!tmp.loadFromString(yaml_content)) {
            return std::vector<aegisgate::Config::ValidationIssue>{
                {aegisgate::Config::ValidationIssue::Error, "yaml",
                 "invalid YAML"}};
        }
        return tmp.validate();
    };
    core_deps.rate_limit = [&limiter](const std::string& uid) {
        return limiter.allow(uid);
    };
    aegisgate::ConfigServiceCore core(std::move(core_deps));

    // ---- bootstrap (Q5) ----------------------------------------------------
    auto bootstrap_path = config.controlPlaneBootstrapYaml();
    auto bs = aegisgate::control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store.get(), &core, bootstrap_path);
    using Outcome = aegisgate::control_plane::bootstrap::Outcome;
    switch (bs.outcome) {
        case Outcome::Bootstrapped:
            spdlog::warn("bootstrapped config_versions from {} (version_id={})",
                          bootstrap_path, bs.version_id);
            break;
        case Outcome::SkippedNotEmpty:
            spdlog::info("config_versions already has history; "
                         "skipping bootstrap");
            break;
        case Outcome::SkippedNoBootstrap:
            spdlog::info("no control_plane.bootstrap_from_active_yaml set; "
                         "skipping bootstrap");
            break;
        case Outcome::FileReadFailed:
        case Outcome::SubmitFailed:
        case Outcome::ApproveFailed:
        case Outcome::ActivateFailed:
            spdlog::critical("bootstrap failed ({}): {}",
                              bs.error_code, bs.error_message);
            return 1;
    }

    // ---- rollout controller (Phase 9.3.4) -----------------------------------
    aegisgate::common::SystemClock system_clock;
    aegisgate::RolloutAuditBridge rollout_audit(&audit);

    auto rollout_metrics =
        std::make_unique<aegisgate::FeedbackBusMetricsProvider>();

    aegisgate::RolloutController::Deps rollout_deps;
    rollout_deps.store       = store.get();
    rollout_deps.config_core = &core;
    rollout_deps.metrics     = rollout_metrics.get();
    rollout_deps.audit       = &rollout_audit;
    rollout_deps.clock       = &system_clock;
    rollout_deps.check_quota = [&store, &system_clock](const std::string& creator) {
        return aegisgate::rolloutQuotaCheck(
            store.get(), creator, /*max_per_24h=*/10,
            system_clock.wallClockMillis());
    };
    rollout_deps.auto_rollback_enabled = aegisgate::autoRollbackEnabledFromEnv;
    aegisgate::RolloutController rollout_ctrl(std::move(rollout_deps));

    aegisgate::RolloutTicker ticker(system_clock, rollout_ctrl,
                                      std::chrono::seconds{5});

    // ---- gRPC surface ------------------------------------------------------
    using namespace aegisgate::control_plane::grpc_adapter;
    AuthInterceptor interceptor(&auth_svc);
    auto extractor = AuthInterceptor::makeUserExtractor(&interceptor);
    ConfigServiceImpl config_svc(&core, extractor);
    RolloutServiceImpl rollout_svc(&rollout_ctrl, extractor);

    ServerBootstrapConfig scfg;
    scfg.listen_address = config.controlPlaneListen();
    scfg.cert_pem = readFile(config.tlsCertPath());
    scfg.key_pem = readFile(config.tlsKeyPath());
    scfg.mutual_tls = config.controlPlaneMutualTls();
    scfg.allowed_client_fingerprints_sha256 =
        config.controlPlaneAllowedClientFingerprints();
    scfg.max_receive_message_bytes = config.controlPlaneMaxYamlBytes();

    std::unique_ptr<grpc::Server> server;
    try {
        server = bootstrapServer(scfg,
                                  {&config_svc, &rollout_svc});
    } catch (const std::exception& e) {
        spdlog::critical("failed to start gRPC server: {}", e.what());
        return 1;
    }
    spdlog::info("control-plane listening on {} (mTLS={})",
                  scfg.listen_address, scfg.mutual_tls ? "on" : "off");
    ticker.start();
    spdlog::info("rollout ticker started (interval=5s)");

    // Dedicated shutdown watcher — blocks in sigwait() (async-signal-safe
    // because the signal is delivered as a regular user-mode event here)
    // and invokes Server::Shutdown from a normal thread context.
    std::thread shutdown_thread([srv = server.get(),
                                  mask = shutdown_mask]() mutable {
        int sig = 0;
        if (sigwait(&mask, &sig) == 0) {
            spdlog::info("received signal {}, initiating graceful shutdown "
                         "(5s drain)", sig);
        } else {
            spdlog::warn("sigwait failed; forcing shutdown");
        }
        srv->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(5));
    });

    server->Wait();

    // Wait() returns as soon as Shutdown() is in progress; join the
    // watcher to make sure it finished its call before we free the Server.
    if (shutdown_thread.joinable()) shutdown_thread.join();

    spdlog::info("gRPC server exited main loop; stopping rollout ticker...");
    ticker.stop();
    spdlog::info("rollout ticker stopped; flushing audit chain...");
    audit.shutdown();
    spdlog::info("aegisgate-control-plane exited cleanly");
    return 0;
}
