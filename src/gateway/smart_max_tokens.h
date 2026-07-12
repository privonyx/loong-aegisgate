#pragma once
#include "core/pipeline.h"

namespace aegisgate {

class ConnectorRegistry;

class SmartMaxTokens : public PipelineStage {
public:
    struct Config {
        bool enabled = true;
        int default_max_output = 2048;
        double max_output_ratio = 2.0;
        int min_output_tokens = 100;
    };

    SmartMaxTokens();
    explicit SmartMaxTokens(Config cfg);

    void setConnectorRegistry(const ConnectorRegistry* registry) {
        registry_ = registry;
    }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "SmartMaxTokens"; }

private:
    Config cfg_;
    const ConnectorRegistry* registry_ = nullptr;
};

} // namespace aegisgate
