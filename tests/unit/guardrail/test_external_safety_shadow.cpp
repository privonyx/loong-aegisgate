#include <gtest/gtest.h>
#include "guardrail/inbound/external_safety_stage.h"
#include "guardrail/inbound/external_safety_api.h"
#include "guardrail/audit.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace aegisgate;

namespace {

// Mock that records call count and can simulate latency, success, flagged.
class MockSafetyApi : public ExternalSafetyApi {
public:
    MockSafetyApi(std::string name, bool flagged, bool success = true,
                  std::chrono::milliseconds latency = std::chrono::milliseconds{0})
        : name_(std::move(name)), flagged_(flagged), success_(success),
          latency_(latency) {}

    SafetyResult check(const std::string& /*text*/) override {
        if (latency_.count() > 0) std::this_thread::sleep_for(latency_);
        ++check_count_;
        SafetyResult r;
        r.provider = name_;
        r.success = success_;
        if (!success_) r.error = "mock error";
        r.flagged = flagged_;
        return r;
    }
    std::string providerName() const override { return name_; }
    bool isConfigured() const override { return true; }
    int checkCount() const { return check_count_.load(); }

private:
    std::string name_;
    bool flagged_;
    bool success_;
    std::chrono::milliseconds latency_;
    std::atomic<int> check_count_{0};
};

class ExplodingSafetyApi : public ExternalSafetyApi {
public:
    SafetyResult check(const std::string& /*text*/) override {
        throw std::runtime_error("simulated provider crash");
    }
    std::string providerName() const override { return "boom"; }
    bool isConfigured() const override { return true; }
};

RequestContext makeCtx(const std::string& text) {
    RequestContext ctx;
    ctx.request_id = "req-shadow";
    ctx.tenant_id = "tenant-x";
    ctx.chat_request.messages = {{"user", text}};
    return ctx;
}

}  // namespace

// 1: shadow_mode=false preserves legacy synchronous Reject behavior.
TEST(ExternalSafetyShadow, ShadowDisabledKeepsSyncBlocking) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = false;
    cfg.mode = ExternalSafetyMode::Any;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", /*flagged=*/true));

    auto ctx = makeCtx("hello");
    auto r = stage.process(ctx);
    EXPECT_EQ(r, StageResult::Reject);
    EXPECT_TRUE(ctx.external_safety_flagged);
    EXPECT_EQ(stage.shadowDispatched(), 0u);
}

// 2: shadow_mode=true returns Continue even if the provider would have flagged.
TEST(ExternalSafetyShadow, ShadowReturnsContinueImmediately) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", /*flagged=*/true,
                                                       /*success=*/true,
                                                       std::chrono::milliseconds{30}));

    auto ctx = makeCtx("hello");
    auto start = std::chrono::steady_clock::now();
    auto r = stage.process(ctx);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(r, StageResult::Continue);
    EXPECT_FALSE(ctx.external_safety_flagged);
    EXPECT_LT(elapsed, std::chrono::milliseconds{20})
        << "shadow path must not block on provider latency";
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{500}));
    EXPECT_EQ(stage.shadowDispatched(), 1u);
}

// 3: SR3 — shadow worker writes an audit entry tagged shadow=true.
TEST(ExternalSafetyShadow, Sr3ShadowWritesAuditTaggedShadow) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", false));
    AuditLogger audit;
    stage.setAuditLogger(&audit);

    auto ctx = makeCtx("hello");
    stage.process(ctx);
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{500}));
    audit.flush(std::chrono::seconds{2});

    auto entries = audit.entries();
    ASSERT_FALSE(entries.empty());
    bool found_shadow = false;
    for (const auto& e : entries) {
        if (e.detail.find("shadow=true") != std::string::npos) {
            found_shadow = true;
            EXPECT_EQ(e.action, "external_safety_shadow");
            EXPECT_EQ(e.tenant_id, "tenant-x");
        }
    }
    EXPECT_TRUE(found_shadow) << "no audit entry tagged shadow=true";
    EXPECT_EQ(stage.shadowAuditWrites(), 1u);
}

// 4: SR6 — inflight cap honored; excess dispatches are skipped with a counter.
TEST(ExternalSafetyShadow, Sr6InflightCapSkipsOverflow) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    cfg.shadow_max_inflight = 2;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>(
        "p1", false, true, std::chrono::milliseconds{100}));

    for (int i = 0; i < 6; ++i) {
        auto ctx = makeCtx("burst-" + std::to_string(i));
        stage.process(ctx);
    }
    EXPECT_GE(stage.shadowSkipped(), 1u);
    EXPECT_LE(stage.shadowDispatched(), 6u);
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{2000}));
}

// 5: shadow worker exception is swallowed; pipeline never sees Reject.
TEST(ExternalSafetyShadow, ShadowWorkerSwallowsProviderException) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<ExplodingSafetyApi>());

    auto ctx = makeCtx("hello");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{500}));
}

// 6: audit_logger == nullptr is tolerated — provider still runs.
TEST(ExternalSafetyShadow, NullAuditLoggerDoesNotCrash) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    ExternalSafetyStage stage(cfg);
    auto* mock = new MockSafetyApi("p1", false);
    stage.addProvider(std::unique_ptr<ExternalSafetyApi>(mock));

    auto ctx = makeCtx("hello");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{500}));
    EXPECT_GE(mock->checkCount(), 1);
    EXPECT_EQ(stage.shadowAuditWrites(), 0u);
}

// 7: shadow_max_inflight is honored as configured.
TEST(ExternalSafetyShadow, ConfiguredMaxInflightHonored) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    cfg.shadow_max_inflight = 0;  // forbid any dispatch
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", false));

    auto ctx = makeCtx("hello");
    EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    EXPECT_EQ(stage.shadowDispatched(), 0u);
    EXPECT_EQ(stage.shadowSkipped(), 1u);
}

// 8: process() in shadow mode is hot-path fast even with a slow provider.
TEST(ExternalSafetyShadow, ProcessLatencyBoundedDespiteSlowProvider) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>(
        "p1", false, true, std::chrono::milliseconds{150}));

    auto ctx = makeCtx("hello");
    auto t0 = std::chrono::steady_clock::now();
    stage.process(ctx);
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(dt, std::chrono::milliseconds{30});
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{1000}));
}

// 9: shadow_audit_ttl is exposed via config (forward-compat field).
TEST(ExternalSafetyShadow, ShadowAuditTtlConfigPresent) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    cfg.shadow_audit_ttl = std::chrono::seconds{3600};
    ExternalSafetyStage stage(cfg);
    EXPECT_EQ(cfg.shadow_audit_ttl.count(), 3600);
    EXPECT_EQ(stage.shadowDispatched(), 0u);
}

// 10: MUTATION — flip SR3 audit write off and assert tests scream.
// We emulate the regression by NOT wiring the audit logger, then asserting
// shadow_audit_writes == 0; if the production code ever bypasses the
// nullptr guard and writes anyway, this expectation flips.
TEST(ExternalSafetyShadow, MutationGuardAuditOnlyWhenLoggerSet) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    ExternalSafetyStage stage(cfg);  // no audit logger set
    stage.addProvider(std::make_unique<MockSafetyApi>("p1", false));

    auto ctx = makeCtx("hello");
    stage.process(ctx);
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::milliseconds{500}));
    EXPECT_EQ(stage.shadowAuditWrites(), 0u)
        << "audit writes must not increment when logger is nullptr";
}

// ============================================================================
// Epic 4.3 — Integration: shadow path must NOT block the hot path even when
// every provider is intentionally slow. These tests assert the strong
// fire-and-forget contract the runtime relies on (otherwise enabling
// shadow_mode in production would silently 10x request latency).
// ============================================================================

// 4.3-A: a SINGLE provider that sleeps 500 ms must not stall process() above
// the 10 ms ceiling specified in plan §7 task 4.3.
TEST(ExternalSafetyShadowIntegration, SlowProviderStaysUnderTenMs) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    cfg.shadow_max_inflight = 4;  // headroom so dispatch is not skipped
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>(
        "slow-p1", /*flagged=*/true, /*success=*/true,
        std::chrono::milliseconds{500}));
    AuditLogger audit;
    stage.setAuditLogger(&audit);

    auto ctx = makeCtx("integration-slow-single");
    auto t0 = std::chrono::steady_clock::now();
    auto r = stage.process(ctx);
    auto dt = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(r, StageResult::Continue);
    EXPECT_FALSE(ctx.external_safety_flagged)
        << "shadow mode must never set the flagged bit on the hot path";
    EXPECT_LT(dt, std::chrono::milliseconds{10})
        << "shadow process() exceeded plan §7 task 4.3 ceiling (10 ms)";

    // The audit + provider work is allowed to finish lazily.
    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::seconds{2}));
    EXPECT_EQ(stage.shadowDispatched(), 1u);
}

// 4.3-B: a BURST of 5 sequential requests against a 500 ms provider must
// stay under 50 ms total — proves dispatch is genuinely async, not a sync
// loop hidden behind a future. If shadow ever regresses to blocking, this
// test would show ~2.5 s wall-clock time.
TEST(ExternalSafetyShadowIntegration, BurstStaysFireAndForget) {
    ExternalSafetyStageConfig cfg;
    cfg.shadow_mode = true;
    cfg.shadow_max_inflight = 16;
    ExternalSafetyStage stage(cfg);
    stage.addProvider(std::make_unique<MockSafetyApi>(
        "slow-burst", /*flagged=*/false, /*success=*/true,
        std::chrono::milliseconds{500}));

    constexpr int kBurst = 5;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kBurst; ++i) {
        auto ctx = makeCtx("integration-burst-" + std::to_string(i));
        EXPECT_EQ(stage.process(ctx), StageResult::Continue);
    }
    auto dt = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(dt, std::chrono::milliseconds{50})
        << "5 shadow dispatches against a 500 ms provider must total < 50 ms";

    ASSERT_TRUE(stage.waitForShadowDrain(std::chrono::seconds{3}));
    EXPECT_EQ(stage.shadowDispatched(), static_cast<size_t>(kBurst));
    EXPECT_EQ(stage.shadowSkipped(), 0u);
}
