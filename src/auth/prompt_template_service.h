#pragma once
#include "storage/persistent_store.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisgate {

struct PromptTemplate {
    std::string id;
    std::string tenant_id;
    std::string name;
    std::string content;
    int version = 1;
    int weight = 100;
    bool is_active = true;
    std::string created_at;
    std::string updated_at;
};

class PromptTemplateService {
public:
    explicit PromptTemplateService(PersistentStore* store);

    std::string create(const PromptTemplate& tpl);
    std::optional<PromptTemplate> get(const std::string& id) const;
    bool update(const PromptTemplate& tpl);
    bool remove(const std::string& id);

    std::vector<PromptTemplate> listByTenant(const std::string& tenant_id,
                                              int limit = 100, int offset = 0) const;
    std::vector<PromptTemplate> listByName(const std::string& tenant_id,
                                            const std::string& name) const;

    std::optional<PromptTemplate> selectTemplate(const std::string& tenant_id,
                                                  const std::string& name) const;

    // TASK-20260709-01 / REV20260707-I5 D5: pick among all active templates
    // for the tenant by weight (same algorithm as selectTemplate, no name filter).
    std::optional<PromptTemplate> selectDefaultActive(
        const std::string& tenant_id) const;

    static std::string render(const std::string& content,
                              const std::unordered_map<std::string, std::string>& vars);

private:
    static std::optional<PromptTemplate> pickWeighted(
        std::vector<PromptTemplate> active);

    PersistentStore* store_;
};

} // namespace aegisgate
