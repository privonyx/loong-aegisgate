# Drogon 流式响应 API 调研

**日期：** 2026-03-21
**任务：** TASK-20260321-03 Task 5
**目的：** 评估 Drogon HttpClient 的流式能力，为 Phase 0.2 真流式上游客户端做准备

---

## 结论摘要

Drogon HttpClient **不支持**上游响应体的增量流式读取。`sendRequest()` 回调在完整响应接收后才触发，无法实现 LLM SSE 流式响应的逐 chunk 下推。Phase 0.2 需要引入替代方案。

## 当前限制

### Drogon HttpClient API 分析

检查 `drogon/HttpClient.h`（vcpkg installed 1.9+ 版本），`sendRequest()` 提供三种变体：

1. **异步回调**：`sendRequest(req, callback, timeout)` — 回调签名 `(ReqResult, HttpResponsePtr)`，仅在完整响应到达后触发
2. **同步阻塞**：`sendRequest(req, timeout)` — 返回 `pair<ReqResult, HttpResponsePtr>`，内部用 promise/future 等待
3. **协程**：`sendRequestCoro(req, timeout)` — C++20 coroutine await

三种变体均等待完整响应体，无 body streaming callback 参数。

### Drogon 的流式 API（仅限服务端出站）

- `HttpResponse::newStreamResponse()` — 同步 pull 模式，回调返回字节
- `HttpResponse::newAsyncStreamResponse()` — 异步 push 模式，通过 `ResponseStreamPtr` 写入

这些 API 用于服务端 → 客户端方向（AegisGate 已在 SSE 下推中使用 `newAsyncStreamResponse`）。不适用于客户端 → 上游方向的响应体流式读取。

### 影响

LLM 提供商的 SSE 流式响应可能持续数十秒。当前架构下，用户在完整响应到达前收不到第一个 token（首字延迟 = 完整生成时间）。AegisGate 的"流式优先"在入站（上游→网关）这一段被 Drogon 的全缓冲行为打断。

## 替代方案

| 方案 | 优点 | 缺点 | 工作量 |
|------|------|------|--------|
| **libcurl multi** | 成熟、CURLOPT_WRITEFUNCTION 支持 chunk 回调、async multi 接口 | 新依赖、回调风格与 Drogon event loop 需适配 | 中（2-3 天） |
| **Boost.Beast** | C++ 原生、header-only、与 Boost.Asio 深度集成 | 重依赖（整个 Boost）、API 低级 | 大（4-5 天） |
| **trantor TcpClient** | Drogon 底层库、已在依赖树中、原生事件循环集成 | 需手写 HTTP/1.1 解析、SSL 处理复杂、非标准用法 | 大（5+ 天） |
| **Drogon 未来版本** | 零额外依赖 | 不可控时间线、可能永远不支持 | 未知 |

## 推荐路径

**Phase 0.2 推荐：libcurl multi + UpstreamClient 实现**

理由：
1. libcurl 是最成熟的 HTTP 客户端库，`CURLOPT_WRITEFUNCTION` 直接支持 chunk-by-chunk body 读取
2. vcpkg 有 `curl` 包，集成简单
3. UpstreamClient 抽象层已建好（TASK-20260321-02），新增 `CurlUpstreamClient` 实现即可
4. 通过 CMake `ENABLE_CURL_UPSTREAM=ON` 可选编译，DrogonUpstreamClient 保持为默认实现

### 实施步骤（Phase 0.2 参考）

1. vcpkg.json 新增 `curl` feature
2. 创建 `CurlUpstreamClient : public UpstreamClient`
3. `sendStreaming()` 使用 `curl_multi_perform` + `CURLOPT_WRITEFUNCTION` 回调
4. 集成到 Drogon event loop（`curl_multi_socket_action` + `curl_multi_fdset`）
5. PipelineAssembler 通过配置选择 upstream 实现
6. 端到端流式验证：上游 SSE → CurlUpstreamClient chunk → Pipeline chunk → SSE 下推

### 预期收益

- 首字延迟从"完整生成时间"降至"首 token 生成时间"（通常 <1s）
- 真正的端到端流式，用户体验显著提升
- 对现有代码的影响最小（UpstreamClient 多态替换）

## 对 Phase 0.2 的影响

| 维度 | 评估 |
|------|------|
| 新增依赖 | libcurl（通过 vcpkg，可选） |
| 变更范围 | 新增 CurlUpstreamClient + CMake 选项 + PipelineAssembler 选择逻辑 |
| 预估工作量 | 2-3 天（含测试和集成） |
| 风险 | curl event loop 与 Drogon event loop 集成需要仔细处理（线程安全、回调时机） |
| 回退方案 | DrogonUpstreamClient 始终可用，curl 失败时自动降级 |
