#include "auth/prompt_template_service.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace aegisgate {

namespace {
std::string generateUuid() {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist;
    auto hex = [&](int n) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(n) << (dist(gen) >> (32 - n * 4));
        return oss.str();
    };
    return hex(8) + "-" + hex(4) + "-" + hex(4) + "-" + hex(4) + "-" + hex(8) + hex(4);
}

std::string nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    std::ostringstream oss;
    oss << std::put_time(&buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
} // namespace

PromptTemplateService::PromptTemplateService(PersistentStore* store) : store_(store) {}

std::string PromptTemplateService::create(const PromptTemplate& tpl) {
    PersistentStore::PromptTemplateRecord rec;
    rec.id = generateUuid();
    rec.tenant_id = tpl.tenant_id;
    rec.name = tpl.name;
    rec.content = tpl.content;
    rec.version = tpl.version;
    rec.weight = tpl.weight;
    rec.is_active = tpl.is_active;
    rec.created_at = nowTimestamp();
    rec.updated_at = rec.created_at;

    if (!store_->insertPromptTemplate(rec)) {
        spdlog::error("PromptTemplateService: failed to create template '{}'", tpl.name);
        return "";
    }
    return rec.id;
}

std::optional<PromptTemplate> PromptTemplateService::get(const std::string& id) const {
    auto rec = store_->getPromptTemplate(id);
    if (!rec) return std::nullopt;

    PromptTemplate tpl;
    tpl.id = rec->id;
    tpl.tenant_id = rec->tenant_id;
    tpl.name = rec->name;
    tpl.content = rec->content;
    tpl.version = rec->version;
    tpl.weight = rec->weight;
    tpl.is_active = rec->is_active;
    tpl.created_at = rec->created_at;
    tpl.updated_at = rec->updated_at;
    return tpl;
}

bool PromptTemplateService::update(const PromptTemplate& tpl) {
    PersistentStore::PromptTemplateRecord rec;
    rec.id = tpl.id;
    rec.tenant_id = tpl.tenant_id;
    rec.name = tpl.name;
    rec.content = tpl.content;
    rec.version = tpl.version;
    rec.weight = tpl.weight;
    rec.is_active = tpl.is_active;
    rec.created_at = tpl.created_at;
    rec.updated_at = nowTimestamp();
    return store_->updatePromptTemplate(rec);
}

bool PromptTemplateService::remove(const std::string& id) {
    return store_->deletePromptTemplate(id);
}

std::vector<PromptTemplate>
PromptTemplateService::listByTenant(const std::string& tenant_id,
                                     int limit, int offset) const {
    auto recs = store_->listPromptTemplates(tenant_id, limit, offset);
    std::vector<PromptTemplate> result;
    result.reserve(recs.size());
    for (const auto& rec : recs) {
        PromptTemplate tpl;
        tpl.id = rec.id;
        tpl.tenant_id = rec.tenant_id;
        tpl.name = rec.name;
        tpl.content = rec.content;
        tpl.version = rec.version;
        tpl.weight = rec.weight;
        tpl.is_active = rec.is_active;
        tpl.created_at = rec.created_at;
        tpl.updated_at = rec.updated_at;
        result.push_back(std::move(tpl));
    }
    return result;
}

std::vector<PromptTemplate>
PromptTemplateService::listByName(const std::string& tenant_id,
                                   const std::string& name) const {
    auto recs = store_->listPromptTemplatesByName(tenant_id, name);
    std::vector<PromptTemplate> result;
    result.reserve(recs.size());
    for (const auto& rec : recs) {
        PromptTemplate tpl;
        tpl.id = rec.id;
        tpl.tenant_id = rec.tenant_id;
        tpl.name = rec.name;
        tpl.content = rec.content;
        tpl.version = rec.version;
        tpl.weight = rec.weight;
        tpl.is_active = rec.is_active;
        tpl.created_at = rec.created_at;
        tpl.updated_at = rec.updated_at;
        result.push_back(std::move(tpl));
    }
    return result;
}

std::optional<PromptTemplate>
PromptTemplateService::pickWeighted(std::vector<PromptTemplate> active) {
    if (active.empty()) return std::nullopt;
    if (active.size() == 1) return active[0];

    int total_weight = 0;
    for (const auto& t : active) total_weight += std::max(0, t.weight);
    if (total_weight <= 0) {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, active.size() - 1);
        return active[dist(gen)];
    }

    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, total_weight - 1);
    int roll = dist(gen);
    int cumulative = 0;
    for (const auto& t : active) {
        cumulative += std::max(0, t.weight);
        if (roll < cumulative) return t;
    }
    return active.back();
}

std::optional<PromptTemplate>
PromptTemplateService::selectTemplate(const std::string& tenant_id,
                                       const std::string& name) const {
    auto templates = listByName(tenant_id, name);
    std::vector<PromptTemplate> active;
    for (auto& t : templates) {
        if (t.is_active) active.push_back(std::move(t));
    }
    return pickWeighted(std::move(active));
}

std::optional<PromptTemplate>
PromptTemplateService::selectDefaultActive(const std::string& tenant_id) const {
    auto templates = listByTenant(tenant_id);
    std::vector<PromptTemplate> active;
    for (auto& t : templates) {
        if (t.is_active) active.push_back(std::move(t));
    }
    return pickWeighted(std::move(active));
}

std::string PromptTemplateService::render(
    const std::string& content,
    const std::unordered_map<std::string, std::string>& vars) {
    std::string result = content;
    for (const auto& [key, value] : vars) {
        std::string placeholder = "{{" + key + "}}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }
    return result;
}

} // namespace aegisgate
