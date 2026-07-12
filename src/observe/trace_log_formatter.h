#pragma once
#include <spdlog/pattern_formatter.h>
#include "observe/tracing.h"

namespace aegisgate {

class TraceIdFlag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& dest) override {
        auto tid = Tracing::currentTraceId();
        if (tid.empty()) tid = "00000000000000000000000000000000";
        dest.append(tid.data(), tid.data() + tid.size());
    }
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<TraceIdFlag>();
    }
};

class SpanIdFlag : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& dest) override {
        auto sid = Tracing::currentSpanId();
        if (sid.empty()) sid = "0000000000000000";
        dest.append(sid.data(), sid.data() + sid.size());
    }
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<SpanIdFlag>();
    }
};

} // namespace aegisgate
