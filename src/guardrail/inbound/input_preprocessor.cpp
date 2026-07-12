#include "guardrail/inbound/input_preprocessor.h"
#include "guardrail/inbound/injection.h"
#include "observe/metrics.h"
#include <spdlog/spdlog.h>

namespace aegisgate {

InputPreprocessor::InputPreprocessor() = default;

InputPreprocessor::InputPreprocessor(int min_base64_length)
    : encoding_detector_(min_base64_length) {}

void InputPreprocessor::setInjectionDetector(const InjectionDetector* detector) {
    injection_detector_ = detector;
}

void InputPreprocessor::setUnicodeNormalization(bool enabled) { unicode_enabled_ = enabled; }
void InputPreprocessor::setEncodingDetection(bool enabled) { encoding_enabled_ = enabled; }

StageResult InputPreprocessor::process(RequestContext& ctx) {
    ctx.normalized_messages.clear();
    ctx.normalized_messages.reserve(ctx.chat_request.messages.size());
    ctx.image_scan_messages.clear();
    ctx.image_scan_messages.reserve(ctx.chat_request.messages.size());

    for (const auto& msg : ctx.chat_request.messages) {
        if (isToolMessage(msg)) {
            ctx.normalized_messages.push_back(msg.content);
            ctx.image_scan_messages.emplace_back();  // keep index alignment
            continue;
        }

        std::string normalized = msg.content;

        if (unicode_enabled_) {
            auto before = normalized;
            normalized = normalizer_.normalize(normalized);
            if (normalized != before) {
                MetricsRegistry::instance().preprocessorNormalizedTotal().inc();
            }
        }

        ctx.normalized_messages.push_back(normalized);

        // TASK-20260707-03 / REV20260707-N19: extract + normalize the image
        // reference text so the vision channel is scanned with the same C3
        // normalization the text channel gets.
        std::string image_scan =
            extractImageRefText(msg.content_parts, max_image_decode_bytes_);
        if (unicode_enabled_ && !image_scan.empty()) {
            image_scan = normalizer_.normalize(image_scan);
        }
        ctx.image_scan_messages.push_back(std::move(image_scan));

        if (encoding_enabled_) {
            auto segments = encoding_detector_.detect(msg.content);
            if (!segments.empty()) {
                MetricsRegistry::instance().preprocessorEncodingDetectedTotal().inc(
                    {}, static_cast<double>(segments.size()));
            }
            if (!segments.empty() && scanDecodedSegments(segments)) {
                spdlog::warn("InputPreprocessor: encoded injection detected in request {}, "
                             "encoding_type={}", ctx.request_id,
                             segments.front().encoding_type);
                return StageResult::Reject;
            }
        }
    }

    ctx.input_preprocessed = true;
    return StageResult::Continue;
}

bool InputPreprocessor::scanDecodedSegments(
    const std::vector<EncodedSegment>& segments) const {
    if (!injection_detector_) return false;
    for (const auto& seg : segments) {
        auto result = injection_detector_->detect(seg.decoded_text);
        if (result.detected) return true;
    }
    return false;
}

} // namespace aegisgate
