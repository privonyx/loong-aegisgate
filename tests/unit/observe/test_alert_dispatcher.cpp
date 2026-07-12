#include <gtest/gtest.h>
#include "observe/alert_dispatcher.h"

using namespace aegisgate;

namespace {

Alert makeTestAlert(const std::string& rule_id = "test_rule",
                    AlertSeverity severity = AlertSeverity::Warning) {
    Alert a;
    a.rule_id = rule_id;
    a.description = "threshold exceeded";
    a.severity = severity;
    a.current_value = 100.0;
    a.threshold = 50.0;
    a.timestamp = "2026-03-25T10:00:00Z";
    return a;
}

} // namespace

TEST(AlertDispatcherTest, DispatchToMultipleChannels) {
    AlertDispatcher dispatcher;

    std::vector<Alert> ch1_received, ch2_received;
    dispatcher.addChannel("ch1", [&](const Alert& a) { ch1_received.push_back(a); });
    dispatcher.addChannel("ch2", [&](const Alert& a) { ch2_received.push_back(a); });

    dispatcher.dispatch(makeTestAlert());

    ASSERT_EQ(ch1_received.size(), 1u);
    ASSERT_EQ(ch2_received.size(), 1u);
    EXPECT_EQ(ch1_received[0].rule_id, "test_rule");
    EXPECT_EQ(ch2_received[0].rule_id, "test_rule");
}

TEST(AlertDispatcherTest, ChannelFailureDoesNotBlockOthers) {
    AlertDispatcher dispatcher;

    std::vector<Alert> ch2_received;
    dispatcher.addChannel("failing", [](const Alert&) {
        throw std::runtime_error("channel failure");
    });
    dispatcher.addChannel("working", [&](const Alert& a) { ch2_received.push_back(a); });

    EXPECT_NO_THROW(dispatcher.dispatch(makeTestAlert()));
    EXPECT_EQ(ch2_received.size(), 1u);
}

TEST(AlertDispatcherTest, RemoveChannel) {
    AlertDispatcher dispatcher;

    int count = 0;
    dispatcher.addChannel("temp", [&](const Alert&) { ++count; });
    EXPECT_EQ(dispatcher.channelCount(), 1u);

    dispatcher.dispatch(makeTestAlert());
    EXPECT_EQ(count, 1);

    dispatcher.removeChannel("temp");
    EXPECT_EQ(dispatcher.channelCount(), 0u);

    dispatcher.dispatch(makeTestAlert());
    EXPECT_EQ(count, 1);
}

TEST(AlertDispatcherTest, EmptyDispatcherIsNoop) {
    AlertDispatcher dispatcher;
    EXPECT_NO_THROW(dispatcher.dispatch(makeTestAlert()));
    EXPECT_EQ(dispatcher.channelCount(), 0u);
}

// P0-G (TASK-20260701-01): channel failures were swallowed with only a log
// line, so an operator could not tell alert delivery was broken. Expose
// success/failure counters so reliability is observable.
TEST(AlertDispatcherTest, TracksDeliveryFailureCount) {
    AlertDispatcher dispatcher;
    dispatcher.addChannel("failing", [](const Alert&) {
        throw std::runtime_error("channel failure");
    });
    dispatcher.addChannel("working", [](const Alert&) {});

    dispatcher.dispatch(makeTestAlert());
    dispatcher.dispatch(makeTestAlert());

    EXPECT_EQ(dispatcher.failedTotal(), 2u);
    EXPECT_EQ(dispatcher.deliveredTotal(), 2u);
}

// P0-G: dispatching with zero channels silently drops the alert. That must be
// counted so "alerting configured but going nowhere" is detectable.
TEST(AlertDispatcherTest, CountsDropWhenNoChannels) {
    AlertDispatcher dispatcher;
    dispatcher.dispatch(makeTestAlert());
    dispatcher.dispatch(makeTestAlert());
    EXPECT_EQ(dispatcher.droppedNoChannelTotal(), 2u);
    EXPECT_EQ(dispatcher.deliveredTotal(), 0u);
}
