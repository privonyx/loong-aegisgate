#include "cache/tokenizer.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

#ifdef AEGISGATE_ENABLE_GUARD
#include <sentencepiece_processor.h>
#endif

namespace aegisgate {

namespace {

uint32_t utf8ToCodepoint(const char* s, size_t remaining, int& bytes) {
    uint8_t c = static_cast<uint8_t>(s[0]);
    if (c < 0x80)                              { bytes = 1; return c; }
    if ((c & 0xE0) == 0xC0 && remaining >= 2)  { bytes = 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && remaining >= 3)  { bytes = 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0 && remaining >= 4)  { bytes = 4; return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); }
    bytes = 1;
    return 0xFFFD;
}

std::string codepointToUtf8(uint32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result += static_cast<char>(cp);
    } else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        result += static_cast<char>(0xF0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return result;
}

} // namespace

bool BertTokenizer::loadVocab(const std::string& vocab_path) {
    std::ifstream ifs(vocab_path);
    if (!ifs.is_open()) {
        spdlog::error("Failed to open vocab file: {}", vocab_path);
        return false;
    }

    vocab_.clear();
    std::string line;
    int idx = 0;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        vocab_[line] = idx++;
    }

    spdlog::info("Loaded {} vocab entries from {}", vocab_.size(), vocab_path);
    return !vocab_.empty();
}

bool BertTokenizer::isChinese(uint32_t cp) const {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FA1F);
}

bool BertTokenizer::isPunctuation(uint32_t cp) const {
    if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) ||
        (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126)) {
        return true;
    }
    // Unicode general punctuation
    if (cp >= 0x2000 && cp <= 0x206F) return true;
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
    return false;
}

bool BertTokenizer::isWhitespace(uint32_t cp) const {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    if (cp == 0x00A0) return true; // NBSP
    return false;
}

std::string BertTokenizer::toLower(const std::string& text) const {
    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        int bytes;
        uint32_t cp = utf8ToCodepoint(text.c_str() + i, text.size() - i, bytes);
        if (cp >= 'A' && cp <= 'Z') {
            cp = cp - 'A' + 'a';
        }
        result += codepointToUtf8(cp);
        i += bytes;
    }
    return result;
}

std::string BertTokenizer::stripAccents(const std::string& text) const {
    // Simplified: only strip common accents in ASCII range
    return text;
}

std::string BertTokenizer::tokenizeChineseChars(const std::string& text) const {
    std::string output;
    output.reserve(text.size() * 2);
    size_t i = 0;
    while (i < text.size()) {
        int bytes;
        uint32_t cp = utf8ToCodepoint(text.c_str() + i, text.size() - i, bytes);
        if (isChinese(cp)) {
            output += ' ';
            output += text.substr(i, bytes);
            output += ' ';
        } else {
            output += text.substr(i, bytes);
        }
        i += bytes;
    }
    return output;
}

std::vector<std::string> BertTokenizer::basicTokenize(const std::string& text) const {
    std::string cleaned = toLower(text);
    cleaned = stripAccents(cleaned);
    cleaned = tokenizeChineseChars(cleaned);

    // Split on whitespace and punctuation
    std::vector<std::string> tokens;
    std::string current;
    size_t i = 0;
    while (i < cleaned.size()) {
        int bytes;
        uint32_t cp = utf8ToCodepoint(cleaned.c_str() + i, cleaned.size() - i, bytes);

        if (isWhitespace(cp)) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else if (isPunctuation(cp)) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            tokens.push_back(cleaned.substr(i, bytes));
        } else {
            current += cleaned.substr(i, bytes);
        }
        i += bytes;
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::vector<std::string> BertTokenizer::wordPieceTokenize(const std::string& token) const {
    if (token.empty()) return {};

    std::vector<std::string> sub_tokens;
    bool is_bad = false;
    size_t start = 0;

    while (start < token.size()) {
        size_t end = token.size();
        std::string cur_substr;
        bool found = false;

        while (start < end) {
            std::string substr = token.substr(start, end - start);
            if (start > 0) {
                substr = "##" + substr;
            }
            if (vocab_.count(substr)) {
                cur_substr = substr;
                found = true;
                break;
            }
            // Move end back by one UTF-8 character
            size_t new_end = end - 1;
            while (new_end > start && (token[new_end] & 0xC0) == 0x80) {
                --new_end;
            }
            end = new_end;
        }

        if (!found) {
            is_bad = true;
            break;
        }

        sub_tokens.push_back(cur_substr);
        start = start + (cur_substr.size() - (start > 0 ? 2 : 0));
        if (start > 0 && cur_substr.size() >= 2 && cur_substr[0] == '#' && cur_substr[1] == '#') {
            start = start + 2; // account for ## prefix adjustment
        }
    }

    if (is_bad) {
        return {"[UNK]"};
    }
    return sub_tokens;
}

int BertTokenizer::tokenToId(const std::string& token) const {
    auto it = vocab_.find(token);
    return (it != vocab_.end()) ? it->second : kUNKTokenId;
}

TokenizerOutput BertTokenizer::encode(const std::string& text,
                                       size_t max_length) const {
    auto basic_tokens = basicTokenize(text);

    std::vector<std::string> wp_tokens;
    for (const auto& token : basic_tokens) {
        auto sub = wordPieceTokenize(token);
        for (auto& s : sub) {
            wp_tokens.push_back(std::move(s));
        }
    }

    if (max_length < 2) max_length = 2;
    if (wp_tokens.size() > max_length - 2) {
        wp_tokens.resize(max_length - 2);
    }

    TokenizerOutput out;
    out.input_ids.reserve(max_length);
    out.attention_mask.reserve(max_length);
    out.token_type_ids.reserve(max_length);

    // [CLS] + tokens + [SEP]
    out.input_ids.push_back(kCLSTokenId);
    out.attention_mask.push_back(1);
    out.token_type_ids.push_back(0);

    for (const auto& tok : wp_tokens) {
        out.input_ids.push_back(tokenToId(tok));
        out.attention_mask.push_back(1);
        out.token_type_ids.push_back(0);
    }

    out.input_ids.push_back(kSEPTokenId);
    out.attention_mask.push_back(1);
    out.token_type_ids.push_back(0);

    // Pad to max_length
    while (out.input_ids.size() < max_length) {
        out.input_ids.push_back(kPadTokenId);
        out.attention_mask.push_back(0);
        out.token_type_ids.push_back(0);
    }

    return out;
}

#ifdef AEGISGATE_ENABLE_GUARD

SpmTokenizer::SpmTokenizer()
    : processor_(std::make_unique<sentencepiece::SentencePieceProcessor>()) {}

SpmTokenizer::~SpmTokenizer() = default;

bool SpmTokenizer::loadModel(const std::string& model_path) {
    auto status = processor_->Load(model_path);
    if (!status.ok()) {
        spdlog::error("Failed to load SentencePiece model: {} ({})",
                      model_path, status.ToString());
        loaded_ = false;
        return false;
    }
    loaded_ = true;
    spdlog::info("Loaded SentencePiece model from {}", model_path);
    return true;
}

TokenizerOutput SpmTokenizer::encode(const std::string& text,
                                     size_t max_length) const {
    if (max_length < 2) max_length = 2;

    std::vector<int> piece_ids;
    if (loaded_ && processor_) {
        auto status = processor_->Encode(text, &piece_ids);
        if (!status.ok()) {
            spdlog::warn("SentencePiece encode failed: {}", status.ToString());
            piece_ids.clear();
        }
    }

    const size_t payload_limit = max_length - 2;  // [CLS] + payload + [SEP]
    if (piece_ids.size() > payload_limit) {
        piece_ids.resize(payload_limit);
    }

    TokenizerOutput out;
    out.input_ids.reserve(max_length);
    out.attention_mask.reserve(max_length);
    // DeBERTa-v3 ONNX export uses input_ids + attention_mask only.
    out.token_type_ids.clear();

    out.input_ids.push_back(kCLSTokenId);
    out.attention_mask.push_back(1);
    for (int id : piece_ids) {
        out.input_ids.push_back(static_cast<int64_t>(id));
        out.attention_mask.push_back(1);
    }
    out.input_ids.push_back(kSEPTokenId);
    out.attention_mask.push_back(1);

    while (out.input_ids.size() < max_length) {
        out.input_ids.push_back(kPadTokenId);
        out.attention_mask.push_back(0);
    }

    return out;
}

#endif

} // namespace aegisgate
