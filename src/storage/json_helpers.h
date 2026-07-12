#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace aegisgate {

inline std::string serializeStringList(const std::vector<std::string>& list) {
    return nlohmann::json(list).dump();
}

inline std::vector<std::string> parseStringList(const std::string& json_str) {
    if (json_str.empty()) return {};
    try {
        return nlohmann::json::parse(json_str).get<std::vector<std::string>>();
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

} // namespace aegisgate
