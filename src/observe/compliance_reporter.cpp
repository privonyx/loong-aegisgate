#include "observe/compliance_reporter.h"
#include "guardrail/audit.h"
#include "auth/encryption.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>

namespace aegisgate {

ComplianceReporter::ComplianceReporter(PersistentStore* store)
    : store_(store) {}

std::string ComplianceReporter::exportAuditsCsv(const std::string& from,
                                                  const std::string& to,
                                                  const std::string& tenant_id) {
    if (!store_) return "";

    auto audits = store_->queryAudits(tenant_id, static_cast<int>(kMaxExportRows), 0);

    std::ostringstream oss;
    oss << "request_id,timestamp,tenant_id,action,stage,detail\n";

    for (const auto& a : audits) {
        if (!from.empty() && a.timestamp < from) continue;
        if (!to.empty() && a.timestamp > to) continue;

        auto escape = [](const std::string& s) -> std::string {
            if (s.find(',') == std::string::npos &&
                s.find('"') == std::string::npos &&
                s.find('\n') == std::string::npos) return s;
            std::string escaped = "\"";
            for (char c : s) {
                if (c == '"') escaped += "\"\"";
                else escaped += c;
            }
            escaped += '"';
            return escaped;
        };

        oss << escape(a.request_id) << ','
            << escape(a.timestamp) << ','
            << escape(a.tenant_id) << ','
            << escape(a.action) << ','
            << escape(a.stage_name) << ','
            << escape(AuditLogger::decryptDetail(a.detail, &Encryption::instance())) << '\n';
    }
    return oss.str();
}

std::string ComplianceReporter::exportCostsCsv(const std::string& from,
                                                 const std::string& to,
                                                 const std::string& tenant_id) {
    if (!store_) return "";

    // SR-3（TASK-20260702-02 P2-3）：tenant 过滤下推到查询层，与审计导出对称。
    // 此前 queryCosts("", ...) 全租户取前 kMaxExportRows 行再内存过滤 → 他租户
    // 占满前 10w 时目标租户行被截断漏计（金额低估）。
    auto costs = store_->queryCosts("", static_cast<int>(kMaxExportRows), 0, tenant_id);

    std::ostringstream oss;
    oss << "request_id,model,timestamp,tenant_id,input_tokens,output_tokens,total_cost\n";

    for (const auto& c : costs) {
        if (!from.empty() && c.timestamp < from) continue;
        if (!to.empty() && c.timestamp > to) continue;

        oss << c.request_id << ','
            << c.model << ','
            << c.timestamp << ','
            << c.tenant_id << ','
            << c.input_tokens << ','
            << c.output_tokens << ','
            << c.total_cost << '\n';
    }
    return oss.str();
}

std::string ComplianceReporter::exportAuditsJson(const std::string& from,
                                                   const std::string& to,
                                                   const std::string& tenant_id) {
    if (!store_) return "[]";

    auto audits = store_->queryAudits(tenant_id, static_cast<int>(kMaxExportRows), 0);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : audits) {
        if (!from.empty() && a.timestamp < from) continue;
        if (!to.empty() && a.timestamp > to) continue;

        nlohmann::json j;
        j["request_id"] = a.request_id;
        j["timestamp"] = a.timestamp;
        j["tenant_id"] = a.tenant_id;
        j["action"] = a.action;
        j["stage"] = a.stage_name;
        j["detail"] = AuditLogger::decryptDetail(a.detail, &Encryption::instance());
        arr.push_back(std::move(j));
    }
    return arr.dump();
}

std::string ComplianceReporter::exportCostsJson(const std::string& from,
                                                  const std::string& to,
                                                  const std::string& tenant_id) {
    if (!store_) return "[]";

    // SR-3（TASK-20260702-02 P2-3）：tenant 过滤下推到查询层（同 CSV 路径）。
    auto costs = store_->queryCosts("", static_cast<int>(kMaxExportRows), 0, tenant_id);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : costs) {
        if (!from.empty() && c.timestamp < from) continue;
        if (!to.empty() && c.timestamp > to) continue;

        nlohmann::json j;
        j["request_id"] = c.request_id;
        j["model"] = c.model;
        j["timestamp"] = c.timestamp;
        j["tenant_id"] = c.tenant_id;
        j["input_tokens"] = c.input_tokens;
        j["output_tokens"] = c.output_tokens;
        j["total_cost"] = c.total_cost;
        arr.push_back(std::move(j));
    }
    return arr.dump();
}

} // namespace aegisgate
