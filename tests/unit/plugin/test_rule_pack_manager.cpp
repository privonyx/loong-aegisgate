#include <gtest/gtest.h>
#include "plugin/rule_pack_manager.h"
#include <filesystem>
#include <fstream>

using namespace aegisgate;
namespace fs = std::filesystem;

class RulePackManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
            ("rpmtest_" + std::to_string(getpid()));
        install_dir_ = test_dir_ / "installed";
        rules_dir_ = test_dir_ / "rules";
        pack_dir_ = test_dir_ / "sample_pack";

        fs::create_directories(install_dir_);
        fs::create_directories(rules_dir_);
        fs::create_directories(pack_dir_ / "rules");

        std::ofstream(rules_dir_ / "injection_patterns.yaml")
            << "patterns:\n  - pattern: \"base rule\"\n";

        std::ofstream(pack_dir_ / "manifest.yaml")
            << "name: test-pack\n"
            << "version: \"1.0.0\"\n"
            << "description: \"Test rule pack\"\n"
            << "tags:\n  - test\n"
            << "rules:\n"
            << "  - file: rules/injection.yaml\n"
            << "    target: injection_patterns\n"
            << "    mode: merge\n";

        std::ofstream(pack_dir_ / "rules" / "injection.yaml")
            << "  - pattern: \"pack rule\"\n";
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
    fs::path install_dir_;
    fs::path rules_dir_;
    fs::path pack_dir_;
};

TEST_F(RulePackManagerTest, InstallFromDirectory) {
    RulePackManager mgr(install_dir_.string());
    EXPECT_TRUE(mgr.install(pack_dir_.string()));
    EXPECT_TRUE(fs::exists(install_dir_ / "test-pack" / "manifest.yaml"));
}

TEST_F(RulePackManagerTest, ListInstalledPacks) {
    RulePackManager mgr(install_dir_.string());
    mgr.install(pack_dir_.string());

    auto packs = mgr.list();
    EXPECT_EQ(packs.size(), 1u);
    EXPECT_EQ(packs[0].name, "test-pack");
    EXPECT_EQ(packs[0].version, "1.0.0");
}

TEST_F(RulePackManagerTest, RemovePack) {
    RulePackManager mgr(install_dir_.string());
    mgr.install(pack_dir_.string());
    EXPECT_TRUE(mgr.remove("test-pack"));
    EXPECT_TRUE(mgr.list().empty());
    EXPECT_FALSE(fs::exists(install_dir_ / "test-pack"));
}

TEST_F(RulePackManagerTest, InvalidManifestRejected) {
    auto bad_dir = test_dir_ / "bad_pack";
    fs::create_directories(bad_dir);
    RulePackManager mgr(install_dir_.string());
    EXPECT_FALSE(mgr.install(bad_dir.string()));
}

TEST_F(RulePackManagerTest, InfoReturnsPack) {
    RulePackManager mgr(install_dir_.string());
    mgr.install(pack_dir_.string());
    auto p = mgr.info("test-pack");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->name, "test-pack");
    EXPECT_EQ(p->description, "Test rule pack");
}

TEST_F(RulePackManagerTest, ApplyMergesRules) {
    RulePackManager mgr(install_dir_.string());
    mgr.install(pack_dir_.string());
    mgr.applyAll(rules_dir_.string());

    std::ifstream f(rules_dir_ / "injection_patterns.yaml");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("[rulepack:test-pack]"), std::string::npos);
    EXPECT_NE(content.find("pack rule"), std::string::npos);
}

TEST_F(RulePackManagerTest, PathTraversalRejected) {
    auto evil_dir = test_dir_ / "evil_pack";
    fs::create_directories(evil_dir / "rules");
    std::ofstream(evil_dir / "manifest.yaml")
        << "name: evil\nversion: \"1.0\"\nrules:\n"
        << "  - file: ../../etc/passwd\n"
        << "    target: injection_patterns\n"
        << "    mode: merge\n";
    RulePackManager mgr(install_dir_.string());
    EXPECT_FALSE(mgr.install(evil_dir.string()));
}

// P0-F (TASK-20260701-01): the manifest `name` field builds the install/remove
// destination path and drives fs::remove_all. A name containing traversal must
// be rejected so a crafted pack cannot write/delete outside the install dir.
TEST_F(RulePackManagerTest, InstallRejectsPackNameTraversal) {
    auto evil_dir = test_dir_ / "evil_name_pack";
    fs::create_directories(evil_dir / "rules");
    std::ofstream(evil_dir / "manifest.yaml")
        << "name: ../escape\nversion: \"1.0\"\nrules:\n"
        << "  - file: rules/x.yaml\n"
        << "    target: injection_patterns\n"
        << "    mode: merge\n";
    RulePackManager mgr(install_dir_.string());
    EXPECT_FALSE(mgr.install(evil_dir.string()));
    EXPECT_FALSE(fs::exists(test_dir_ / "escape"));
}

// P0-F: remove(name) is CLI-reachable and feeds fs::remove_all(install_dir/name).
// A traversal name must be refused so it cannot delete arbitrary directories.
TEST_F(RulePackManagerTest, RemoveRejectsPathTraversal) {
    auto victim = test_dir_ / "victim";
    fs::create_directories(victim);
    std::ofstream(victim / "keep.txt") << "important\n";

    RulePackManager mgr(install_dir_.string());
    EXPECT_FALSE(mgr.remove("../victim"));
    EXPECT_TRUE(fs::exists(victim / "keep.txt"));
}

TEST_F(RulePackManagerTest, InvalidTargetRejected) {
    auto bad_target_dir = test_dir_ / "bad_target_pack";
    fs::create_directories(bad_target_dir / "rules");
    std::ofstream(bad_target_dir / "manifest.yaml")
        << "name: bad-target\nversion: \"1.0\"\nrules:\n"
        << "  - file: rules/x.yaml\n"
        << "    target: /etc/shadow\n"
        << "    mode: merge\n";
    RulePackManager mgr(install_dir_.string());
    EXPECT_FALSE(mgr.install(bad_target_dir.string()));
}

// C9 (REV20260702-C9): a manifest `file` with an absolute path contains no ".."
// token, so the old traversal check passed it; fs::path operator/ then discards
// pack_dir (`pack_dir / "/etc/passwd"` == "/etc/passwd") and reads an arbitrary
// host file into rules_dir. validateManifest must reject absolute file paths.
TEST_F(RulePackManagerTest, ManifestRejectsAbsoluteFilePath) {
    auto evil_dir = test_dir_ / "abs_pack";
    fs::create_directories(evil_dir / "rules");
    std::ofstream(evil_dir / "manifest.yaml")
        << "name: abs-pack\nversion: \"1.0\"\nrules:\n"
        << "  - file: /etc/passwd\n"
        << "    target: injection_patterns\n"
        << "    mode: merge\n";
    RulePackManager mgr(install_dir_.string());
    EXPECT_FALSE(mgr.install(evil_dir.string()));
}

// C9 defense-in-depth: even if a malicious manifest is planted directly into the
// install dir (bypassing install-time validation), applyAll must not read an
// absolute-path source file into the rules dir.
TEST_F(RulePackManagerTest, ApplyAllRejectsAbsoluteFilePath) {
    auto secret = test_dir_ / "secret.txt";
    std::ofstream(secret) << "TOPSECRET\n";

    auto planted = install_dir_ / "abs-pack";
    fs::create_directories(planted);
    std::ofstream(planted / "manifest.yaml")
        << "name: abs-pack\nversion: \"1.0\"\nrules:\n"
        << "  - file: " << secret.string() << "\n"
        << "    target: injection_patterns\n"
        << "    mode: replace\n";

    RulePackManager mgr(install_dir_.string());
    mgr.applyAll(rules_dir_.string());

    std::ifstream f(rules_dir_ / "injection_patterns.yaml");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("TOPSECRET"), std::string::npos);
}
