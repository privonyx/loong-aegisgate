#include "guardrail/inbound/guard_classifier.h"
#include "guardrail/inbound/guard_decision.h"
#include "guardrail/admin/guard_admin_controller.h"
#include "guardrail/audit.h"
#include "guardrail/guard_explanation_builder.h"
#include <spdlog/spdlog.h>
#include <cstdint>

#ifdef AEGISGATE_ENABLE_GUARD
#include "cache/tokenizer.h"
#include <array>
#include <algorithm>
#endif

namespace aegisgate {

namespace {

// Returns true when CJK characters dominate the text. The bundled guard model
// is English-trained and produces high-confidence false positives on Chinese/
// Japanese/Korean input, so process() skips it for such messages and leans on
// the rule-engine cn_* injection patterns (which run before this stage).
bool isPredominantlyCjk(const std::string& s) {
    size_t cjk = 0, total = 0;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (size_t k = 1; k < len && i + k < n; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        i += len;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') continue;
        ++total;
        const bool is_cjk =
            (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
            (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Extension A
            (cp >= 0x3000 && cp <= 0x303F) ||   // CJK Symbols & Punctuation
            (cp >= 0x3040 && cp <= 0x30FF) ||   // Hiragana + Katakana
            (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul Syllables
            (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
            (cp >= 0xFF00 && cp <= 0xFFEF);     // Halfwidth & Fullwidth Forms
        if (is_cjk) ++cjk;
    }
    if (total == 0) return false;
    return (static_cast<double>(cjk) / static_cast<double>(total)) >= 0.20;
}

} // namespace

GuardClassifier::GuardClassifier(const std::string& model_path,
                                 const std::string& vocab_path)
    : GuardClassifier(model_path, vocab_path, "") {}

GuardClassifier::GuardClassifier(const std::string& model_path,
                                 const std::string& vocab_path,
                                 const std::string& spm_model_path)
    : model_path_(model_path),
      vocab_path_(vocab_path),
      spm_model_path_(spm_model_path) {
#ifdef AEGISGATE_ENABLE_GUARD
    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "guard");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), opts);

        bool tokenizer_loaded = false;
        if (!spm_model_path.empty()) {
            spm_tokenizer_ = std::make_unique<SpmTokenizer>();
            tokenizer_loaded = spm_tokenizer_->loadModel(spm_model_path);
        } else {
            tokenizer_ = std::make_unique<BertTokenizer>();
            tokenizer_loaded = tokenizer_->loadVocab(vocab_path);
        }
        if (!tokenizer_loaded) {
            spdlog::warn("GuardClassifier: failed to load tokenizer");
            ready_ = false;
            return;
        }
        if (label_names_.empty()) {
            label_names_ = {"safe", "injection"};
        }
        ready_ = true;
        spdlog::info("GuardClassifier: loaded model from {}", model_path);
    } catch (const std::exception& e) {
        spdlog::warn("GuardClassifier: failed to load model: {} (fail-open)", e.what());
        ready_ = false;
    }
#else
    spdlog::info("GuardClassifier: ONNX not enabled, running in pass-through mode");
#endif
}

GuardClassifier::~GuardClassifier() = default;

bool GuardClassifier::isReady() const { return ready_; }

void GuardClassifier::setThreshold(float threshold) { threshold_ = threshold; }

void GuardClassifier::setCategories(const std::vector<std::string>& categories) {
#ifdef AEGISGATE_ENABLE_GUARD
    label_names_ = categories;
#else
    (void)categories;
#endif
}

GuardResult GuardClassifier::classify(const std::string& text) const {
    // P1-2 / SR-3: test seam — exercise the intercept pipeline without ONNX.
    if (classify_hook_) return classify_hook_(text);
#ifdef AEGISGATE_ENABLE_GUARD
    if (!ready_ || !session_ || (!spm_tokenizer_ && !tokenizer_)) {
        return {true, "safe", 0.0f, threshold_};
    }

    try {
        auto tokens = spm_tokenizer_ ? spm_tokenizer_->encode(text)
                                     : tokenizer_->encode(text);
        Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
            OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

        std::array<int64_t, 2> input_shape = {
            1, static_cast<int64_t>(tokens.input_ids.size())};

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(
            mem_info, tokens.input_ids.data(), tokens.input_ids.size(),
            input_shape.data(), input_shape.size()));
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(
            mem_info, tokens.attention_mask.data(), tokens.attention_mask.size(),
            input_shape.data(), input_shape.size()));

        std::vector<const char*> input_names = {"input_ids", "attention_mask"};
        std::vector<const char*> output_names = {"logits"};

        auto outputs = session_->Run(Ort::RunOptions{nullptr},
            input_names.data(), inputs.data(), inputs.size(),
            output_names.data(), output_names.size());

        auto tensor_info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto shape = tensor_info.GetShape();
        size_t element_count = 1;
        for (int64_t dim : shape) {
            if (dim > 0) {
                element_count *= static_cast<size_t>(dim);
            }
        }
        const float* data = outputs[0].GetTensorData<float>();
        std::vector<float> logits(data, data + element_count);
        auto decision = guard_detail::evaluate(logits, /*safe_index=*/0, threshold_);

        const std::string category =
            (decision.label_index < label_names_.size())
                ? label_names_[decision.label_index]
                : (decision.unsafe ? "unsafe" : "safe");
        return {!decision.unsafe, category, decision.score, threshold_};
    } catch (const std::exception& e) {
        spdlog::warn("GuardClassifier inference failed: {} (fail-open)", e.what());
        return {true, "safe", 0.0f, threshold_};
    }
#else
    (void)text;
    return {true, "safe", 0.0f, threshold_};
#endif
}

StageResult GuardClassifier::process(RequestContext& ctx) {
    // Honest degradation (P1-2 / SR-2): with no model and no test hook the
    // classifier cannot inspect. Default fail-open (pass-through); fail-closed
    // (C3 opt-in) rejects + audits instead. Either way the log is honest — it
    // does NOT claim the model is active.
    if (!ready_ && !classify_hook_) {
        if (fail_closed_) {
            spdlog::warn("GuardClassifier: fail-closed degrade (no model / "
                         "inference unavailable) — rejecting request {}",
                         ctx.request_id);
            if (audit_logger_) {
                audit_logger_->logAction(ctx.request_id, ctx.tenant_id, name(),
                                         "blocked", "guard_fail_closed");
            }
            return StageResult::Reject;
        }
        return StageResult::Continue;
    }

    // P0-4 / C3: classify InputPreprocessor's canonicalised text (via
    // ctx.scanText, which falls back to raw content when preprocessing didn't run
    // or arrays are misaligned) so obfuscated unsafe content is normalized before
    // scoring.
    const auto& messages = ctx.chat_request.messages;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (isToolMessage(msg)) continue;
        const std::string& content = ctx.scanText(i);
        if (skip_cjk_ && isPredominantlyCjk(content)) {
            // English-trained model is unreliable on CJK; rule-engine cn_*
            // patterns (earlier stage) cover Chinese injection instead.
            continue;
        }
        auto result = classify(content);
        if (!result.safe) {
            spdlog::warn("GuardClassifier: unsafe content in request {}: "
                         "category={}, score={:.3f}",
                         ctx.request_id, result.category, result.score);
            if (audit_logger_) {
                audit_logger_->logAction(
                    ctx.request_id, ctx.tenant_id, name(), "blocked",
                    "guard category=" + result.category);
            }
            // TASK-20260708-03 / REV20260707-C2: L3 ML classifier verdict
            // recorded as structured GuardExplanation. Nullable-safe (SR-3).
            if (guard_admin_controller_) {
                guard_admin_controller_->recordExplanation(
                    ctx.request_id,
                    guard::GuardExplanationBuilder::fromMlClassifier(
                        result, model_version_));
            }
            return StageResult::Reject;
        }
    }
    return StageResult::Continue;
}

} // namespace aegisgate
