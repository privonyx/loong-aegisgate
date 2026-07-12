#include <gtest/gtest.h>
#include "guardrail/inbound/unicode_normalizer.h"

using namespace aegisgate;

class UnicodeNormalizerTest : public ::testing::Test {
protected:
    UnicodeNormalizer normalizer_;
};

TEST_F(UnicodeNormalizerTest, NormalizesFullwidthAscii) {
    // Ｈｅｌｌｏ (U+FF28 U+FF45 U+FF4C U+FF4C U+FF4F) → Hello
    std::string fw = "\xEF\xBC\xA8\xEF\xBD\x85\xEF\xBD\x8C\xEF\xBD\x8C\xEF\xBD\x8F";
    EXPECT_EQ(normalizer_.normalize(fw), "Hello");
}

TEST_F(UnicodeNormalizerTest, NormalizesFullwidthDigits) {
    // ０１２３ (U+FF10-U+FF13) → 0123
    std::string fw = "\xEF\xBC\x90\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93";
    EXPECT_EQ(normalizer_.normalize(fw), "0123");
}

TEST_F(UnicodeNormalizerTest, StripsZeroWidthChars) {
    std::string text = "he\xE2\x80\x8Bllo";  // he + ZWSP(U+200B) + llo
    EXPECT_EQ(normalizer_.normalize(text), "hello");
}

TEST_F(UnicodeNormalizerTest, StripsBOM) {
    std::string text = "\xEF\xBB\xBFhello";  // BOM(U+FEFF) + hello
    EXPECT_EQ(normalizer_.normalize(text), "hello");
}

TEST_F(UnicodeNormalizerTest, NormalizesCyrillicConfusables) {
    // Cyrillic а (U+0430) = UTF-8 0xD0 0xB0 → Latin a
    std::string cyrillic_a = "\xD0\xB0";
    EXPECT_EQ(normalizer_.normalize(cyrillic_a), "a");
}

TEST_F(UnicodeNormalizerTest, NormalizesGreekConfusables) {
    // Greek Α (U+0391) = UTF-8 0xCE 0x91 → Latin A
    std::string greek_A = "\xCE\x91";
    EXPECT_EQ(normalizer_.normalize(greek_A), "A");
}

TEST_F(UnicodeNormalizerTest, NormalizesMathBoldLetters) {
    // 𝐀 (U+1D400) = UTF-8 F0 9D 90 80 → A
    std::string math_A = "\xF0\x9D\x90\x80";
    EXPECT_EQ(normalizer_.normalize(math_A), "A");
}

TEST_F(UnicodeNormalizerTest, PreservesNormalAscii) {
    EXPECT_EQ(normalizer_.normalize("Hello World 123!"), "Hello World 123!");
}

TEST_F(UnicodeNormalizerTest, PreservesNormalChinese) {
    std::string cn = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"; // 你好世界
    EXPECT_EQ(normalizer_.normalize(cn), cn);
}

TEST_F(UnicodeNormalizerTest, DetectsConfusables) {
    std::string fw = "\xEF\xBC\xA8\xEF\xBD\x85\xEF\xBD\x8C\xEF\xBD\x8C\xEF\xBD\x8F";
    EXPECT_TRUE(UnicodeNormalizer::hasConfusables(fw));
    EXPECT_FALSE(UnicodeNormalizer::hasConfusables("Hello"));
}

TEST_F(UnicodeNormalizerTest, StripZeroWidthReturnsCount) {
    std::string text = "h\xE2\x80\x8B" "e\xE2\x80\x8B" "llo"; // h + ZWSP + e + ZWSP + llo
    EXPECT_EQ(normalizer_.stripZeroWidth(text), 2u);
    EXPECT_EQ(text, "hello");
}

TEST_F(UnicodeNormalizerTest, StripsSoftHyphen) {
    std::string text = "ig\xC2\xADnore"; // ig + SHY(U+00AD) + nore
    EXPECT_EQ(normalizer_.normalize(text), "ignore");
}

TEST_F(UnicodeNormalizerTest, MixedConfusablesAndNormal) {
    // Cyrillic "а" + normal "bc" = "abc"
    std::string mixed = "\xD0\xB0" "bc";
    EXPECT_EQ(normalizer_.normalize(mixed), "abc");
}

TEST_F(UnicodeNormalizerTest, EmptyInput) {
    EXPECT_EQ(normalizer_.normalize(""), "");
}
