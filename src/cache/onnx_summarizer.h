#pragma once
#include "cache/conversation_summarizer.h"
#include <chrono>
#include <memory>

namespace aegisgate {

class PIIFilter;

// ONNX-runtime backed summarizer (D3=B primary path, CR1 scheme B primary).
//
// The class is unconditionally compiled (PIMPL) so the rest of the codebase
// can reference it regardless of AEGISGATE_ENABLE_ONNX. When ONNX is disabled
// at compile time, or when the model file cannot be loaded at runtime, the
// instance stays in a "not ready" state and `summarize()` returns an empty
// string -- enabling the CompositeSummarizer fallback path.
//
// Security:
//   SR7 hard timeout: summarize() wraps inference in std::async + future.wait_for(timeout_);
//                     on timeout returns "" + spdlog::warn.
//   SR4 PII masking : when PIIFilter is injected, messages are masked before
//                     reaching the ONNX session.
class OnnxSummarizer : public ConversationSummarizer {
public:
    explicit OnnxSummarizer(const std::string& model_path,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(200),
                            const PIIFilter* pii = nullptr);
    ~OnnxSummarizer();

    OnnxSummarizer(const OnnxSummarizer&) = delete;
    OnnxSummarizer& operator=(const OnnxSummarizer&) = delete;

    std::string summarize(const std::vector<Message>& msgs) override;
    std::string name() const override { return "Onnx"; }

    bool isReady() const { return ready_; }
    int timeoutMs() const { return static_cast<int>(timeout_.count()); }

    static constexpr size_t kMaxInputTokens = 4096;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::chrono::milliseconds timeout_;
    bool ready_ = false;
    const PIIFilter* pii_ = nullptr;
};

} // namespace aegisgate
