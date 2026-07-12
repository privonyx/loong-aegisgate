#include <gtest/gtest.h>

#include "aegisgate/error_codes.h"
#include "gateway/rate_limiter.h"
#include "multimodal/modality_rate_limiter.h"
#include "observe/metrics.h"
#include "server/modality_quota_enforcer.h"

#include <memory>

using namespace aegisgate;

namespace {

// Phase 6.1 Epic 5.1c (B1, TASK-20260515-01).
//
// Tests target the pure helper enforceModalityQuota() so we avoid spinning
// up Drogon's event loop. The helper is what processProxyRequest invokes
// BEFORE upstream dispatch.
//
// Spec mapping:
//   SR-NEW3 (modality enforcement integrity)  -> AllowsAndBlocks*, ReturnsCorrectErrorCode
//   SR-NEW4 (fail-open semantics)             -> FailOpen* tests
//   D3 decision (metrics counter)             -> IncrementsCounter
//
// Mutation testing (Epic 2.3) re-uses these tests:
//   M1 skip enforcement       -> BlocksWhenQuotaExhausted FAIL
//   M2 wrong status code      -> ReturnsCorrectErrorCode FAIL
//   M3 skip metrics           -> IncrementsCounter FAIL
//   M4 fail-closed regression -> FailOpenWhenLimiterNull FAIL

class ModalityQuotaEnforcerTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().resetAll();
        backing_ = std::make_unique<RateLimiter>(
            RateLimiter::Config{1000.0, 100.0});  // wide global default
        limiter_ = std::make_unique<ModalityRateLimiter>(*backing_);
    }

    std::unique_ptr<RateLimiter> backing_;
    std::unique_ptr<ModalityRateLimiter> limiter_;
};

// --- Happy path ---

TEST_F(ModalityQuotaEnforcerTest, AllowsWhenWithinQuota) {
    // Set a permissive quota: 10 tokens, refill 10/s.
    limiter_->setQuota(Modality::ImageGen, RateLimiter::Config{10.0, 10.0});
    auto err = enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                     limiter_.get(),
                                     MetricsRegistry::instance());
    EXPECT_FALSE(err.has_value());
}

TEST_F(ModalityQuotaEnforcerTest, BlocksWhenQuotaExhausted) {
    // Tight quota: 2 tokens, no refill within test window.
    limiter_->setQuota(Modality::ImageGen, RateLimiter::Config{2.0, 0.001});
    // First two calls succeed.
    EXPECT_FALSE(enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                       limiter_.get(),
                                       MetricsRegistry::instance()));
    EXPECT_FALSE(enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                       limiter_.get(),
                                       MetricsRegistry::instance()));
    // Third must be blocked.
    auto err = enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                     limiter_.get(),
                                     MetricsRegistry::instance());
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->http_status, 429);
}

TEST_F(ModalityQuotaEnforcerTest, ReturnsCorrectErrorCode) {
    limiter_->setQuota(Modality::ImageGen, RateLimiter::Config{1.0, 0.001});
    enforceModalityQuota(Modality::ImageGen, "tenant-a", limiter_.get(),
                         MetricsRegistry::instance());
    auto err = enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                     limiter_.get(),
                                     MetricsRegistry::instance());
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->http_status, 429);
    EXPECT_EQ(err->error_code, toAegisCode(ErrorCode::ModalityQuotaExceeded));
    EXPECT_STREQ(err->error_type.c_str(),
                 toErrorType(ErrorCode::ModalityQuotaExceeded));
    EXPECT_FALSE(err->message.empty());
}

// --- Metrics counter (D3) ---

TEST_F(ModalityQuotaEnforcerTest, IncrementsCounter) {
    limiter_->setQuota(Modality::AudioSpeech, RateLimiter::Config{1.0, 0.001});
    LabelSet label;
    label.labels = {{"modality", "audio_speech"}};
    auto& counter = MetricsRegistry::instance().modalityRateLimitedTotal();
    double before = counter.get(label);

    enforceModalityQuota(Modality::AudioSpeech, "x", limiter_.get(),
                         MetricsRegistry::instance());
    enforceModalityQuota(Modality::AudioSpeech, "x", limiter_.get(),
                         MetricsRegistry::instance());

    double after = counter.get(label);
    EXPECT_GE(after - before, 1.0);
}

// --- Per-tenant isolation (defense-in-depth on top of RateLimiter sharding) ---

TEST_F(ModalityQuotaEnforcerTest, TenantsHaveIsolatedBuckets) {
    limiter_->setQuota(Modality::ImageGen, RateLimiter::Config{1.0, 0.001});
    EXPECT_FALSE(enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                       limiter_.get(),
                                       MetricsRegistry::instance()));
    EXPECT_TRUE(enforceModalityQuota(Modality::ImageGen, "tenant-a",
                                      limiter_.get(),
                                      MetricsRegistry::instance())
                    .has_value());
    // Different tenant must still be allowed.
    EXPECT_FALSE(enforceModalityQuota(Modality::ImageGen, "tenant-b",
                                       limiter_.get(),
                                       MetricsRegistry::instance()));
}

// --- Fail-open semantics (SR-NEW4) ---

TEST_F(ModalityQuotaEnforcerTest, FailOpenWhenLimiterNull) {
    auto err = enforceModalityQuota(Modality::ImageGen, "anyone",
                                     /*limiter=*/nullptr,
                                     MetricsRegistry::instance());
    EXPECT_FALSE(err.has_value());
}

TEST_F(ModalityQuotaEnforcerTest, FailOpenWhenNoQuotaForModality) {
    // limiter exists but Embedding has no quota configured.
    limiter_->setQuota(Modality::ImageGen, RateLimiter::Config{1.0, 0.001});
    auto err = enforceModalityQuota(Modality::Embedding, "tenant-a",
                                     limiter_.get(),
                                     MetricsRegistry::instance());
    EXPECT_FALSE(err.has_value());
}

TEST_F(ModalityQuotaEnforcerTest, FailOpenWhenModalityUnknown) {
    limiter_->setQuota(Modality::ImageGen, RateLimiter::Config{1.0, 0.001});
    auto err = enforceModalityQuota(Modality::Unknown, "tenant-a",
                                     limiter_.get(),
                                     MetricsRegistry::instance());
    EXPECT_FALSE(err.has_value());
}

}  // namespace
