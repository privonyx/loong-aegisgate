// Phase 9.3 Epic 4 Task 4.1 — per-user Submit rate limit (SR10).
//
// Wires the existing sharded RateLimiter into ConfigServiceCore. key = user
// id, bucket sized so the 10th submit in a window passes and the 11th trips
// RATE_LIMITED. Cross-user isolation is verified so one noisy operator never
// blocks the rest of the team.

#include "control_plane/config_service_core.h"
#include "gateway/rate_limiter.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace aegisgate {
namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000LL;

class SubmitRateLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());

        // 10 tokens, refill 1/s so a burst of 10 passes but the 11th within
        // the same second is denied. Matches the plan default of
        // "10 tokens/min/user" scaled for deterministic unit testing.
        RateLimiter::Config rl_cfg;
        rl_cfg.max_tokens = 10;
        rl_cfg.refill_rate = 0.01;  // extremely slow refill so the 11th denies
        limiter_ = std::make_unique<RateLimiter>(rl_cfg);

        ConfigServiceCore::Deps deps;
        deps.store = store_.get();
        deps.audit = audit_.get();
        deps.clock = []() { return kT0; };
        deps.validator = [](const std::string&) {
            return std::vector<Config::ValidationIssue>{};
        };
        deps.rate_limit = [this](const std::string& uid) {
            return limiter_->allow(uid);
        };
        svc_ = std::make_unique<ConfigServiceCore>(std::move(deps));
    }

    // Unique yaml per iteration so sha256 dedupe doesn't shadow the rate
    // limit check (dedupe runs earlier — but we still want every call to
    // reach the rate-limit stage deterministically).
    static std::string yamlFor(int i) {
        return "seq: " + std::to_string(i) + "\n";
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<RateLimiter>           limiter_;
    std::unique_ptr<ConfigServiceCore>     svc_;
};

TEST_F(SubmitRateLimitTest, FirstTenSubmissionsAllowed) {
    for (int i = 0; i < 10; ++i) {
        auto r = svc_->submit(yamlFor(i), "alice", "", false);
        EXPECT_EQ(r.error_code, "") << "iter " << i << " msg=" << r.error_message;
    }
}

TEST_F(SubmitRateLimitTest, EleventhSubmissionRateLimited) {
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(svc_->submit(yamlFor(i), "alice", "", false).error_code, "");
    }
    auto r = svc_->submit(yamlFor(10), "alice", "", false);
    EXPECT_EQ(r.error_code, "RATE_LIMITED");
    // Store must not have grown past 10 persisted records.
    EXPECT_EQ(store_->listConfigVersions({}).size(), 10u);
}

TEST_F(SubmitRateLimitTest, DifferentUsersIsolated) {
    // Alice burns her budget, Bob must still be able to submit.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(svc_->submit(yamlFor(i), "alice", "", false).error_code, "");
    }
    EXPECT_EQ(svc_->submit(yamlFor(10), "alice", "", false).error_code,
              "RATE_LIMITED");

    auto r = svc_->submit(yamlFor(100), "bob", "", false);
    EXPECT_EQ(r.error_code, "");
}

TEST_F(SubmitRateLimitTest, RateLimitRunsAfterDedupe) {
    // A duplicate submission should still return ALREADY_EXISTS rather than
    // RATE_LIMITED, because dedupe happens before the rate-limit gate. This
    // matters for operator experience: an accidental re-submit of the same
    // YAML should not look like the user got throttled.
    std::string yaml = "foo: bar\n";
    ASSERT_EQ(svc_->submit(yaml, "alice", "", false).error_code, "");
    auto r = svc_->submit(yaml, "alice", "", false);
    EXPECT_EQ(r.error_code, "ALREADY_EXISTS");
}

TEST_F(SubmitRateLimitTest, ValidateOnlyStillConsumesBudget) {
    // validate_only=true must still consume a token — otherwise a malicious
    // user could DoS the Config::validate path (expensive) with no ceiling.
    for (int i = 0; i < 10; ++i) {
        auto r = svc_->submit(yamlFor(i), "alice", "", /*validate_only=*/true);
        EXPECT_EQ(r.error_code, "") << "iter " << i;
    }
    auto r = svc_->submit(yamlFor(10), "alice", "", /*validate_only=*/true);
    EXPECT_EQ(r.error_code, "RATE_LIMITED");
}

} // namespace
} // namespace aegisgate
