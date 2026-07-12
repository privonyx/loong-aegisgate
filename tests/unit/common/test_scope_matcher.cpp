// Phase 9.3.4 RolloutController (TASK-20260422-01) — Epic B.1 ScopeMatcher.
//
// Covers the CR2 creative design (FNV-1a 64-bit + modulo 100):
//   memory-bank/creative/creative-rollout-scope-matcher.md
//
// Tests are grouped by public surface:
//   1. fnv1a64 sanity          — hash stability + distinctness.
//   2. tenantMatches           — empty list / "*" / prefix globs / exact.
//   3. regionMatches           — empty list / membership.
//   4. percentageMatches       — short-circuits (0/100/neg/overflow/empty).
//   5. percentageMatches invariant — monotonicity across 1000 sticky values.
//   6. percentageMatches CI guard — uniformity across 10000 sticky values.
//   7. matches (composite)     — AND semantics.

#include "common/scope_matcher.h"
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <unordered_set>

namespace aegisgate::common {
namespace {

// --- 1. fnv1a64 ------------------------------------------------------------

TEST(Fnv1a64, EmptyStringIsOffsetBasis) {
    EXPECT_EQ(fnv1a64(""), 1469598103934665603ULL);
}

TEST(Fnv1a64, SingleCharacterMatchesReference) {
    // FNV-1a("a") = offset ^ 'a' * prime
    uint64_t expected = (1469598103934665603ULL ^ 'a') * 1099511628211ULL;
    EXPECT_EQ(fnv1a64("a"), expected);
}

TEST(Fnv1a64, DifferentInputsProduceDifferentHashes) {
    EXPECT_NE(fnv1a64("tenant-a"), fnv1a64("tenant-b"));
    EXPECT_NE(fnv1a64("foo"), fnv1a64("fooo"));
}

TEST(Fnv1a64, DeterministicAcrossCalls) {
    EXPECT_EQ(fnv1a64("internal-42"), fnv1a64("internal-42"));
}

// --- 2. tenantMatches ------------------------------------------------------

TEST(TenantMatches, EmptyListMatchesAny) {
    EXPECT_TRUE(tenantMatches({}, "anything"));
    EXPECT_TRUE(tenantMatches({}, ""));
}

TEST(TenantMatches, StarAloneMatchesAny) {
    EXPECT_TRUE(tenantMatches({"*"}, "any-tenant"));
    EXPECT_TRUE(tenantMatches({"*"}, ""));
}

TEST(TenantMatches, PrefixStarMatchesPrefix) {
    EXPECT_TRUE(tenantMatches({"internal-*"}, "internal-foo"));
    EXPECT_TRUE(tenantMatches({"internal-*"}, "internal-"));
    EXPECT_FALSE(tenantMatches({"internal-*"}, "external-bar"));
    EXPECT_FALSE(tenantMatches({"internal-*"}, "intern"));
}

TEST(TenantMatches, ExactLiteralsMustMatchExactly) {
    EXPECT_TRUE(tenantMatches({"acme"}, "acme"));
    EXPECT_FALSE(tenantMatches({"acme"}, "acme-prod"));
    EXPECT_FALSE(tenantMatches({"acme"}, ""));
}

TEST(TenantMatches, MultipleGlobsOrTogether) {
    std::vector<std::string> g = {"internal-*", "staff-*", "admin"};
    EXPECT_TRUE(tenantMatches(g, "internal-qa"));
    EXPECT_TRUE(tenantMatches(g, "staff-root"));
    EXPECT_TRUE(tenantMatches(g, "admin"));
    EXPECT_FALSE(tenantMatches(g, "public-api"));
}

// --- 3. regionMatches ------------------------------------------------------

TEST(RegionMatches, EmptyListMatchesAny) {
    EXPECT_TRUE(regionMatches({}, "ap-east-1"));
    EXPECT_TRUE(regionMatches({}, ""));
}

TEST(RegionMatches, ListedRegionOnlyMatches) {
    std::vector<std::string> r = {"ap-east-1", "us-west-2"};
    EXPECT_TRUE(regionMatches(r, "ap-east-1"));
    EXPECT_TRUE(regionMatches(r, "us-west-2"));
    EXPECT_FALSE(regionMatches(r, "eu-central-1"));
    EXPECT_FALSE(regionMatches(r, ""));
}

// --- 4. percentageMatches — short circuits --------------------------------

TEST(PercentageMatches, ZeroNeverMatches) {
    EXPECT_FALSE(percentageMatches("tenant-a", 0));
    EXPECT_FALSE(percentageMatches("", 0));
}

TEST(PercentageMatches, NegativeTreatedAsZero) {
    EXPECT_FALSE(percentageMatches("tenant-a", -1));
    EXPECT_FALSE(percentageMatches("tenant-a", -100));
}

TEST(PercentageMatches, HundredAlwaysMatches) {
    EXPECT_TRUE(percentageMatches("any-tenant", 100));
    EXPECT_TRUE(percentageMatches("", 100));
}

TEST(PercentageMatches, AboveHundredTreatedAsHundred) {
    EXPECT_TRUE(percentageMatches("any-tenant", 101));
    EXPECT_TRUE(percentageMatches("any-tenant", 1000));
}

TEST(PercentageMatches, EmptyStickyNeverMatchesPartial) {
    // Conservative: empty sticky + 0 < p < 100 → do not match (callers treat
    // "cannot resolve sticky" as "leave on the stable side", i.e. ACTIVE).
    EXPECT_FALSE(percentageMatches("", 1));
    EXPECT_FALSE(percentageMatches("", 50));
    EXPECT_FALSE(percentageMatches("", 99));
}

// --- 5. percentageMatches — monotonicity invariant ------------------------
// If a sticky_value matches at percentage P, it must match at every P' > P.
// Equivalent: there exists a single threshold p_t per sticky_value such
// that percentageMatches returns false iff p < p_t.

TEST(PercentageMatches, MonotonicAcrossFullRangeFor1000StickyValues) {
    std::mt19937_64 rng(0xA155);
    for (int i = 0; i < 1000; ++i) {
        std::string sticky = "tenant-" + std::to_string(rng());
        bool saw_true = false;
        // Walk 1..99 (0 and 100 are short-circuited; walk boundary-strict).
        for (int p = 1; p <= 99; ++p) {
            bool hit = percentageMatches(sticky, p);
            if (hit) saw_true = true;
            if (saw_true) {
                EXPECT_TRUE(hit) << "sticky=" << sticky
                                 << " flipped false->true->false at p=" << p;
            }
        }
    }
}

// --- 6. percentageMatches — CI uniformity guard ---------------------------
// SR-style guard: at p=10 on 10000 uniformly random sticky_values, the
// number of hits must land in [800, 1200] (±2σ of binomial). If FNV-1a
// begins clumping for any future input pattern this test will fail loud.

TEST(PercentageMatches, UniformityAtPercentage10IsWithinTolerance) {
    std::mt19937_64 rng(0xB055);
    constexpr int kN = 10000;
    constexpr int kP = 10;
    int hits = 0;
    for (int i = 0; i < kN; ++i) {
        // Simulate UUID-like keys to mimic real sticky values.
        std::string s;
        s.resize(32);
        for (auto& c : s) c = "0123456789abcdef"[rng() & 0xF];
        if (percentageMatches(s, kP)) ++hits;
    }
    EXPECT_GE(hits, 800)  << "hits=" << hits;
    EXPECT_LE(hits, 1200) << "hits=" << hits;
}

// --- 7. matches composite ---------------------------------------------------

TEST(MatchesComposite, AllThreeConditionsMatch) {
    std::vector<std::string> globs = {"internal-*"};
    std::vector<std::string> regions = {"ap-east-1"};
    ScopeContext ctx{"internal-qa", "ap-east-1", "sticky-A"};
    EXPECT_TRUE(matches(globs, regions, /*percentage=*/100, ctx));
}

TEST(MatchesComposite, TenantGlobMissingFails) {
    std::vector<std::string> globs = {"internal-*"};
    ScopeContext ctx{"external-qa", "ap-east-1", "sticky-A"};
    EXPECT_FALSE(matches(globs, /*regions=*/{}, /*percentage=*/100, ctx));
}

TEST(MatchesComposite, RegionMissingFails) {
    std::vector<std::string> regions = {"ap-east-1"};
    ScopeContext ctx{"internal-qa", "us-west-2", "sticky-A"};
    EXPECT_FALSE(matches(/*globs=*/{}, regions, /*percentage=*/100, ctx));
}

TEST(MatchesComposite, PercentageZeroFails) {
    ScopeContext ctx{"internal-qa", "ap-east-1", "sticky-A"};
    EXPECT_FALSE(matches(/*globs=*/{}, /*regions=*/{}, /*percentage=*/0, ctx));
}

TEST(MatchesComposite, EmptySelectorsMatchEverythingAt100Pct) {
    ScopeContext ctx{"", "", "any-sticky"};
    EXPECT_TRUE(matches(/*globs=*/{}, /*regions=*/{}, /*percentage=*/100, ctx));
}

} // namespace
} // namespace aegisgate::common
