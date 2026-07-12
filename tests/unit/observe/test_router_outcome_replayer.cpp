// Phase 11.2 TASK-20260521-03 — RouterOutcomeReplayer unit tests.
//
// SR6 contract:
//   - Every audit entry's request body MUST flow through PIIFilter::mask()
//     before being passed to the routers under test.
//   - ReplayResult.total_pii_masked == ReplayResult.total_replayed
//     (counter invariant).

#include <gtest/gtest.h>
#include "observe/router_outcome_replayer.h"
#include "guardrail/inbound/pii_filter.h"
#include "gateway/router.h"
#include "gateway/connector/registry.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

using namespace aegisgate;

namespace {

class CountingRouter : public Router {
public:
    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry&) override {
        invocations_++;
        for (const auto& m : ctx.chat_request.messages) {
            seen_messages_.push_back(m.content);
        }
        return ctx.chat_request.model.empty() ? "gpt-4o" : ctx.chat_request.model;
    }
    int invocations_ = 0;
    std::vector<std::string> seen_messages_;
};

std::string writeJsonl(const std::vector<nlohmann::json>& entries,
                        const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path);
    for (const auto& e : entries) f << e.dump() << "\n";
    return path.string();
}

nlohmann::json makeChatEntry(const std::string& req_id,
                              const std::string& model,
                              const std::string& msg) {
    nlohmann::json detail;
    detail["model"] = model;
    detail["messages"] = nlohmann::json::array(
        {{{"role", "user"}, {"content", msg}}});
    nlohmann::json e;
    e["request_id"] = req_id;
    e["action"] = "chat_request";
    e["detail"] = detail.dump();
    return e;
}

}  // namespace

class RouterOutcomeReplayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        baseline_ = std::make_shared<CountingRouter>();
        new_router_ = std::make_shared<CountingRouter>();
        pii_ = std::make_shared<PIIFilter>();
    }
    std::shared_ptr<CountingRouter> baseline_;
    std::shared_ptr<CountingRouter> new_router_;
    std::shared_ptr<PIIFilter> pii_;
    ConnectorRegistry registry_;
};

// T1: 100 entries replayed, both routers invoked equal count.
TEST_F(RouterOutcomeReplayerTest, ReplaysAllChatRequestEntries) {
    std::vector<nlohmann::json> entries;
    for (int i = 0; i < 100; ++i) {
        entries.push_back(makeChatEntry(
            "req-" + std::to_string(i), "gpt-4o", "hello world"));
    }
    auto path = writeJsonl(entries, "test_replayer_basic.jsonl");

    RouterOutcomeReplayer replayer(baseline_, new_router_, pii_);
    ReplayConfig cfg;
    cfg.audit_log_path = path;
    cfg.limit = 1000;
    auto result = replayer.replay(cfg, registry_);

    EXPECT_EQ(result.total_entries_read, 100);
    EXPECT_EQ(result.total_replayed, 100);
    EXPECT_EQ(result.total_skipped_invalid, 0);
    EXPECT_EQ(baseline_->invocations_, 100);
    EXPECT_EQ(new_router_->invocations_, 100);

    std::filesystem::remove(path);
}

// === M3 + M4 anchor: PII mask MUST be invoked AND counter MUST advance. ===
// Mutation injection log:
//   M3 Inject: router_outcome_replayer.cpp — comment out pii_->mask() call
//      Expected: this test FAILs (masked content == original "13800138000")
//      Verified: 2026-05-22, exit_code=1
//      Restore + verified PASS.
//   M4 Inject: router_outcome_replayer.cpp — comment out total_pii_masked++
//      Expected: this test FAILs (total_pii_masked stays 0)
//      Verified: 2026-05-22, exit_code=1
//      Restore + verified PASS.
TEST_F(RouterOutcomeReplayerTest, replayer_masks_all_request_bodies) {
    std::vector<nlohmann::json> entries;
    entries.push_back(makeChatEntry(
        "req-pii-1", "gpt-4o",
        "my phone is 13800138000 and email foo@example.com"));
    entries.push_back(makeChatEntry(
        "req-pii-2", "gpt-4o",
        "id card 110101199001010013 should be hidden"));
    auto path = writeJsonl(entries, "test_replayer_pii.jsonl");

    RouterOutcomeReplayer replayer(baseline_, new_router_, pii_);
    ReplayConfig cfg;
    cfg.audit_log_path = path;
    cfg.limit = 100;
    auto result = replayer.replay(cfg, registry_);

    EXPECT_EQ(result.total_replayed, 2);
    EXPECT_EQ(result.total_pii_masked, 2)
        << "M4: counter invariant total_pii_masked == total_replayed";

    // M3: none of the seen messages may contain the raw PII.
    auto contains = [&](const std::string& needle) {
        for (const auto& m : baseline_->seen_messages_) {
            if (m.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    EXPECT_FALSE(contains("13800138000"))
        << "M3: phone number leaked into router input";
    EXPECT_FALSE(contains("110101199001010013"))
        << "M3: id card leaked into router input";
    // Replacement markers must be present somewhere across the 2 entries.
    EXPECT_TRUE(contains("[PHONE]"))
        << "M3: PIIFilter [PHONE] marker missing";
    EXPECT_TRUE(contains("[ID_CARD]"))
        << "M3: PIIFilter [ID_CARD] marker missing";

    std::filesystem::remove(path);
}

// T3: non-chat_request entries skipped.
TEST_F(RouterOutcomeReplayerTest, SkipsNonChatRequestEntries) {
    std::vector<nlohmann::json> entries;
    entries.push_back(makeChatEntry("req-1", "gpt-4o", "ok"));
    {
        nlohmann::json e;
        e["request_id"] = "req-2";
        e["action"] = "guardrail_blocked";
        e["detail"] = "blocked";
        entries.push_back(e);
    }
    entries.push_back(makeChatEntry("req-3", "gpt-4o", "ok"));
    auto path = writeJsonl(entries, "test_replayer_skip.jsonl");

    RouterOutcomeReplayer replayer(baseline_, new_router_, pii_);
    ReplayConfig cfg;
    cfg.audit_log_path = path;
    cfg.limit = 100;
    auto result = replayer.replay(cfg, registry_);

    EXPECT_EQ(result.total_entries_read, 3);
    EXPECT_EQ(result.total_replayed, 2);
    EXPECT_EQ(baseline_->invocations_, 2);

    std::filesystem::remove(path);
}

// T4: malformed JSON detail counts as skipped_invalid.
TEST_F(RouterOutcomeReplayerTest, SkipsInvalidDetailJson) {
    std::vector<nlohmann::json> entries;
    {
        nlohmann::json e;
        e["request_id"] = "req-1";
        e["action"] = "chat_request";
        e["detail"] = "not a valid json string";
        entries.push_back(e);
    }
    entries.push_back(makeChatEntry("req-2", "gpt-4o", "good"));
    auto path = writeJsonl(entries, "test_replayer_invalid.jsonl");

    RouterOutcomeReplayer replayer(baseline_, new_router_, pii_);
    ReplayConfig cfg;
    cfg.audit_log_path = path;
    cfg.limit = 100;
    auto result = replayer.replay(cfg, registry_);

    EXPECT_EQ(result.total_replayed, 1);
    EXPECT_EQ(result.total_skipped_invalid, 1);

    std::filesystem::remove(path);
}

// T5: limit respected.
TEST_F(RouterOutcomeReplayerTest, LimitTruncatesReplay) {
    std::vector<nlohmann::json> entries;
    for (int i = 0; i < 50; ++i) {
        entries.push_back(makeChatEntry(
            "req-" + std::to_string(i), "gpt-4o", "ok"));
    }
    auto path = writeJsonl(entries, "test_replayer_limit.jsonl");

    RouterOutcomeReplayer replayer(baseline_, new_router_, pii_);
    ReplayConfig cfg;
    cfg.audit_log_path = path;
    cfg.limit = 7;
    auto result = replayer.replay(cfg, registry_);

    EXPECT_EQ(result.total_replayed, 7);
    EXPECT_EQ(baseline_->invocations_, 7);

    std::filesystem::remove(path);
}

// T6: missing file returns clean empty result (no crash).
TEST_F(RouterOutcomeReplayerTest, MissingFileReturnsEmptyResult) {
    RouterOutcomeReplayer replayer(baseline_, new_router_, pii_);
    ReplayConfig cfg;
    cfg.audit_log_path = "/nonexistent/audit.jsonl";
    cfg.limit = 10;
    auto result = replayer.replay(cfg, registry_);

    EXPECT_EQ(result.total_entries_read, 0);
    EXPECT_EQ(result.total_replayed, 0);
}
