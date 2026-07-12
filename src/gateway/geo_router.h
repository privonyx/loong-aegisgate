#pragma once
#include "gateway/router.h"
#include "gateway/connector/base.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisgate {

struct GeoConfig {
    enum class Affinity { Strict, Prefer, Any };

    bool enabled = false;
    Affinity affinity = Affinity::Prefer;
    std::string default_client_region = "us-east";
    std::vector<std::string> header_names = {"X-AegisGate-Region", "X-Client-Region"};
    // (cidr, region) list; invalid entries are ignored at lookup time.
    std::vector<std::pair<std::string, std::string>> ip_region_map;
    // Arbitrary alias → canonical region (applied after lowercase trim).
    std::unordered_map<std::string, std::string> region_aliases;

    static Affinity parseAffinity(const std::string& s);
};

// GeoRouter is a decorator that filters candidate models by the caller's
// geographic region before delegating the actual choice to the wrapped router.
// When disabled, GeoRouter forwards every request verbatim to the underlying
// router without touching RequestContext.
class GeoRouter : public Router {
public:
    GeoRouter(std::unique_ptr<Router> base, GeoConfig config);

    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;

    // TASK-20260703-02 C12：装饰器必须把 outcome 反馈转发给内层路由，否则被装饰的
    // 学习型路由（MLRouter/BanditRouter）永远收不到 EMA 更新。
    void reportOutcome(const std::string& model, double latency_ms,
                       bool success) override {
        if (base_) base_->reportOutcome(model, latency_ms, success);
    }

    // Extracts "region:<name>" tags from a ModelInfo's tag list.
    static std::vector<std::string> modelRegions(const ModelInfo& info);

private:
    std::string inferClientRegion(const RequestContext& ctx) const;
    std::string lookupRegionByIp(const std::string& ip) const;
    std::string normalizeRegion(std::string region) const;
    bool isResidencyStrict(const RequestContext& ctx) const;

    // Returns model IDs allowed under the given client_region / residency.
    std::vector<std::string> filterModelsByRegion(
        const ConnectorRegistry& registry,
        const std::string& client_region,
        bool residency_strict) const;

    // Utility: is the given model ID compliant with the allowed list?
    static bool isAllowed(const std::string& model,
                          const std::vector<std::string>& allowed);

    // Regional tag of the chosen model, "unknown" if unregionalized, empty
    // if the model is not in the registry.
    static std::string regionOfModel(const ConnectorRegistry& registry,
                                     const std::string& model,
                                     const std::string& prefer_region);

    std::unique_ptr<Router> base_;
    GeoConfig config_;
};

} // namespace aegisgate
