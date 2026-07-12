// Baseline tests preserved verbatim from Phase 9.x (T1-T5 below).
// Phase 11.2 TASK-20260521-03 extends with T6-T8 for the new
// aegisctl replay-routes subcommand (offline router comparison + SR6
// PII masking).

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <nlohmann/json.hpp>

#include "cli/replay_cli.h"

namespace {

std::string createTempAuditLog(const std::vector<nlohmann::json>& entries) {
    auto path = std::filesystem::temp_directory_path() / "test_replay.jsonl";
    std::ofstream f(path);
    for (const auto& e : entries) {
        f << e.dump() << "\n";
    }
    return path.string();
}

} // namespace

TEST(ReplayParsingTest, ValidAuditEntryHasRequiredFields) {
    nlohmann::json entry;
    entry["request_id"] = "req-001";
    entry["action"] = "chat_request";
    nlohmann::json detail;
    detail["model"] = "gpt-4";
    detail["messages"] = nlohmann::json::array({{{"role", "user"}, {"content", "hello"}}});
    entry["detail"] = detail.dump();

    auto parsed_detail = nlohmann::json::parse(entry["detail"].get<std::string>());
    EXPECT_TRUE(parsed_detail.contains("model"));
    EXPECT_TRUE(parsed_detail.contains("messages"));
    EXPECT_EQ(parsed_detail["model"], "gpt-4");
    EXPECT_EQ(parsed_detail["messages"].size(), 1u);
}

TEST(ReplayParsingTest, SkipsNonRequestEntries) {
    nlohmann::json entry;
    entry["request_id"] = "req-002";
    entry["action"] = "guardrail_blocked";
    entry["detail"] = "blocked by policy";

    auto action = entry.value("action", "");
    EXPECT_NE(action, "request_received");
    EXPECT_NE(action, "chat_request");
}

TEST(ReplayParsingTest, SkipsInvalidJsonDetail) {
    nlohmann::json entry;
    entry["request_id"] = "req-003";
    entry["action"] = "chat_request";
    entry["detail"] = "not a json string";

    bool parsed_ok = false;
    try {
        auto d = nlohmann::json::parse(entry["detail"].get<std::string>());
        parsed_ok = true;
    } catch (...) {}
    EXPECT_FALSE(parsed_ok);
}

TEST(ReplayParsingTest, ReadsMultipleEntriesFromFile) {
    std::vector<nlohmann::json> entries;
    for (int i = 0; i < 5; ++i) {
        nlohmann::json e;
        e["request_id"] = "req-" + std::to_string(i);
        e["action"] = "chat_request";
        nlohmann::json d;
        d["model"] = "gpt-4";
        d["messages"] = nlohmann::json::array({{{"role", "user"}, {"content", "msg " + std::to_string(i)}}});
        e["detail"] = d.dump();
        entries.push_back(e);
    }

    auto path = createTempAuditLog(entries);
    std::ifstream f(path);
    ASSERT_TRUE(f.is_open());

    int count = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto entry = nlohmann::json::parse(line);
        EXPECT_EQ(entry["action"], "chat_request");
        ++count;
    }
    EXPECT_EQ(count, 5);

    std::filesystem::remove(path);
}

TEST(ReplayParsingTest, LimitRespectsMaxCount) {
    std::vector<nlohmann::json> entries;
    for (int i = 0; i < 10; ++i) {
        nlohmann::json e;
        e["request_id"] = "req-" + std::to_string(i);
        e["action"] = "chat_request";
        nlohmann::json d;
        d["model"] = "test-model";
        d["messages"] = nlohmann::json::array({{{"role", "user"}, {"content", "test"}}});
        e["detail"] = d.dump();
        entries.push_back(e);
    }

    auto path = createTempAuditLog(entries);
    std::ifstream f(path);

    int limit = 3, sent = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (limit > 0 && sent >= limit) break;
        auto entry = nlohmann::json::parse(line);
        if (entry.value("action", "") == "chat_request") ++sent;
    }
    EXPECT_EQ(sent, 3);

    std::filesystem::remove(path);
}

// === Phase 11.2 TASK-20260521-03 — replay-routes CLI coverage ===

namespace {

std::string writeChatRequestJsonl(int count, const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path);
    for (int i = 0; i < count; ++i) {
        nlohmann::json detail;
        detail["model"] = "gpt-4o";
        detail["messages"] = nlohmann::json::array(
            {{{"role", "user"},
              {"content", "phone 13800138000 token id 110101199001010013"}}});
        nlohmann::json e;
        e["request_id"] = "req-" + std::to_string(i);
        e["action"] = "chat_request";
        e["detail"] = detail.dump();
        f << e.dump() << "\n";
    }
    return path.string();
}

}  // namespace

// T6: replay-routes without --audit-log returns usage error (exit 2).
TEST(ReplayRoutesCliTest, MissingAuditLogFails) {
    std::ostringstream out, err;
    int rc = aegisgate::cli::runReplayRoutesCommand({}, out, err);
    EXPECT_EQ(rc, 2);
    EXPECT_NE(err.str().find("--audit-log"), std::string::npos);
}

// T7: replay-routes parser defaults dry_run=true.
TEST(ReplayRoutesCliTest, ParserDefaultsToDryRun) {
    auto args = aegisgate::cli::parseReplayRoutesArgs(
        {"--audit-log", "/tmp/x.jsonl"});
    EXPECT_TRUE(args.dry_run);
    EXPECT_EQ(args.baseline_strategy, "hybrid");
    EXPECT_EQ(args.new_strategy, "cost-first");
    EXPECT_EQ(args.limit, 100);
}

// T8: replay-routes JSON output contains total_pii_masked (SR6 surfacing).
TEST(ReplayRoutesCliTest, JsonOutputIncludesPiiMaskedCount) {
    auto path = writeChatRequestJsonl(3, "test_replay_routes_t8.jsonl");

    std::ostringstream out, err;
    int rc = aegisgate::cli::runReplayRoutesCommand(
        {"--audit-log", path, "--limit", "10"}, out, err);
    EXPECT_EQ(rc, 0) << err.str();

    auto j = nlohmann::json::parse(out.str());
    EXPECT_TRUE(j.contains("total_pii_masked"));
    EXPECT_EQ(j["total_pii_masked"], 3);
    EXPECT_EQ(j["total_replayed"], 3);
    EXPECT_EQ(j["total_pii_masked"], j["total_replayed"])
        << "SR6 invariant: counter == replayed";

    std::filesystem::remove(path);
}

// T9: unrecognized flag yields exit 2 with helpful error.
TEST(ReplayRoutesCliTest, UnknownFlagFails) {
    std::ostringstream out, err;
    int rc = aegisgate::cli::runReplayRoutesCommand(
        {"--audit-log", "/tmp/x.jsonl", "--bogus"}, out, err);
    EXPECT_EQ(rc, 2);
    EXPECT_NE(err.str().find("unrecognized"), std::string::npos);
}
