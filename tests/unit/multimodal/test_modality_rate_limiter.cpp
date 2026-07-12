#include "multimodal/modality_rate_limiter.h"
#include <gtest/gtest.h>

using namespace aegisgate;

TEST(ModalityRateLimiterTest, MakeKeyFormatStable) {
    EXPECT_EQ(ModalityRateLimiter::makeKey(Modality::Embedding, "sk-x"),
              "modality:embedding:sk-x");
    EXPECT_EQ(ModalityRateLimiter::makeKey(Modality::ImageGen, "tenant-1"),
              "modality:image_gen:tenant-1");
}

TEST(ModalityRateLimiterTest, WithoutQuotaFallsBackToBacking) {
    RateLimiter::Config global{10.0, 10.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);
    EXPECT_FALSE(ml.hasQuota(Modality::Embedding));
    EXPECT_EQ(ml.configuredQuotaCount(), 0u);
    // 10 tokens available globally, ten allows must pass.
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(ml.allow(Modality::Embedding, "sk", 1.0)) << "i=" << i;
    }
}

TEST(ModalityRateLimiterTest, SetQuotaTakesEffect) {
    RateLimiter::Config global{1000.0, 1000.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);

    // Embedding gets a tight 3-token bucket, no refill.
    ml.setQuota(Modality::Embedding, {3.0, 0.0});
    EXPECT_TRUE(ml.hasQuota(Modality::Embedding));

    EXPECT_TRUE(ml.allow(Modality::Embedding, "sk-1", 1.0));
    EXPECT_TRUE(ml.allow(Modality::Embedding, "sk-1", 1.0));
    EXPECT_TRUE(ml.allow(Modality::Embedding, "sk-1", 1.0));
    EXPECT_FALSE(ml.allow(Modality::Embedding, "sk-1", 1.0));
}

TEST(ModalityRateLimiterTest, IsolationAcrossIdentities) {
    RateLimiter::Config global{1000.0, 1000.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);

    ml.setQuota(Modality::Embedding, {2.0, 0.0});
    EXPECT_TRUE(ml.allow(Modality::Embedding, "tenant-a"));
    EXPECT_TRUE(ml.allow(Modality::Embedding, "tenant-a"));
    EXPECT_FALSE(ml.allow(Modality::Embedding, "tenant-a"));
    // tenant-b has its own bucket
    EXPECT_TRUE(ml.allow(Modality::Embedding, "tenant-b"));
    EXPECT_TRUE(ml.allow(Modality::Embedding, "tenant-b"));
    EXPECT_FALSE(ml.allow(Modality::Embedding, "tenant-b"));
}

TEST(ModalityRateLimiterTest, IsolationAcrossModalities) {
    RateLimiter::Config global{1000.0, 1000.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);

    ml.setQuota(Modality::Embedding, {2.0, 0.0});
    ml.setQuota(Modality::ImageGen, {5.0, 0.0});
    // exhaust embedding
    EXPECT_TRUE(ml.allow(Modality::Embedding, "sk"));
    EXPECT_TRUE(ml.allow(Modality::Embedding, "sk"));
    EXPECT_FALSE(ml.allow(Modality::Embedding, "sk"));
    // image-gen has its own 5-token bucket
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(ml.allow(Modality::ImageGen, "sk"));
    EXPECT_FALSE(ml.allow(Modality::ImageGen, "sk"));
}

TEST(ModalityRateLimiterTest, ClearQuotaResetsToGlobal) {
    RateLimiter::Config global{1000.0, 0.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);

    ml.setQuota(Modality::Embedding, {2.0, 0.0});
    EXPECT_TRUE(ml.hasQuota(Modality::Embedding));
    ml.clearQuota(Modality::Embedding);
    EXPECT_FALSE(ml.hasQuota(Modality::Embedding));
}

// Mutation Test (D7+P1#1): if the keying scheme ever forgets to prefix
// with modality, two different modalities would share the same bucket
// for the same identity. This test enforces non-collision.
TEST(ModalityRateLimiterTest, MutationGuard_BucketsKeyedByModality) {
    RateLimiter::Config global{1000.0, 1000.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);
    ml.setQuota(Modality::Embedding, {1.0, 0.0});
    ml.setQuota(Modality::ImageGen, {1.0, 0.0});
    EXPECT_TRUE(ml.allow(Modality::Embedding, "x"));
    // If keys collided, the next allow would FAIL because Embedding's
    // bucket would already be empty.
    EXPECT_TRUE(ml.allow(Modality::ImageGen, "x"))
        << "buckets must be keyed by (modality, identity), not identity alone";
}

TEST(ModalityRateLimiterTest, ConfiguredQuotaCountReflectsSet) {
    RateLimiter::Config global{1000.0, 1000.0};
    RateLimiter backing(global);
    ModalityRateLimiter ml(backing);
    EXPECT_EQ(ml.configuredQuotaCount(), 0u);
    ml.setQuota(Modality::Embedding, {1.0, 0.0});
    ml.setQuota(Modality::Moderation, {1.0, 0.0});
    EXPECT_EQ(ml.configuredQuotaCount(), 2u);
    ml.clearQuota(Modality::Moderation);
    EXPECT_EQ(ml.configuredQuotaCount(), 1u);
}
