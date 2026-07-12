#include "cache/onnx_summarizer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace aegisgate;

// These tests run regardless of AEGISGATE_ENABLE_ONNX:
// when ONNX is off the summarizer is permanently not-ready and returns "".
// When ONNX is on but the model path does not exist, the same behavior is
// observed (load fails -> ready_=false).

TEST(OnnxSummarizerTest, ReturnsEmptyIfModelMissing) {
    OnnxSummarizer s("/nonexistent/path.onnx");
    EXPECT_FALSE(s.isReady());
    EXPECT_EQ(s.summarize({{"user", "hello"}}), "");
}

TEST(OnnxSummarizerTest, NameIsOnnx) {
    OnnxSummarizer s("/nonexistent/path.onnx");
    EXPECT_EQ(s.name(), "Onnx");
}

TEST(OnnxSummarizerTest, EmptyMessagesProducesEmpty) {
    OnnxSummarizer s("/nonexistent/path.onnx");
    EXPECT_EQ(s.summarize({}), "");
}

TEST(OnnxSummarizerTest, TimeoutZeroProducesEmpty_SR7) {
    OnnxSummarizer s("/tmp/test-model.onnx", std::chrono::milliseconds(0));
    EXPECT_EQ(s.summarize({{"user", "x"}}), "");
}

TEST(OnnxSummarizerTest, TimeoutPositiveAccepted) {
    OnnxSummarizer s("/tmp/test-model.onnx", std::chrono::milliseconds(500));
    EXPECT_EQ(s.timeoutMs(), 500);
}

TEST(OnnxSummarizerTest, InputTruncationDoesNotCrash_SR7) {
    OnnxSummarizer s("/nonexistent/path.onnx");
    std::vector<Message> msgs(100, {"user", std::string(200, 'x')});
    auto out = s.summarize(msgs);
    EXPECT_EQ(out, "");
}

TEST(OnnxSummarizerTest, NotReadyRepeatedCallsAreSafe) {
    OnnxSummarizer s("/nonexistent/path.onnx");
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(s.summarize({{"user", "q"}}), "");
    }
}

TEST(OnnxSummarizerTest, ConcurrentSummarizeCallsAreSafe) {
    OnnxSummarizer s("/nonexistent/path.onnx");
    std::vector<std::thread> ths;
    std::atomic<int> done{0};
    for (int i = 0; i < 4; ++i) {
        ths.emplace_back([&] {
            for (int j = 0; j < 20; ++j) s.summarize({{"user", "q"}});
            ++done;
        });
    }
    for (auto& t : ths) t.join();
    EXPECT_EQ(done.load(), 4);
}
