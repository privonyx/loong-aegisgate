#pragma once
#include "multimodal/modality_handler.h"
#include "multimodal/modality_upstream.h"
#include <string>

namespace aegisgate {

// CR2 §4.3 — five concrete (Modality, openai) Handlers.
//
// Each handler:
//   - holds a reference to a ModalityUpstream (production: adapter over
//     OpenAIConnector; test: FakeUpstream)
//   - returns the canonical endpoint string so the Router can construct a
//     ProxyRequest if it ever needs to override req.endpoint
//   - exposes a coarse estimateCost(req) used by RoutingPolicy::Cheapest

class OpenAIEmbeddingHandler : public ModalityHandler {
public:
    explicit OpenAIEmbeddingHandler(ModalityUpstream& upstream,
                                    double cost_per_1k_tokens = 0.00002);
    ProxyResponse handle(const ProxyRequest& req, const std::string& api_key) override;
    Modality modality() const override { return Modality::Embedding; }
    std::string provider() const override { return "openai"; }
    double estimateCost(const ProxyRequest& req) const override;
    std::string name() const override { return "openai_embedding"; }

    static constexpr const char* kEndpoint = "/v1/embeddings";
private:
    ModalityUpstream& upstream_;
    double cost_per_1k_tokens_;
};

class OpenAIImageGenHandler : public ModalityHandler {
public:
    explicit OpenAIImageGenHandler(ModalityUpstream& upstream,
                                    double cost_per_image = 0.04);
    ProxyResponse handle(const ProxyRequest& req, const std::string& api_key) override;
    Modality modality() const override { return Modality::ImageGen; }
    std::string provider() const override { return "openai"; }
    double estimateCost(const ProxyRequest& req) const override;
    std::string name() const override { return "openai_image_gen"; }

    static constexpr const char* kEndpoint = "/v1/images/generations";
private:
    ModalityUpstream& upstream_;
    double cost_per_image_;
};

class OpenAIAudioTranscribeHandler : public ModalityHandler {
public:
    explicit OpenAIAudioTranscribeHandler(ModalityUpstream& upstream,
                                          double cost_per_minute = 0.006);
    ProxyResponse handle(const ProxyRequest& req, const std::string& api_key) override;
    Modality modality() const override { return Modality::AudioTranscribe; }
    std::string provider() const override { return "openai"; }
    double estimateCost(const ProxyRequest& req) const override;
    std::string name() const override { return "openai_audio_transcribe"; }

    static constexpr const char* kEndpoint = "/v1/audio/transcriptions";
private:
    ModalityUpstream& upstream_;
    double cost_per_minute_;
};

class OpenAIAudioSpeechHandler : public ModalityHandler {
public:
    explicit OpenAIAudioSpeechHandler(ModalityUpstream& upstream,
                                       double cost_per_1k_chars = 0.015);
    ProxyResponse handle(const ProxyRequest& req, const std::string& api_key) override;
    Modality modality() const override { return Modality::AudioSpeech; }
    std::string provider() const override { return "openai"; }
    double estimateCost(const ProxyRequest& req) const override;
    std::string name() const override { return "openai_audio_speech"; }

    static constexpr const char* kEndpoint = "/v1/audio/speech";
private:
    ModalityUpstream& upstream_;
    double cost_per_1k_chars_;
};

class OpenAIModerationHandler : public ModalityHandler {
public:
    explicit OpenAIModerationHandler(ModalityUpstream& upstream,
                                      double cost_per_call = 0.0);
    ProxyResponse handle(const ProxyRequest& req, const std::string& api_key) override;
    Modality modality() const override { return Modality::Moderation; }
    std::string provider() const override { return "openai"; }
    double estimateCost(const ProxyRequest& req) const override;
    std::string name() const override { return "openai_moderation"; }

    static constexpr const char* kEndpoint = "/v1/moderations";
private:
    ModalityUpstream& upstream_;
    double cost_per_call_;
};

} // namespace aegisgate
