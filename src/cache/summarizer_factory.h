#pragma once
#include "cache/conversation_summarizer.h"
#include <chrono>
#include <memory>
#include <string>

namespace aegisgate {

class PIIFilter;

struct SummarizerConfig {
    std::string type = "rule";           // "rule" | "onnx"
    size_t max_chars = 512;
    size_t top_keywords = 5;
    std::string onnx_model_path;
    int onnx_timeout_ms = 200;            // SR7
};

// CR1 dispatch (D3=B). Returns the appropriate summarizer:
//   type == "onnx" + ENABLE_ONNX + model loads: CompositeSummarizer(Onnx, RuleBased)
//   type == "onnx" + load fails / ONNX off    : RuleBasedSummarizer (logged)
//   type == "rule" (default)                  : RuleBasedSummarizer
//
// SR4: the same PIIFilter pointer is forwarded into both inner summarizers.
std::unique_ptr<ConversationSummarizer>
makeSummarizer(const SummarizerConfig& cfg, const PIIFilter* pii);

} // namespace aegisgate
