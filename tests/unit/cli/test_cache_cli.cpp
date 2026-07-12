#include <gtest/gtest.h>
#include "cli/cache_cli.h"
#include <sstream>
#include <unordered_map>

using namespace aegisgate;
using namespace aegisgate::cache_cli;

namespace {

EnvLookup makeEnv(std::unordered_map<std::string, std::string> kv) {
    return [kv = std::move(kv)](const char* name) -> const char* {
        if (!name) return nullptr;
        auto it = kv.find(name);
        if (it == kv.end()) return nullptr;
        return it->second.c_str();
    };
}

}  // namespace

// 1: dump arg parsing — required --output, optional --backend, --config
TEST(CacheCliArgs, ParseDumpHappyPath) {
    DumpArgs args;
    std::stringstream err;
    ASSERT_TRUE(parseDumpArgs(
        {"--backend", "hnswlib", "--output", "/tmp/snap.bin",
         "--config", "/etc/aegisgate.yaml"},
        args, err));
    EXPECT_EQ(args.backend, "hnswlib");
    EXPECT_EQ(args.output, "/tmp/snap.bin");
    EXPECT_EQ(args.config_path, "/etc/aegisgate.yaml");
    EXPECT_FALSE(args.help);
}

TEST(CacheCliArgs, ParseDumpRejectsMissingOutput) {
    DumpArgs args;
    std::stringstream err;
    EXPECT_FALSE(parseDumpArgs({"--backend", "hnswlib"}, args, err));
    EXPECT_NE(err.str().find("output"), std::string::npos);
}

// 2: restore arg parsing — required --input + --target, tenant allowlist split
TEST(CacheCliArgs, ParseRestoreSplitsTenantAllowlist) {
    RestoreArgs args;
    std::stringstream err;
    ASSERT_TRUE(parseRestoreArgs(
        {"--input", "/tmp/snap.bin", "--target", "qdrant://localhost:6333/c",
         "--tenant-allowlist", "alpha,bravo,charlie"},
        args, err));
    EXPECT_EQ(args.input, "/tmp/snap.bin");
    EXPECT_EQ(args.target, "qdrant://localhost:6333/c");
    ASSERT_EQ(args.tenant_allowlist.size(), 3u);
    EXPECT_EQ(args.tenant_allowlist[0], "alpha");
    EXPECT_EQ(args.tenant_allowlist[1], "bravo");
    EXPECT_EQ(args.tenant_allowlist[2], "charlie");
}

TEST(CacheCliArgs, ParseRestoreRejectsMissingInput) {
    RestoreArgs args;
    std::stringstream err;
    EXPECT_FALSE(parseRestoreArgs({"--target", "qdrant://"}, args, err));
    EXPECT_NE(err.str().find("input"), std::string::npos);
}

// 3: SR8 — requireApiKey rejects when env var is missing or empty
TEST(CacheCliSr8, RequiresApiKeyEnvVar) {
    EXPECT_FALSE(requireApiKey(makeEnv({})));
    EXPECT_FALSE(requireApiKey(makeEnv({{kApiKeyEnvVar, ""}})));
    EXPECT_TRUE(requireApiKey(makeEnv({{kApiKeyEnvVar, "secret"}})));
}

// 4: emitJsonProgress writes structured JSON with phase and count fields
TEST(CacheCliJson, EmitProgressIsValidJson) {
    std::stringstream out;
    emitJsonProgress(out, "dump", 1234);
    auto s = out.str();
    EXPECT_NE(s.find("\"phase\""), std::string::npos);
    EXPECT_NE(s.find("dump"), std::string::npos);
    EXPECT_NE(s.find("1234"), std::string::npos);
}

// 5: dump --help short-circuits without erroring
TEST(CacheCliArgs, ParseDumpAcceptsHelp) {
    DumpArgs args;
    std::stringstream err;
    EXPECT_TRUE(parseDumpArgs({"--help"}, args, err));
    EXPECT_TRUE(args.help);
}

// 6: restore --tenant-allowlist with empty token is filtered out
// =============================================================================
// Mutation Test (D7 + P1#1, plan Epic 3.4 step 5):
// Simulates "what if a future refactor removes the sha256 verify branch in
// CacheMigrator::restore?". We construct a tampered snapshot whose body
// disagrees with the trailing digest, then call restore directly. The test
// asserts the live implementation rejects the file (checksum_ok = false and
// entries_restored = 0). If a mutation removes the verify branch, this
// assertion will flip and surface the regression.
// =============================================================================
#include "cache/cache_migrator.h"
#include "cache/hnsw_vector_store.h"
#include "cache/embedder.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

TEST(CacheCliMutation, RestoreRejectsTamperedBodyWhenDigestStale) {
    namespace fs = std::filesystem;
    auto path = (fs::temp_directory_path() /
                 ("aegisgate_mutation_" + std::to_string(::getpid()) + ".bin"))
                    .string();

    aegisgate::HnswVectorStore src(8, 100, 64);
    src.initialize();
    aegisgate::HashEmbedder emb(8);
    src.insert("tenant:t|c:1", "id-1", emb.embed("alpha"));

    aegisgate::CacheMigrator m;
    auto stats = m.dump(src, path);
    ASSERT_EQ(stats.entries_written, 1u);

    // Tamper with a single body byte but DO NOT refresh the trailer; this
    // models a corruption event the SR2 sha256 must catch.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        f.seekg(0, std::ios::end);
        auto sz = static_cast<size_t>(f.tellg());
        size_t flip = sz - 50;  // somewhere in the body, away from trailer
        f.seekg(flip);
        char b = 0;
        f.read(&b, 1);
        b ^= 0x7F;
        f.seekp(flip);
        f.write(&b, 1);
    }

    aegisgate::HnswVectorStore dst(8, 100, 64);
    dst.initialize();
    auto rs = m.restore(path, dst);
    EXPECT_FALSE(rs.checksum_ok) << "SR2 sha256 verify regression!";
    EXPECT_EQ(rs.entries_restored, 0u) << "Tampered body must not be replayed";
    EXPECT_EQ(dst.size(), 0u) << "Target index must remain empty";

    std::error_code ec;
    fs::remove(path, ec);
}

TEST(CacheCliArgs, ParseRestoreFiltersEmptyAllowlistTokens) {
    RestoreArgs args;
    std::stringstream err;
    ASSERT_TRUE(parseRestoreArgs(
        {"--input", "i", "--target", "t",
         "--tenant-allowlist", "alpha,,bravo, "},
        args, err));
    ASSERT_EQ(args.tenant_allowlist.size(), 2u);
    EXPECT_EQ(args.tenant_allowlist[0], "alpha");
    EXPECT_EQ(args.tenant_allowlist[1], "bravo");
}
