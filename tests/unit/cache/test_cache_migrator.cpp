#include <gtest/gtest.h>
#include "cache/cache_migrator.h"
#include "cache/hnsw_vector_store.h"
#include "cache/embedder.h"
#include "core/crypto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace fs = std::filesystem;
using namespace aegisgate;

namespace {

std::string makeTempPath(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("aegisgate_migrator_" + tag + "_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(std::rand()) + ".bin");
    return base.string();
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), sz);
    return bytes;
}

uint32_t readU32LE(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t readU64LE(const uint8_t* p) {
    return uint64_t(p[0]) | (uint64_t(p[1]) << 8) |
           (uint64_t(p[2]) << 16) | (uint64_t(p[3]) << 24) |
           (uint64_t(p[4]) << 32) | (uint64_t(p[5]) << 40) |
           (uint64_t(p[6]) << 48) | (uint64_t(p[7]) << 56);
}

class CacheMigratorDumpTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(8);
        store_ = std::make_unique<HnswVectorStore>(8, 20000, 64);
        ASSERT_TRUE(store_->initialize());
        out_path_ = makeTempPath("dump");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(out_path_, ec);
    }
    std::vector<float> embed(const std::string& s) { return embedder_->embed(s); }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> store_;
    std::string out_path_;
};

} // namespace

// --------------------------------------------------------------------------
// Test 1: dump produces magic + version + dim + count header
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, HeaderHasMagicVersionDimCount) {
    store_->insert("tenant:a|conv:1", "id1", embed("alpha"));
    store_->insert("tenant:a|conv:1", "id2", embed("bravo"));

    CacheMigrator m;
    auto stats = m.dump(*store_, out_path_);
    EXPECT_EQ(stats.entries_written, 2u);

    auto bytes = readAll(out_path_);
    ASSERT_GE(bytes.size(), 32u + 32u);  // header + sha256 tail
    EXPECT_EQ(std::memcmp(bytes.data(), CacheMigrator::kMagic, 16), 0);
    EXPECT_EQ(readU32LE(bytes.data() + 16), CacheMigrator::kFormatVersion);
    EXPECT_EQ(readU32LE(bytes.data() + 20), 8u);  // dim
    EXPECT_EQ(readU64LE(bytes.data() + 24), 2u);  // entry count
}

// --------------------------------------------------------------------------
// Test 2: empty index dumps a well-formed header + zero entries + sha256
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, EmptyIndexProducesValidEmptySnapshot) {
    CacheMigrator m;
    auto stats = m.dump(*store_, out_path_);
    EXPECT_EQ(stats.entries_written, 0u);

    auto bytes = readAll(out_path_);
    ASSERT_EQ(bytes.size(), 32u + 32u);
    EXPECT_EQ(readU64LE(bytes.data() + 24), 0u);
    EXPECT_TRUE(m.verifyChecksum(out_path_));
}

// --------------------------------------------------------------------------
// Test 3: tail sha256 (SR2) matches a fresh hash of preceding bytes
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, TailSha256MatchesPrecedingBytes) {
    for (int i = 0; i < 5; ++i) {
        store_->insert("p", "id_" + std::to_string(i),
                       embed("v" + std::to_string(i)));
    }
    CacheMigrator m;
    auto stats = m.dump(*store_, out_path_);

    auto bytes = readAll(out_path_);
    ASSERT_GE(bytes.size(), 32u);
    std::string preceding(bytes.begin(), bytes.end() - 32);
    std::string computed = crypto::sha256(preceding);
    EXPECT_EQ(computed, stats.sha256_hex);
    EXPECT_TRUE(m.verifyChecksum(out_path_));
}

// --------------------------------------------------------------------------
// Test 4: corrupting a single byte invalidates the checksum
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, CorruptedSnapshotFailsChecksum) {
    store_->insert("p", "id1", embed("hello"));
    CacheMigrator m;
    m.dump(*store_, out_path_);
    ASSERT_TRUE(m.verifyChecksum(out_path_));

    {
        std::fstream f(out_path_, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        // Flip a byte well inside the entry region (offset 50).
        f.seekg(50);
        char b = 0;
        f.read(&b, 1);
        b ^= 0x40;
        f.seekp(50);
        f.write(&b, 1);
    }
    EXPECT_FALSE(m.verifyChecksum(out_path_));
}

// --------------------------------------------------------------------------
// Test 5: every inserted entry is encoded with vec_len = dim*4 + payload_len
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, EntryEncodingHasVecAndPayloadLengths) {
    store_->insert("tenant:t|conv:c", "id-x", embed("payload-shape"));
    CacheMigrator m;
    auto stats = m.dump(*store_, out_path_);
    EXPECT_EQ(stats.entries_written, 1u);

    auto bytes = readAll(out_path_);
    ASSERT_GE(bytes.size(), 32u + 4u + 32u + 4u + 32u);
    // Header (32) | vec_len (4) | vec_bytes (dim*4=32) | payload_len (4) | payload (>=1) | sha256 (32)
    uint32_t vec_len = readU32LE(bytes.data() + 32);
    EXPECT_EQ(vec_len, 8u * sizeof(float));
    uint32_t payload_len = readU32LE(bytes.data() + 32 + 4 + vec_len);
    EXPECT_GT(payload_len, 0u);
    EXPECT_LT(payload_len, 1024u);  // sanity: small JSON

    // Total file size matches header(32) + entry(4+vec_len+4+payload_len) + sha256(32)
    size_t expected = 32u + 4u + vec_len + 4u + payload_len + 32u;
    EXPECT_EQ(bytes.size(), expected);
}

// --------------------------------------------------------------------------
// Test 6: dump on an enumeration-unsupported backend returns 0 entries
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, EnumerateUnsupportedYieldsEmptySnapshotZeroEntries) {
    // Stub store that returns false from enumerate()
    struct UnsupportedStore : public VectorStore {
        bool initialize() override { return true; }
        bool insert(const std::string&, const std::string&,
                    const std::vector<float>&) override { return true; }
        bool remove(const std::string&, const std::string&) override { return false; }
        std::vector<VectorSearchResult> search(
            const std::string&, const std::vector<float>&, size_t,
            float) const override { return {}; }
        size_t size() const override { return 5; }
        std::string backendName() const override { return "stub"; }
    };
    UnsupportedStore stub;

    CacheMigrator m;
    auto stats = m.dump(stub, out_path_);
    EXPECT_EQ(stats.entries_written, 0u);

    auto bytes = readAll(out_path_);
    ASSERT_EQ(bytes.size(), 32u + 32u);
    EXPECT_EQ(readU64LE(bytes.data() + 24), 0u);
    EXPECT_TRUE(m.verifyChecksum(out_path_));
}

// --------------------------------------------------------------------------
// Test 7: stress dump with 10K entries — size scales, checksum still valid
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, LargeIndexDumpsAndVerifiesChecksum) {
    constexpr size_t kN = 10000;
    for (size_t i = 0; i < kN; ++i) {
        store_->insert("p_" + std::to_string(i % 8),
                       "id_" + std::to_string(i),
                       embed("text_" + std::to_string(i)));
    }
    CacheMigrator m;
    auto stats = m.dump(*store_, out_path_);
    EXPECT_EQ(stats.entries_written, kN);
    EXPECT_TRUE(m.verifyChecksum(out_path_));

    auto bytes = readAll(out_path_);
    ASSERT_GT(bytes.size(), 32u + 32u);
    EXPECT_EQ(readU64LE(bytes.data() + 24), kN);
}

// --------------------------------------------------------------------------
// Test 8: dump partition_key + id are preserved in payload (round-trip key)
// --------------------------------------------------------------------------
TEST_F(CacheMigratorDumpTest, PayloadPreservesPartitionAndId) {
    store_->insert("tenant:abc|conv:42", "entry-id-zeta", embed("data"));
    CacheMigrator m;
    m.dump(*store_, out_path_);

    auto bytes = readAll(out_path_);
    ASSERT_GE(bytes.size(), 32u + 4u + 32u + 4u + 32u);
    uint32_t vec_len = readU32LE(bytes.data() + 32);
    uint32_t payload_len = readU32LE(bytes.data() + 32 + 4 + vec_len);
    std::string payload(
        reinterpret_cast<const char*>(bytes.data() + 32 + 4 + vec_len + 4),
        payload_len);
    EXPECT_NE(payload.find("tenant:abc|conv:42"), std::string::npos)
        << "partition key not encoded: " << payload;
    EXPECT_NE(payload.find("entry-id-zeta"), std::string::npos)
        << "entry id not encoded: " << payload;
}

// ===========================================================================
// Epic 3.3 — CacheMigrator::restore (SR2 sha256, allowlist, versioning)
// ===========================================================================

class CacheMigratorRestoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<HashEmbedder>(8);
        source_ = std::make_unique<HnswVectorStore>(8, 1000, 64);
        ASSERT_TRUE(source_->initialize());
        target_ = std::make_unique<HnswVectorStore>(8, 1000, 64);
        ASSERT_TRUE(target_->initialize());
        snap_ = makeTempPath("restore");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(snap_, ec);
    }
    std::vector<float> embed(const std::string& s) { return embedder_->embed(s); }

    std::unique_ptr<HashEmbedder> embedder_;
    std::unique_ptr<HnswVectorStore> source_;
    std::unique_ptr<HnswVectorStore> target_;
    std::string snap_;
};

// 1: round-trip — every dumped entry shows up on the target with same partition+id
TEST_F(CacheMigratorRestoreTest, RoundTripRestoresAllEntries) {
    source_->insert("tenant:a|conv:1", "id-1", embed("alpha"));
    source_->insert("tenant:b|conv:1", "id-2", embed("bravo"));
    source_->insert("tenant:c|conv:1", "id-3", embed("charlie"));

    CacheMigrator m;
    auto d = m.dump(*source_, snap_);
    ASSERT_EQ(d.entries_written, 3u);

    auto r = m.restore(snap_, *target_);
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_TRUE(r.version_ok);
    EXPECT_EQ(r.entries_read, 3u);
    EXPECT_EQ(r.entries_restored, 3u);
    EXPECT_EQ(r.entries_corrupted, 0u);

    auto hit = target_->search("tenant:b|conv:1", embed("bravo"), 1, 0.0f);
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0].id, "id-2");
}

// 2: SR2 — sha256 mismatch causes immediate failure, target untouched
TEST_F(CacheMigratorRestoreTest, Sr2Sha256MismatchAbortsBeforeAnyInsert) {
    source_->insert("tenant:x|conv:1", "id1", embed("data"));
    CacheMigrator m;
    m.dump(*source_, snap_);
    {
        std::fstream f(snap_, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        f.seekg(0, std::ios::end);
        auto len = static_cast<size_t>(f.tellg());
        // Flip one byte inside the entry region (not in the trailing 32-byte
        // digest so the *body* differs from what the trailing hash certifies).
        size_t flip = len - 40;
        f.seekg(flip);
        char b = 0;
        f.read(&b, 1);
        b ^= 0x11;
        f.seekp(flip);
        f.write(&b, 1);
    }
    auto r = m.restore(snap_, *target_);
    EXPECT_FALSE(r.checksum_ok);
    EXPECT_EQ(r.entries_restored, 0u);
    EXPECT_EQ(target_->size(), 0u);
}

// 3: SR2 — tenant allowlist drops non-matching partition keys
TEST_F(CacheMigratorRestoreTest, Sr2TenantAllowlistFilters) {
    source_->insert("tenant:keep|conv:1", "id-k1", embed("k1"));
    source_->insert("tenant:keep|conv:2", "id-k2", embed("k2"));
    source_->insert("tenant:drop|conv:1", "id-d", embed("d"));
    CacheMigrator m;
    m.dump(*source_, snap_);

    auto r = m.restore(snap_, *target_, {"keep"});
    EXPECT_EQ(r.entries_read, 3u);
    EXPECT_EQ(r.entries_restored, 2u);
    EXPECT_EQ(r.entries_skipped_tenant, 1u);
    EXPECT_TRUE(target_->search("tenant:drop|conv:1", embed("d"), 1).empty());
    EXPECT_EQ(target_->size(), 2u);
}

// 4: bad magic — restore refuses the file (sha256 recomputed to isolate path)
TEST_F(CacheMigratorRestoreTest, RejectsBadMagic) {
    source_->insert("p", "id", embed("x"));
    CacheMigrator m;
    m.dump(*source_, snap_);

    // Overwrite the first 2 magic bytes and *also* refresh the SR2 trailer
    // so this case exercises the magic-check branch, not the sha256 branch.
    auto bytes = readAll(snap_);
    ASSERT_GE(bytes.size(), 64u);
    bytes[0] = 'X'; bytes[1] = 'X';
    size_t body_len = bytes.size() - 32;
    std::string body(reinterpret_cast<const char*>(bytes.data()), body_len);
    std::string hex = crypto::sha256(body);
    auto h2d = [](char c) { return (c >= 'a') ? 10 + (c - 'a') : c - '0'; };
    for (size_t i = 0; i < 32; ++i) {
        bytes[body_len + i] =
            static_cast<uint8_t>((h2d(hex[2*i]) << 4) | h2d(hex[2*i+1]));
    }
    {
        std::ofstream f(snap_, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    auto r = m.restore(snap_, *target_);
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_EQ(r.entries_restored, 0u);
    EXPECT_FALSE(r.failure_reason.empty());
    EXPECT_NE(r.failure_reason.find("magic"), std::string::npos);
}

// 5: unknown version — refuses (forward-compat guard). sha256 recomputed.
TEST_F(CacheMigratorRestoreTest, RejectsUnknownVersion) {
    source_->insert("p", "id", embed("x"));
    CacheMigrator m;
    m.dump(*source_, snap_);

    auto bytes = readAll(snap_);
    ASSERT_GE(bytes.size(), 64u);
    bytes[16] = 0xFF; bytes[17] = 0xFF; bytes[18] = 0xFF; bytes[19] = 0xFF;
    size_t body_len = bytes.size() - 32;
    std::string body(reinterpret_cast<const char*>(bytes.data()), body_len);
    std::string hex = crypto::sha256(body);
    auto h2d = [](char c) { return (c >= 'a') ? 10 + (c - 'a') : c - '0'; };
    for (size_t i = 0; i < 32; ++i) {
        bytes[body_len + i] =
            static_cast<uint8_t>((h2d(hex[2*i]) << 4) | h2d(hex[2*i+1]));
    }
    {
        std::ofstream f(snap_, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    auto r = m.restore(snap_, *target_);
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_FALSE(r.version_ok);
    EXPECT_EQ(r.entries_restored, 0u);
}

// 6: replay is idempotent — restoring the same snapshot twice gives the same size
TEST_F(CacheMigratorRestoreTest, ReplayIsIdempotent) {
    for (int i = 0; i < 4; ++i) {
        source_->insert("tenant:t|conv:1", "id_" + std::to_string(i),
                        embed("v" + std::to_string(i)));
    }
    CacheMigrator m;
    m.dump(*source_, snap_);
    auto r1 = m.restore(snap_, *target_);
    auto sz_after_first = target_->size();
    auto r2 = m.restore(snap_, *target_);
    EXPECT_EQ(target_->size(), sz_after_first);  // duplicate ids no-op
    EXPECT_EQ(r1.entries_restored, 4u);
    EXPECT_EQ(r2.entries_restored, 4u);
}

// 7: empty snapshot restores without error
TEST_F(CacheMigratorRestoreTest, EmptySnapshotRestoresZeroEntries) {
    CacheMigrator m;
    m.dump(*source_, snap_);  // 0 entries
    auto r = m.restore(snap_, *target_);
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_EQ(r.entries_read, 0u);
    EXPECT_EQ(r.entries_restored, 0u);
    EXPECT_EQ(target_->size(), 0u);
}

// 8: truncated file (missing trailer) is rejected
TEST_F(CacheMigratorRestoreTest, TruncatedFileRejected) {
    source_->insert("p", "id", embed("x"));
    CacheMigrator m;
    m.dump(*source_, snap_);
    fs::resize_file(snap_, 40);  // chop off most of the body and trailer
    auto r = m.restore(snap_, *target_);
    EXPECT_EQ(r.entries_restored, 0u);
    EXPECT_FALSE(r.failure_reason.empty());
}

// 9: dim mismatch — target with different vector dim refuses inserts
TEST_F(CacheMigratorRestoreTest, DimensionMismatchSkipsInsert) {
    source_->insert("p", "id", embed("x"));
    CacheMigrator m;
    m.dump(*source_, snap_);

    // Target has dim=16 instead of 8.
    auto wrong = std::make_unique<HnswVectorStore>(16, 1000, 64);
    ASSERT_TRUE(wrong->initialize());
    auto r = m.restore(snap_, *wrong);
    // Either flagged via failure_reason or corrupted counter; either way, no
    // poisoned inserts land in the target.
    EXPECT_EQ(wrong->size(), 0u);
}

// 10: payload without partition/id is treated as corrupted
TEST_F(CacheMigratorRestoreTest, CorruptedPayloadIsCounted) {
    source_->insert("p", "id", embed("x"));
    CacheMigrator m;
    m.dump(*source_, snap_);

    auto bytes = readAll(snap_);
    ASSERT_GE(bytes.size(), 32u + 4u + 32u + 4u + 32u);
    uint32_t vec_len = readU32LE(bytes.data() + 32);
    uint32_t payload_len = readU32LE(bytes.data() + 32 + 4 + vec_len);
    size_t payload_off = 32 + 4 + vec_len + 4;
    // Replace payload with same-length whitespace (still valid utf-8, no JSON
    // keys), then recompute sha256 over the body and write back.
    for (uint32_t i = 0; i < payload_len; ++i) bytes[payload_off + i] = ' ';
    size_t body_len = bytes.size() - 32;
    std::string body(reinterpret_cast<const char*>(bytes.data()), body_len);
    std::string hex = crypto::sha256(body);
    auto h2d = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        return 10 + (c - 'a');
    };
    for (size_t i = 0; i < 32; ++i) {
        bytes[body_len + i] =
            static_cast<uint8_t>((h2d(hex[2*i]) << 4) | h2d(hex[2*i+1]));
    }
    {
        std::ofstream f(snap_, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    auto r = m.restore(snap_, *target_);
    EXPECT_TRUE(r.checksum_ok);
    EXPECT_EQ(r.entries_read, 1u);
    EXPECT_EQ(r.entries_restored, 0u);
    EXPECT_GE(r.entries_corrupted, 1u);
}

