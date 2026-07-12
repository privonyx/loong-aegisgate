#pragma once
#include "core/pipeline.h"
#include <string>

namespace aegisgate {

class QualityMonitor;

class QualityScorer : public PipelineStage {
public:
    struct Config {
        int min_response_length = 10;
        int max_response_length = 50000;
        double max_repetition_ratio = 0.5;
    };

    QualityScorer();
    explicit QualityScorer(Config cfg);
    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "QualityScorer"; }

    // P0-5: when set, each processed response feeds the per-model quality EMA
    // that admin dashboards (case-study trends) read. Borrowed, may be null.
    void setQualityMonitor(QualityMonitor* monitor) { quality_monitor_ = monitor; }

private:
    double scoreLengthAdequacy(const std::string& response) const;
    double scoreCompleteness(const std::string& response) const;
    double scoreRepetition(const std::string& response) const;
    double scoreFormatCompliance(const RequestContext& ctx) const;
    Config cfg_;
    QualityMonitor* quality_monitor_ = nullptr;  // P0-5: borrowed, may be null
};

}  // namespace aegisgate
