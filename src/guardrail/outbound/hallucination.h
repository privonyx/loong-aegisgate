#pragma once
#include "core/pipeline.h"
#include <re2/re2.h>
#include <string>
#include <vector>

namespace aegisgate {

struct HallucinationResult {
    double confidence_score = 1.0;
    bool flagged = false;
    std::vector<std::string> suspicious_claims;
    std::string reason;
};

struct GroundTruthConfig {
    bool enabled = false;
    float min_groundedness = 0.5f;
};

class HallucinationDetector : public PipelineStage {
public:
    HallucinationDetector();

    void setThreshold(double threshold);
    void setGroundTruthConfig(const GroundTruthConfig& config);

    HallucinationResult analyze(const std::string& output,
                                 const std::string& input) const;

    double measureGroundedness(const std::string& output,
                               const std::vector<std::string>& reference_texts) const;

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "HallucinationDetector"; }

private:
    double countSpecificClaims(const std::string& text) const;
    double measureInputRelevance(const std::string& output,
                                  const std::string& input) const;

    double threshold_ = 0.3;
    GroundTruthConfig ground_truth_config_;
    re2::RE2 date_re_{R"(\b\d{4}[-/]\d{1,2}[-/]\d{1,2}\b)"};
    re2::RE2 url_re_{R"(https?://[^\s]+)"};
    re2::RE2 num_re_{R"(\b\d+\.?\d*\s*%)"};
};

} // namespace aegisgate
