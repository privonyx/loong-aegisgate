#include <gtest/gtest.h>
#include "observe/tracing.h"

using namespace aegisgate;

TEST(TracingTest, SingletonExists) {
    auto& t = Tracing::instance();
    (void)t;
}

TEST(TracingTest, DisabledByDefault) {
    EXPECT_FALSE(Tracing::instance().isEnabled());
}

TEST(TracingTest, InitShutdownWhenDisabled) {
    TracingConfig cfg;
    cfg.enabled = false;
    Tracing::instance().initialize(cfg);
    EXPECT_FALSE(Tracing::instance().isEnabled());
    Tracing::instance().shutdown();
}

TEST(TracingTest, CurrentIdsEmptyWhenDisabled) {
    EXPECT_TRUE(Tracing::currentTraceId().empty());
    EXPECT_TRUE(Tracing::currentSpanId().empty());
}

TEST(TracingTest, ScopedSpanNoOpWhenDisabled) {
    ScopedSpan span;
    EXPECT_FALSE(static_cast<bool>(span));
    span.setAttribute("key", "value");
    span.setAttribute("key", 42);
    span.addEvent("event");
    span.setError("error");
    span.end();
}

#ifdef AEGISGATE_ENABLE_OTEL
// Tests use SimpleSpanProcessor to avoid BatchSpanProcessor segfault
// (opentelemetry-cpp #3071, #3394).
TEST(TracingOtelTest, EnableWithConfig) {
    TracingConfig cfg;
    cfg.enabled = true;
    cfg.otlp_endpoint = "http://localhost:4318";
    cfg.service_name = "test-aegisgate";
    cfg.sample_ratio = 1.0;
    cfg.use_simple_processor = true;

    auto& t = Tracing::instance();
    t.initialize(cfg);
    EXPECT_TRUE(t.isEnabled());

    auto tracer = t.tracer();
    ASSERT_NE(tracer, nullptr);

    auto span = tracer->StartSpan("test-span");
    ASSERT_NE(span, nullptr);
    EXPECT_TRUE(span->GetContext().IsValid());
    span->End();

    t.shutdown();
    EXPECT_FALSE(t.isEnabled());
}

TEST(TracingOtelTest, SpanAttributesAndStatus) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    auto tracer = t.tracer();
    auto span = tracer->StartSpan("test-attrs");
    span->SetAttribute("aegisgate.model", "gpt-4");
    span->SetAttribute("aegisgate.tokens", 42);
    span->SetStatus(otel_trace::StatusCode::kOk);
    span->End();

    t.shutdown();
}

TEST(TracingOtelTest, CurrentTraceIdAndSpanId) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    auto tracer = t.tracer();
    auto span = tracer->StartSpan("test-ids");
    auto scope = otel_trace::Tracer::WithActiveSpan(span);

    auto trace_id = Tracing::currentTraceId();
    auto span_id = Tracing::currentSpanId();
    EXPECT_EQ(trace_id.size(), 32u);
    EXPECT_EQ(span_id.size(), 16u);
    EXPECT_NE(trace_id, "00000000000000000000000000000000");

    span->End();
    t.shutdown();
}

TEST(TracingOtelTest, ContextExtractAndInject) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    std::unordered_map<std::string, std::string> in_headers;
    in_headers["traceparent"] =
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

    auto ctx = t.extractContext(in_headers);

    auto tracer = t.tracer();
    otel_trace::StartSpanOptions opts;
    opts.parent = ctx;
    auto span = tracer->StartSpan("child-span", {}, opts);
    EXPECT_TRUE(span->GetContext().IsValid());

    char trace_buf[32];
    span->GetContext().trace_id().ToLowerBase16(trace_buf);
    EXPECT_EQ(std::string(trace_buf, 32), "4bf92f3577b34da6a3ce929d0e0e4736");

    auto scope = otel_trace::Tracer::WithActiveSpan(span);
    auto active_ctx = opentelemetry::context::RuntimeContext::GetCurrent();

    std::unordered_map<std::string, std::string> out_headers;
    t.injectContext(active_ctx, out_headers);
    EXPECT_FALSE(out_headers["traceparent"].empty());
    EXPECT_NE(out_headers["traceparent"].find(
                  "4bf92f3577b34da6a3ce929d0e0e4736"),
              std::string::npos);

    span->End();
    t.shutdown();
}

TEST(TracingOtelTest, ScopedSpanCreateAndAutoEnd) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    {
        auto tracer = t.tracer();
        auto parent = tracer->StartSpan("parent");
        auto scope = otel_trace::Tracer::WithActiveSpan(parent);
        auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();

        ScopedSpan child("test.child", ctx, {{"key", "val"}});
        EXPECT_TRUE(static_cast<bool>(child));
        child.setAttribute("model", "gpt-4");
        child.setAttribute("tokens", 42);
        child.addEvent("checkpoint");

        parent->End();
    }
    t.shutdown();
}

TEST(TracingOtelTest, ScopedSpanMoveSemantics) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    auto tracer = t.tracer();
    auto parent = tracer->StartSpan("parent");
    auto scope = otel_trace::Tracer::WithActiveSpan(parent);
    auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();

    ScopedSpan s1("test.move", ctx);
    EXPECT_TRUE(static_cast<bool>(s1));

    ScopedSpan s2 = std::move(s1);
    EXPECT_FALSE(static_cast<bool>(s1));
    EXPECT_TRUE(static_cast<bool>(s2));

    s2.end();
    EXPECT_FALSE(static_cast<bool>(s2));

    parent->End();
    t.shutdown();
}

TEST(TracingOtelTest, ScopedSpanExplicitEnd) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    auto tracer = t.tracer();
    auto parent = tracer->StartSpan("parent");
    auto scope = otel_trace::Tracer::WithActiveSpan(parent);
    auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();

    ScopedSpan s("test.explicit", ctx);
    s.setError("something failed");
    s.end();
    EXPECT_FALSE(static_cast<bool>(s));
    s.end();

    parent->End();
    t.shutdown();
}

TEST(TracingOtelTest, InjectIfEnabled) {
    TracingConfig cfg{true, "http://localhost:4318", "test", 1.0};
    cfg.use_simple_processor = true;
    auto& t = Tracing::instance();
    t.initialize(cfg);

    auto tracer = t.tracer();
    auto span = tracer->StartSpan("test-inject");
    auto scope = otel_trace::Tracer::WithActiveSpan(span);

    std::unordered_map<std::string, std::string> headers;
    t.injectIfEnabled(headers);
    EXPECT_FALSE(headers["traceparent"].empty());

    span->End();
    t.shutdown();
}

TEST(TracingOtelTest, InitFailsGracefullyOnBadEndpoint) {
    TracingConfig cfg;
    cfg.enabled = true;
    cfg.otlp_endpoint = "http://definitely-not-a-real-host:99999";
    cfg.service_name = "test";
    cfg.use_simple_processor = true;

    auto& t = Tracing::instance();
    t.initialize(cfg);
    EXPECT_TRUE(t.isEnabled());

    auto tracer = t.tracer();
    auto span = tracer->StartSpan("should-not-crash");
    span->End();

    t.shutdown();
}

#endif // AEGISGATE_ENABLE_OTEL
