#include <gtest/gtest.h>
#include "cache/tokenizer.h"
#include <fstream>
#include <cstdio>

using namespace aegisgate;

namespace {

std::string createTempVocab() {
    std::string path = "test_vocab_tmp.txt";
    std::ofstream ofs(path);
    // Minimal BERT vocab: [PAD]=0, ..., [UNK]=100, [CLS]=101, [SEP]=102
    for (int i = 0; i < 100; ++i) {
        ofs << "[unused" << i << "]\n";
    }
    ofs << "[UNK]\n";     // 100
    ofs << "[CLS]\n";     // 101
    ofs << "[SEP]\n";     // 102
    ofs << "[MASK]\n";    // 103
    // Basic ASCII tokens
    for (char c = 'a'; c <= 'z'; ++c) {
        ofs << c << "\n";
    }
    // A few word tokens
    ofs << "hello\n";
    ofs << "world\n";
    ofs << "test\n";
    ofs << "##ing\n";
    ofs << "##ed\n";
    ofs << "##s\n";
    // Chinese character tokens
    ofs << "\xe4\xbd\xa0\n";  // 你
    ofs << "\xe5\xa5\xbd\n";  // 好
    ofs << "\xe4\xb8\x96\n";  // 世
    ofs << "\xe7\x95\x8c\n";  // 界
    ofs.close();
    return path;
}

void removeTempVocab(const std::string& path) {
    std::remove(path.c_str());
}

} // namespace

class BertTokenizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        vocab_path_ = createTempVocab();
        ASSERT_TRUE(tokenizer_.loadVocab(vocab_path_));
    }

    void TearDown() override {
        removeTempVocab(vocab_path_);
    }

    BertTokenizer tokenizer_;
    std::string vocab_path_;
};

TEST_F(BertTokenizerTest, LoadVocab) {
    EXPECT_TRUE(tokenizer_.isLoaded());
    EXPECT_GT(tokenizer_.vocabSize(), 100u);
}

TEST_F(BertTokenizerTest, FailsOnMissingFile) {
    BertTokenizer t;
    EXPECT_FALSE(t.loadVocab("nonexistent_vocab.txt"));
    EXPECT_FALSE(t.isLoaded());
}

TEST_F(BertTokenizerTest, EncodeAddsSpecialTokens) {
    auto out = tokenizer_.encode("hello", 16);
    EXPECT_EQ(out.input_ids.size(), 16u);
    EXPECT_EQ(out.attention_mask.size(), 16u);
    EXPECT_EQ(out.token_type_ids.size(), 16u);

    // [CLS]=101 at start, [SEP]=102 at end of real tokens
    EXPECT_EQ(out.input_ids[0], 101);
    // Find [SEP]
    bool found_sep = false;
    for (auto id : out.input_ids) {
        if (id == 102) { found_sep = true; break; }
    }
    EXPECT_TRUE(found_sep);
}

TEST_F(BertTokenizerTest, PadsToMaxLength) {
    auto out = tokenizer_.encode("test", 32);
    EXPECT_EQ(out.input_ids.size(), 32u);

    // Padding tokens should have attention_mask=0
    size_t real_tokens = 0;
    for (auto m : out.attention_mask) {
        if (m == 1) real_tokens++;
    }
    EXPECT_GT(real_tokens, 0u);
    EXPECT_LT(real_tokens, 32u);
}

TEST_F(BertTokenizerTest, AllTokenTypeIdsZero) {
    auto out = tokenizer_.encode("hello world", 16);
    for (auto t : out.token_type_ids) {
        EXPECT_EQ(t, 0);
    }
}

TEST_F(BertTokenizerTest, ChineseCharactersSplit) {
    // Chinese chars should be individually tokenized
    auto out = tokenizer_.encode("\xe4\xbd\xa0\xe5\xa5\xbd", 16); // 你好
    // Should have: [CLS] + 你 + 好 + [SEP] = 4 real tokens
    size_t real_tokens = 0;
    for (auto m : out.attention_mask) {
        if (m == 1) real_tokens++;
    }
    EXPECT_GE(real_tokens, 4u);
}

TEST_F(BertTokenizerTest, EmptyInputProducesOnlySpecialTokens) {
    auto out = tokenizer_.encode("", 16);
    // [CLS] + [SEP] = 2 real tokens
    EXPECT_EQ(out.input_ids[0], 101);
    EXPECT_EQ(out.input_ids[1], 102);
    EXPECT_EQ(out.attention_mask[0], 1);
    EXPECT_EQ(out.attention_mask[1], 1);
    EXPECT_EQ(out.attention_mask[2], 0);
}

TEST_F(BertTokenizerTest, UnknownTokensMappedToUNK) {
    auto out = tokenizer_.encode("zzzzzznotinvocab", 16);
    // Should contain [UNK]=100 for unknown token
    bool found_unk = false;
    for (auto id : out.input_ids) {
        if (id == 100) { found_unk = true; break; }
    }
    EXPECT_TRUE(found_unk);
}

TEST_F(BertTokenizerTest, TruncatesLongInput) {
    std::string long_text;
    for (int i = 0; i < 100; ++i) {
        long_text += "hello world test ";
    }
    auto out = tokenizer_.encode(long_text, 32);
    EXPECT_EQ(out.input_ids.size(), 32u);
}
