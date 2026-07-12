#include "feature_gate.h"
#include "core/crypto.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <unordered_map>
#include <shared_mutex>

namespace aegisgate {

namespace {

constexpr const char* LICENSE_SALT = "aegisgate-v1-f7e2a9c4d1b8";

std::string sha256Hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        spdlog::error("EVP_MD_CTX_new failed");
        return "";
    }
    if (!EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) ||
        !EVP_DigestUpdate(ctx, input.data(), input.size()) ||
        !EVP_DigestFinal_ex(ctx, hash, &hash_len)) {
        EVP_MD_CTX_free(ctx);
        spdlog::error("SHA-256 computation failed");
        return "";
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

bool isExpired(const std::string& expires_str) {
    std::tm tm = {};
    std::istringstream ss(expires_str);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) return true;

    auto expiry = std::mktime(&tm);
    auto now = std::time(nullptr);
    return now > expiry;
}

const std::unordered_map<std::string, Feature>& stringToFeatureMap() {
    static const std::unordered_map<std::string, Feature> m = {
#define AEGIS_FEATURE(name, str) {str, Feature::name},
        AEGIS_FEATURE_LIST
#undef AEGIS_FEATURE
    };
    return m;
}

} // namespace

FeatureGate::FeatureGate(Edition edition) : edition_(edition) {}

FeatureGate::FeatureGate(FeatureGate&& other) noexcept
    : edition_(other.edition_),
      license_valid_(other.license_valid_),
      customer_(std::move(other.customer_)),
      expires_(std::move(other.expires_)),
      enabled_features_(std::move(other.enabled_features_)),
      sig_method_(other.sig_method_),
      license_path_(std::move(other.license_path_)) {}

FeatureGate& FeatureGate::operator=(FeatureGate&& other) noexcept {
    if (this != &other) {
        edition_ = other.edition_;
        license_valid_ = other.license_valid_;
        customer_ = std::move(other.customer_);
        expires_ = std::move(other.expires_);
        enabled_features_ = std::move(other.enabled_features_);
        sig_method_ = other.sig_method_;
        license_path_ = std::move(other.license_path_);
    }
    return *this;
}

std::string FeatureGate::featureToString(Feature feature) {
    switch (feature) {
#define AEGIS_FEATURE(name, str) case Feature::name: return str;
        AEGIS_FEATURE_LIST
#undef AEGIS_FEATURE
    }
    return "unknown";
}

std::optional<Feature> FeatureGate::featureFromString(const std::string& str) {
    const auto& m = stringToFeatureMap();
    auto it = m.find(str);
    if (it != m.end()) return it->second;
    return std::nullopt;
}

std::vector<Feature> FeatureGate::allFeatures() {
    return {
#define AEGIS_FEATURE(name, str) Feature::name,
        AEGIS_FEATURE_LIST
#undef AEGIS_FEATURE
    };
}

void FeatureGate::enableAllFeatures() {
    for (auto f : allFeatures()) {
        enabled_features_.insert(f);
    }
}

void FeatureGate::resolveDependencies() {
    auto autoEnable = [&](Feature dependent, Feature dependency, const char* reason) {
        if (enabled_features_.count(dependent) && !enabled_features_.count(dependency)) {
            enabled_features_.insert(dependency);
            spdlog::info("Auto-enabled {} (required by {})",
                         featureToString(dependency), reason);
        }
    };

    autoEnable(Feature::WebManagement,   Feature::RBAC, "WebManagement");
    autoEnable(Feature::SSO,             Feature::RBAC, "SSO");
    autoEnable(Feature::ComplianceReport, Feature::RBAC, "ComplianceReport");
}

std::string FeatureGate::buildLicensePayload(
    const std::string& edition,
    const std::string& customer,
    const std::string& expires,
    const std::vector<std::string>& features) {

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
    return payload;
}

bool FeatureGate::validateEd25519Signature(const std::string& payload,
                                            const std::string& signature_b64,
                                            const std::string& public_key_b64) {
    if (signature_b64.empty() || public_key_b64.empty()) return false;

    // Decode base64 public key
    BIO* key_bio = BIO_new_mem_buf(public_key_b64.data(),
                                    static_cast<int>(public_key_b64.size()));
    if (!key_bio) return false;

    // base64 decode the signature
    BIO* sig_b64_bio = BIO_new(BIO_f_base64());
    BIO* sig_mem_bio = BIO_new_mem_buf(signature_b64.data(),
                                        static_cast<int>(signature_b64.size()));
    sig_b64_bio = BIO_push(sig_b64_bio, sig_mem_bio);
    BIO_set_flags(sig_b64_bio, BIO_FLAGS_BASE64_NO_NL);

    std::vector<uint8_t> sig_bytes(256);
    int sig_len = BIO_read(sig_b64_bio, sig_bytes.data(),
                           static_cast<int>(sig_bytes.size()));
    BIO_free_all(sig_b64_bio);
    if (sig_len <= 0) {
        BIO_free(key_bio);
        return false;
    }
    sig_bytes.resize(static_cast<size_t>(sig_len));

    // base64 decode the public key into DER
    BIO* pub_b64_bio = BIO_new(BIO_f_base64());
    BIO* pub_mem_bio = BIO_new_mem_buf(public_key_b64.data(),
                                        static_cast<int>(public_key_b64.size()));
    pub_b64_bio = BIO_push(pub_b64_bio, pub_mem_bio);
    BIO_set_flags(pub_b64_bio, BIO_FLAGS_BASE64_NO_NL);

    std::vector<uint8_t> pub_der(256);
    int pub_len = BIO_read(pub_b64_bio, pub_der.data(),
                           static_cast<int>(pub_der.size()));
    BIO_free_all(pub_b64_bio);
    BIO_free(key_bio);
    if (pub_len <= 0) return false;

    const uint8_t* pub_ptr = pub_der.data();
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &pub_ptr, pub_len);
    if (!pkey) return false;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool ok = EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) == 1;
    if (ok) {
        int result = EVP_DigestVerify(
            md_ctx,
            sig_bytes.data(), sig_bytes.size(),
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        ok = (result == 1);
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool FeatureGate::loadLicense(const std::string& license_path) {
    std::unique_lock<std::shared_mutex> lock(fg_mutex_);
    if (edition_ != Edition::Enterprise) {
        license_valid_ = false;
        return true;
    }

    license_path_ = license_path;
    return parseLicenseJson(license_path);
}

bool FeatureGate::reloadLicense(const std::string& license_path) {
    std::unique_lock<std::shared_mutex> lock(fg_mutex_);
    auto path = license_path.empty() ? license_path_ : license_path;
    if (path.empty()) return false;

    enabled_features_.clear();
    license_valid_ = false;
    edition_ = Edition::Enterprise;
    return parseLicenseJson(path);
}

bool FeatureGate::parseLicenseJson(const std::string& license_path) {
    if (license_path.empty()) {
        spdlog::warn("Enterprise edition requires a license file — "
                     "falling back to Community");
        edition_ = Edition::Community;
        return false;
    }

    try {
        std::ifstream ifs(license_path);
        if (!ifs.is_open()) {
            spdlog::warn("Cannot open license file: {} — "
                         "falling back to Community", license_path);
            edition_ = Edition::Community;
            return false;
        }

        auto j = nlohmann::json::parse(ifs);
        auto edition_str = j.value("edition", "");
        customer_ = j.value("customer", "");
        expires_ = j.value("expires", "");
        auto license_key = j.value("license_key", "");

        if (edition_str != "enterprise") {
            spdlog::warn("License edition mismatch: expected 'enterprise', "
                         "got '{}' — falling back to Community", edition_str);
            edition_ = Edition::Community;
            return false;
        }

        std::vector<std::string> feature_strings;
        bool has_features_field = j.contains("features");
        if (has_features_field) {
            feature_strings = j["features"].get<std::vector<std::string>>();
        }

        // Check for Ed25519 signature (preferred) or fall back to checksum
        auto signature = j.value("signature", "");
        auto public_key = j.value("public_key", "");

        bool sig_valid = false;
        if (!signature.empty() && !public_key.empty()) {
            auto payload = buildLicensePayload(edition_str, customer_,
                                               expires_, feature_strings);
            sig_valid = validateEd25519Signature(payload, signature, public_key);
            if (sig_valid) {
                sig_method_ = LicenseSignatureMethod::Ed25519;
                spdlog::info("License validated with Ed25519 signature");
            } else {
                spdlog::warn("Ed25519 signature verification failed — "
                             "falling back to Community");
                edition_ = Edition::Community;
                return false;
            }
        } else {
            sig_valid = validateLicenseKey(edition_str, customer_, expires_,
                                           license_key, feature_strings);
            if (!sig_valid) {
                spdlog::warn("Invalid license key — falling back to Community");
                edition_ = Edition::Community;
                return false;
            }
            sig_method_ = LicenseSignatureMethod::ChecksumV1;
        }

        if (isExpired(expires_)) {
            spdlog::warn("License expired on {} — falling back to Community",
                         expires_);
            edition_ = Edition::Community;
            license_valid_ = false;
            return false;
        }

        license_valid_ = true;

        if (!has_features_field) {
            enableAllFeatures();
        } else {
            for (const auto& fs : feature_strings) {
                auto feat = featureFromString(fs);
                if (feat) {
                    enabled_features_.insert(*feat);
                } else {
                    spdlog::warn("Unknown feature in license: '{}' — ignored", fs);
                }
            }
            resolveDependencies();
        }

        spdlog::info("Enterprise license valid: customer={}, expires={}, "
                     "features={}, method={}",
                     customer_, expires_, enabled_features_.size(),
                     sig_method_ == LicenseSignatureMethod::Ed25519
                         ? "ed25519" : "checksum_v1");
        return true;

    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse license file: {} — "
                     "falling back to Community", e.what());
        edition_ = Edition::Community;
        return false;
    }
}

bool FeatureGate::isLicenseValid() const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    return license_valid_;
}

std::set<Feature> FeatureGate::enabledFeatures() const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    return enabled_features_;
}

const std::string& FeatureGate::customer() const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    return customer_;
}

const std::string& FeatureGate::expires() const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    return expires_;
}

LicenseSignatureMethod FeatureGate::signatureMethod() const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    return sig_method_;
}

bool FeatureGate::isEnterprise() const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    return edition_ == Edition::Enterprise && license_valid_;
}

bool FeatureGate::isEnabled(Feature feature) const {
    std::shared_lock<std::shared_mutex> lock(fg_mutex_);
    if (edition_ != Edition::Enterprise || !license_valid_) return false;
    return enabled_features_.count(feature) > 0;
}

bool FeatureGate::validateLicenseKey(const std::string& edition,
                                      const std::string& customer,
                                      const std::string& expires,
                                      const std::string& license_key,
                                      const std::vector<std::string>& features) {
    if (license_key.size() < 16) return false;

    auto payload = buildLicensePayload(edition, customer, expires, features);

    std::string full_hash = sha256Hex(payload + LICENSE_SALT);
    std::string expected_checksum = full_hash.substr(0, 16);

    std::string actual_checksum = license_key.substr(license_key.size() - 16);

    return crypto::constantTimeEquals(expected_checksum, actual_checksum);
}

FeatureGate FeatureGate::createUnlocked(Edition edition) {
    FeatureGate gate(edition);
    if (edition == Edition::Enterprise) {
        gate.license_valid_ = true;
        gate.customer_ = "test-unlocked";
        gate.expires_ = "2099-12-31";
        gate.enableAllFeatures();
    }
    return gate;
}

} // namespace aegisgate
