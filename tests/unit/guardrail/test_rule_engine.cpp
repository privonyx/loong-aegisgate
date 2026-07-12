#include <gtest/gtest.h>
#include "guardrail/rule_engine.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"
#include <fstream>
#include <filesystem>
#include <thread>
#include <vector>

using namespace aegisgate;

class RuleEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto eg = FeatureGate::createUnlocked(Edition::Enterprise);
        enterprise_gate_ = std::make_unique<FeatureGate>(std::move(eg));
        community_gate_ = std::make_unique<FeatureGate>(Edition::Community);
    }
    std::unique_ptr<FeatureGate> enterprise_gate_;
    std::unique_ptr<FeatureGate> community_gate_;
};

TEST_F(RuleEngineTest, EnterpriseRuleMatches) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r1", "Block dangerous keywords", 10, RuleAction::Block);
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "dangerous");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This is dangerous content"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.rule_id, "r1");
    EXPECT_EQ(result.action, RuleAction::Block);
}

// P1-C: a Block rule rejecting a request is a security-relevant decision and
// MUST be audited (RuleEngine previously had no audit wiring at all).
TEST_F(RuleEngineTest, BlockWritesAudit) {
    AuditLogger audit;
    std::vector<AuditEntry> captured;
    audit.setSink([&](const AuditEntry& e) { captured.push_back(e); });

    RuleEngine engine(*enterprise_gate_);
    engine.setAuditLogger(&audit);
    engine.addRule("r1", "Block dangerous keywords", 10, RuleAction::Block);
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "dangerous");

    RequestContext ctx;
    ctx.request_id = "rule-req";
    ctx.tenant_id = "tenant-y";
    ctx.chat_request.messages = {{"user", "This is dangerous content"}};

    EXPECT_EQ(engine.process(ctx), StageResult::Reject);
    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(captured.back().stage_name, "RuleEngine");
    EXPECT_EQ(captured.back().action, "blocked");
    EXPECT_NE(captured.back().detail.find("r1"), std::string::npos);
}

// C3 (REV20260702-C3): the raw message uses full-width letters that don't match
// the keyword; the normalized view does. extractText must scan normalized text
// (via ctx.scanText) so this obfuscation bypass is caught.
TEST_F(RuleEngineTest, ScansNormalizedText) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r1", "Block dangerous keywords", 10, RuleAction::Block);
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "dangerous");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "ｄａｎｇｅｒｏｕｓ content"}};
    ctx.normalized_messages = {"dangerous content"};
    ctx.input_preprocessed = true;

    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.rule_id, "r1");
}

// P1-C: a non-blocking (Warn) match must NOT emit a "blocked" audit entry.
TEST_F(RuleEngineTest, WarnDoesNotWriteBlockedAudit) {
    AuditLogger audit;
    std::vector<AuditEntry> captured;
    audit.setSink([&](const AuditEntry& e) { captured.push_back(e); });

    RuleEngine engine(*enterprise_gate_);
    engine.setAuditLogger(&audit);
    engine.addRule("rw", "Warn only", 5, RuleAction::Warn);
    engine.addConditionToRule("rw", ConditionType::KeywordContains, "warn");

    RequestContext ctx;
    ctx.request_id = "rule-warn";
    ctx.chat_request.messages = {{"user", "please warn me"}};

    EXPECT_EQ(engine.process(ctx), StageResult::Continue);
    for (const auto& e : captured) {
        EXPECT_NE(e.action, "blocked");
    }
}

TEST_F(RuleEngineTest, CommunitySkipsRules) {
    RuleEngine engine(*community_gate_);
    engine.addRule("r1", "Block test", 10, RuleAction::Block);
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "blocked");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This should be blocked"}};
    auto result = engine.evaluate(ctx);
    EXPECT_FALSE(result.matched);
}

TEST_F(RuleEngineTest, RegexConditionMatches) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r2", "Block SQL injection", 20, RuleAction::Block);
    engine.addConditionToRule("r2", ConditionType::RegexMatch,
                              "(?i)(DROP\\s+TABLE|DELETE\\s+FROM)");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Run this: DROP TABLE users"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.rule_id, "r2");
}

TEST_F(RuleEngineTest, LengthCheckWorks) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r3", "Limit input length", 5, RuleAction::Warn);
    engine.addConditionToRule("r3", ConditionType::LengthCheck, "10");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This is a long message that exceeds the limit"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.action, RuleAction::Warn);
}

TEST_F(RuleEngineTest, ModelMatchWorks) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r4", "Block GPT-4 usage", 15, RuleAction::Block);
    engine.addConditionToRule("r4", ConditionType::ModelMatch, "gpt-4");

    RequestContext ctx;
    ctx.chat_request.model = "gpt-4";
    ctx.chat_request.messages = {{"user", "Hello"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
}

TEST_F(RuleEngineTest, PriorityOrderIsRespected) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("low", "Low priority", 1, RuleAction::Warn);
    engine.addConditionToRule("low", ConditionType::KeywordContains, "test");
    engine.addRule("high", "High priority", 100, RuleAction::Block);
    engine.addConditionToRule("high", ConditionType::KeywordContains, "test");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This is a test"}};
    auto result = engine.evaluate(ctx);
    EXPECT_EQ(result.rule_id, "high");
    EXPECT_EQ(result.action, RuleAction::Block);
}

TEST_F(RuleEngineTest, NoMatchReturnsAllow) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r5", "Block bad", 10, RuleAction::Block);
    engine.addConditionToRule("r5", ConditionType::KeywordContains, "nothere");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "Normal text"}};
    auto result = engine.evaluate(ctx);
    EXPECT_FALSE(result.matched);
    EXPECT_EQ(result.action, RuleAction::Allow);
}

TEST_F(RuleEngineTest, PipelineBlocksOnBlock) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r6", "Pipeline block", 10, RuleAction::Block);
    engine.addConditionToRule("r6", ConditionType::KeywordContains, "blocked");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This should be blocked"}};
    EXPECT_EQ(engine.process(ctx), StageResult::Reject);
}

TEST_F(RuleEngineTest, PipelineContinuesOnWarn) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r7", "Pipeline warn", 10, RuleAction::Warn);
    engine.addConditionToRule("r7", ConditionType::KeywordContains, "warned");

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This should be warned"}};
    EXPECT_EQ(engine.process(ctx), StageResult::Continue);
}

TEST_F(RuleEngineTest, RuleCountWorks) {
    RuleEngine engine(*enterprise_gate_);
    EXPECT_EQ(engine.ruleCount(), 0u);
    engine.addRule("a", "A", 1, RuleAction::Allow);
    engine.addRule("b", "B", 2, RuleAction::Allow);
    EXPECT_EQ(engine.ruleCount(), 2u);
}

TEST_F(RuleEngineTest, ModifyActionReplacesKeyword) {
    RuleEngine engine(*enterprise_gate_);
    engine.addRule("r1", "Replace bad", 10, RuleAction::Modify, "[REDACTED]");
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "badword");

    RequestContext ctx;
    ctx.chat_request.messages.push_back({"user", "This has badword in it"});
    auto result = engine.process(ctx);
    EXPECT_EQ(result, StageResult::Continue);
    EXPECT_TRUE(ctx.chat_request.messages[0].content.find("badword") ==
                std::string::npos);
    EXPECT_TRUE(ctx.chat_request.messages[0].content.find("[REDACTED]") !=
                std::string::npos);
}

// --- YAML loading tests ---

class RuleEngineYamlTest : public ::testing::Test {
protected:
    void SetUp() override {
        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("rule_engine_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    void writeYaml(const std::string& filename, const std::string& content) {
        std::ofstream f(tmp_dir_ / filename);
        f << content;
    }

    std::string yamlPath(const std::string& filename) {
        return (tmp_dir_ / filename).string();
    }

    std::unique_ptr<FeatureGate> gate_;
    std::filesystem::path tmp_dir_;
};

TEST_F(RuleEngineYamlTest, LoadFromYaml) {
    writeYaml("rules.yaml", R"(
version: 1
rules:
  - id: test-block
    description: "Block test keyword"
    priority: 10
    enabled: true
    conditions:
      - type: keyword_contains
        value: "forbidden"
    action: block
  - id: test-warn
    description: "Warn on long input"
    priority: 5
    enabled: true
    conditions:
      - type: length_check
        value: "10"
    action: warn
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("rules.yaml")));
    EXPECT_EQ(engine.ruleCount(), 2u);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This is forbidden content"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.rule_id, "test-block");
    EXPECT_EQ(result.action, RuleAction::Block);
}

TEST_F(RuleEngineYamlTest, LoadFromYamlRegex) {
    writeYaml("regex.yaml", R"(
version: 1
rules:
  - id: regex-rule
    description: "Regex rule"
    priority: 10
    enabled: true
    conditions:
      - type: regex_match
        value: "(?i)drop\\s+table"
    action: block
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("regex.yaml")));

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "DROP TABLE users"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.action, RuleAction::Block);
}

TEST_F(RuleEngineYamlTest, LoadFromYamlModifyAction) {
    writeYaml("modify.yaml", R"(
version: 1
rules:
  - id: modify-rule
    description: "Modify rule"
    priority: 10
    enabled: true
    conditions:
      - type: keyword_contains
        value: "secret"
    action: modify
    action_param: "[REDACTED]"
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("modify.yaml")));

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "The secret is here"}};
    engine.process(ctx);
    EXPECT_NE(ctx.chat_request.messages[0].content.find("[REDACTED]"),
              std::string::npos);
}

TEST_F(RuleEngineYamlTest, LoadFromYamlDisabledRuleSkipped) {
    writeYaml("disabled.yaml", R"(
version: 1
rules:
  - id: disabled-rule
    description: "Disabled"
    priority: 10
    enabled: false
    conditions:
      - type: keyword_contains
        value: "anything"
    action: block
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("disabled.yaml")));
    EXPECT_EQ(engine.ruleCount(), 1u);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "anything goes"}};
    auto result = engine.evaluate(ctx);
    EXPECT_FALSE(result.matched);
}

TEST_F(RuleEngineYamlTest, LoadFromYamlInvalidFile) {
    RuleEngine engine(*gate_);
    EXPECT_FALSE(engine.loadFromYaml("/nonexistent/path.yaml"));
    EXPECT_EQ(engine.ruleCount(), 0u);
}

TEST_F(RuleEngineYamlTest, LoadFromYamlInvalidContent) {
    writeYaml("bad.yaml", "this is not: [valid yaml: {{{}}}");

    RuleEngine engine(*gate_);
    EXPECT_FALSE(engine.loadFromYaml(yamlPath("bad.yaml")));
    EXPECT_EQ(engine.ruleCount(), 0u);
}

TEST_F(RuleEngineYamlTest, LoadFromYamlInvalidConditionType) {
    writeYaml("badtype.yaml", R"(
version: 1
rules:
  - id: bad-type
    description: "Invalid condition type"
    priority: 10
    enabled: true
    conditions:
      - type: nonexistent_type
        value: "test"
    action: block
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("badtype.yaml")));
    EXPECT_EQ(engine.ruleCount(), 1u);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "test"}};
    auto result = engine.evaluate(ctx);
    EXPECT_FALSE(result.matched);
}

TEST_F(RuleEngineYamlTest, ReloadRulesHotSwap) {
    writeYaml("v1.yaml", R"(
version: 1
rules:
  - id: v1-rule
    description: "Version 1"
    priority: 10
    enabled: true
    conditions:
      - type: keyword_contains
        value: "alpha"
    action: block
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("v1.yaml")));
    EXPECT_EQ(engine.ruleCount(), 1u);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "alpha test"}};
    EXPECT_TRUE(engine.evaluate(ctx).matched);

    writeYaml("v2.yaml", R"(
version: 2
rules:
  - id: v2-rule
    description: "Version 2"
    priority: 10
    enabled: true
    conditions:
      - type: keyword_contains
        value: "beta"
    action: warn
)");

    EXPECT_TRUE(engine.reloadRules(yamlPath("v2.yaml")));
    EXPECT_EQ(engine.ruleCount(), 1u);

    EXPECT_FALSE(engine.evaluate(ctx).matched);

    RequestContext ctx2;
    ctx2.chat_request.messages = {{"user", "beta test"}};
    auto result = engine.evaluate(ctx2);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.rule_id, "v2-rule");
    EXPECT_EQ(result.action, RuleAction::Warn);
}

TEST_F(RuleEngineYamlTest, ReloadRulesInvalidKeepsOld) {
    writeYaml("good.yaml", R"(
version: 1
rules:
  - id: good-rule
    description: "Good rule"
    priority: 10
    enabled: true
    conditions:
      - type: keyword_contains
        value: "hello"
    action: block
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("good.yaml")));
    EXPECT_EQ(engine.ruleCount(), 1u);

    EXPECT_FALSE(engine.reloadRules("/nonexistent/bad.yaml"));
    EXPECT_EQ(engine.ruleCount(), 1u);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "hello world"}};
    EXPECT_TRUE(engine.evaluate(ctx).matched);
}

TEST_F(RuleEngineYamlTest, ConcurrentReadDuringReload) {
    writeYaml("concurrent.yaml", R"(
version: 1
rules:
  - id: concurrent-rule
    description: "Concurrent"
    priority: 10
    enabled: true
    conditions:
      - type: keyword_contains
        value: "concurrent"
    action: block
)");

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromYaml(yamlPath("concurrent.yaml")));

    std::atomic<bool> stop{false};
    std::atomic<int> eval_count{0};

    auto reader = [&] {
        while (!stop.load()) {
            RequestContext ctx;
            ctx.chat_request.messages = {{"user", "concurrent test"}};
            engine.evaluate(ctx);
            eval_count.fetch_add(1);
        }
    };

    std::thread t1(reader);
    std::thread t2(reader);

    // Give reader threads a moment to start looping before the main
    // thread races through reloads — otherwise on fast machines (and
    // especially under the OTel build matrix scheduling) the reloads
    // finish AND `stop` is flipped to true before either reader gets
    // a chance to execute evaluate() even once, producing a flaky
    // eval_count == 0 failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < 10; ++i) {
        engine.reloadRules(yamlPath("concurrent.yaml"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Let readers run at least briefly after the last reload.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop.store(true);
    t1.join();
    t2.join();

    EXPECT_GT(eval_count.load(), 0);
}

// --- PersistentStore integration tests ---

class RuleEngineStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        gate_ = std::make_unique<FeatureGate>(
            FeatureGate::createUnlocked(Edition::Enterprise));
        store_.initialize();
    }

    std::unique_ptr<FeatureGate> gate_;
    MemoryPersistentStore store_;
};

TEST_F(RuleEngineStoreTest, SaveToStoreCreatesVersion) {
    RuleEngine engine(*gate_);
    engine.addRule("r1", "Block test", 10, RuleAction::Block);
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "forbidden");

    EXPECT_TRUE(engine.saveToStore(store_, "tenant-1"));

    auto active = store_.getActiveRuleSet("tenant-1");
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version, 1);
    EXPECT_TRUE(active->is_active);
    EXPECT_FALSE(active->rules_json.empty());
}

TEST_F(RuleEngineStoreTest, SaveToStoreIncrementsVersion) {
    RuleEngine engine(*gate_);
    engine.addRule("r1", "V1", 10, RuleAction::Block);
    engine.addConditionToRule("r1", ConditionType::KeywordContains, "test");
    EXPECT_TRUE(engine.saveToStore(store_, "tenant-1"));

    engine.addRule("r2", "V2", 20, RuleAction::Warn);
    engine.addConditionToRule("r2", ConditionType::LengthCheck, "100");
    EXPECT_TRUE(engine.saveToStore(store_, "tenant-1"));

    auto versions = store_.listRuleSetVersions("tenant-1", 10);
    EXPECT_EQ(versions.size(), 2u);
    EXPECT_EQ(versions[0].version, 2);
    EXPECT_TRUE(versions[0].is_active);
    EXPECT_FALSE(versions[1].is_active);
}

TEST_F(RuleEngineStoreTest, LoadFromStoreRestoresRules) {
    RuleEngine saver(*gate_);
    saver.addRule("r1", "Block forbidden", 10, RuleAction::Block);
    saver.addConditionToRule("r1", ConditionType::KeywordContains, "forbidden");
    saver.addRule("r2", "Regex SQL", 20, RuleAction::Block);
    saver.addConditionToRule("r2", ConditionType::RegexMatch, "(?i)drop\\s+table");
    EXPECT_TRUE(saver.saveToStore(store_, "tenant-1"));

    RuleEngine loader(*gate_);
    EXPECT_TRUE(loader.loadFromStore(store_, "tenant-1"));
    EXPECT_EQ(loader.ruleCount(), 2u);

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "This is forbidden"}};
    auto result = loader.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.action, RuleAction::Block);
}

// TASK-20260702-01 P1-4：reloadFromStoreOrYaml — DB 优先 / 无激活集回退 YAML。
TEST_F(RuleEngineStoreTest, ReloadFromStoreOrYamlPrefersActiveStoreSet) {
    RuleEngine saver(*gate_);
    saver.addRule("r1", "block", 10, RuleAction::Block);
    saver.addConditionToRule("r1", ConditionType::KeywordContains, "storeword");
    ASSERT_TRUE(saver.saveToStore(store_, "tenant-x"));

    RuleEngine engine(*gate_);
    // yaml path 不存在也应命中 store（证明 DB 优先）。
    EXPECT_TRUE(engine.reloadFromStoreOrYaml(store_, "tenant-x", "/nonexistent/x.yaml"));
    EXPECT_EQ(engine.ruleCount(), 1u);
    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "has storeword"}};
    EXPECT_TRUE(engine.evaluate(ctx).matched);
}

TEST_F(RuleEngineStoreTest, ReloadFromStoreOrYamlFallsBackToYaml) {
    auto tmp = std::filesystem::temp_directory_path() /
               ("re_fallback_" + std::to_string(getpid()) + ".yaml");
    std::ofstream(tmp) << R"(
version: 1
rules:
  - id: yamlrule
    description: "from yaml"
    priority: 5
    enabled: true
    conditions:
      - type: keyword_contains
        value: "yamlword"
    action: warn
)";
    RuleEngine engine(*gate_);
    // 无激活规则集 → 回退 YAML。
    EXPECT_FALSE(engine.reloadFromStoreOrYaml(store_, "no-such-tenant", tmp.string()));
    EXPECT_EQ(engine.ruleCount(), 1u);
    std::filesystem::remove(tmp);
}

TEST_F(RuleEngineStoreTest, LoadFromStoreNoActive) {
    RuleEngine engine(*gate_);
    EXPECT_FALSE(engine.loadFromStore(store_, "nonexistent-tenant"));
    EXPECT_EQ(engine.ruleCount(), 0u);
}

// TASK-20260702-02 P2-4（SR-4）：per-tenant 规则集请求期强制。
namespace {
Tenant makeTenant(const std::string& id) {
    Tenant t;
    t.id = id;
    t.name = id;
    t.status = "active";
    t.created_at = "2026-01-01T00:00:00Z";
    t.updated_at = t.created_at;
    return t;
}
void saveTenantRule(MemoryPersistentStore& store, FeatureGate& gate,
                    const std::string& tenant, const std::string& rule_id,
                    const std::string& keyword) {
    RuleEngine s(gate);
    s.addRule(rule_id, "", 10, RuleAction::Block);
    s.addConditionToRule(rule_id, ConditionType::KeywordContains, keyword);
    s.saveToStore(store, tenant);
}
RequestContext ctxFor(const std::string& tenant, const std::string& text) {
    RequestContext ctx;
    ctx.tenant_id = tenant;
    ctx.chat_request.messages = {{"user", text}};
    return ctx;
}
}  // namespace

TEST_F(RuleEngineStoreTest, ProcessRoutesToTenantRuleSet) {
    store_.insertTenant(makeTenant("t1"));
    store_.insertTenant(makeTenant("t2"));
    saveTenantRule(store_, *gate_, "t1", "r1", "aaa");
    saveTenantRule(store_, *gate_, "t2", "r2", "bbb");

    RuleEngine engine(*gate_);
    engine.loadAllTenants(store_);

    EXPECT_TRUE(engine.evaluate(ctxFor("t1", "has aaa")).matched);
    EXPECT_FALSE(engine.evaluate(ctxFor("t1", "has bbb")).matched)
        << "SR-4: t1 must not match t2's rule set";
    EXPECT_TRUE(engine.evaluate(ctxFor("t2", "has bbb")).matched);
    EXPECT_FALSE(engine.evaluate(ctxFor("t2", "has aaa")).matched);
}

TEST_F(RuleEngineStoreTest, ProcessFallsBackToGlobalWhenNoTenantBucket) {
    saveTenantRule(store_, *gate_, "", "g", "glob");  // 全局激活集
    store_.insertTenant(makeTenant("t1"));
    saveTenantRule(store_, *gate_, "t1", "r1", "aaa");

    RuleEngine engine(*gate_);
    engine.reloadFromStoreOrYaml(store_, "", "/nonexistent.yaml");  // 全局桶
    engine.loadAllTenants(store_);

    // 无桶租户 → 全局
    EXPECT_TRUE(engine.evaluate(ctxFor("t3", "has glob")).matched);
    // 空 tenant → 全局
    EXPECT_TRUE(engine.evaluate(ctxFor("", "has glob")).matched);
    // t1 有桶 → 不用全局
    EXPECT_FALSE(engine.evaluate(ctxFor("t1", "has glob")).matched);
    EXPECT_TRUE(engine.evaluate(ctxFor("t1", "has aaa")).matched);
}

TEST_F(RuleEngineStoreTest, ReloadTenantRefreshesSingleBucket) {
    store_.insertTenant(makeTenant("t1"));
    store_.insertTenant(makeTenant("t2"));
    saveTenantRule(store_, *gate_, "t1", "r1", "aaa");
    saveTenantRule(store_, *gate_, "t2", "r2", "bbb");

    RuleEngine engine(*gate_);
    engine.loadAllTenants(store_);

    // 为 t1 激活新版本（改为拦截 ccc）；saveToStore 自动激活最新版。
    saveTenantRule(store_, *gate_, "t1", "r1b", "ccc");
    EXPECT_TRUE(engine.reloadTenant(store_, "t1"));

    EXPECT_TRUE(engine.evaluate(ctxFor("t1", "has ccc")).matched);
    EXPECT_FALSE(engine.evaluate(ctxFor("t1", "has aaa")).matched);
    // t2 不受影响
    EXPECT_TRUE(engine.evaluate(ctxFor("t2", "has bbb")).matched);
}

TEST_F(RuleEngineStoreTest, ActivateVersionSwitchesActive) {
    PersistentStore::RuleSetRecord v1{1, "t1", R"([{"id":"r1","description":"v1","priority":10,"enabled":true,"action":"block","action_param":"","conditions":[{"type":"keyword_contains","value":"v1-word"}]}])", "2026-01-01T00:00:00Z", true};
    PersistentStore::RuleSetRecord v2{2, "t1", R"([{"id":"r2","description":"v2","priority":10,"enabled":true,"action":"warn","action_param":"","conditions":[{"type":"keyword_contains","value":"v2-word"}]}])", "2026-01-02T00:00:00Z", false};
    store_.insertRuleSet("t1", v1);
    store_.insertRuleSet("t1", v2);

    EXPECT_TRUE(store_.activateRuleSetVersion("t1", 2));

    auto active = store_.getActiveRuleSet("t1");
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version, 2);

    RuleEngine engine(*gate_);
    EXPECT_TRUE(engine.loadFromStore(store_, "t1"));

    RequestContext ctx;
    ctx.chat_request.messages = {{"user", "v2-word here"}};
    auto result = engine.evaluate(ctx);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.rule_id, "r2");
}
