#pragma once
#include "storage/persistent_store.h"
#include <functional>
#include <string>

namespace aegisgate {

struct MigrationResult {
    int64_t audits_migrated = 0;
    int64_t costs_migrated = 0;
    int64_t audits_skipped = 0;
    int64_t costs_skipped = 0;
    bool success = false;
    std::string error;
};

class MigrationTool {
public:
    static constexpr int kBatchSize = 500;

    using ProgressCallback = std::function<void(const std::string& phase,
                                                 int64_t done, int64_t total)>;

    MigrationResult migrate(PersistentStore& source, PersistentStore& target,
                             ProgressCallback progress = nullptr);

private:
    int64_t migrateAudits(PersistentStore& source, PersistentStore& target,
                           int64_t& skipped, ProgressCallback& progress);
    int64_t migrateCosts(PersistentStore& source, PersistentStore& target,
                          int64_t& skipped, ProgressCallback& progress);
};

} // namespace aegisgate
