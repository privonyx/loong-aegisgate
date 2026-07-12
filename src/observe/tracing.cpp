#include "observe/tracing.h"

#ifdef AEGISGATE_ENABLE_OTEL

#if __has_include("version.h")
#include "version.h"
#endif
#ifndef AEGISGATE_VERSION
#define AEGISGATE_VERSION "0.0.0-dev"
#endif

#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/exporters/memory/in_memory_span_exporter_factory.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#include <opentelemetry/sdk/trace/samplers/always_on_factory.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/context/propagation/global_propagator.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <spdlog/spdlog.h>

namespace aegisgate {

namespace {

class HeaderCarrier
    : public opentelemetry::context::propagation::TextMapCarrier {
public:
    explicit HeaderCarrier(std::unordered_map<std::string, std::string>& headers)
        : headers_(headers) {}

    opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view key) const noexcept override {
        auto it = headers_.find(std::string(key));
        if (it != headers_.end()) {
            return opentelemetry::nostd::string_view(it->second);
        }
        return "";
    }

    void Set(opentelemetry::nostd::string_view key,
             opentelemetry::nostd::string_view value) noexcept override {
        headers_[std::string(key)] = std::string(value);
    }

private:
    std::unordered_map<std::string, std::string>& headers_;
};

} // namespace

Tracing& Tracing::instance() {
    static Tracing t;
    return t;
}

void Tracing::initialize(const TracingConfig& config) {
    if (!config.enabled) {
        enabled_ = false;
        spdlog::info("OpenTelemetry tracing disabled by configuration");
        return;
    }

    namespace otlp = opentelemetry::exporter::otlp;
    namespace sdk_trace = opentelemetry::sdk::trace;
    namespace resource = opentelemetry::sdk::resource;

    try {
        otlp::OtlpHttpExporterOptions exporter_opts;
        exporter_opts.url = config.otlp_endpoint + "/v1/traces";
        std::unique_ptr<sdk_trace::SpanProcessor> processor;
        if (config.use_simple_processor) {
            std::shared_ptr<
                opentelemetry::exporter::memory::InMemorySpanData>
                data;
            auto mem_exporter = opentelemetry::exporter::memory::
                InMemorySpanExporterFactory::Create(data);
            processor = sdk_trace::SimpleSpanProcessorFactory::Create(
                std::move(mem_exporter));
        } else {
            auto exporter =
                otlp::OtlpHttpExporterFactory::Create(exporter_opts);
            sdk_trace::BatchSpanProcessorOptions processor_opts;
            processor_opts.max_queue_size = 2048;
            processor_opts.schedule_delay_millis =
                std::chrono::milliseconds(5000);
            processor_opts.max_export_batch_size = 512;
            processor = sdk_trace::BatchSpanProcessorFactory::Create(
                std::move(exporter), processor_opts);
        }

        auto res = resource::Resource::Create({
            {"service.name", config.service_name},
            {"service.version", AEGISGATE_VERSION},
        });

        std::unique_ptr<sdk_trace::Sampler> sampler;
        if (config.sample_ratio < 1.0 && config.sample_ratio > 0.0) {
            sampler = std::make_unique<sdk_trace::TraceIdRatioBasedSampler>(
                config.sample_ratio);
        } else {
            sampler = sdk_trace::AlwaysOnSamplerFactory::Create();
        }

        auto provider = sdk_trace::TracerProviderFactory::Create(
            std::move(processor), res, std::move(sampler));

        opentelemetry::trace::Provider::SetTracerProvider(std::move(provider));

        opentelemetry::context::propagation::GlobalTextMapPropagator::
            SetGlobalPropagator(
                opentelemetry::nostd::shared_ptr<
                    opentelemetry::context::propagation::TextMapPropagator>(
                    new opentelemetry::trace::propagation::HttpTraceContext()));

        enabled_ = true;
        spdlog::info(
            "OpenTelemetry tracing initialized: endpoint={}, service={}, "
            "sample_ratio={}",
            config.otlp_endpoint, config.service_name, config.sample_ratio);

    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize OpenTelemetry: {}", e.what());
        enabled_ = false;
    }
}

void Tracing::shutdown() {
    if (!enabled_) return;

    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    if (provider) {
        auto* sdk_provider =
            dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(
                provider.get());
        if (sdk_provider) {
            sdk_provider->Shutdown();
        }
    }
    enabled_ = false;
    spdlog::info("OpenTelemetry tracing shut down");
}

opentelemetry::nostd::shared_ptr<otel_trace::Tracer> Tracing::tracer() {
    return opentelemetry::trace::Provider::GetTracerProvider()->GetTracer(
        "aegisgate", AEGISGATE_VERSION);
}

opentelemetry::context::Context Tracing::extractContext(
    const std::unordered_map<std::string, std::string>& headers) {
    auto mutable_headers = headers;
    HeaderCarrier carrier(mutable_headers);
    auto propagator = opentelemetry::context::propagation::
        GlobalTextMapPropagator::GetGlobalPropagator();
    auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    return propagator->Extract(carrier, ctx);
}

void Tracing::injectContext(
    const opentelemetry::context::Context& ctx,
    std::unordered_map<std::string, std::string>& headers) {
    HeaderCarrier carrier(headers);
    auto propagator = opentelemetry::context::propagation::
        GlobalTextMapPropagator::GetGlobalPropagator();
    propagator->Inject(carrier, ctx);
}

std::string Tracing::currentTraceId() {
    auto span = otel_trace::Tracer::GetCurrentSpan();
    if (!span || !span->GetContext().IsValid()) return "";
    char buf[32];
    span->GetContext().trace_id().ToLowerBase16(buf);
    return std::string(buf, 32);
}

std::string Tracing::currentSpanId() {
    auto span = otel_trace::Tracer::GetCurrentSpan();
    if (!span || !span->GetContext().IsValid()) return "";
    char buf[16];
    span->GetContext().span_id().ToLowerBase16(buf);
    return std::string(buf, 16);
}

void Tracing::injectIfEnabled(
    std::unordered_map<std::string, std::string>& headers) {
    if (!enabled_) return;
    auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    injectContext(ctx, headers);
}

// --- ScopedSpan implementation ---

ScopedSpan::ScopedSpan(const std::string& name,
    const opentelemetry::context::Context& parent_ctx,
    std::initializer_list<
        std::pair<opentelemetry::nostd::string_view,
                  opentelemetry::common::AttributeValue>> attrs) {
    if (!Tracing::instance().isEnabled()) return;
    auto tracer = Tracing::instance().tracer();
    otel_trace::StartSpanOptions opts;
    opts.parent = parent_ctx;
    span_ = tracer->StartSpan(name, attrs, opts);
}

void ScopedSpan::setAttribute(const std::string& key,
                              const std::string& val) {
    if (span_ && !ended_) span_->SetAttribute(key, val);
}

void ScopedSpan::setAttribute(const std::string& key, int val) {
    if (span_ && !ended_) span_->SetAttribute(key, val);
}

void ScopedSpan::addEvent(const std::string& name) {
    if (span_ && !ended_) span_->AddEvent(name);
}

void ScopedSpan::setError(const std::string& msg) {
    if (span_ && !ended_) {
        span_->SetStatus(otel_trace::StatusCode::kError, msg);
    }
}

void ScopedSpan::end() {
    if (span_ && !ended_) {
        span_->End();
        ended_ = true;
    }
}

ScopedSpan::~ScopedSpan() {
    end();
}

ScopedSpan::ScopedSpan(ScopedSpan&& o) noexcept
    : span_(std::move(o.span_)), ended_(o.ended_) {
    o.ended_ = true;
}

ScopedSpan& ScopedSpan::operator=(ScopedSpan&& o) noexcept {
    if (this != &o) {
        end();
        span_ = std::move(o.span_);
        ended_ = o.ended_;
        o.ended_ = true;
    }
    return *this;
}

} // namespace aegisgate

#endif // AEGISGATE_ENABLE_OTEL
