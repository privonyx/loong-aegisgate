#pragma once
#include "config.h"
#include <string>
#include <set>
#include <optional>
#include <vector>
#include <shared_mutex>

namespace aegisgate {

#define AEGIS_FEATURE_LIST \
    AEGIS_FEATURE(AdvancedRouting,      "advanced_routing")      \
    AEGIS_FEATURE(CustomRules,          "custom_rules")          \
    AEGIS_FEATURE(WebManagement,        "web_management")        \
    AEGIS_FEATURE(ClusterDeployment,    "cluster_deployment")    \
    AEGIS_FEATURE(RBAC,                 "rbac")                  \
    AEGIS_FEATURE(SSO,                  "sso")                   \
    AEGIS_FEATURE(ComplianceReport,     "compliance_report")     \
    AEGIS_FEATURE(Alerting,             "alerting")              \
    AEGIS_FEATURE(PluginSystem,         "plugin_system")         \
    AEGIS_FEATURE(AgentOrchestration,   "agent_orchestration")   \
    AEGIS_FEATURE(RAGPipeline,          "rag_pipeline")

enum class Feature {
#define AEGIS_FEATURE(name, str) name,
    AEGIS_FEATURE_LIST
#undef AEGIS_FEATURE
};

enum class LicenseSignatureMethod { ChecksumV1, Ed25519 };

class FeatureGate {
public:
    explicit FeatureGate(Edition edition);

    FeatureGate(const FeatureGate&) = delete;
    FeatureGate& operator=(const FeatureGate&) = delete;
    FeatureGate(FeatureGate&& other) noexcept;
    FeatureGate& operator=(FeatureGate&& other) noexcept;

    bool loadLicense(const std::string& license_path);
    bool isLicenseValid() const;

    bool isEnterprise() const;
    bool isEnabled(Feature feature) const;
    std::set<Feature> enabledFeatures() const;

    const std::string& customer() const;
    const std::string& expires() const;
    LicenseSignatureMethod signatureMethod() const;

    static std::string featureToString(Feature feature);
    static std::optional<Feature> featureFromString(const std::string& str);
    static std::vector<Feature> allFeatures();

    static bool validateLicenseKey(const std::string& edition,
                                   const std::string& customer,
                                   const std::string& expires,
                                   const std::string& license_key,
                                   const std::vector<std::string>& features = {});

    static bool validateEd25519Signature(const std::string& payload,
                                         const std::string& signature_b64,
                                         const std::string& public_key_b64);

    static std::string buildLicensePayload(const std::string& edition,
                                           const std::string& customer,
                                           const std::string& expires,
                                           const std::vector<std::string>& features);

    static FeatureGate createUnlocked(Edition edition);

    bool reloadLicense(const std::string& license_path);

private:
    void resolveDependencies();
    void enableAllFeatures();
    bool parseLicenseJson(const std::string& license_path);

    Edition edition_;
    bool license_valid_ = false;
    std::string customer_;
    std::string expires_;
    std::set<Feature> enabled_features_;
    LicenseSignatureMethod sig_method_ = LicenseSignatureMethod::ChecksumV1;
    std::string license_path_;
    mutable std::shared_mutex fg_mutex_;
};

} // namespace aegisgate
