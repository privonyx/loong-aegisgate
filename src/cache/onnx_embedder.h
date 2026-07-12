#pragma once

#ifdef AEGISGATE_ENABLE_ONNX

#include "cache/embedder.h"
#include "cache/tokenizer.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>

namespace aegisgate {

class OnnxEmbedder : public Embedder {
public:
    OnnxEmbedder(const std::string& model_path,
                 const std::string& vocab_path,
                 size_t max_seq_length = 512,
                 int num_threads = 1);

    std::vector<float> embed(const std::string& text) override;
    size_t dimension() const override { return dimension_; }
    bool isReady() const { return ready_; }

private:
    std::vector<float> meanPooling(const std::vector<float>& hidden_states,
                                    const std::vector<int64_t>& attention_mask,
                                    size_t seq_len) const;
    void normalize(std::vector<float>& vec) const;

    BertTokenizer tokenizer_;
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    size_t max_seq_length_;
    size_t dimension_ = 0;
    bool ready_ = false;
};

} // namespace aegisgate

#endif // AEGISGATE_ENABLE_ONNX
