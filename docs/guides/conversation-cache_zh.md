# 多轮对话缓存升级指南

> 覆盖功能：Phase 6.4 多轮对话缓存（`conversation_id` + `ConversationSummarizer` + 跨租户硬隔离）
> 可用版本：v1.2+（TASK-20260513-01 落地）

本指南说明如何让 `SemanticCache` 在多轮对话场景下既能享受语义命中带来的成本节省，又**不会**把 A 租户的对话内容回放给 B 租户。

## 它解决什么问题

在没有这套机制之前，AegisGate 只用最后一条用户消息计算缓存键。两个完全无关的对话只要末尾问题措辞接近，就会命中同一条缓存。这在多租户 / 多角色场景里既是**精度问题**，也是**安全问题**。

升级后由三个协作组件解决：

| 组件 | 关注点 | 文件 |
|---|---|---|
| `ConversationIdResolver` | 客户端可显式指定 `conversation_id`；省略时按消息历史 SHA-256 推导；首轮兜底用 `request_id` | `src/cache/conversation_id_resolver.{h,cpp}` |
| `ConversationSummarizer` 接口 + `CompositeSummarizer` | 把历史消息压缩成摘要文本，喂给缓存键。`CompositeSummarizer` = `OnnxSummarizer` 主路径 + `RuleBasedSummarizer` 回落（CR1 方案 B） | `src/cache/{conversation_summarizer.h, rule_based_summarizer.{h,cpp}, onnx_summarizer.{h,cpp}, composite_summarizer.{h,cpp}, summarizer_factory.{h,cpp}}` |
| `SemanticCache::extractCacheKeyInfoV2` | partition_key 强制混入 `tenant_id` 和 `conversation_id`，使用 SHA-256（替换原来的 `std::hash`） | `src/cache/semantic_cache.{h,cpp}` |

## 启用步骤

1. 在 `config/aegisgate.yaml` 打开开关：

   ```yaml
   cache:
     conversation_cache:
       enabled: true
       summarizer:
         type: rule_based       # 社区版默认；onnx 需要 build-cp-on / build-cp-pg 编译路径
         max_summary_ms: 200    # SR7：硬超时，超过即回落
         max_input_tokens: 4096 # 输入截断，防止 runaway context 拖慢摘要
       id_resolver:
         enabled: true
   ```

2. 客户端**可选**地在请求体里带上 `metadata.conversation_id`（任意字符串，建议 UUID 或前缀化的会话 ID）。例如：

   ```json
   {
     "model": "gpt-4",
     "messages": [...],
     "metadata": { "conversation_id": "tenant-acme:user-42:session-7f3a" }
   }
   ```

   未带时，服务端会用消息历史的 SHA-256 推导一个稳定 ID，效果与客户端显式传入等价（仅多一次哈希计算）。

3. 重启 / SIGHUP 后启动日志会出现：

   ```
   SemanticCache: ConversationIdResolver active (client metadata.conversation_id wins, hash fallback)
   SemanticCache: ConversationSummarizer wired (type=rule_based, max_summary_ms=200)
   ```

## 选 ONNX 还是 RuleBased

| 场景 | 推荐 | 原因 |
|---|---|---|
| 社区版 / 离线 / 受限环境 | `rule_based` | 零依赖，关键词 + 长度截断；总能跑 |
| 已部署 ONNX 模型 + 高语义需求 | `onnx`（自动 fallback 到 rule_based） | `CompositeSummarizer` 在 onnx 返空 / isReady=false 时**自动**回落，并通过 `fallback_count_` 上报 fallback 频次到日志 |
| 测试环境想强制走某条路径 | 直接配 `type: rule_based` | 无需额外编译标志 |

`CompositeSummarizer` 的容错语义：primary 返空 → fallback 接管 → `fallback_count_++` + 一条 `spdlog::warn`。运维可通过日志中 `fallback_count` 趋势判断是否需要更新 ONNX 模型。

## 安全（SR1+SR4）

- **SR1 跨租户硬隔离**：partition_key 第一段总是 `tenant_id` 的 SHA-256；只要 `tenant_id` 不同，缓存就是物理隔离的（不仅是逻辑过滤）。`tests/unit/cache/test_semantic_cache_v2_isolation` 的 mutation 实验证实：把 `tenant_id` 从 key 中拿掉，跨租户隔离测试立即 FAIL。
- **SR4 PII 不入摘要**：`SummarizerFactory` 把 `PIIFilter` 指针注入到 `RuleBasedSummarizer` 和 `OnnxSummarizer` 两侧；任何 PII fixture 在摘要文本生成前已被 `mask()` 替换。
- **SR7 ONNX 硬超时**：`OnnxSummarizer::summarize` 用 `std::future` + `wait_for(max_summary_ms)` 包住模型调用，超时立即放弃，让 `CompositeSummarizer` 回落到 RuleBased。

## 验证

- ctest（CP+ON 路径）会自动跑 `ConversationIdResolverTest`、`SummarizerFactoryTest`、`CompositeSummarizerTest`、`SemanticCacheV2IsolationTest` 等套件。
- 手动跨对话验证：用相同 `tenant_id` 但不同 `conversation_id` 发两次相似末问，第一次 MISS、第二次仍 MISS（不再误命中）；同一 `conversation_id` 重复同问 → HIT。

## 常见问题

- **客户端能不能不传 `conversation_id`？** 能。服务端会自动按消息历史 hash 推导。
- **`conversation_id` 跨租户被复用怎么办？** 不会出问题：`tenant_id` 永远先于 `conversation_id` 进入 partition_key，租户层是物理隔离。
- **回落触发后摘要质量会下降吗？** 是的，但 RuleBased 已能覆盖大部分对话场景。可观测 `fallback_count` 趋势，必要时升级 ONNX 模型。
