// Phase 9.3 Epic 6 Task 6.3 — bootstrap_helper unit tests.
//
// Exercises every Outcome branch of bootstrapFromActiveYamlIfEmpty without
// touching gRPC, TLS, or the real filesystem beyond a gtest-managed temp
// directory. The goal is to lock in the "never overwrite existing history"
// invariant which is the only thing protecting operators from accidental
// data loss during a mis-deploy.

#include "control_plane/bootstrap_helper.h"
#include "control_plane/config_service_core.h"
#include "core/config.h"
#include "guardrail/audit.h"
#include "storage/memory_persistent_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace aegisgate {
namespace {

using control_plane::bootstrap::Outcome;

class BootstrapHelperTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryPersistentStore>();
        ASSERT_TRUE(store_->initialize());
        audit_ = std::make_unique<AuditLogger>();
        audit_->setPersistentStore(store_.get());

        ConfigServiceCore::Deps deps;
        deps.store = store_.get();
        deps.audit = audit_.get();
        // Always-valid validator so the bootstrap path hits Submit/Approve/
        // Activate without being short-circuited by SR3. The SubmitFailed
        // test overrides this to inject an Error.
        deps.validator = [this](const std::string&) {
            return validator_issues_;
        };
        core_ = std::make_unique<ConfigServiceCore>(std::move(deps));

        temp_dir_ = std::filesystem::temp_directory_path() /
                     ("aegisgate-bootstrap-" +
                      std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        if (audit_) audit_->shutdown();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::string writeSeedYaml(const std::string& contents,
                                const std::string& name = "seed.yaml") {
        auto path = (temp_dir_ / name).string();
        std::ofstream ofs(path, std::ios::binary);
        ofs << contents;
        return path;
    }

    std::unique_ptr<MemoryPersistentStore> store_;
    std::unique_ptr<AuditLogger>           audit_;
    std::unique_ptr<ConfigServiceCore>     core_;
    std::vector<Config::ValidationIssue>   validator_issues_;
    std::filesystem::path                  temp_dir_;
};

TEST_F(BootstrapHelperTest, EmptyStoreAndNoPathYieldsSkipped) {
    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store_.get(), core_.get(), /*bootstrap_yaml_path=*/"");
    EXPECT_EQ(r.outcome, Outcome::SkippedNoBootstrap);
    EXPECT_TRUE(r.version_id.empty());
}

TEST_F(BootstrapHelperTest, NullStoreYieldsSkipped) {
    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        nullptr, core_.get(), "/tmp/unused.yaml");
    EXPECT_EQ(r.outcome, Outcome::SkippedNoBootstrap);
    EXPECT_EQ(r.error_code, "INVALID_ARGUMENT");
}

TEST_F(BootstrapHelperTest, MissingFileYieldsFileReadFailed) {
    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store_.get(), core_.get(),
        (temp_dir_ / "nonexistent.yaml").string());
    EXPECT_EQ(r.outcome, Outcome::FileReadFailed);
    EXPECT_EQ(r.error_code, "BOOTSTRAP_FILE_READ_FAILED");
}

TEST_F(BootstrapHelperTest, HappyPathPersistsOneActiveVersion) {
    auto path = writeSeedYaml("server:\n  port: 8080\n");

    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store_.get(), core_.get(), path);
    ASSERT_EQ(r.outcome, Outcome::Bootstrapped);
    ASSERT_FALSE(r.version_id.empty());

    // Invariant: exactly one ACTIVE row in the store, with actor
    // system_bootstrap on both submit and activate.
    auto active = store_->getActiveConfig();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->version_id, r.version_id);
    EXPECT_EQ(active->status, ConfigStatus::ACTIVE);
    EXPECT_EQ(active->submitter, "system_bootstrap");
    EXPECT_EQ(active->reviewer, "system_bootstrap");
    EXPECT_EQ(active->activator, "system_bootstrap");
}

TEST_F(BootstrapHelperTest, NonEmptyStoreIsLeftAlone) {
    // Seed the store via a normal submit so we can verify bootstrap does
    // NOT overwrite an existing row even if the seed file changes.
    auto submit = core_->submit("seed: 1\n", "real-user",
                                  "human-controlled", /*validate_only=*/false);
    ASSERT_TRUE(submit.error_code.empty());
    ASSERT_FALSE(submit.record.version_id.empty());

    auto path = writeSeedYaml("should: not-replace\n");
    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store_.get(), core_.get(), path);
    EXPECT_EQ(r.outcome, Outcome::SkippedNotEmpty);

    // The real-user submit is still the only row.
    ConfigVersionQuery q;
    auto all = store_->listConfigVersions(q);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all.front().submitter, "real-user");
}

TEST_F(BootstrapHelperTest, InvalidYamlYieldsSubmitFailed) {
    // Inject a validator error so the submit path returns SR3 rejection,
    // which should be surfaced as SubmitFailed (NOT a silent success).
    validator_issues_.push_back(
        {Config::ValidationIssue::Error, "top-level",
         "malformed for test"});

    auto path = writeSeedYaml("whatever: 1\n");
    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store_.get(), core_.get(), path);
    EXPECT_EQ(r.outcome, Outcome::SubmitFailed);
    EXPECT_FALSE(r.error_code.empty());

    // Store still empty — the failed submit left no record.
    ConfigVersionQuery q;
    auto all = store_->listConfigVersions(q);
    EXPECT_TRUE(all.empty());
}

TEST_F(BootstrapHelperTest, OversizedFileYieldsSubmitFailed_SR2) {
    // 1 MiB + 1 byte exceeds ConfigServiceCore's SR2 cap. Bootstrap does
    // not special-case this — the generic Submit error surfaces.
    std::string oversized(1024u * 1024u + 1u, 'x');
    auto path = writeSeedYaml("# comment\n" + oversized);
    auto r = control_plane::bootstrap::bootstrapFromActiveYamlIfEmpty(
        store_.get(), core_.get(), path);
    EXPECT_EQ(r.outcome, Outcome::SubmitFailed);
    EXPECT_EQ(r.error_code, "PAYLOAD_TOO_LARGE");
}

} // namespace
} // namespace aegisgate
