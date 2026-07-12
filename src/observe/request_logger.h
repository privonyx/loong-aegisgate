#pragma once
#include "core/pipeline.h"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <vector>

namespace aegisgate {

struct RequestLogEntry {
    std::string request_id;
    std::string tenant_id;
    std::string app_id;
    std::string model;
    std::string provider;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    double duration_ms = 0.0;
    std::string status;
    std::string error_detail;
    std::string timestamp;
};

using LogSink = std::function<void(const nlohmann::json&)>;

class RequestLogger : public PipelineStage {
public:
    RequestLogger();

    void setSink(LogSink sink);
    void setMaskApiKeys(bool mask);

    nlohmann::json formatEntry(const RequestLogEntry& entry) const;
    void logRequest(const RequestLogEntry& entry);

    static std::string maskApiKey(const std::string& key);

    const std::vector<nlohmann::json>& logs() const { return logs_; }
    void clear();

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "RequestLogger"; }

private:
    std::string currentTimestamp() const;

    LogSink sink_;
    std::vector<nlohmann::json> logs_;
    bool mask_api_keys_ = true;
    size_t max_logs_ = 50000;
};

} // namespace aegisgate
