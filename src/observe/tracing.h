#pragma once
#include <string>
#include <unordered_map>
#include <initializer_list>
#include <utility>

#ifdef AEGISGATE_ENABLE_OTEL

#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer_provider.h>
#include <opentelemetry/context/context.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/common/attribute_value.h>

namespace aegisgate {

namespace otel_trace = opentelemetry::trace;

struct TracingConfig {
    bool enabled = false;
    std::string otlp_endpoint = "http://localhost:4318";
    std::string service_name = "aegisgate";
    double sample_ratio = 1.0;
    bool use_simple_processor = false;
};

class Tracing {
public:
    static Tracing& instance();

    void initialize(const TracingConfig& config);
    void shutdown();
    bool isEnabled() const { return enabled_; }

    opentelemetry::nostd::shared_ptr<otel_trace::Tracer> tracer();

    opentelemetry::context::Context extractContext(
        const std::unordered_map<std::string, std::string>& headers);
    void injectContext(
        const opentelemetry::context::Context& ctx,
        std::unordered_map<std::string, std::string>& headers);
    void injectIfEnabled(
        std::unordered_map<std::string, std::string>& headers);

    static std::string currentTraceId();
    static std::string currentSpanId();

private:
    Tracing() = default;
    bool enabled_ = false;
};

class ScopedSpan {
public:
    ScopedSpan() = default;

    ScopedSpan(const std::string& name,
               const opentelemetry::context::Context& parent_ctx,
               std::initializer_list<
                   std::pair<opentelemetry::nostd::string_view,
                             opentelemetry::common::AttributeValue>> attrs = {});

    void setAttribute(const std::string& key, const std::string& val);
    void setAttribute(const std::string& key, int val);
    void addEvent(const std::string& name);
    void setError(const std::string& msg);
    void end();
    explicit operator bool() const { return span_ && !ended_; }

    ~ScopedSpan();
    ScopedSpan(ScopedSpan&& o) noexcept;
    ScopedSpan& operator=(ScopedSpan&& o) noexcept;
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;

private:
    opentelemetry::nostd::shared_ptr<otel_trace::Span> span_;
    bool ended_ = false;
};

} // namespace aegisgate

#else // !AEGISGATE_ENABLE_OTEL

namespace aegisgate {

struct TracingConfig {
    bool enabled = false;
    std::string otlp_endpoint;
    std::string service_name = "aegisgate";
    double sample_ratio = 1.0;
    bool use_simple_processor = false;
};

class Tracing {
public:
    static Tracing& instance() {
        static Tracing t;
        return t;
    }
    void initialize(const TracingConfig&) {}
    void shutdown() {}
    bool isEnabled() const { return false; }
    void injectIfEnabled(
        std::unordered_map<std::string, std::string>&) {}
    static std::string currentTraceId() { return ""; }
    static std::string currentSpanId() { return ""; }

private:
    Tracing() = default;
};

class ScopedSpan {
public:
    ScopedSpan() = default;
    template<typename... Args>
    explicit ScopedSpan(Args&&...) {}
    void setAttribute(const std::string&, const std::string&) {}
    void setAttribute(const std::string&, int) {}
    void addEvent(const std::string&) {}
    void setError(const std::string&) {}
    void end() {}
    explicit operator bool() const { return false; }
};

} // namespace aegisgate

#endif // AEGISGATE_ENABLE_OTEL
