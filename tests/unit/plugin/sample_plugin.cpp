#include "plugin/plugin_stage.h"

class SamplePlugin : public aegisgate::PipelineStage {
public:
    aegisgate::StageResult process(aegisgate::RequestContext& /*ctx*/) override {
        return aegisgate::StageResult::Continue;
    }
    std::string name() const override { return "SamplePlugin"; }
};

AEGISGATE_PLUGIN_API aegisgate::PipelineStage* aegisgate_plugin_create() {
    return new SamplePlugin();
}

AEGISGATE_PLUGIN_API void aegisgate_plugin_destroy(aegisgate::PipelineStage* p) {
    delete p;
}

AEGISGATE_PLUGIN_API const char* aegisgate_plugin_info() {
    return R"({"name":"sample","version":"1.0.0","description":"Test plugin","api_version":1})";
}
