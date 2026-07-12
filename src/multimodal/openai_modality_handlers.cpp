#include "multimodal/openai_modality_handlers.h"

namespace aegisgate {

namespace {

// estimateCost is intentionally coarse: it's only used to break ties between
// equivalent providers under RoutingPolicy::Cheapest. Detailed per-token /
// per-second cost accounting still happens inside CostTracker after the
// upstream call completes.

double estimateByBodyKb(const ProxyRequest& req, double rate) {
    const double kb = static_cast<double>(req.raw_body.size()) / 1024.0;
    return kb * rate;
}

} // namespace

// ----- Embedding -----
OpenAIEmbeddingHandler::OpenAIEmbeddingHandler(ModalityUpstream& upstream,
                                               double cost_per_1k_tokens)
    : upstream_(upstream), cost_per_1k_tokens_(cost_per_1k_tokens) {}

ProxyResponse OpenAIEmbeddingHandler::handle(const ProxyRequest& req,
                                              const std::string& api_key) {
    return upstream_.proxy(req, api_key);
}

double OpenAIEmbeddingHandler::estimateCost(const ProxyRequest& req) const {
    // Rough: assume 4 chars/token; cost = (chars/4)/1000 * cost_per_1k.
    const double tokens_estimate = static_cast<double>(req.raw_body.size()) / 4.0;
    return (tokens_estimate / 1000.0) * cost_per_1k_tokens_;
}

// ----- ImageGen -----
OpenAIImageGenHandler::OpenAIImageGenHandler(ModalityUpstream& upstream,
                                             double cost_per_image)
    : upstream_(upstream), cost_per_image_(cost_per_image) {}

ProxyResponse OpenAIImageGenHandler::handle(const ProxyRequest& req,
                                             const std::string& api_key) {
    return upstream_.proxy(req, api_key);
}

double OpenAIImageGenHandler::estimateCost(const ProxyRequest& /*req*/) const {
    // Single-shot per call; multi-image batching is implicit in body size but
    // production cost tracker computes the precise figure post-response.
    return cost_per_image_;
}

// ----- AudioTranscribe -----
OpenAIAudioTranscribeHandler::OpenAIAudioTranscribeHandler(
    ModalityUpstream& upstream, double cost_per_minute)
    : upstream_(upstream), cost_per_minute_(cost_per_minute) {}

ProxyResponse OpenAIAudioTranscribeHandler::handle(const ProxyRequest& req,
                                                     const std::string& api_key) {
    return upstream_.proxy(req, api_key);
}

double OpenAIAudioTranscribeHandler::estimateCost(const ProxyRequest& req) const {
    // Assume audio body size correlates with duration; rough 64KB/minute.
    const double minutes = static_cast<double>(req.raw_body.size()) / (64.0 * 1024.0);
    return minutes * cost_per_minute_;
}

// ----- AudioSpeech -----
OpenAIAudioSpeechHandler::OpenAIAudioSpeechHandler(ModalityUpstream& upstream,
                                                    double cost_per_1k_chars)
    : upstream_(upstream), cost_per_1k_chars_(cost_per_1k_chars) {}

ProxyResponse OpenAIAudioSpeechHandler::handle(const ProxyRequest& req,
                                                 const std::string& api_key) {
    return upstream_.proxy(req, api_key);
}

double OpenAIAudioSpeechHandler::estimateCost(const ProxyRequest& req) const {
    return estimateByBodyKb(req, cost_per_1k_chars_);
}

// ----- Moderation -----
OpenAIModerationHandler::OpenAIModerationHandler(ModalityUpstream& upstream,
                                                  double cost_per_call)
    : upstream_(upstream), cost_per_call_(cost_per_call) {}

ProxyResponse OpenAIModerationHandler::handle(const ProxyRequest& req,
                                                const std::string& api_key) {
    return upstream_.proxy(req, api_key);
}

double OpenAIModerationHandler::estimateCost(const ProxyRequest& /*req*/) const {
    return cost_per_call_;
}

} // namespace aegisgate
