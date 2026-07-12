#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aegisgate {

class RulePackManager {
public:
    struct InstalledPack {
        std::string name;
        std::string version;
        std::string description;
        std::vector<std::string> tags;
        std::string installed_at;
    };

    explicit RulePackManager(const std::string& install_dir);

    bool install(const std::string& source_dir);
    bool remove(const std::string& name);
    std::vector<InstalledPack> list() const;
    std::optional<InstalledPack> info(const std::string& name) const;
    bool applyAll(const std::string& rules_dir) const;

private:
    static const std::vector<std::string>& allowedTargets();
    bool validateManifest(const std::string& manifest_path) const;
    static InstalledPack parseManifest(const std::string& manifest_path);

    // A pack name must be a single, benign path component: it is concatenated
    // onto install_dir_ and passed to fs::remove_all, so any separator or
    // traversal token would let a crafted manifest / CLI arg escape the install
    // directory and write or delete arbitrary paths.
    static bool isSafePackName(const std::string& name);

    // C9 (REV20260702-C9): a manifest rule `file` must resolve to a path that
    // stays inside pack_dir. Rejects empty, absolute (fs::path operator/ would
    // otherwise discard pack_dir), and ".." escapes via weakly_canonical +
    // component-wise prefix check. Returns the safe resolved path or nullopt.
    static std::optional<std::filesystem::path> resolveSafeSourceFile(
        const std::filesystem::path& pack_dir, const std::string& file);

    std::string install_dir_;
};

}  // namespace aegisgate
