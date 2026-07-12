#include "plugin/rule_pack_manager.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cctype>

namespace aegisgate {

namespace fs = std::filesystem;

bool RulePackManager::isSafePackName(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    if (name.front() == '.') return false;  // reject dotfiles / hidden entries
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

std::optional<fs::path> RulePackManager::resolveSafeSourceFile(
    const fs::path& pack_dir, const std::string& file) {
    if (file.empty()) return std::nullopt;

    std::error_code ec;
    fs::path base = fs::weakly_canonical(pack_dir, ec);
    if (ec) base = pack_dir.lexically_normal();
    // fs::path operator/ discards the left side when `file` is absolute, so this
    // catches absolute paths; weakly_canonical also collapses ".." escapes.
    fs::path candidate = fs::weakly_canonical(pack_dir / file, ec);
    if (ec) candidate = (pack_dir / file).lexically_normal();

    // Component-wise prefix check: candidate must stay inside base. Iterating by
    // path component (not string prefix) avoids "/a/bc" matching base "/a/b".
    auto bit = base.begin();
    auto cit = candidate.begin();
    for (; bit != base.end(); ++bit, ++cit) {
        if (cit == candidate.end() || *cit != *bit) return std::nullopt;
    }
    return candidate;
}

const std::vector<std::string>& RulePackManager::allowedTargets() {
    static const std::vector<std::string> targets = {
        "injection_patterns", "pii_patterns", "topic_whitelist", "custom_rules"
    };
    return targets;
}

RulePackManager::RulePackManager(const std::string& install_dir)
    : install_dir_(install_dir) {
    fs::create_directories(install_dir_);
}

bool RulePackManager::validateManifest(const std::string& manifest_path) const {
    if (!fs::exists(manifest_path)) {
        spdlog::error("RulePackManager: manifest.yaml not found at {}", manifest_path);
        return false;
    }
    try {
        auto root = YAML::LoadFile(manifest_path);
        if (!root["name"] || !root["version"]) {
            spdlog::error("RulePackManager: manifest missing name or version");
            return false;
        }
        auto rules = root["rules"];
        if (!rules || !rules.IsSequence()) {
            spdlog::error("RulePackManager: manifest missing rules section");
            return false;
        }
        for (const auto& rule : rules) {
            auto target = rule["target"].as<std::string>("");
            auto& allowed = allowedTargets();
            bool valid = false;
            for (const auto& t : allowed) {
                if (t == target) { valid = true; break; }
            }
            if (!valid) {
                spdlog::error("RulePackManager: invalid target '{}' in manifest", target);
                return false;
            }

            auto file = rule["file"].as<std::string>("");
            auto pack_dir = fs::path(manifest_path).parent_path();
            if (!resolveSafeSourceFile(pack_dir, file).has_value()) {
                spdlog::error("RulePackManager: unsafe rule file path '{}' "
                              "(absolute or escapes pack dir)", file);
                return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        spdlog::error("RulePackManager: manifest parse error: {}", e.what());
        return false;
    }
}

RulePackManager::InstalledPack RulePackManager::parseManifest(
    const std::string& manifest_path) {
    InstalledPack pack;
    try {
        auto root = YAML::LoadFile(manifest_path);
        pack.name = root["name"].as<std::string>("");
        pack.version = root["version"].as<std::string>("0.0.0");
        pack.description = root["description"].as<std::string>("");
        if (auto tags = root["tags"]) {
            for (const auto& t : tags) {
                pack.tags.push_back(t.as<std::string>());
            }
        }
    } catch (...) {}
    return pack;
}

bool RulePackManager::install(const std::string& source_dir) {
    auto manifest_path = fs::path(source_dir) / "manifest.yaml";
    if (!validateManifest(manifest_path.string())) {
        return false;
    }

    auto pack = parseManifest(manifest_path.string());
    if (!isSafePackName(pack.name)) {
        spdlog::error("RulePackManager: refusing unsafe pack name '{}'", pack.name);
        return false;
    }

    auto dest = fs::path(install_dir_) / pack.name;
    if (fs::exists(dest)) {
        spdlog::warn("RulePackManager: pack '{}' already installed, replacing", pack.name);
        fs::remove_all(dest);
    }

    try {
        fs::copy(source_dir, dest, fs::copy_options::recursive);
    } catch (const std::exception& e) {
        spdlog::error("RulePackManager: copy failed: {}", e.what());
        return false;
    }

    spdlog::info("RulePackManager: installed '{}' v{}", pack.name, pack.version);
    return true;
}

bool RulePackManager::remove(const std::string& name) {
    if (!isSafePackName(name)) {
        spdlog::error("RulePackManager: refusing unsafe pack name '{}'", name);
        return false;
    }
    auto dest = fs::path(install_dir_) / name;
    if (!fs::exists(dest)) {
        spdlog::warn("RulePackManager: pack '{}' not found", name);
        return false;
    }
    fs::remove_all(dest);
    spdlog::info("RulePackManager: removed '{}'", name);
    return true;
}

std::vector<RulePackManager::InstalledPack> RulePackManager::list() const {
    std::vector<InstalledPack> result;
    if (!fs::exists(install_dir_)) return result;

    for (const auto& entry : fs::directory_iterator(install_dir_)) {
        if (!entry.is_directory()) continue;
        auto manifest = entry.path() / "manifest.yaml";
        if (!fs::exists(manifest)) continue;
        auto pack = parseManifest(manifest.string());
        if (!pack.name.empty()) {
            result.push_back(std::move(pack));
        }
    }
    return result;
}

std::optional<RulePackManager::InstalledPack> RulePackManager::info(
    const std::string& name) const {
    auto manifest = fs::path(install_dir_) / name / "manifest.yaml";
    if (!fs::exists(manifest)) return std::nullopt;
    auto pack = parseManifest(manifest.string());
    if (pack.name.empty()) return std::nullopt;
    return pack;
}

bool RulePackManager::applyAll(const std::string& rules_dir) const {
    if (!fs::exists(install_dir_)) return true;

    for (const auto& pack_dir : fs::directory_iterator(install_dir_)) {
        if (!pack_dir.is_directory()) continue;
        auto manifest_path = pack_dir.path() / "manifest.yaml";
        if (!fs::exists(manifest_path)) continue;

        try {
            auto root = YAML::LoadFile(manifest_path.string());
            auto pack_name = root["name"].as<std::string>("");
            auto rules = root["rules"];
            if (!rules || !rules.IsSequence()) continue;

            for (const auto& rule : rules) {
                auto file = rule["file"].as<std::string>("");
                auto target = rule["target"].as<std::string>("");
                auto mode = rule["mode"].as<std::string>("merge");

                // C9 defense-in-depth: re-validate the file path even if the
                // manifest was planted directly in the install dir.
                auto safe_source = resolveSafeSourceFile(pack_dir.path(), file);
                if (!safe_source.has_value()) {
                    spdlog::warn("RulePackManager: skipping unsafe rule file '{}' "
                                 "in pack '{}'", file, pack_name);
                    continue;
                }
                auto source_file = *safe_source;
                auto target_file = fs::path(rules_dir) / (target + ".yaml");

                if (!fs::exists(source_file)) {
                    spdlog::warn("RulePackManager: source file '{}' not found in pack '{}'",
                                  file, pack_name);
                    continue;
                }

                std::ifstream src(source_file);
                std::string content((std::istreambuf_iterator<char>(src)),
                                     std::istreambuf_iterator<char>());

                if (mode == "merge") {
                    std::string marker = "# [rulepack:" + pack_name + "]";
                    if (fs::exists(target_file)) {
                        std::ifstream existing(target_file);
                        std::string existing_content(
                            (std::istreambuf_iterator<char>(existing)),
                             std::istreambuf_iterator<char>());
                        if (existing_content.find(marker) != std::string::npos) {
                            continue;
                        }
                    }

                    std::ofstream out(target_file, std::ios::app);
                    out << "\n" << marker << "\n" << content << "\n";
                } else if (mode == "replace") {
                    std::ofstream out(target_file, std::ios::trunc);
                    std::string marker = "# [rulepack:" + pack_name + "]";
                    out << marker << "\n" << content;
                }

                spdlog::info("RulePackManager: applied {} -> {} (mode={})",
                              file, target, mode);
            }
        } catch (const std::exception& e) {
            spdlog::error("RulePackManager: error applying pack: {}", e.what());
        }
    }

    return true;
}

}  // namespace aegisgate
