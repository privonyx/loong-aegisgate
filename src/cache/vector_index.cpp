#include "cache/vector_index.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

namespace aegisgate {

namespace {

constexpr uint32_t kIdMapMagic = 0x31444956u;  // 'VDI1' LE
constexpr uint32_t kIdMapVersion = 1;

std::string idMapPathFor(const std::string& index_path) { return index_path + ".idmap"; }

} // namespace

VectorIndex::VectorIndex(size_t dim, size_t max_elements, size_t M, size_t ef_construction)
    : dim_(dim), max_elements_(max_elements) {
    space_ = std::make_unique<hnswlib::InnerProductSpace>(dim);
    index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        space_.get(), max_elements, M, ef_construction,
        /* random_seed= */ 100, /* allow_replace_deleted= */ true);
    index_->setEf(50);
}

bool VectorIndex::insert(const std::string& id, const std::vector<float>& vec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vec.size() != dim_) {
        spdlog::warn("VectorIndex::insert dimension mismatch: expected {}, got {}",
                     dim_, vec.size());
        return false;
    }
    if (id_to_label_.count(id)) return true;  // already backed by a vector

    if (index_->cur_element_count >= max_elements_) {
        spdlog::warn("VectorIndex::insert capacity exhausted, skipping");
        return false;
    }

    hnswlib::labeltype label = next_label_++;
    index_->addPoint(vec.data(), label, /* replace_deleted= */ true);
    id_to_label_[id] = label;
    label_to_id_[label] = id;
    return true;
}

bool VectorIndex::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = id_to_label_.find(id);
    if (it == id_to_label_.end()) return false;

    index_->markDelete(it->second);
    label_to_id_.erase(it->second);
    id_to_label_.erase(it);
    return true;
}

std::vector<SearchResult> VectorIndex::search(const std::vector<float>& query,
                                                size_t top_k,
                                                float threshold) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (query.size() != dim_) {
        spdlog::warn("VectorIndex::search dimension mismatch: expected {}, got {}",
                     dim_, query.size());
        return {};
    }
    if (id_to_label_.empty()) return {};

    size_t actual_k = std::min(top_k, id_to_label_.size());
    auto result = index_->searchKnn(query.data(), actual_k);

    std::vector<SearchResult> results;
    while (!result.empty()) {
        auto [dist, label] = result.top();
        result.pop();

        auto it = label_to_id_.find(label);
        if (it == label_to_id_.end()) continue;

        // Inner product distance: similarity = 1 - distance for IP space
        float similarity = 1.0f - dist;
        if (threshold > 0.0f && similarity < threshold) continue;

        results.push_back({it->second, dist, similarity});
    }

    // Sort by similarity descending
    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) {
                  return a.similarity > b.similarity;
              });

    return results;
}

bool VectorIndex::forEach(const ForEachVisitor& visitor) const {
    if (!visitor) return false;
    // Snapshot {id, label} under lock; vector retrieval is then done out-of-lock
    // since getDataByLabel takes its own internal locks.
    std::vector<std::pair<std::string, hnswlib::labeltype>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(id_to_label_.size());
        for (const auto& [id, lbl] : id_to_label_) snapshot.emplace_back(id, lbl);
    }
    for (const auto& [id, lbl] : snapshot) {
        std::vector<float> vec;
        try {
            vec = index_->getDataByLabel<float>(lbl);
        } catch (const std::exception&) {
            continue;  // label removed concurrently; skip
        }
        if (!visitor(id, vec)) return false;
    }
    return true;
}

size_t VectorIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id_to_label_.size();
}

bool VectorIndex::contains(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id_to_label_.count(id) > 0;
}

void VectorIndex::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    index_->saveIndex(path);
    const std::string mpath = idMapPathFor(path);
    std::ofstream out(mpath, std::ios::binary);
    if (!out) {
        spdlog::error("VectorIndex::save failed to open id map: {}", mpath);
        return;
    }
    out.write(reinterpret_cast<const char*>(&kIdMapMagic), sizeof(kIdMapMagic));
    out.write(reinterpret_cast<const char*>(&kIdMapVersion), sizeof(kIdMapVersion));
    uint64_t dim_u = static_cast<uint64_t>(dim_);
    uint64_t max_u = static_cast<uint64_t>(max_elements_);
    uint64_t next_u = static_cast<uint64_t>(next_label_);
    uint64_t cnt_u = static_cast<uint64_t>(id_to_label_.size());
    out.write(reinterpret_cast<const char*>(&dim_u), sizeof(dim_u));
    out.write(reinterpret_cast<const char*>(&max_u), sizeof(max_u));
    out.write(reinterpret_cast<const char*>(&next_u), sizeof(next_u));
    out.write(reinterpret_cast<const char*>(&cnt_u), sizeof(cnt_u));
    for (const auto& [id, label] : id_to_label_) {
        uint64_t lab = static_cast<uint64_t>(label);
        uint32_t idlen = static_cast<uint32_t>(id.size());
        out.write(reinterpret_cast<const char*>(&lab), sizeof(lab));
        out.write(reinterpret_cast<const char*>(&idlen), sizeof(idlen));
        if (idlen) out.write(id.data(), idlen);
    }
    if (!out) {
        spdlog::error("VectorIndex::save id map write failed: {}", mpath);
    }
}

void VectorIndex::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_->loadIndex(path, space_.get());
    id_to_label_.clear();
    label_to_id_.clear();
    next_label_ = 0;

    const std::string mpath = idMapPathFor(path);
    std::ifstream in(mpath, std::ios::binary);
    if (!in) {
        spdlog::warn("VectorIndex::load id map missing (legacy index?): {}", mpath);
        next_label_ = static_cast<hnswlib::labeltype>(index_->cur_element_count);
        return;
    }
    uint32_t magic = 0, ver = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&ver), sizeof(ver));
    if (!in || magic != kIdMapMagic || ver != kIdMapVersion) {
        spdlog::error("VectorIndex::load invalid id map header: {}", mpath);
        next_label_ = static_cast<hnswlib::labeltype>(index_->cur_element_count);
        return;
    }
    uint64_t dim_u = 0, max_u = 0, next_u = 0, cnt_u = 0;
    in.read(reinterpret_cast<char*>(&dim_u), sizeof(dim_u));
    in.read(reinterpret_cast<char*>(&max_u), sizeof(max_u));
    in.read(reinterpret_cast<char*>(&next_u), sizeof(next_u));
    in.read(reinterpret_cast<char*>(&cnt_u), sizeof(cnt_u));
    if (!in || dim_u != static_cast<uint64_t>(dim_) ||
        max_u != static_cast<uint64_t>(max_elements_)) {
        spdlog::error("VectorIndex::load id map dimension/max_elements mismatch: {}", mpath);
        next_label_ = static_cast<hnswlib::labeltype>(index_->cur_element_count);
        return;
    }
    next_label_ = static_cast<hnswlib::labeltype>(next_u);
    for (uint64_t i = 0; i < cnt_u; i++) {
        uint64_t lab = 0;
        uint32_t idlen = 0;
        in.read(reinterpret_cast<char*>(&lab), sizeof(lab));
        in.read(reinterpret_cast<char*>(&idlen), sizeof(idlen));
        if (!in) {
            spdlog::error("VectorIndex::load truncated id map: {}", mpath);
            id_to_label_.clear();
            label_to_id_.clear();
            next_label_ = static_cast<hnswlib::labeltype>(index_->cur_element_count);
            return;
        }
        std::string id(idlen, '\0');
        if (idlen) {
            in.read(id.data(), idlen);
            if (!in) {
                spdlog::error("VectorIndex::load truncated id map (id bytes): {}", mpath);
                id_to_label_.clear();
                label_to_id_.clear();
                next_label_ = static_cast<hnswlib::labeltype>(index_->cur_element_count);
                return;
            }
        }
        const auto label = static_cast<hnswlib::labeltype>(lab);
        auto [it, _] = id_to_label_.emplace(std::move(id), label);
        label_to_id_[label] = it->first;
    }
    if (in.bad()) {
        spdlog::error("VectorIndex::load id map read error: {}", mpath);
        id_to_label_.clear();
        label_to_id_.clear();
        next_label_ = static_cast<hnswlib::labeltype>(index_->cur_element_count);
    }
}

} // namespace aegisgate
