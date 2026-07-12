#include "multimodal/openai_modality_handlers.h"
#include <gtest/gtest.h>

using namespace aegisgate;

namespace {

class FakeUpstream : public ModalityUpstream {
public:
    ProxyResponse proxy(const ProxyRequest& req, const std::string& api_key) override {
        last_endpoint = req.endpoint;
        last_api_key = api_key;
        last_body = req.raw_body;
        ++calls;
        ProxyResponse r;
        r.http_status = 200;
        r.body = R"({"ok":true})";
        return r;
    }
    std::string last_endpoint;
    std::string last_api_key;
    std::string last_body;
    size_t calls = 0;
};

ProxyRequest mkReq(const std::string& endpoint, const std::string& body) {
    ProxyRequest r;
    r.endpoint = endpoint;
    r.raw_body = body;
    return r;
}

} // namespace

TEST(ModalityHandlersTest, EmbeddingPassthroughAndIdentity) {
    FakeUpstream up;
    OpenAIEmbeddingHandler h(up);
    auto resp = h.handle(mkReq("/v1/embeddings", "input"), "sk-emb");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(up.last_api_key, "sk-emb");
    EXPECT_EQ(h.modality(), Modality::Embedding);
    EXPECT_EQ(h.provider(), "openai");
    EXPECT_EQ(h.name(), "openai_embedding");
}

TEST(ModalityHandlersTest, EmbeddingEstimateCostScalesWithBody) {
    FakeUpstream up;
    OpenAIEmbeddingHandler h(up);
    auto small = h.estimateCost(mkReq("/v1/embeddings", "x"));
    auto big = h.estimateCost(mkReq("/v1/embeddings", std::string(40000, 'x')));
    EXPECT_LT(small, big);
    EXPECT_GT(big, 0.0);
}

TEST(ModalityHandlersTest, ImageGenPassthroughAndIdentity) {
    FakeUpstream up;
    OpenAIImageGenHandler h(up);
    auto resp = h.handle(mkReq("/v1/images/generations", R"({"prompt":"x"})"), "sk-img");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(h.modality(), Modality::ImageGen);
    EXPECT_EQ(h.name(), "openai_image_gen");
    EXPECT_EQ(up.calls, 1u);
}

TEST(ModalityHandlersTest, ImageGenEstimateCostFlatPerCall) {
    FakeUpstream up;
    OpenAIImageGenHandler h(up, 0.04);
    EXPECT_DOUBLE_EQ(h.estimateCost(mkReq("/v1/images/generations", "x")), 0.04);
}

TEST(ModalityHandlersTest, AudioTranscribePassthroughAndIdentity) {
    FakeUpstream up;
    OpenAIAudioTranscribeHandler h(up);
    auto resp = h.handle(mkReq("/v1/audio/transcriptions", "binary"), "sk-at");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(h.modality(), Modality::AudioTranscribe);
    EXPECT_EQ(h.name(), "openai_audio_transcribe");
}

TEST(ModalityHandlersTest, AudioTranscribeEstimateCostScalesWithBody) {
    FakeUpstream up;
    OpenAIAudioTranscribeHandler h(up);
    auto silent = h.estimateCost(mkReq("/v1/audio/transcriptions", ""));
    auto large = h.estimateCost(mkReq("/v1/audio/transcriptions", std::string(128 * 1024, 'x')));
    EXPECT_EQ(silent, 0.0);
    EXPECT_GT(large, 0.0);
}

TEST(ModalityHandlersTest, AudioSpeechPassthroughAndIdentity) {
    FakeUpstream up;
    OpenAIAudioSpeechHandler h(up);
    auto resp = h.handle(mkReq("/v1/audio/speech", R"({"input":"hello"})"), "sk-as");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(h.modality(), Modality::AudioSpeech);
    EXPECT_EQ(h.name(), "openai_audio_speech");
}

TEST(ModalityHandlersTest, AudioSpeechEstimateCostScalesWithBody) {
    FakeUpstream up;
    OpenAIAudioSpeechHandler h(up);
    auto small = h.estimateCost(mkReq("/v1/audio/speech", "x"));
    auto big = h.estimateCost(mkReq("/v1/audio/speech", std::string(10000, 'x')));
    EXPECT_LT(small, big);
}

TEST(ModalityHandlersTest, ModerationPassthroughAndIdentity) {
    FakeUpstream up;
    OpenAIModerationHandler h(up);
    auto resp = h.handle(mkReq("/v1/moderations", R"({"input":"x"})"), "sk-mod");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(h.modality(), Modality::Moderation);
    EXPECT_EQ(h.name(), "openai_moderation");
}

TEST(ModalityHandlersTest, ModerationEstimateCostIsFlat) {
    FakeUpstream up;
    OpenAIModerationHandler h(up, 0.0);
    EXPECT_DOUBLE_EQ(h.estimateCost(mkReq("/v1/moderations", "anything")), 0.0);
}
