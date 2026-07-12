#ifdef AEGISGATE_ENABLE_ONNX

#include "cache/onnx_embedder.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <numeric>

namespace aegisgate {

OnnxEmbedder::OnnxEmbedder(const std::string& model_path,
                             const std::string& vocab_path,
                             size_t max_seq_length,
                             int num_threads)
    : max_seq_length_(max_seq_length) {

    if (!tokenizer_.loadVocab(vocab_path)) {
        spdlog::error("OnnxEmbedder: failed to load vocab from {}", vocab_path);
        return;
    }

    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "aegisgate");

        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(num_threads);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), opts);

        // Detect output dimension from model metadata
        auto output_info = session_->GetOutputTypeInfo(0);
        auto tensor_info = output_info.GetTensorTypeAndShapeInfo();
        auto shape = tensor_info.GetShape();
        // Shape: [batch, seq_len, hidden_size] or [batch, hidden_size]
        dimension_ = static_cast<size_t>(shape.back());

        ready_ = true;
        spdlog::info("OnnxEmbedder loaded: model={}, vocab={}, dim={}",
                     model_path, vocab_path, dimension_);
    } catch (const Ort::Exception& e) {
        spdlog::error("OnnxEmbedder ONNX error: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::error("OnnxEmbedder init error: {}", e.what());
    }
}

std::vector<float> OnnxEmbedder::embed(const std::string& text) {
    if (!ready_) {
        return std::vector<float>(dimension_ > 0 ? dimension_ : 512, 0.0f);
    }

    auto encoded = tokenizer_.encode(text, max_seq_length_);

    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);

    std::array<int64_t, 2> input_shape = {1,
        static_cast<int64_t>(encoded.input_ids.size())};

    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        mem_info, encoded.input_ids.data(), encoded.input_ids.size(),
        input_shape.data(), input_shape.size()));

    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        mem_info, encoded.attention_mask.data(), encoded.attention_mask.size(),
        input_shape.data(), input_shape.size()));

    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        mem_info, encoded.token_type_ids.data(), encoded.token_type_ids.size(),
        input_shape.data(), input_shape.size()));

    std::vector<const char*> input_names = {
        "input_ids", "attention_mask", "token_type_ids"};
    std::vector<const char*> output_names = {"last_hidden_state"};

    try {
        auto outputs = session_->Run(Ort::RunOptions{nullptr},
            input_names.data(), inputs.data(), inputs.size(),
            output_names.data(), output_names.size());

        auto& output_tensor = outputs[0];
        auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
        auto shape = type_info.GetShape();
        const float* data = output_tensor.GetTensorData<float>();

        if (shape.size() < 2) {
            spdlog::error("Unexpected ONNX output shape: ndim={}", shape.size());
            return std::vector<float>(dimension_, 0.0f);
        }
        size_t hidden_size = static_cast<size_t>(shape.back());
        size_t seq_len = (shape.size() >= 3)
            ? static_cast<size_t>(shape[1])
            : 1;
        std::vector<float> hidden(data, data + seq_len * hidden_size);

        auto result = meanPooling(hidden, encoded.attention_mask, seq_len);
        normalize(result);
        return result;
    } catch (const Ort::Exception& e) {
        spdlog::error("OnnxEmbedder inference error: {}", e.what());
        return std::vector<float>(dimension_, 0.0f);
    }
}

std::vector<float> OnnxEmbedder::meanPooling(
    const std::vector<float>& hidden_states,
    const std::vector<int64_t>& attention_mask,
    size_t seq_len) const {

    std::vector<float> result(dimension_, 0.0f);
    float mask_sum = 0.0f;

    for (size_t t = 0; t < seq_len; ++t) {
        float mask = static_cast<float>(attention_mask[t]);
        mask_sum += mask;
        for (size_t d = 0; d < dimension_; ++d) {
            result[d] += hidden_states[t * dimension_ + d] * mask;
        }
    }

    if (mask_sum > 0.0f) {
        for (auto& v : result) v /= mask_sum;
    }
    return result;
}

void OnnxEmbedder::normalize(std::vector<float>& vec) const {
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-8f) {
        for (float& v : vec) v /= norm;
    }
}

} // namespace aegisgate

#endif // AEGISGATE_ENABLE_ONNX
