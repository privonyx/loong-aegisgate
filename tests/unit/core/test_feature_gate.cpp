#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <set>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "core/feature_gate.h"

using namespace aegisgate;

namespace {

std::string sha256Hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(hash[i]);
    return oss.str();
}

const char* LICENSE_SALT = "aegisgate-v1-f7e2a9c4d1b8";

std::string generateLicenseKey(const std::string& edition,
                                const std::string& customer,
                                const std::string& expires,
                                const std::vector<std::string>& features = {}) {
    std::string payload = edition + ":" + customer + ":" + expires;
    if (!features.empty()) {
        auto sorted = features;
        std::sort(sorted.begin(), sorted.end());
        std::string feat_str;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) feat_str += ",";
            feat_str += sorted[i];
        }
        payload += ":" + feat_str;
    }
    std::string full_hash = sha256Hex(payload + LICENSE_SALT);
    return "AEGIS-ENT-" + full_hash.substr(0, 16) + full_hash.substr(0, 16);
}

std::string writeTempLicense(const nlohmann::json& j, const std::string& name) {
    std::string path = "/tmp/test_license_" + name + ".json";
    std::ofstream ofs(path);
    ofs << j.dump();
    return path;
}

} // namespace

// --- Existing tests (preserved & updated) ---

TEST(FeatureGateTest, CommunityDefaults) {
    FeatureGate gate(Edition::Community);
    EXPECT_FALSE(gate.isEnterprise());
    EXPECT_FALSE(gate.isEnabled(Feature::CustomRules));
    EXPECT_FALSE(gate.isEnabled(Feature::WebManagement));
    EXPECT_FALSE(gate.isEnabled(Feature::RBAC));
    EXPECT_FALSE(gate.isEnabled(Feature::SSO));
    EXPECT_FALSE(gate.isEnabled(Feature::AdvancedRouting));
    EXPECT_FALSE(gate.isEnabled(Feature::ClusterDeployment));
    EXPECT_FALSE(gate.isEnabled(Feature::ComplianceReport));
    EXPECT_FALSE(gate.isEnabled(Feature::Alerting));
}

TEST(FeatureGateTest, EnterpriseWithoutLicenseFallsBackToCommunity) {
    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.isEnterprise());
    EXPECT_FALSE(gate.isLicenseValid());
    EXPECT_FALSE(gate.isEnabled(Feature::CustomRules));
}

TEST(FeatureGateTest, EnterpriseWithEmptyPathFallsBack) {
    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.loadLicense(""));
    EXPECT_FALSE(gate.isEnterprise());
}

TEST(FeatureGateTest, EnterpriseWithMissingFileFallsBack) {
    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.loadLicense("/tmp/nonexistent_license.json"));
    EXPECT_FALSE(gate.isEnterprise());
}

TEST(FeatureGateTest, EnterpriseWithInvalidLicenseKeyFallsBack) {
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "test"},
        {"expires", "2099-12-31"},
        {"license_key", "AEGIS-ENT-invalid-key"}
    };
    auto path = writeTempLicense(j, "invalid_key");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.loadLicense(path));
    EXPECT_FALSE(gate.isEnterprise());
    EXPECT_FALSE(gate.isLicenseValid());

    std::remove(path.c_str());
}

TEST(FeatureGateTest, ValidateLicenseKeyMechanism) {
    std::string edition = "enterprise";
    std::string customer = "test-corp";
    std::string expires = "2099-12-31";

    EXPECT_FALSE(FeatureGate::validateLicenseKey(
        edition, customer, expires, "too-short"));
    EXPECT_FALSE(FeatureGate::validateLicenseKey(
        edition, customer, expires, "AEGIS-ENT-test-0000000000000000"));
}

TEST(FeatureGateTest, EnterpriseWithExpiredLicenseFallsBack) {
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "test"},
        {"expires", "2020-01-01"},
        {"license_key", "AEGIS-ENT-expired-0000000000000000"}
    };
    auto path = writeTempLicense(j, "expired");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.loadLicense(path));
    EXPECT_FALSE(gate.isEnterprise());

    std::remove(path.c_str());
}

TEST(FeatureGateTest, CommunitySkipsLicenseCheck) {
    FeatureGate gate(Edition::Community);
    EXPECT_TRUE(gate.loadLicense(""));
    EXPECT_FALSE(gate.isEnterprise());
    EXPECT_FALSE(gate.isLicenseValid());
}

// --- New granular feature tests ---

TEST(FeatureGateTest, FeatureToStringMapping) {
    EXPECT_EQ(FeatureGate::featureToString(Feature::AdvancedRouting), "advanced_routing");
    EXPECT_EQ(FeatureGate::featureToString(Feature::CustomRules), "custom_rules");
    EXPECT_EQ(FeatureGate::featureToString(Feature::WebManagement), "web_management");
    EXPECT_EQ(FeatureGate::featureToString(Feature::ClusterDeployment), "cluster_deployment");
    EXPECT_EQ(FeatureGate::featureToString(Feature::RBAC), "rbac");
    EXPECT_EQ(FeatureGate::featureToString(Feature::SSO), "sso");
    EXPECT_EQ(FeatureGate::featureToString(Feature::ComplianceReport), "compliance_report");
    EXPECT_EQ(FeatureGate::featureToString(Feature::Alerting), "alerting");
}

TEST(FeatureGateTest, FeatureFromStringMapping) {
    EXPECT_EQ(FeatureGate::featureFromString("advanced_routing"), Feature::AdvancedRouting);
    EXPECT_EQ(FeatureGate::featureFromString("custom_rules"), Feature::CustomRules);
    EXPECT_EQ(FeatureGate::featureFromString("web_management"), Feature::WebManagement);
    EXPECT_EQ(FeatureGate::featureFromString("cluster_deployment"), Feature::ClusterDeployment);
    EXPECT_EQ(FeatureGate::featureFromString("rbac"), Feature::RBAC);
    EXPECT_EQ(FeatureGate::featureFromString("sso"), Feature::SSO);
    EXPECT_EQ(FeatureGate::featureFromString("compliance_report"), Feature::ComplianceReport);
    EXPECT_EQ(FeatureGate::featureFromString("alerting"), Feature::Alerting);
    EXPECT_EQ(FeatureGate::featureFromString("nonexistent"), std::nullopt);
    EXPECT_EQ(FeatureGate::featureFromString(""), std::nullopt);
}

TEST(FeatureGateTest, EnterpriseLicenseWithSpecificFeatures) {
    std::vector<std::string> features = {"rbac", "custom_rules"};
    auto key = generateLicenseKey("enterprise", "acme", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "acme"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "specific_features");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnterprise());

    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_TRUE(gate.isEnabled(Feature::CustomRules));
    EXPECT_FALSE(gate.isEnabled(Feature::WebManagement));
    EXPECT_FALSE(gate.isEnabled(Feature::SSO));
    EXPECT_FALSE(gate.isEnabled(Feature::Alerting));
    EXPECT_FALSE(gate.isEnabled(Feature::ComplianceReport));
    EXPECT_FALSE(gate.isEnabled(Feature::AdvancedRouting));
    EXPECT_FALSE(gate.isEnabled(Feature::ClusterDeployment));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, EnterpriseLicenseWithAllFeatures) {
    std::vector<std::string> features = {
        "rbac", "custom_rules", "web_management", "sso",
        "alerting", "compliance_report", "advanced_routing", "cluster_deployment"
    };
    auto key = generateLicenseKey("enterprise", "bigcorp", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "bigcorp"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "all_features");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnterprise());

    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_TRUE(gate.isEnabled(Feature::CustomRules));
    EXPECT_TRUE(gate.isEnabled(Feature::WebManagement));
    EXPECT_TRUE(gate.isEnabled(Feature::SSO));
    EXPECT_TRUE(gate.isEnabled(Feature::Alerting));
    EXPECT_TRUE(gate.isEnabled(Feature::ComplianceReport));
    EXPECT_TRUE(gate.isEnabled(Feature::AdvancedRouting));
    EXPECT_TRUE(gate.isEnabled(Feature::ClusterDeployment));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, FeatureDependencyAutoEnable) {
    // web_management depends on rbac — rbac should be auto-enabled
    std::vector<std::string> features = {"web_management"};
    auto key = generateLicenseKey("enterprise", "dep-test", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "dep-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "dep_web");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnabled(Feature::WebManagement));
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, SSODependencyAutoEnablesRBAC) {
    std::vector<std::string> features = {"sso"};
    auto key = generateLicenseKey("enterprise", "sso-test", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "sso-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "dep_sso");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnabled(Feature::SSO));
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, ComplianceReportDependencyAutoEnablesRBAC) {
    std::vector<std::string> features = {"compliance_report"};
    auto key = generateLicenseKey("enterprise", "comp-test", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "comp-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "dep_comp");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnabled(Feature::ComplianceReport));
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, InvalidFeatureStringIgnored) {
    std::vector<std::string> features = {"rbac", "nonexistent_feature", "alerting"};
    auto key = generateLicenseKey("enterprise", "inv-test", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "inv-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "invalid_feature");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_TRUE(gate.isEnabled(Feature::Alerting));
    EXPECT_FALSE(gate.isEnabled(Feature::WebManagement));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, EmptyFeaturesListDisablesAll) {
    std::vector<std::string> features = {};
    auto key = generateLicenseKey("enterprise", "empty-test", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "empty-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "empty_features");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnterprise());
    EXPECT_FALSE(gate.isEnabled(Feature::RBAC));
    EXPECT_FALSE(gate.isEnabled(Feature::WebManagement));
    EXPECT_FALSE(gate.isEnabled(Feature::CustomRules));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, LegacyLicenseWithoutFeaturesField) {
    auto key = generateLicenseKey("enterprise", "legacy-corp", "2099-12-31");
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "legacy-corp"},
        {"expires", "2099-12-31"},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "legacy");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnterprise());

    // Legacy license without features field enables all
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_TRUE(gate.isEnabled(Feature::WebManagement));
    EXPECT_TRUE(gate.isEnabled(Feature::SSO));
    EXPECT_TRUE(gate.isEnabled(Feature::CustomRules));
    EXPECT_TRUE(gate.isEnabled(Feature::Alerting));
    EXPECT_TRUE(gate.isEnabled(Feature::ComplianceReport));
    EXPECT_TRUE(gate.isEnabled(Feature::AdvancedRouting));
    EXPECT_TRUE(gate.isEnabled(Feature::ClusterDeployment));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, CreateUnlockedEnablesAllFeatures) {
    auto gate = FeatureGate::createUnlocked(Edition::Enterprise);
    EXPECT_TRUE(gate.isEnterprise());
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_TRUE(gate.isEnabled(Feature::WebManagement));
    EXPECT_TRUE(gate.isEnabled(Feature::SSO));
    EXPECT_TRUE(gate.isEnabled(Feature::CustomRules));
    EXPECT_TRUE(gate.isEnabled(Feature::Alerting));
    EXPECT_TRUE(gate.isEnabled(Feature::ComplianceReport));
    EXPECT_TRUE(gate.isEnabled(Feature::AdvancedRouting));
    EXPECT_TRUE(gate.isEnabled(Feature::ClusterDeployment));
}

TEST(FeatureGateTest, CreateUnlockedCommunityDisablesAll) {
    auto gate = FeatureGate::createUnlocked(Edition::Community);
    EXPECT_FALSE(gate.isEnterprise());
    EXPECT_FALSE(gate.isEnabled(Feature::RBAC));
}

TEST(FeatureGateTest, EnabledFeaturesReturnsCorrectSet) {
    auto gate = FeatureGate::createUnlocked(Edition::Enterprise);
    auto features = gate.enabledFeatures();
    EXPECT_GE(features.size(), 9u);
    EXPECT_TRUE(features.count(Feature::RBAC));
    EXPECT_TRUE(features.count(Feature::WebManagement));
    EXPECT_TRUE(features.count(Feature::PluginSystem));
}

TEST(FeatureGateTest, AllFeaturesHaveStringMapping) {
    for (auto f : FeatureGate::allFeatures()) {
        auto str = FeatureGate::featureToString(f);
        EXPECT_NE(str, "unknown") << "Feature missing string mapping";
        auto roundtrip = FeatureGate::featureFromString(str);
        ASSERT_TRUE(roundtrip.has_value()) << "Feature '" << str << "' missing fromString mapping";
        EXPECT_EQ(*roundtrip, f) << "Roundtrip failed for: " << str;
    }
}

TEST(FeatureGateTest, AllFeaturesCountIsConsistent) {
    auto all = FeatureGate::allFeatures();
    EXPECT_GE(all.size(), 8u);

    std::set<std::string> strings;
    for (auto f : all) {
        strings.insert(FeatureGate::featureToString(f));
    }
    EXPECT_EQ(strings.size(), all.size());
}

TEST(FeatureGateTest, BuildLicensePayloadConsistent) {
    auto p1 = FeatureGate::buildLicensePayload("enterprise", "acme", "2099-12-31",
                                                {"rbac", "sso"});
    auto p2 = FeatureGate::buildLicensePayload("enterprise", "acme", "2099-12-31",
                                                {"sso", "rbac"});
    EXPECT_EQ(p1, p2);
}

TEST(FeatureGateTest, BuildLicensePayloadWithoutFeatures) {
    auto p = FeatureGate::buildLicensePayload("enterprise", "acme", "2099-12-31", {});
    EXPECT_EQ(p, "enterprise:acme:2099-12-31");
}

TEST(FeatureGateTest, Ed25519InvalidSignatureRejected) {
    EXPECT_FALSE(FeatureGate::validateEd25519Signature("test", "bad_sig", "bad_key"));
    EXPECT_FALSE(FeatureGate::validateEd25519Signature("test", "", ""));
}

TEST(FeatureGateTest, SignatureMethodDefaultIsChecksumV1) {
    auto gate = FeatureGate::createUnlocked(Edition::Enterprise);
    EXPECT_EQ(gate.signatureMethod(), LicenseSignatureMethod::ChecksumV1);
}

TEST(FeatureGateTest, ReloadLicenseWithEmptyPathFails) {
    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.reloadLicense(""));
}

TEST(FeatureGateTest, ReloadLicenseReloadsSameFile) {
    std::vector<std::string> features = {"rbac"};
    auto key = generateLicenseKey("enterprise", "reload-test", "2099-12-31", features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "reload-test"},
        {"expires", "2099-12-31"},
        {"features", features},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "reload");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_TRUE(gate.loadLicense(path));
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_FALSE(gate.isEnabled(Feature::SSO));

    std::vector<std::string> features2 = {"rbac", "sso"};
    auto key2 = generateLicenseKey("enterprise", "reload-test", "2099-12-31", features2);
    nlohmann::json j2 = {
        {"edition", "enterprise"},
        {"customer", "reload-test"},
        {"expires", "2099-12-31"},
        {"features", features2},
        {"license_key", key2}
    };
    {
        std::ofstream ofs(path);
        ofs << j2.dump();
    }

    EXPECT_TRUE(gate.reloadLicense(""));
    EXPECT_TRUE(gate.isEnabled(Feature::RBAC));
    EXPECT_TRUE(gate.isEnabled(Feature::SSO));

    std::remove(path.c_str());
}

TEST(FeatureGateTest, TamperedFeaturesListFailsValidation) {
    // Generate key with rbac only, but put rbac + alerting in the file
    std::vector<std::string> signed_features = {"rbac"};
    auto key = generateLicenseKey("enterprise", "tamper-test", "2099-12-31", signed_features);
    nlohmann::json j = {
        {"edition", "enterprise"},
        {"customer", "tamper-test"},
        {"expires", "2099-12-31"},
        {"features", std::vector<std::string>{"rbac", "alerting"}},
        {"license_key", key}
    };
    auto path = writeTempLicense(j, "tampered");

    FeatureGate gate(Edition::Enterprise);
    EXPECT_FALSE(gate.loadLicense(path));
    EXPECT_FALSE(gate.isEnterprise());

    std::remove(path.c_str());
}
