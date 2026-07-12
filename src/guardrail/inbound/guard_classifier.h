#pragma once
#include "core/pipeline.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

#ifdef AEGISGATE_ENABLE_GUARD
#include <onnxruntime_cxx_api.h>
#endif

namespace aegisgate {

class BertTokenizer;
class SpmTokenizer;
class AuditLogger;

namespace guard {
class GuardAdminController;
}  // namespace guard

struct GuardResult {
    bool safe = true;
    std::string category = "safe";
    float score = 0.0f;
    float threshold = 0.5f;
};

class GuardClassifier : public PipelineStage {
public:
    GuardClassifier(const std::string& model_path, const std::string& vocab_path);
    GuardClassifier(const std::string& model_path,
                    const std::string& vocab_path,
                    const std::string& spm_model_path);
    ~GuardClassifier();

    bool isReady() const;
    GuardResult classify(const std::string& text) const;
    void setThreshold(float threshold);
    void setCategories(const std::vector<std::string>& categories);

    // C3: fail policy. Default fail-open (no model -> pass-through). When set to
    // closed, an unready classifier rejects requests (+audit) instead of passing
    // through — for high-security deployments.
    void setFailClosed(bool fail_closed) { fail_closed_ = fail_closed; }

    // The bundled guard model (deberta-v3-base-prompt-injection-v2) is trained
    // on English and false-positives heavily on CJK text. When enabled (default)
    // process() skips model inference for predominantly-CJK messages and relies
    // on the rule-engine cn_* injection patterns (which run earlier) instead.
    void setSkipCjk(bool skip) { skip_cjk_ = skip; }

    // P1-1 / P1-2: borrowed AuditLogger; an unsafe reject writes a "blocked"
    // audit entry. Ownership stays with the caller (pipeline).
    void setAuditLogger(AuditLogger* logger) { audit_logger_ = logger; }

    // TASK-20260708-03 / REV20260707-C2: borrowed GuardAdminController; when
    // set, an unsafe reject also records a structured `GuardExplanation`
    // (L3 layer, ML classifier verdict) for admin lookup. Nullable = no-op.
    void setGuardAdminController(guard::GuardAdminController* controller) {
        guard_admin_controller_ = controller;
    }
    // Optional model_version tag surfaced in the recorded explanation
    // payload's `model_version` field. Empty by default; wired by
    // pipeline_assembler when the guard model registry is available.
    void setModelVersion(const std::string& version) { model_version_ = version; }

    // P1-2 / SR-3: test seam to exercise the intercept pipeline (unsafe ->
    // Reject) without a real ONNX model. When set, classify() delegates to the
    // hook and process() runs even if no model is loaded. Test-only.
    void setClassifyHookForTest(
        std::function<GuardResult(const std::string&)> hook) {
        classify_hook_ = std::move(hook);
    }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "GuardClassifier"; }

private:
#ifdef AEGISGATE_ENABLE_GUARD
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<BertTokenizer> tokenizer_;
    std::unique_ptr<SpmTokenizer> spm_tokenizer_;
    std::vector<std::string> label_names_;
#endif
    std::string model_path_;
    std::string vocab_path_;
    std::string spm_model_path_;
    float threshold_ = 0.5f;
    bool ready_ = false;
    bool fail_closed_ = false;  // C3: default fail-open
    bool skip_cjk_ = true;      // skip English-only model on CJK text
    AuditLogger* audit_logger_ = nullptr;  // P1-2: borrowed, may be null
    // TASK-20260708-03 / REV20260707-C2: borrowed, may be null.
    guard::GuardAdminController* guard_admin_controller_ = nullptr;
    std::string model_version_;  // filled in via setModelVersion, empty by default
    std::function<GuardResult(const std::string&)> classify_hook_;  // test seam
};

} // namespace aegisgate
