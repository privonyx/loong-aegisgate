#pragma once
#include "core/pipeline.h"
#include "guardrail/inbound/unicode_normalizer.h"
#include "guardrail/inbound/encoding_detector.h"

namespace aegisgate {

class InjectionDetector;

class InputPreprocessor : public PipelineStage {
public:
    InputPreprocessor();
    explicit InputPreprocessor(int min_base64_length);

    void setInjectionDetector(const InjectionDetector* detector);
    void setUnicodeNormalization(bool enabled);
    void setEncodingDetection(bool enabled);
    // TASK-20260707-03 / REV20260707-N19: cap for decoding data: URI text
    // payloads in the image-reference scan surface (SR-4 DoS guard).
    void setMaxImageDecodeBytes(size_t bytes) { max_image_decode_bytes_ = bytes; }

    StageResult process(RequestContext& ctx) override;
    std::string name() const override { return "InputPreprocessor"; }

private:
    bool scanDecodedSegments(const std::vector<EncodedSegment>& segments) const;

    UnicodeNormalizer normalizer_;
    EncodingDetector encoding_detector_;
    const InjectionDetector* injection_detector_ = nullptr;
    bool unicode_enabled_ = true;
    bool encoding_enabled_ = true;
    size_t max_image_decode_bytes_ = kDefaultImageScanDecodeBytes;
};

} // namespace aegisgate
