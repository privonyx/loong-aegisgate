#include "multimodal/modality.h"
#include <gtest/gtest.h>

using namespace aegisgate;

TEST(ModalityTest, EndpointToModality) {
    EXPECT_EQ(modalityFromEndpoint("/v1/embeddings"), Modality::Embedding);
    EXPECT_EQ(modalityFromEndpoint("/v1/images/generations"), Modality::ImageGen);
    EXPECT_EQ(modalityFromEndpoint("/v1/audio/transcriptions"), Modality::AudioTranscribe);
    EXPECT_EQ(modalityFromEndpoint("/v1/audio/speech"), Modality::AudioSpeech);
    EXPECT_EQ(modalityFromEndpoint("/v1/moderations"), Modality::Moderation);
}

TEST(ModalityTest, ModalityToString) {
    EXPECT_EQ(modalityToString(Modality::Embedding), "embedding");
    EXPECT_EQ(modalityToString(Modality::ImageGen), "image_gen");
    EXPECT_EQ(modalityToString(Modality::AudioTranscribe), "audio_transcribe");
    EXPECT_EQ(modalityToString(Modality::AudioSpeech), "audio_speech");
    EXPECT_EQ(modalityToString(Modality::Moderation), "moderation");
    EXPECT_EQ(modalityToString(Modality::Unknown), "unknown");
}

TEST(ModalityTest, ModalityFromString) {
    EXPECT_EQ(modalityFromString("embedding"), Modality::Embedding);
    EXPECT_EQ(modalityFromString("image_gen"), Modality::ImageGen);
    EXPECT_EQ(modalityFromString("audio_transcribe"), Modality::AudioTranscribe);
    EXPECT_EQ(modalityFromString("audio_speech"), Modality::AudioSpeech);
    EXPECT_EQ(modalityFromString("moderation"), Modality::Moderation);
    EXPECT_EQ(modalityFromString("garbage"), Modality::Unknown);
}

TEST(ModalityTest, UnknownEndpointMapsToUnknown) {
    EXPECT_EQ(modalityFromEndpoint("/v1/chat/completions"), Modality::Unknown);
    EXPECT_EQ(modalityFromEndpoint("/garbage"), Modality::Unknown);
    EXPECT_EQ(modalityFromEndpoint(""), Modality::Unknown);
}

TEST(ModalityTest, EnumValuesAreStableWireIdentifiers) {
    // Stability guard — these values land in metric labels / persisted records.
    EXPECT_EQ(static_cast<int>(Modality::Embedding), 0);
    EXPECT_EQ(static_cast<int>(Modality::ImageGen), 1);
    EXPECT_EQ(static_cast<int>(Modality::AudioTranscribe), 2);
    EXPECT_EQ(static_cast<int>(Modality::AudioSpeech), 3);
    EXPECT_EQ(static_cast<int>(Modality::Moderation), 4);
    EXPECT_EQ(static_cast<int>(Modality::Unknown), 99);
}
