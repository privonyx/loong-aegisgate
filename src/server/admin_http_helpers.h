#pragma once
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <aegisgate/error_codes.h>
#include "auth/auth_models.h"
#include "server/admin_controller.h"   // AdminController / AdminResult / AuthContext
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

class GatewayRuntime;

// TASK-20260605-05 — admin HTTP 层样板收敛（creative D2=C / free-function
// helper，**非基类** / 规避 drogon HttpController<T> CRTP 继承链风险）。
//
// 这些 free function 是 CORS / CSP / 响应封装 / 认证 / AdminController 装配的
// 单一真相来源；`AdminHttpController` 同名成员函数改薄委托至此，新拆分出的域
// handler 直接调用 `withAdminAuth` 收敛 OPTIONS→authenticate→buildAdmin→响应
// 这条主样板链。逐字提取自原 admin_http_controller.cpp，行为零变化。
namespace admin_http {

// TASK-20260702-02 P2-1（SR-1）：CORS 决策纯函数 `admin::decideCors` 落在
// admin_session（aegisgate_core，可单测），此处 applyCorsHeaders 调用之。
bool isOriginAllowed(const std::string& origin);

// TASK-20260702-02 P2-5（SR-5）：统一取真实客户端 IP。反代后 TCP peer 恒为代理
// IP；仅当 peer 属于 admin.trusted_proxies 时采信 X-Forwarded-For（否则用 peer）。
// 收敛此前 6 处散落的 req->peerAddr().toIp() 直取。
std::string clientIp(const drogon::HttpRequestPtr& req);
void applyCorsHeaders(const drogon::HttpRequestPtr& req,
                      const drogon::HttpResponsePtr& resp);
void applyCspHeader(const drogon::HttpResponsePtr& resp);

drogon::HttpResponsePtr makeAdminResponse(int status, const nlohmann::json& body,
                                          const drogon::HttpRequestPtr& req);
drogon::HttpResponsePtr makeAdminError(ErrorCode code, const std::string& msg,
                                       const drogon::HttpRequestPtr& req);
drogon::HttpResponsePtr handlePreflight(const drogon::HttpRequestPtr& req);

// 统一 AdminResult → HttpResponse（is_error 双路径共用同一封装：body 已含
// error envelope，仅 status 不同）。
drogon::HttpResponsePtr forwardAdminResult(const AdminResult& result,
                                           const drogon::HttpRequestPtr& req);

std::optional<AuthContext> authenticateRequest(const drogon::HttpRequestPtr& req);

// 集中装配 AdminController（统一注入 store/auth/audit/savings/cache/autonomy/
// case-study 数据源）。新增依赖只需改这一处。
AdminController buildAdmin(GatewayRuntime& runtime);

// 收敛标准 admin handler 主样板链：OPTIONS 预检短路 + 认证（失败 401）+
// AdminController 装配 + 响应封装。`fn` 收到已认证上下文 + 就绪的
// AdminController，返回 AdminResult（body 解析失败等可直接返回
// AdminResult::error，与旧 makeAdminError 路径等价）。
void withAdminAuth(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    const std::function<AdminResult(const AuthContext&, AdminController&)>& fn);

} // namespace admin_http
} // namespace aegisgate
