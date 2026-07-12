#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace sentencepiece {
class SentencePieceProcessor;
}

namespace aegisgate {

struct TokenizerOutput {
    std::vector<int64_t> input_ids;
    std::vector<int64_t> attention_mask;
    std::vector<int64_t> token_type_ids;
};

class BertTokenizer {
public:
    bool loadVocab(const std::string& vocab_path);
    bool isLoaded() const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    TokenizerOutput encode(const std::string& text,
                           size_t max_length = 512) const;

private:
    std::vector<std::string> basicTokenize(const std::string& text) const;
    std::vector<std::string> wordPieceTokenize(const std::string& token) const;
    bool isChinese(uint32_t cp) const;
    bool isPunctuation(uint32_t cp) const;
    bool isWhitespace(uint32_t cp) const;
    std::string toLower(const std::string& text) const;
    std::string stripAccents(const std::string& text) const;
    std::string tokenizeChineseChars(const std::string& text) const;

    int tokenToId(const std::string& token) const;

    std::unordered_map<std::string, int> vocab_;

    static constexpr int kPadTokenId = 0;
    static constexpr int kUNKTokenId = 100;
    static constexpr int kCLSTokenId = 101;
    static constexpr int kSEPTokenId = 102;
};

#ifdef AEGISGATE_ENABLE_GUARD
class SpmTokenizer {
public:
    SpmTokenizer();
    ~SpmTokenizer();

    bool loadModel(const std::string& model_path);
    bool isLoaded() const { return loaded_; }

    TokenizerOutput encode(const std::string& text,
                           size_t max_length = 512) const;

private:
    std::unique_ptr<sentencepiece::SentencePieceProcessor> processor_;
    bool loaded_ = false;

    static constexpr int64_t kPadTokenId = 0;
    static constexpr int64_t kCLSTokenId = 1;
    static constexpr int64_t kSEPTokenId = 2;
};
#endif

} // namespace aegisgate
