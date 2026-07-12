#include "multimodal/modality.h"

namespace aegisgate {

std::string modalityToString(Modality m) {
    switch (m) {
        case Modality::Embedding:        return "embedding";
        case Modality::ImageGen:         return "image_gen";
        case Modality::AudioTranscribe:  return "audio_transcribe";
        case Modality::AudioSpeech:      return "audio_speech";
        case Modality::Moderation:       return "moderation";
        case Modality::Unknown:          return "unknown";
    }
    return "unknown";
}

Modality modalityFromString(const std::string& s) {
    if (s == "embedding")       return Modality::Embedding;
    if (s == "image_gen")       return Modality::ImageGen;
    if (s == "audio_transcribe")return Modality::AudioTranscribe;
    if (s == "audio_speech")    return Modality::AudioSpeech;
    if (s == "moderation")      return Modality::Moderation;
    return Modality::Unknown;
}

Modality modalityFromEndpoint(const std::string& endpoint) {
    // OpenAI v1 endpoints; matches src/server/api_controller.cpp proxy table.
    if (endpoint == "/v1/embeddings")              return Modality::Embedding;
    if (endpoint == "/v1/images/generations")      return Modality::ImageGen;
    if (endpoint == "/v1/audio/transcriptions")    return Modality::AudioTranscribe;
    if (endpoint == "/v1/audio/speech")            return Modality::AudioSpeech;
    if (endpoint == "/v1/moderations")             return Modality::Moderation;
    return Modality::Unknown;
}

} // namespace aegisgate
