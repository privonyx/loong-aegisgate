#include "cli/migrate.h"
#include <spdlog/spdlog.h>
#include <unordered_set>

namespace aegisgate {

MigrationResult MigrationTool::migrate(PersistentStore& source,
                                        PersistentStore& target,
                                        ProgressCallback progress) {
    MigrationResult result;

    if (!source.isHealthy()) {
        result.error = "Source store is not healthy";
        return result;
    }
    if (!target.isHealthy()) {
        result.error = "Target store is not healthy";
        return result;
    }

    try {
        int64_t audit_skipped = 0;
        result.audits_migrated = migrateAudits(source, target, audit_skipped, progress);
        result.audits_skipped = audit_skipped;

        int64_t cost_skipped = 0;
        result.costs_migrated = migrateCosts(source, target, cost_skipped, progress);
        result.costs_skipped = cost_skipped;

        result.success = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    return result;
}

int64_t MigrationTool::migrateAudits(PersistentStore& source,
                                       PersistentStore& target,
                                       int64_t& skipped,
                                       ProgressCallback& progress) {
    int64_t total = source.auditCount();
    int64_t migrated = 0;
    int offset = 0;

    std::unordered_set<std::string> existing_ids;
    int fetch_limit = (total < 1000000) ? static_cast<int>(total + kBatchSize) : 1000000;
    auto existing = target.queryAudits("", fetch_limit, 0);
    for (const auto& e : existing) {
        existing_ids.insert(e.request_id);
    }

    while (true) {
        auto batch = source.queryAudits("", kBatchSize, offset);
        if (batch.empty()) break;

        for (const auto& entry : batch) {
            if (existing_ids.count(entry.request_id)) {
                ++skipped;
            } else if (target.insertAudit(entry)) {
                ++migrated;
                existing_ids.insert(entry.request_id);
            }
        }

        offset += kBatchSize;
        if (progress) progress("audits", migrated, total);
    }

    return migrated;
}

int64_t MigrationTool::migrateCosts(PersistentStore& source,
                                      PersistentStore& target,
                                      int64_t& skipped,
                                      ProgressCallback& progress) {
    int64_t total = source.costRecordCount();
    int64_t migrated = 0;
    int offset = 0;

    std::unordered_set<std::string> existing_ids;
    int fetch_limit = (total < 1000000) ? static_cast<int>(total + kBatchSize) : 1000000;
    auto existing = target.queryCosts("", fetch_limit, 0);
    for (const auto& e : existing) {
        existing_ids.insert(e.request_id);
    }

    while (true) {
        auto batch = source.queryCosts("", kBatchSize, offset);
        if (batch.empty()) break;

        for (const auto& record : batch) {
            if (existing_ids.count(record.request_id)) {
                ++skipped;
            } else if (target.insertCostRecord(record)) {
                ++migrated;
                existing_ids.insert(record.request_id);
            }
        }

        offset += kBatchSize;
        if (progress) progress("costs", migrated, total);
    }

    return migrated;
}

} // namespace aegisgate
