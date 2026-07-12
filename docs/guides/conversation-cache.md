# Multi-turn Conversation Cache Guide

> Feature: Phase 6.4 conversation cache (`conversation_id` + `ConversationSummarizer` + cross-tenant hard isolation)
> Available since: v1.2 (delivered by TASK-20260513-01)
> 中文版：[`conversation-cache_zh.md`](conversation-cache_zh.md)

This guide explains how `SemanticCache` can keep the cost savings of semantic
hits in multi-turn conversations while **never** replaying tenant A's
conversation content to tenant B.

## What it solves

Without this mechanism, AegisGate hashed only the last user message to compute
the cache key. Two completely unrelated conversations would collide whenever
their final question wording was close — a **precision** problem and a
**security** problem in multi-tenant / multi-role setups.

Three collaborating components fix this:

| Component | Role | File |
|---|---|---|
| `ConversationIdResolver` | Client may set `conversation_id` explicitly; when omitted it is derived from a SHA-256 of the message history; the first-turn fallback uses `request_id`. | `src/cache/conversation_id_resolver.{h,cpp}` |
| `ConversationSummarizer` interface + `CompositeSummarizer` | Compresses history into summary text fed into the cache key. `CompositeSummarizer` = `OnnxSummarizer` primary + `RuleBasedSummarizer` fallback (CR1 scheme B). | `src/cache/{conversation_summarizer.h, rule_based_summarizer.{h,cpp}, onnx_summarizer.{h,cpp}, composite_summarizer.{h,cpp}, summarizer_factory.{h,cpp}}` |
| `SemanticCache::extractCacheKeyInfoV2` | The partition key always mixes in `tenant_id` and `conversation_id` using SHA-256 (replacing the legacy `std::hash`). | `src/cache/semantic_cache.{h,cpp}` |

## Enable it

1. Turn on the switch in `config/aegisgate.yaml`:

   ```yaml
   cache:
     conversation_cache:
       enabled: true
       summarizer:
         type: rule_based       # community default; onnx needs the build-cp-on / build-cp-pg targets
         max_summary_ms: 200    # SR7: hard timeout — fall back once exceeded
         max_input_tokens: 4096 # input truncation, prevents runaway context from slowing summarization
       id_resolver:
         enabled: true
   ```

2. Clients may **optionally** include `metadata.conversation_id` in the request body (any string; a UUID or prefixed session ID is recommended). For example:

   ```json
   {
     "model": "gpt-4",
     "messages": [...],
     "metadata": { "conversation_id": "tenant-acme:user-42:session-7f3a" }
   }
   ```

   When omitted, the server derives a stable ID from the SHA-256 of the message history — equivalent to a client-supplied ID (just one extra hash computation).

3. After a restart / SIGHUP the startup log shows:

   ```
   SemanticCache: ConversationIdResolver active (client metadata.conversation_id wins, hash fallback)
   SemanticCache: ConversationSummarizer wired (type=rule_based, max_summary_ms=200)
   ```

## ONNX vs RuleBased

| Scenario | Recommended | Why |
|---|---|---|
| Community / offline / restricted env | `rule_based` | zero dependencies, keyword + length truncation; always runs |
| ONNX model deployed + high semantic need | `onnx` (auto-fallback to rule_based) | `CompositeSummarizer` **automatically** falls back when onnx returns empty / `isReady=false`, and reports fallback frequency to the log via `fallback_count_` |
| Test env forcing a specific path | set `type: rule_based` directly | no extra build flag required |

`CompositeSummarizer` fault-tolerance semantics: primary returns empty → fallback
takes over → `fallback_count_++` plus one `spdlog::warn`. Operators can watch the
`fallback_count` trend to decide whether the ONNX model needs updating.

## Security (SR1 + SR4)

- **SR1 cross-tenant hard isolation**: the first segment of the partition key is always the SHA-256 of `tenant_id`; as long as `tenant_id` differs, caches are physically isolated (not merely logically filtered). The mutation experiment in `tests/unit/cache/test_semantic_cache_v2_isolation` confirms it: remove `tenant_id` from the key and the cross-tenant isolation test fails immediately.
- **SR4 PII never enters the summary**: `SummarizerFactory` injects a `PIIFilter` pointer into both `RuleBasedSummarizer` and `OnnxSummarizer`; any PII fixture is `mask()`-ed before the summary text is generated.
- **SR7 ONNX hard timeout**: `OnnxSummarizer::summarize` wraps the model call with `std::future` + `wait_for(max_summary_ms)`, giving up on timeout so `CompositeSummarizer` falls back to RuleBased.

## Verification

- `ctest` (CP+ON path) runs `ConversationIdResolverTest`, `SummarizerFactoryTest`, `CompositeSummarizerTest`, `SemanticCacheV2IsolationTest`, etc.
- Manual cross-conversation check: send two similar final questions with the same `tenant_id` but different `conversation_id` — first MISS, second still MISS (no false hit); repeat the same question under the same `conversation_id` → HIT.

## FAQ

- **Can the client omit `conversation_id`?** Yes. The server derives it automatically from the message-history hash.
- **What if a `conversation_id` is reused across tenants?** No problem: `tenant_id` always enters the partition key before `conversation_id`, so the tenant layer is physically isolated.
- **Does summary quality drop after a fallback?** Yes, but RuleBased already covers most conversation scenarios. Watch the `fallback_count` trend and upgrade the ONNX model if needed.
