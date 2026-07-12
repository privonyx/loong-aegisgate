// 一次性脚手架程序：验证 drogon 路由优先级假设。
// 用途：TASK-20260508-01 Epic 0.1 — 验证主方案设计依赖的 3 项核心假设。
// 验证完成后即从 CMakeLists.txt 移除 target，但保留源文件作为 ADR 证据。
//
// 假设 1a：当 controller 注册了 POST /a/b 而 regex 注册了 GET /a(/.*)?，
//          GET /a/b 应落到 regex handler（method 区分，不被 controller 阻挡）
// 假设 1b：POST /a/b 应落到 controller（regex GET 不阻挡 POST）
// 假设 2：  GET /a/anything 应被 regex handler 接管
// 假设 3：  addALocation("/admin/", ..., dist) 应优先服务真实文件
//
// 用法：构建后由 scripts/test-drogon-routing-priority.sh 启动 + curl 验证

#include <drogon/drogon.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

int main() {
    auto& app = drogon::app();

    app.registerHandler(
        "/a/b",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setBody("CONTROLLER");
            cb(r);
        },
        {drogon::Post});

    app.registerHandlerViaRegex(
        R"(^/a(/.*)?$)",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setBody("REGEX");
            cb(r);
        },
        {drogon::Get});

    // 修订方案 Y：单个 regex handler 同时服务静态文件（newFileResponse 自动处理
    // MIME / 304 协商缓存）和 SPA fallback（命中文件 → 文件，未命中 → index.html）。
    // 放弃 addALocation，因为它一旦匹配前缀就不再 fall-through 到 regex handler。
    const std::string static_root = "/tmp/drogon-routing-test-static";
    std::filesystem::create_directories(static_root + "/assets");
    {
        std::ofstream(static_root + "/real.txt") << "STATIC_FILE";
        std::ofstream(static_root + "/assets/foo.js") << "ASSET_JS";
        std::ofstream(static_root + "/index.html") << "INDEX_HTML";
    }

    const std::string index_html = "INDEX_HTML";

    app.registerHandlerViaRegex(
        R"(^/admin(/.*)?$)",
        [static_root, index_html](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const auto& path = req->path();

            // SR1: 路径穿越拒绝
            if (path.find("..") != std::string::npos) {
                cb(drogon::HttpResponse::newNotFoundResponse());
                return;
            }

            // 防御性排除（drogon 路由优先级保证 controller 精确路径独占 path
            // 节点，这里是兜底）
            if (path.rfind("/admin/api/", 0) == 0 ||
                path.rfind("/admin/auth/", 0) == 0 ||
                path == "/admin/ws") {
                cb(drogon::HttpResponse::newNotFoundResponse());
                return;
            }

            // /admin、/admin/ → SPA fallback（直接走 index.html）
            std::string sub;
            constexpr std::size_t kPrefixLen = 7;  // "/admin/"
            if (path.size() > kPrefixLen) {
                sub = path.substr(kPrefixLen);
            }

            if (!sub.empty()) {
                std::filesystem::path file_path =
                    std::filesystem::path(static_root) / sub;
                std::error_code ec;
                if (std::filesystem::is_regular_file(file_path, ec)) {
                    auto canon = std::filesystem::weakly_canonical(file_path, ec);
                    if (!ec &&
                        canon.string().rfind(static_root, 0) == 0) {
                        cb(drogon::HttpResponse::newFileResponse(
                            canon.string(), "", drogon::CT_NONE, "", req));
                        return;
                    }
                }
            }

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeString("text/html; charset=utf-8");
            resp->setBody(index_html);
            cb(resp);
        },
        {drogon::Get});

    std::thread([&app] {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        std::cerr << "[scaffold] auto-shutdown after 8s\n";
        app.quit();
    }).detach();

    app.setLogLevel(trantor::Logger::kWarn);
    app.addListener("127.0.0.1", 19999);
    std::cerr << "[scaffold] listening on 127.0.0.1:19999\n";
    app.run();
    return 0;
}
