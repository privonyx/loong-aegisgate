#include "observe/request_logger.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace aegisgate {

RequestLogger::RequestLogger() = default;

void RequestLogger::setSink(LogSink sink) {
    sink_ = std::move(sink);
}

void RequestLogger::setMaskApiKeys(bool mask) {
    mask_api_keys_ = mask;
}

std::string RequestLogger::maskApiKey(const std::string& key) {
    if (key.size() < 8) return "****";
    return key.substr(0, 4) + "..." + key.substr(key.size() - 4);
}

nlohmann::json RequestLogger::formatEntry(const RequestLogEntry& entry) const {
    nlohmann::json j;
    j["request_id"] = entry.request_id;
    j["tenant_id"] = entry.tenant_id;
    j["app_id"] = entry.app_id;
    j["model"] = entry.model;
    j["provider"] = entry.provider;
    j["tokens"]["prompt"] = entry.prompt_tokens;
    j["tokens"]["completion"] = entry.completion_tokens;
    j["tokens"]["total"] = entry.total_tokens;
    j["duration_ms"] = entry.duration_ms;
    j["status"] = entry.status;
    if (!entry.error_detail.empty()) {
        j["error"] = entry.error_detail;
    }
    j["timestamp"] = entry.timestamp.empty() ? currentTimestamp() : entry.timestamp;
    return j;
}

void RequestLogger::logRequest(const RequestLogEntry& entry) {
    auto j = formatEntry(entry);
    if (logs_.size() >= max_logs_) {
        logs_.erase(logs_.begin());
    }
    logs_.push_back(j);

    if (sink_) {
        sink_(j);
    }

    spdlog::info("Request completed: id={} model={} tokens={} duration={:.1f}ms status={}",
                 entry.request_id, entry.model, entry.total_tokens,
                 entry.duration_ms, entry.status);
}

void RequestLogger::clear() {
    logs_.clear();
}

StageResult RequestLogger::process(RequestContext& ctx) {
    auto now = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double, std::milli>(
        now - ctx.start_time).count();

    RequestLogEntry entry;
    entry.request_id = ctx.request_id;
    entry.tenant_id = ctx.tenant_id;
    entry.app_id = ctx.app_id;
    entry.model = ctx.target_model.empty() ? ctx.chat_request.model : ctx.target_model;
    entry.prompt_tokens = ctx.token_usage.prompt_tokens;
    entry.completion_tokens = ctx.token_usage.completion_tokens;
    entry.total_tokens = ctx.token_usage.total_tokens;
    entry.duration_ms = duration;
    entry.status = "ok";
    entry.timestamp = currentTimestamp();

    logRequest(entry);
    return StageResult::Continue;
}

std::string RequestLogger::currentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace aegisgate
