#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <drogon/drogon.h>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "core/config.h"
#include "server/gateway_runtime.h"
#include "observe/crash_handler.h"
#include "observe/tracing.h"
#include "observe/trace_log_formatter.h"
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef AEGISGATE_VERSION
#define AEGISGATE_VERSION "0.0.0-dev"
#endif

namespace {

volatile sig_atomic_t g_reload_requested = 0;

void handleShutdownSignal(int /*sig*/) {
    aegisgate::GatewayRuntime::instance().beginShutdown();
    drogon::app().quit();
}

void handleSighup(int /*sig*/) {
    g_reload_requested = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    spdlog::info("AegisGate v{} starting...", AEGISGATE_VERSION);

    aegisgate::Config config;
    std::string config_path = "config/aegisgate.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }
    if (!config.loadFromFile(config_path)) {
        spdlog::critical("Cannot start: failed to load config from {}", config_path);
        return 1;
    }

    if (config.persistentBackend() == "sqlite") {
        auto db_path = config.sqlitePath();
        auto parent = std::filesystem::path(db_path).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            try {
                std::filesystem::create_directories(parent);
            } catch (const std::exception& e) {
                spdlog::critical("Cannot create SQLite directory {}: {}",
                                 parent.string(), e.what());
                return 1;
            }
        }
    }

    // Setup logging: file + stdout, or stdout only
    {
        auto level_str = config.logLevel();
        auto log_file = config.logFile();
        auto level = spdlog::level::from_str(level_str);

        if (!log_file.empty()) {
            auto parent = std::filesystem::path(log_file).parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent)) {
                std::filesystem::create_directories(parent);
            }
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, false);
            auto logger = std::make_shared<spdlog::logger>(
                "aegisgate", spdlog::sinks_init_list{console_sink, file_sink});
            logger->set_level(level);
            logger->flush_on(spdlog::level::info);
            spdlog::set_default_logger(logger);
        } else {
            spdlog::set_level(level);
        }
    }

    // 尽早安装崩溃处理器：致命信号 / 未捕获异常时写出 backtrace 崩溃日志
    // 到 logs/crash-<pid>-<epoch>.log 与 stderr（TASK-20260615-05）。
    {
        std::string crash_dir = "logs";
        auto lf = config.logFile();
        if (!lf.empty()) {
            auto parent = std::filesystem::path(lf).parent_path();
            if (!parent.empty()) crash_dir = parent.string();
        }
        std::error_code ec;
        std::filesystem::create_directories(crash_dir, ec);
        aegisgate::installCrashHandler({crash_dir, AEGISGATE_VERSION});
    }

    // TASK-20260622-01 E1 (G1): strict 后端校验在 assemble 内可能抛出（误配 redis/pg
    // 但实际回退 memory）。fail-closed —— 非零退出而非带病启动。
    try {
        aegisgate::GatewayRuntime::instance().initialize(config);
    } catch (const std::exception& e) {
        spdlog::critical("Startup aborted: {}", e.what());
        return 1;
    }

#ifdef AEGISGATE_ENABLE_OTEL
    aegisgate::Tracing::instance().initialize(config.telemetryConfig());
    if (aegisgate::Tracing::instance().isEnabled()) {
        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<aegisgate::TraceIdFlag>('K');
        formatter->add_flag<aegisgate::SpanIdFlag>('J');
        formatter->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [tid=%K sid=%J] %v");
        spdlog::set_formatter(std::move(formatter));
    }
#endif

    int http_port = config.serverPort();
    if (const char* env_port = std::getenv("AEGISGATE_PORT")) {
        try {
            http_port = std::stoi(env_port);
        } catch (...) {
        }
    }

    int tls_port = config.tlsPort();
    if (tls_port <= 0) {
        tls_port = http_port + 1;
    }
    if (const char* env_tls = std::getenv("AEGISGATE_TLS_PORT")) {
        try {
            tls_port = std::stoi(env_tls);
        } catch (...) {
        }
    }

    auto& app = drogon::app();
    app.setLogLevel(trantor::Logger::kWarn)
       .addListener(config.serverHost(), http_port)
       .setThreadNum(config.serverThreads())
       .setClientMaxBodySize(config.maxRequestBodySize());

    if (config.tlsEnabled()) {
        auto cert = config.tlsCertPath();
        auto key = config.tlsKeyPath();
        if (!cert.empty() && !key.empty()) {
            app.addListener(config.serverHost(), tls_port, true, cert, key);
            spdlog::info("TLS enabled on port {}", tls_port);
        } else {
            spdlog::warn("TLS enabled but cert/key paths not configured");
        }
    }

    std::signal(SIGTERM, handleShutdownSignal);
    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGHUP, handleSighup);

    app.getLoop()->runEvery(1.0, [] {
        if (g_reload_requested) {
            g_reload_requested = 0;
            spdlog::info("SIGHUP received — reloading configuration");
            aegisgate::GatewayRuntime::instance().reloadConfig();
        }
    });

    // Admin SPA: regex handler 同时承担 /admin/ 子路径下的静态资源服务
    // （HttpResponse::newFileResponse 自动处理 MIME / 304 协商缓存）和
    // SPA fallback（dist 中无对应文件时返回 index.html）。
    //
    // 设计文档: docs/specs/2026-05-08-admin-panel-routing-normalization-design.md
    // 路由优先级实测: scripts/test-drogon-routing-priority.sh (TASK-20260508-01 Epic 0.1)
    auto admin_static_dir = config.adminStaticDir();
    if (!admin_static_dir.empty() && std::filesystem::is_directory(admin_static_dir)) {
        spdlog::info("Admin panel static dir: {}", admin_static_dir);

        auto static_root_canon = std::filesystem::canonical(admin_static_dir);
        auto index_path = static_root_canon / "index.html";

        if (std::filesystem::exists(index_path)) {
            std::string index_html;
            {
                std::ifstream ifs(index_path);
                index_html.assign(std::istreambuf_iterator<char>(ifs),
                                  std::istreambuf_iterator<char>());
            }

            std::string static_root_str = static_root_canon.string();

            app.registerHandlerViaRegex(
                R"(^/admin(/.*)?$)",
                [static_root_str, index_html](
                    const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    const auto& path = req->path();

                    // SR1: 路径穿越拒绝（drogon 在某些版本对非常规 path 的
                    // 规范化不一致，主动检查更稳）
                    if (path.find("..") != std::string::npos) {
                        callback(drogon::HttpResponse::newNotFoundResponse());
                        return;
                    }

                    // SR4: 防御性排除（drogon 路由优先级保证 controller 精确路径
                    // 独占 path 节点，这里是兜底——确保万一去掉某个 controller
                    // 时不会泄漏 SPA HTML 到 API namespace）
                    if (path.rfind("/admin/api/", 0) == 0 ||
                        path.rfind("/admin/auth/", 0) == 0 ||
                        path == "/admin/ws") {
                        callback(drogon::HttpResponse::newNotFoundResponse());
                        return;
                    }

                    // /admin、/admin/ → SPA fallback（不查文件，直接 index.html）
                    std::string sub;
                    constexpr std::size_t kPrefixLen = 7;  // "/admin/"
                    if (path.size() > kPrefixLen) {
                        sub = path.substr(kPrefixLen);
                    }

                    if (!sub.empty()) {
                        std::filesystem::path file_path =
                            std::filesystem::path(static_root_str) / sub;
                        std::error_code ec;
                        if (std::filesystem::is_regular_file(file_path, ec)) {
                            // 安全：canonical 后必须仍以 static_root 为前缀
                            auto canon =
                                std::filesystem::weakly_canonical(file_path, ec);
                            if (!ec && canon.string().rfind(static_root_str, 0) == 0) {
                                callback(drogon::HttpResponse::newFileResponse(
                                    canon.string(), "", drogon::CT_NONE, "", req));
                                return;
                            }
                        }
                    }

                    // SPA fallback：返回 index.html
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeString("text/html; charset=utf-8");
                    resp->setBody(index_html);
                    callback(resp);
                },
                {drogon::Get});

            spdlog::info("Admin SPA + static handler registered for /admin* (GET)");
        } else {
            spdlog::warn("Admin static dir exists but index.html missing: {}",
                         index_path.string());
        }
    }

    spdlog::info("Listening on {}:{}", config.serverHost(), http_port);
    try {
        app.run();
    } catch (const std::exception& e) {
        spdlog::critical("Drogon failed to start: {}", e.what());
        return 1;
    }

    // --- Post-run shutdown sequence ---
    spdlog::info("Drogon event loop stopped, starting orderly shutdown...");

    auto watchdog = std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        spdlog::error("Shutdown watchdog expired (30s), forcing exit");
        std::_Exit(1);
    });
    watchdog.detach();

    aegisgate::GatewayRuntime::instance().shutdown();

#ifdef AEGISGATE_ENABLE_OTEL
    aegisgate::Tracing::instance().shutdown();
#endif

    return 0;
}
