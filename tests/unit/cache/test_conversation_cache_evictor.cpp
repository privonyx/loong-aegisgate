#include "cache/conversation_cache_evictor.h"
#include <gtest/gtest.h>

using namespace aegisgate;
using namespace std::chrono_literals;

namespace {
ConversationEntryStats make(std::string key, uint64_t hits,
                            std::chrono::steady_clock::time_point last,
                            double savings,
                            std::chrono::steady_clock::time_point created) {
    ConversationEntryStats s;
    s.partition_key = std::move(key);
    s.hit_count = hits;
    s.last_accessed = last;
    s.estimated_savings_usd = savings;
    s.created_at = created;
    return s;
}
} // namespace

TEST(ConversationCacheEvictorTest, EmptyEntriesProducesEmptyResult) {
    ConversationCacheEvictor e;
    auto v = e.selectEvictees({}, 5, std::chrono::steady_clock::now());
    EXPECT_TRUE(v.empty());
}

TEST(ConversationCacheEvictorTest, NZeroProducesEmptyResult) {
    ConversationCacheEvictor e;
    auto now = std::chrono::steady_clock::now();
    auto v = e.selectEvictees({make("a", 1, now, 0.1, now)}, 0, now);
    EXPECT_TRUE(v.empty());
}

TEST(ConversationCacheEvictorTest, ScoreMonotonicInHits) {
    ConversationCacheEvictor e;
    auto now = std::chrono::steady_clock::now();
    auto low = make("low", 1, now, 0.0, now);
    auto high = make("high", 100, now, 0.0, now);
    double s_low = e.score(low, now, 100, 0.0);
    double s_high = e.score(high, now, 100, 0.0);
    EXPECT_GT(s_high, s_low);
}

TEST(ConversationCacheEvictorTest, EvicteesAreLowestScoreFirst) {
    ConversationCacheEvictor e;
    auto now = std::chrono::steady_clock::now();
    std::vector<ConversationEntryStats> entries = {
        make("hot",  100, now,         5.0, now),
        make("cold", 1,   now - 7200s, 0.1, now - 80000s),
        make("warm", 10,  now - 600s,  1.0, now - 1000s),
    };
    auto v = e.selectEvictees(entries, 2, now);
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], "cold");
    EXPECT_EQ(v[1], "warm");
}

TEST(ConversationCacheEvictorTest, GlobalMaxZeroDegradesGracefully) {
    ConversationCacheEvictor e;
    auto now = std::chrono::steady_clock::now();
    std::vector<ConversationEntryStats> entries = {
        make("a", 0, now, 0.0, now),
        make("b", 0, now, 0.0, now),
    };
    auto v = e.selectEvictees(entries, 1, now);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "a"); // lex tiebreak
}

TEST(ConversationCacheEvictorTest, RecencyAndFreshnessDecay) {
    ConversationCacheEvictor::Weights w;
    ConversationCacheEvictor::Horizons h;
    ConversationCacheEvictor e(w, h);
    auto now = std::chrono::steady_clock::now();
    // Same hits / savings; differ only on recency.
    auto recent = make("recent", 5, now,           0.5, now);
    auto stale  = make("stale",  5, now - 7200s,   0.5, now);
    double s_recent = e.score(recent, now, 5, 0.5);
    double s_stale  = e.score(stale,  now, 5, 0.5);
    EXPECT_GT(s_recent, s_stale);
}

// Mutation Test (D7+P1#1): temporarily zero all weights -> selectEvictees
// returns lex-sorted (no scoring discrimination). This is the "anti-pattern"
// we explicitly want to detect if someone reintroduces a bug that ignores
// the score.
TEST(ConversationCacheEvictorTest, ZeroWeightsMutationDegradesToLexSort) {
    ConversationCacheEvictor::Weights w{0.0, 0.0, 0.0, 0.0};
    ConversationCacheEvictor::Horizons h;
    ConversationCacheEvictor e(w, h);
    auto now = std::chrono::steady_clock::now();
    std::vector<ConversationEntryStats> entries = {
        make("zebra", 100, now,        5.0, now),
        make("alpha", 1,   now - 9000s, 0.1, now - 80000s),
        make("mike",  10,  now,        1.0, now),
    };
    auto v = e.selectEvictees(entries, 3, now);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "alpha");
    EXPECT_EQ(v[1], "mike");
    EXPECT_EQ(v[2], "zebra");
}
