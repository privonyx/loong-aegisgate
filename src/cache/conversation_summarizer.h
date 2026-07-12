#pragma once
#include "aegisgate/types.h"
#include <string>
#include <vector>

namespace aegisgate {

// Abstract interface for conversation summarization (CR1, D3=B).
// Implementations:
//   - RuleBasedSummarizer (zero-deps, always available; D3=B fallback path)
//   - OnnxSummarizer (AEGISGATE_ENABLE_ONNX; D3=B primary path, SR7 timeout)
//   - CompositeSummarizer (decorator; primary + fallback, fires fallback on empty)
//
// SECURITY: implementations must apply PII masking (SR4) on the message text
// *before* running the summarization algorithm.
class ConversationSummarizer {
public:
    virtual ~ConversationSummarizer() = default;
    virtual std::string summarize(const std::vector<Message>& msgs) = 0;
    virtual std::string name() const = 0;
};

} // namespace aegisgate
