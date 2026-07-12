#include "cache/composite_summarizer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace aegisgate;

namespace {

class FakeSummarizer : public ConversationSummarizer {
public:
    explicit FakeSummarizer(std::string fixed, std::string name = "Fake")
        : fixed_(std::move(fixed)), name_(std::move(name)) {}

    std::string summarize(const std::vector<Message>&) override {
        ++call_count_;
        return fixed_;
    }
    std::string name() const override { return name_; }

    size_t callCount() const { return call_count_.load(); }

private:
    std::string fixed_;
    std::string name_;
    std::atomic<size_t> call_count_{0};
};

} // namespace

TEST(CompositeSummarizerTest, PrimaryNonEmpty_ReturnsPrimaryNoFallback) {
    auto primary = std::make_unique<FakeSummarizer>("primary-out", "Primary");
    auto fallback = std::make_unique<FakeSummarizer>("fallback-out", "Fallback");
    auto* primary_raw = primary.get();
    auto* fallback_raw = fallback.get();
    CompositeSummarizer comp(std::move(primary), std::move(fallback));

    auto out = comp.summarize({{"user", "hi"}});
    EXPECT_EQ(out, "primary-out");
    EXPECT_EQ(comp.fallbackCount(), 0u);
    EXPECT_EQ(primary_raw->callCount(), 1u);
    EXPECT_EQ(fallback_raw->callCount(), 0u);
}

TEST(CompositeSummarizerTest, PrimaryEmpty_TriggersFallback) {
    auto primary = std::make_unique<FakeSummarizer>("", "Primary");
    auto fallback = std::make_unique<FakeSummarizer>("fallback-out", "Fallback");
    auto* fallback_raw = fallback.get();
    CompositeSummarizer comp(std::move(primary), std::move(fallback));

    auto out = comp.summarize({{"user", "hi"}});
    EXPECT_EQ(out, "fallback-out");
    EXPECT_EQ(comp.fallbackCount(), 1u);
    EXPECT_EQ(fallback_raw->callCount(), 1u);
}

TEST(CompositeSummarizerTest, FallbackCountAccumulates) {
    auto primary = std::make_unique<FakeSummarizer>("");
    auto fallback = std::make_unique<FakeSummarizer>("out");
    CompositeSummarizer comp(std::move(primary), std::move(fallback));

    for (int i = 0; i < 5; ++i) comp.summarize({{"user", "x"}});
    EXPECT_EQ(comp.fallbackCount(), 5u);
}

TEST(CompositeSummarizerTest, NameJoinsPrimaryAndFallback) {
    CompositeSummarizer comp(
        std::make_unique<FakeSummarizer>("o", "Onnx"),
        std::make_unique<FakeSummarizer>("o", "RuleBased"));
    EXPECT_EQ(comp.name(), "Onnx+RuleBased");
}

TEST(CompositeSummarizerTest, ConcurrentFallbackCountIsThreadSafe) {
    auto primary = std::make_unique<FakeSummarizer>("");
    auto fallback = std::make_unique<FakeSummarizer>("out");
    CompositeSummarizer comp(std::move(primary), std::move(fallback));

    constexpr int kThreads = 4;
    constexpr int kCallsPerThread = 100;
    std::vector<std::thread> ths;
    for (int i = 0; i < kThreads; ++i) {
        ths.emplace_back([&] {
            for (int j = 0; j < kCallsPerThread; ++j) {
                comp.summarize({{"user", "x"}});
            }
        });
    }
    for (auto& t : ths) t.join();
    EXPECT_EQ(comp.fallbackCount(),
              static_cast<size_t>(kThreads * kCallsPerThread));
}
