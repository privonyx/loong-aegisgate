#pragma once
// Header-only pure-function helpers for GuardClassifier decision logic.
//
// Kept free of ONNX / tokenizer dependencies so the numeric decision path
// (softmax + thresholding) is unit-testable offline, independent of a loaded
// model. The classifier wires raw logits from ONNX into evaluate() and maps
// the result onto GuardResult. (header-only helper 抽离范式 / N=4 稳定)
#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace aegisgate::guard_detail {

// Numerically stable softmax (subtract-max). Empty in -> empty out.
inline std::vector<float> softmax(const std::vector<float>& logits) {
    std::vector<float> out(logits.size());
    if (logits.empty()) return out;
    const float m = *std::max_element(logits.begin(), logits.end());
    float sum = 0.0f;
    for (size_t i = 0; i < logits.size(); ++i) {
        out[i] = std::exp(logits[i] - m);
        sum += out[i];
    }
    if (sum > 0.0f) {
        for (float& v : out) v /= sum;
    }
    return out;
}

struct GuardScore {
    bool unsafe = false;
    size_t label_index = 0;  // safe_index when safe; else the flagged label
    float score = 0.0f;      // P(flagged label) when unsafe; else P(safe)
};

// Decision rule (creative C3): compute softmax probabilities, then flag the
// highest-probability NON-safe label whose probability >= threshold. A lower
// threshold is more aggressive (flags more). For binary [safe, injection] this
// reduces to "P(injection) >= threshold".
inline GuardScore evaluate(const std::vector<float>& logits,
                           size_t safe_index,
                           float threshold) {
    GuardScore r;
    r.label_index = safe_index;
    const auto probs = softmax(logits);
    if (probs.empty()) return r;  // no signal -> safe (fail-open handled by caller)

    size_t best_unsafe = safe_index;
    float best_unsafe_p = -1.0f;
    for (size_t i = 0; i < probs.size(); ++i) {
        if (i == safe_index) continue;
        if (probs[i] > best_unsafe_p) {
            best_unsafe_p = probs[i];
            best_unsafe = i;
        }
    }

    if (best_unsafe_p >= threshold) {
        r.unsafe = true;
        r.label_index = best_unsafe;
        r.score = best_unsafe_p;
    } else {
        r.unsafe = false;
        r.label_index = safe_index;
        r.score = (safe_index < probs.size()) ? probs[safe_index] : 0.0f;
    }
    return r;
}

} // namespace aegisgate::guard_detail
