#pragma once
#include "storage/persistent_store.h"
#include <string>

namespace aegisgate {

class ComplianceReporter {
public:
    explicit ComplianceReporter(PersistentStore* store);

    std::string exportAuditsCsv(const std::string& from, const std::string& to,
                                 const std::string& tenant_id = "");
    std::string exportCostsCsv(const std::string& from, const std::string& to,
                                const std::string& tenant_id = "");
    std::string exportAuditsJson(const std::string& from, const std::string& to,
                                  const std::string& tenant_id = "");
    std::string exportCostsJson(const std::string& from, const std::string& to,
                                 const std::string& tenant_id = "");

    static constexpr size_t kMaxExportRows = 100000;

private:
    PersistentStore* store_;
};

} // namespace aegisgate
