// Phase 11.1 TASK-20260523-01 — Epic 1.2 MemoryGuardModelRegistry tests.
//
// Verifies the IGuardModelRegistry contract (D1=C "registry+ABI 双层"):
//   * insert / get / list / promote / revert
//   * status state-machine: shadow -> live, live -> retired (revert)
//   * forbidden transitions are rejected
//   * promote enforces "only one live per model_id" invariant
//   * artifact_sha256 immutability after insert (T01 tamper defense)

#include "guardrail/model/i_guard_model_registry.h"
#include "guardrail/model/memory_guard_model_registry.h"

#include <gtest/gtest.h>

using aegisgate::guard::GuardModelStatus;
using aegisgate::guard::IGuardModelRegistry;
using aegisgate::guard::MemoryGuardModelRegistry;
using aegisgate::guard::ModelRegistryRecord;

namespace {

ModelRegistryRecord makeRecord(std::string version,
                               GuardModelStatus status = GuardModelStatus::Shadow,
                               std::string sha = "sha-default") {
    ModelRegistryRecord r;
    r.model_id = "guardrail";
    r.version = std::move(version);
    r.path = "/models/guardrail-" + r.version + ".onnx";
    r.classifier_threshold = 0.5f;
    r.status = status;
    r.promoted_at_ms = 0;
    r.artifact_sha256 = std::move(sha);
    r.metrics_summary = R"({"win_rate":0.62})";
    return r;
}

}  // namespace

TEST(MemoryGuardModelRegistryTest, InsertAndGetRoundTrip) {
    MemoryGuardModelRegistry reg;
    auto rec = makeRecord("v1.0.0");
    ASSERT_TRUE(reg.insert(rec).ok);

    auto fetched = reg.get("guardrail", "v1.0.0");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->version, "v1.0.0");
    EXPECT_EQ(fetched->status, GuardModelStatus::Shadow);
    EXPECT_EQ(fetched->artifact_sha256, "sha-default");
}

TEST(MemoryGuardModelRegistryTest, GetMissingReturnsNullopt) {
    MemoryGuardModelRegistry reg;
    EXPECT_FALSE(reg.get("guardrail", "v0.0.0").has_value());
}

TEST(MemoryGuardModelRegistryTest, InsertDuplicateRejected) {
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1")).ok);
    auto dup = reg.insert(makeRecord("v1", GuardModelStatus::Shadow, "sha-other"));
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(dup.error_code, "duplicate_version");
}

TEST(MemoryGuardModelRegistryTest, PromoteShadowToLive) {
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1")).ok);

    auto pr = reg.promote("guardrail", "v1", /*promoted_at_ms=*/1700000000000);
    EXPECT_TRUE(pr.ok);

    auto fetched = reg.get("guardrail", "v1");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->status, GuardModelStatus::Live);
    EXPECT_EQ(fetched->promoted_at_ms, 1700000000000);
}

TEST(MemoryGuardModelRegistryTest, PromoteDemoteOldLive) {
    // Invariant: at most one Live per model_id. Promoting v2 must demote v1.
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Live)).ok);
    ASSERT_TRUE(reg.insert(makeRecord("v2", GuardModelStatus::Shadow)).ok);

    auto pr = reg.promote("guardrail", "v2", /*promoted_at_ms=*/1700000000001);
    EXPECT_TRUE(pr.ok);

    auto v1 = reg.get("guardrail", "v1");
    auto v2 = reg.get("guardrail", "v2");
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v1->status, GuardModelStatus::Retired);
    EXPECT_EQ(v2->status, GuardModelStatus::Live);
}

TEST(MemoryGuardModelRegistryTest, PromoteRetiredRejected) {
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Retired)).ok);
    auto pr = reg.promote("guardrail", "v1", 0);
    EXPECT_FALSE(pr.ok);
    EXPECT_EQ(pr.error_code, "illegal_transition");
}

TEST(MemoryGuardModelRegistryTest, RevertLiveToRetired) {
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Live)).ok);
    auto rr = reg.revert("guardrail", "v1");
    EXPECT_TRUE(rr.ok);
    auto v1 = reg.get("guardrail", "v1");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1->status, GuardModelStatus::Retired);
}

TEST(MemoryGuardModelRegistryTest, RevertShadowRejected) {
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Shadow)).ok);
    auto rr = reg.revert("guardrail", "v1");
    EXPECT_FALSE(rr.ok);
    EXPECT_EQ(rr.error_code, "illegal_transition");
}

TEST(MemoryGuardModelRegistryTest, ListReturnsAllRecords) {
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Live)).ok);
    ASSERT_TRUE(reg.insert(makeRecord("v2")).ok);
    ASSERT_TRUE(reg.insert(makeRecord("v3")).ok);

    auto all = reg.list("guardrail");
    EXPECT_EQ(all.size(), 3u);

    auto onlyLive = reg.listByStatus("guardrail", GuardModelStatus::Live);
    EXPECT_EQ(onlyLive.size(), 1u);
    EXPECT_EQ(onlyLive[0].version, "v1");
}

TEST(MemoryGuardModelRegistryTest, ArtifactShaImmutableAfterInsert) {
    // T01 tamper defense: promote / revert must NOT mutate artifact_sha256.
    MemoryGuardModelRegistry reg;
    ASSERT_TRUE(reg.insert(makeRecord("v1", GuardModelStatus::Shadow, "sha-original")).ok);
    ASSERT_TRUE(reg.promote("guardrail", "v1", 0).ok);
    auto fetched = reg.get("guardrail", "v1");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->artifact_sha256, "sha-original");
}
