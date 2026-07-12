// Phase 9.3 Epic 2 Task 2.1 — RED step.
// Verifies that the PersistentStore base class provides no-op defaults for
// the new ConfigBundle versioning virtuals. Subclasses opt in by overriding.

#include "control_plane/config_version_record.h"
#include "storage/persistent_store.h"
#include <gtest/gtest.h>

namespace aegisgate {
namespace {

// Minimal concrete subclass that only satisfies pure virtuals; relies on
// the base class defaults for every config_version_* virtual we add.
class StubStoreForConfigDefaults : public PersistentStore {
public:
    bool initialize() override { return true; }
    void close() override {}
    bool isHealthy() const override { return true; }
    std::string backendName() const override { return "stub"; }

    bool insertAudit(const AuditEntry&) override { return true; }
    std::vector<AuditEntry> queryAudits(const std::string&, int, int) override { return {}; }
    int64_t auditCount(const std::string&) override { return 0; }

    bool insertCostRecord(const CostRecord&) override { return true; }
    std::vector<CostRecord> queryCosts(const std::string&, int, int,
                                       const std::string&) override { return {}; }
    int64_t costRecordCount(const std::string&, const std::string&) override { return 0; }

    int64_t pruneAudits(int) override { return 0; }
    int64_t pruneCostRecords(int) override { return 0; }
};

TEST(ConfigVersionStoreDefault, InsertReturnsFalse) {
    StubStoreForConfigDefaults s;
    ConfigVersionRecord rec{};
    rec.version_id = "01J8ABCDEFGHJKMNPQRSTUVWXY";
    rec.content_sha256 = "abc";
    rec.yaml_content = "server: {}";
    rec.size_bytes = 10;
    rec.status = ConfigStatus::PENDING;
    EXPECT_FALSE(s.insertConfigVersion(rec));
}

TEST(ConfigVersionStoreDefault, UpdateStatusReturnsFalse) {
    StubStoreForConfigDefaults s;
    EXPECT_FALSE(s.updateConfigStatus(
        "01J8ABCDEFGHJKMNPQRSTUVWXY",
        ConfigStatus::ACTIVE,
        "alice",
        "lgtm",
        1713600000000));
}

TEST(ConfigVersionStoreDefault, GetVersionReturnsNullopt) {
    StubStoreForConfigDefaults s;
    EXPECT_FALSE(s.getConfigVersion("01J8ABCDEFGHJKMNPQRSTUVWXY").has_value());
}

TEST(ConfigVersionStoreDefault, ListVersionsReturnsEmpty) {
    StubStoreForConfigDefaults s;
    ConfigVersionQuery q{};
    EXPECT_TRUE(s.listConfigVersions(q).empty());
}

TEST(ConfigVersionStoreDefault, GetActiveReturnsNullopt) {
    StubStoreForConfigDefaults s;
    EXPECT_FALSE(s.getActiveConfig().has_value());
}

TEST(ConfigVersionStoreDefault, ActivateReturnsFalse) {
    StubStoreForConfigDefaults s;
    EXPECT_FALSE(s.activateConfig(
        "01J8ABCDEFGHJKMNPQRSTUVWXY", "alice", 1713600000000));
}

TEST(ConfigStatusConversion, EnumToStringIsStable) {
    EXPECT_STREQ(configStatusToString(ConfigStatus::PENDING), "PENDING");
    EXPECT_STREQ(configStatusToString(ConfigStatus::APPROVED), "APPROVED");
    EXPECT_STREQ(configStatusToString(ConfigStatus::REJECTED), "REJECTED");
    EXPECT_STREQ(configStatusToString(ConfigStatus::ACTIVE), "ACTIVE");
    EXPECT_STREQ(configStatusToString(ConfigStatus::SUPERSEDED), "SUPERSEDED");
}

TEST(ConfigStatusConversion, StringToEnumRoundtrip) {
    EXPECT_EQ(configStatusFromString("PENDING").value(), ConfigStatus::PENDING);
    EXPECT_EQ(configStatusFromString("APPROVED").value(), ConfigStatus::APPROVED);
    EXPECT_EQ(configStatusFromString("REJECTED").value(), ConfigStatus::REJECTED);
    EXPECT_EQ(configStatusFromString("ACTIVE").value(), ConfigStatus::ACTIVE);
    EXPECT_EQ(configStatusFromString("SUPERSEDED").value(),
              ConfigStatus::SUPERSEDED);
    EXPECT_FALSE(configStatusFromString("UNKNOWN").has_value());
    EXPECT_FALSE(configStatusFromString("").has_value());
    EXPECT_FALSE(configStatusFromString("pending").has_value()); // case-sensitive
}

} // namespace
} // namespace aegisgate
