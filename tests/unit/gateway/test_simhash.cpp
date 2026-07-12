#include <gtest/gtest.h>
#include "gateway/simhash.h"
#include <string>

using namespace aegisgate;

TEST(SimHashTest, IdenticalTextsHaveZeroDistance) {
    const std::string a = "Please summarize the quarterly report for Q3.";
    EXPECT_EQ(hamming64(simhash64(a), simhash64(a)), 0);
}

TEST(SimHashTest, DeterministicAcrossCalls) {
    const std::string t = "hello world abuse fingerprint";
    EXPECT_EQ(simhash64(t), simhash64(t));
}

TEST(SimHashTest, NearEditHasLowDistance) {
    const std::string a =
        "Please summarize the quarterly report for Q3 and highlight risks.";
    std::string b = a;
    b[10] = 'X';  // single-char edit
    int d = hamming64(simhash64(a), simhash64(b));
    EXPECT_LE(d, 3) << "near-edit hamming=" << d;
}

TEST(SimHashTest, UnrelatedTextsHaveHighDistance) {
    const std::string a = "Please summarize the quarterly report for Q3.";
    const std::string b = "How do I bake sourdough bread at home today?";
    int d = hamming64(simhash64(a), simhash64(b));
    EXPECT_GE(d, 10) << "unrelated hamming=" << d;
}

TEST(SimHashTest, TruncationPrefixIsDeterministic) {
    // Production truncates before hashing (SR-1). Same prefix → same fingerprint.
    std::string long_text(9000, 'a');
    for (int i = 0; i < 9000; ++i) {
        long_text[static_cast<size_t>(i)] = static_cast<char>('a' + (i % 26));
    }
    std::string truncated = long_text.substr(0, 8192);
    EXPECT_EQ(simhash64(truncated), simhash64(std::string_view(long_text).substr(0, 8192)));
}

TEST(SimHashTest, EmptyAndShortStillProduceHash) {
    EXPECT_EQ(hamming64(simhash64(""), simhash64("")), 0);
    EXPECT_EQ(hamming64(simhash64("ab"), simhash64("ab")), 0);
}
