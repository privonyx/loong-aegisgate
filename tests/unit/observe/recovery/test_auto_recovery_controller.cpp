// Phase 11.4 self-healing ops (TASK-20260519-01) — Epic 1 task 1.2.
//
// AutoRecoveryController base class tests.
//
// Coverage (6 tests):
//   1. CooldownActiveSkipsEvaluation         — M1 mutation target
//   2. CooldownExpiredAllowsEvaluation
//   3. SR17DisabledSkipsEvaluation           — M2 mutation target
//   4. BreachedTriggersRecovery
//   5. NotBreachedDoesNotTrigger
//   6. PerSubjectCooldownIsolation

#include "common/clock.h"
#include "observe/recovery/auto_recovery_controller.h"
#include "observe/recovery/signal_snapshot.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace aegisgate;

namespace {

// Concrete derived class for testing the base class behaviour.
class StubRecoveryController : public AutoRecoveryController {
public:
    explicit StubRecoveryController(Deps d) : AutoRecoveryController(std::move(d)) {}

    // Expose the protected entry point for direct tests.
    using AutoRecoveryController::evaluate;

    std::vector<std::string> collect_calls;
    std::vector<std::string> evaluate_calls;
    std::vector<std::string> trigger_calls;

    bool         force_breached = false;
    std::string  forced_reason  = "test_breach";

protected:
    SignalSnapshot collectMetrics(std::string_view subject,
                                   std::chrono::seconds /*window*/) override {
        collect_calls.emplace_back(subject);
        SignalSnapshot s;
        s.timestamp_ms = 1000;
        s.sample_count = 100;
        return s;
    }

    BreachVerdict evaluateBreach(std::string_view subject,
                                   const SignalSnapshot& /*sig*/,
                                   const SignalSnapshot& /*baseline*/) override {
        evaluate_calls.emplace_back(subject);
        BreachVerdict v;
        v.breached = force_breached;
        v.reason   = forced_reason;
        return v;
    }

    void triggerRecovery(std::string_view subject,
                          const BreachVerdict& /*verdict*/) override {
        trigger_calls.emplace_back(subject);
    }
};

AutoRecoveryController::Deps makeDeps(common::FakeClock* clock,
                                       bool enabled = true,
                                       std::int64_t cooldown_ms = 5 * 60 * 1000) {
    AutoRecoveryController::Deps d;
    d.clock              = clock;
    d.autonomy_enabled   = [enabled] { return enabled; };
    d.cooldown_ms        = cooldown_ms;
    return d;
}

} // namespace

// --- Test 1: cooldown active ------------------------------------------------

TEST(AutoRecoveryControllerTest, CooldownActiveSkipsEvaluation) {
    common::FakeClock clock(0);
    StubRecoveryController ctrl(makeDeps(&clock));
    ctrl.force_breached = true;

    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());
    EXPECT_EQ(ctrl.trigger_calls.size(), 1u) << "first call must trigger";
    EXPECT_EQ(ctrl.collect_calls.size(), 1u);

    // Advance < cooldown (default 5min). Evaluate again — must early-exit
    // before collectMetrics.
    clock.advance(std::chrono::minutes(2));
    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());

    EXPECT_EQ(ctrl.collect_calls.size(), 1u)
        << "collectMetrics must NOT be called while cooldown is active "
        << "(M1 mutation target)";
    EXPECT_EQ(ctrl.trigger_calls.size(), 1u);
}

// --- Test 2: cooldown expired -----------------------------------------------

TEST(AutoRecoveryControllerTest, CooldownExpiredAllowsEvaluation) {
    common::FakeClock clock(0);
    StubRecoveryController ctrl(makeDeps(&clock));
    ctrl.force_breached = true;

    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());
    ASSERT_EQ(ctrl.trigger_calls.size(), 1u);

    clock.advance(std::chrono::minutes(6));  // > 5min cooldown
    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());
    EXPECT_EQ(ctrl.collect_calls.size(), 2u);
    EXPECT_EQ(ctrl.trigger_calls.size(), 2u);
}

// --- Test 3: SR17 enable flag ----------------------------------------------

TEST(AutoRecoveryControllerTest, SR17DisabledSkipsEvaluation) {
    common::FakeClock clock(0);
    auto deps = makeDeps(&clock, /*enabled=*/false);
    StubRecoveryController ctrl(deps);
    ctrl.force_breached = true;

    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());

    EXPECT_TRUE(ctrl.collect_calls.empty())
        << "SR17 disabled must short-circuit before collectMetrics "
        << "(M2 mutation target)";
    EXPECT_TRUE(ctrl.trigger_calls.empty());
}

// --- Test 4: breach triggers recovery --------------------------------------

TEST(AutoRecoveryControllerTest, BreachedTriggersRecovery) {
    common::FakeClock clock(1000);
    StubRecoveryController ctrl(makeDeps(&clock));
    ctrl.force_breached = true;
    ctrl.forced_reason  = "p99_breach";

    ctrl.evaluate("subject-x", clock.nowMillis(), clock.wallClockMillis());

    ASSERT_EQ(ctrl.collect_calls.size(), 1u);
    ASSERT_EQ(ctrl.evaluate_calls.size(), 1u);
    ASSERT_EQ(ctrl.trigger_calls.size(), 1u);
    EXPECT_EQ(ctrl.trigger_calls[0], "subject-x");
}

// --- Test 5: no breach, no trigger -----------------------------------------

TEST(AutoRecoveryControllerTest, NotBreachedDoesNotTrigger) {
    common::FakeClock clock(0);
    StubRecoveryController ctrl(makeDeps(&clock));
    ctrl.force_breached = false;

    ctrl.evaluate("subject-x", clock.nowMillis(), clock.wallClockMillis());

    EXPECT_EQ(ctrl.collect_calls.size(), 1u);
    EXPECT_EQ(ctrl.evaluate_calls.size(), 1u);
    EXPECT_TRUE(ctrl.trigger_calls.empty());
}

// --- Test 6: per-subject cooldown isolation ---------------------------------

TEST(AutoRecoveryControllerTest, PerSubjectCooldownIsolation) {
    common::FakeClock clock(0);
    StubRecoveryController ctrl(makeDeps(&clock));
    ctrl.force_breached = true;

    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());
    ASSERT_EQ(ctrl.trigger_calls.size(), 1u);

    // No clock advance — subject B should still trigger because its own
    // cooldown has never started.
    ctrl.evaluate("rollout-b", clock.nowMillis(), clock.wallClockMillis());
    EXPECT_EQ(ctrl.trigger_calls.size(), 2u);

    // Subject A still in cooldown → skipped.
    ctrl.evaluate("rollout-a", clock.nowMillis(), clock.wallClockMillis());
    EXPECT_EQ(ctrl.trigger_calls.size(), 2u);
}
