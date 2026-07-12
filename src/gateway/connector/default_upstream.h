#pragma once
#include "upstream_client.h"
#include "drogon_upstream_client.h"
#include <memory>
#include <string>
#ifdef AEGISGATE_ENABLE_CURL
#include "curl_upstream_client.h"
#endif

namespace aegisgate {

// 默认上游客户端选择，集中于此以避免各连接器重复 #ifdef。
//
// 开启 ENABLE_CURL 时使用 CurlUpstreamClient：它基于 CURLOPT_WRITEFUNCTION
// 在收到每个数据块时立刻逐行回调（真流式逐 token）。这是流式（stream:true）
// 能力的正确实现。
//
// 未开启时回退 DrogonUpstreamClient：其 sendStreaming 会等上游完整响应收完
// 后再按行一次性回调（伪流式，无真增量），仅作为不依赖 libcurl 的降级实现。
inline std::unique_ptr<UpstreamClient> makeDefaultUpstreamClient(
    const std::string& base_url) {
#ifdef AEGISGATE_ENABLE_CURL
    return std::make_unique<CurlUpstreamClient>(base_url);
#else
    return std::make_unique<DrogonUpstreamClient>(base_url);
#endif
}

} // namespace aegisgate
