#pragma once
#include <string>

namespace aegisgate {

// Phase 6.1 — modality enum + endpoint mapping (CR2 D4=C).
//
// IMPORTANT: enum values are stable wire-level identifiers used in
// CostTracker.modality / RateLimiter.modality_quotas / Prometheus labels.
// DO NOT renumber existing values.
enum class Modality {
    Embedding = 0,
    ImageGen = 1,
    AudioTranscribe = 2,
    AudioSpeech = 3,
    Moderation = 4,
    Unknown = 99
};

std::string modalityToString(Modality m);
Modality modalityFromString(const std::string& s);
Modality modalityFromEndpoint(const std::string& endpoint);

} // namespace aegisgate
