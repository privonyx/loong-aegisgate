// Phase 9.3 Epic 3 Task 3.2 — ULID generator.
//
// Invariants (plan §3.2):
//   * 26-character Crockford Base32 string (10 timestamp + 16 randomness).
//   * All characters live in the Crockford Base32 alphabet (no I/L/O/U).
//   * Within a single millisecond 100 generations must be unique.
//   * ULIDs minted at strictly later milliseconds are lexicographically greater,
//     matching the monotonic-sort property callers rely on for listConfigVersions.

#include "control_plane/ulid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

namespace aegisgate {
namespace {

constexpr char kCrockfordAlphabet[] =
    "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool isCrockford(char c) {
    for (char a : kCrockfordAlphabet) {
        if (a == 0) return false;
        if (a == c) return true;
    }
    return false;
}

TEST(UlidTest, TwentySixCharacters) {
    Ulid gen;
    std::string id = gen.generate();
    EXPECT_EQ(id.size(), 26u);
}

TEST(UlidTest, OnlyCrockfordAlphabet) {
    Ulid gen;
    for (int i = 0; i < 50; ++i) {
        std::string id = gen.generate();
        for (char c : id) {
            EXPECT_TRUE(isCrockford(c))
                << "id=" << id << " has non-Crockford char '" << c << "'";
        }
    }
}

TEST(UlidTest, NoForbiddenLetters) {
    // Crockford explicitly excludes I, L, O, U to avoid visual ambiguity.
    Ulid gen;
    for (int i = 0; i < 50; ++i) {
        std::string id = gen.generate();
        for (char c : id) {
            EXPECT_NE(c, 'I');
            EXPECT_NE(c, 'L');
            EXPECT_NE(c, 'O');
            EXPECT_NE(c, 'U');
        }
    }
}

TEST(UlidTest, HundredUniqueInSameBurst) {
    // Even without artificial clock pinning the randomness fills 80 bits, so
    // 100 rapid generations must not collide.
    Ulid gen;
    std::unordered_set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        auto id = gen.generate();
        ASSERT_TRUE(seen.insert(id).second)
            << "duplicate id=" << id << " at iteration " << i;
    }
}

TEST(UlidTest, LexicographicallyMonotonicAcrossMillis) {
    Ulid gen;
    std::string first = gen.generate();
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    std::string later = gen.generate();
    EXPECT_LT(first, later) << "first=" << first << " later=" << later;
}

TEST(UlidTest, TimestampPrefixReflectsWallClock) {
    // First 10 chars decode back to a timestamp within +/-50ms of now.
    using namespace std::chrono;
    Ulid gen;
    auto before = duration_cast<milliseconds>(
                      system_clock::now().time_since_epoch()).count();
    std::string id = gen.generate();
    auto after = duration_cast<milliseconds>(
                     system_clock::now().time_since_epoch()).count();

    auto decoded = Ulid::decodeTimestamp(id);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_GE(*decoded, before - 2);
    EXPECT_LE(*decoded, after + 2);
}

} // namespace
} // namespace aegisgate
