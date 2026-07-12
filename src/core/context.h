#pragma once
#include "auth/auth_models.h"
#include "aegisgate/types.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

#ifdef AEGISGATE_ENABLE_OTEL
#include <opentelemetry/trace/span.h>
#include <opentelemetry/context/context.h>
#include <opentelemetry/nostd/shared_ptr.h>
namespace otel_trace_ctx = opentelemetry::trace;
#endif

namespace aegisgate {

struct RetrievalSource {
    std::string chunk_id;
    std::string document_id;
    std::string content;
    float relevance = 0.0f;

    RetrievalSource() = default;
    RetrievalSource(std::string cid_in, std::string did_in, std::string c_in, float r_in)
        : chunk_id(std::move(cid_in)), document_id(std::move(did_in)),
          content(std::move(c_in)), relevance(r_in) {}
};

struct Citation {
    std::string chunk_id;
    std::string document_id;
    float relevance = 0.0f;
    std::string text_snippet;

    Citation() = default;
    Citation(std::string cid_in, std::string did_in, float rel_in, std::string snippet_in)
        : chunk_id(std::move(cid_in)), document_id(std::move(did_in)),
          relevance(rel_in), text_snippet(std::move(snippet_in)) {}
};

struct RequestContext {
    std::string request_id;
    std::string api_key;
    std::string tenant_id;
    std::string app_id;

    ChatRequest chat_request;

    std::string target_model;
    std::vector<float> embedding;
    bool cache_hit = false;
    std::string cached_response;

    // P1-7: why the router picked target_model. Written by Router::selectModel
    // (e.g. "user_pinned" | "router_economy" | "router_quality" |
    // "router_default") and copied into CostRecord by CostTracker so the cost
    // ledger explains each routing decision. Empty until a router sets it.
    std::string routing_decision_reason;

    TokenUsage token_usage;
    std::chrono::steady_clock::time_point start_time;

    // Streaming support
    std::string accumulated_response;
    std::string stream_model;
    bool is_streaming = false;
    std::string chunk_output;

    // RBAC auth context (populated when Feature::RBAC is enabled)
    std::optional<AuthContext> auth_context;

    // Hallucination detection
    double hallucination_score = 1.0;
    bool hallucination_flagged = false;

    // Output quality scoring (B4-3)
    double quality_score = -1.0;

    // A/B test routing (B4-2)
    std::string ab_experiment;
    std::string ab_variant;

    // Input preprocessing (Phase 1.2)
    std::vector<std::string> normalized_messages;
    bool input_preprocessed = false;

    // TASK-20260707-03 / REV20260707-N19: scannable text extracted from each
    // message's multimodal image_url parts (URL literal + decoded data: text
    // payload), normalized in lock-step with normalized_messages. Index-aligned
    // with chat_request.messages. Consumed via scanImageText() so inbound stages
    // can detect PII/keywords/injection hidden in the vision reference channel.
    std::vector<std::string> image_scan_messages;

    // Token optimization
    int tokens_estimated = 0;
    int tokens_saved_compression = 0;

    // External safety (L4)
    bool external_safety_checked = false;
    bool external_safety_flagged = false;

    // Tool/function calling
    nlohmann::json accumulated_tool_calls;
    bool has_tools = false;

    // Streaming outbound rejection tracking (B02)
    bool stream_rejected = false;

    // Name of the inbound stage that rejected the request (set by
    // Pipeline::execute on StageResult::Reject). The gateway maps it to a
    // precise ErrorCode instead of always reporting a generic injection error.
    std::string reject_stage;

    // RAG retrieval (Phase 8.2)
    std::vector<RetrievalSource> retrieval_sources;
    std::vector<Citation> citations;
    float groundedness_score = -1.0f;

    // Phase 11.5 E3.1 — outbound response headers stamped by inbound
    // stages (e.g. BudgetGuardStage sets X-AegisGate-Budget-Guard:
    // triggered). Read by the HTTP outbound layer before flushing.
    std::unordered_map<std::string, std::string> response_headers;

    // TASK-20260709-01 / REV20260707-I5 D7: inbound request headers forwarded
    // from the HTTP layer (whitelist). PromptTemplateStage reads
    // X-AegisGate-Template for optional name override.
    std::unordered_map<std::string, std::string> request_headers;

    // C3 (REV20260702-C3): unified accessor for the text guardrails should scan.
    // Returns the normalized view when preprocessing produced an aligned vector,
    // otherwise falls back to raw message content. Index-aligned with
    // chat_request.messages. Centralizing the normalized-vs-raw decision here
    // stops full-width/homoglyph bypasses from slipping past any stage that
    // previously scanned raw content directly.
    const std::string& scanText(size_t i) const {
        if (input_preprocessed &&
            normalized_messages.size() == chat_request.messages.size()) {
            return normalized_messages[i];
        }
        return chat_request.messages[i].content;
    }

    // TASK-20260707-03 / REV20260707-N19: accessor for the image-reference scan
    // surface. Returns the normalized view when the preprocessor produced an
    // aligned vector; otherwise falls back to a freshly-computed raw extraction
    // (mirrors scanText's normalized-vs-raw fallback so detection never depends
    // on preprocessing being enabled). Returned by value: image references are a
    // detection-only side channel, never written back to the upstream request.
    std::string scanImageText(size_t i) const {
        if (input_preprocessed &&
            image_scan_messages.size() == chat_request.messages.size()) {
            return image_scan_messages[i];
        }
        return extractImageRefText(chat_request.messages[i].content_parts,
                                   kDefaultImageScanDecodeBytes);
    }

#ifdef AEGISGATE_ENABLE_OTEL
    opentelemetry::context::Context trace_ctx;
    opentelemetry::nostd::shared_ptr<otel_trace_ctx::Span> root_span;
#endif
};

} // namespace aegisgate
