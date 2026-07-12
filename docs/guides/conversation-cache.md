# Multi-turn Conversation Cache Guide

> Feature: Phase 6.4 conversation cache (`conversation_id` + `ConversationSummarizer` + cross-tenant hard isolation)
> Available since: v1.2 (delivered by TASK-20260513-01)
> 中文版本：[`conversation-cache_zh.md`](conversation-cache_zh.md) — authoritative source

This page is a fast-reading mirror of the Chinese guide. Refer to that
document for the full data-flow diagram, mutation evidence, and FAQ.

## What it solves

Without this upgrade `SemanticCache` only hashed the last user turn,
which made two unrelated conversations collide whenever their final
question wording was close. Three components now collaborate to fix
both the precision and the cross-tenant safety problem:

| Component | Role |
|---|---|
| `ConversationIdResolver` | Honours client-supplied `metadata.conversation_id`; falls back to a SHA-256 over message history; first-turn fallback uses `request_id`. |
| `ConversationSummarizer` (`CompositeSummarizer`) | Compresses history into summary text fed into the cache key. Composite = ONNX primary + RuleBased fallback (CR1 scheme B). |
| `SemanticCache::extractCacheKeyInfoV2` | Mixes `tenant_id` + `conversation_id` into the partition key with SHA-256, replacing the legacy `std::hash`. |

## Enable it

```yaml
cache:
  conversation_cache:
    enabled: true
    summarizer:
      type: rule_based       # use 'onnx' on build-cp-on / build-cp-pg targets
      max_summary_ms: 200    # SR7 hard timeout
      max_input_tokens: 4096
    id_resolver:
      enabled: true
```

Clients may optionally include `metadata.conversation_id` in the request
body. When omitted the resolver hashes the message history.

Startup log:

```
SemanticCache: ConversationIdResolver active (client metadata.conversation_id wins, hash fallback)
SemanticCache: ConversationSummarizer wired (type=rule_based, max_summary_ms=200)
```

## ONNX vs RuleBased

| Scenario | Recommended type |
|---|---|
| Community / offline / restricted env | `rule_based` |
| Production with ONNX model and best summary quality | `onnx` (auto-fallbacks to rule_based on empty / not-ready) |
| Test environment with deterministic output | `rule_based` |

`CompositeSummarizer` increments `fallback_count_` and logs a warning
each time the primary returns empty so operators can monitor regression.

## Security

- **SR1 cross-tenant isolation** — partition key prefix is always
  `sha256(tenant_id)`; mutation removing the prefix immediately fails
  the isolation test.
- **SR4 PII never reaches summary text** — `SummarizerFactory` injects
  `PIIFilter` into both inner summarizers; PII fixtures are `mask()`-ed
  before summarization.
- **SR7 ONNX hard timeout** — `std::future::wait_for(max_summary_ms)`
  bounds the model call.

## References

- Design: `docs/specs/2026-05-13-phase6-completion-design.md` §5
- Creative: `memory-bank/creative/creative-conversation-summarizer.md`
- Plan: `docs/plans/2026-05-13-phase6-completion.md` §4
