#pragma once
#include "aegisgate/types.h"
#include "observe/cost_tracker.h"
#include "multimodal/modality.h"
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace aegisgate {

// P1-4: parse token usage out of a proxied upstream response body. Embeddings
// responses carry {"usage":{"prompt_tokens":N,"total_tokens":N}}; image/audio
// endpoints usually carry none -> {0,0}. Never throws (malformed bodies and
// missing fields degrade to zero), so it is safe on the request hot path.
//
// Extracted as a header-only helper so proxy billing is unit-testable without
// a live upstream connector (processProxyRequest itself dispatches to the
// network and has no offline harness).
inline std::pair<int, int> parseProxyUsageTokens(const std::string& body) {
    int input = 0, output = 0;
    auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("usage") ||
        !j["usage"].is_object()) {
        return {0, 0};
    }
    const auto& u = j["usage"];
    if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer()) {
        input = u["prompt_tokens"].get<int>();
    } else if (u.contains("input_tokens") && u["input_tokens"].is_number_integer()) {
        input = u["input_tokens"].get<int>();
    }
    if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer()) {
        output = u["completion_tokens"].get<int>();
    } else if (u.contains("output_tokens") && u["output_tokens"].is_number_integer()) {
        output = u["output_tokens"].get<int>();
    }
    // Embeddings: only total_tokens present -> count it as input tokens.
    if (input == 0 && output == 0 && u.contains("total_tokens") &&
        u["total_tokens"].is_number_integer()) {
        input = u["total_tokens"].get<int>();
    }
    return {input, output};
}

// P1-4: assemble a CostRecord for a proxy/multimodal request. The tracker
// prices the parsed (input, output) tokens against its pricing table; modality
// is derived from the endpoint so summaryByModality() is no longer dead code.
// Cost is best-effort (D2=B): models absent from the pricing table bill 0 but
// the record — with correct modality and tokens — is still produced.
inline CostRecord buildProxyCostRecord(CostTracker& tracker,
                                       const std::string& endpoint,
                                       const std::string& model,
                                       const std::string& tenant_id,
                                       const std::string& request_id,
                                       const std::string& timestamp,
                                       const ProxyResponse& resp) {
    auto tokens = parseProxyUsageTokens(resp.body);
    CostRecord rec = tracker.calculate(model, tokens.first, tokens.second);
    rec.request_id = request_id;
    rec.tenant_id = tenant_id;
    rec.modality = modalityToString(modalityFromEndpoint(endpoint));
    rec.routing_decision_reason = "proxy_passthrough";
    rec.timestamp = timestamp;
    return rec;
}

} // namespace aegisgate
