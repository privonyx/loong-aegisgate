#pragma once
#include "core/context.h"
#include "connector/registry.h"
#include <string>

namespace aegisgate {

class Router {
public:
    virtual ~Router() = default;
    virtual std::string selectModel(RequestContext& ctx,
                                     const ConnectorRegistry& registry) = 0;

    // P1-E: outcome feedback hook. Learning routers (MLRouter,
    // MultiObjectiveRouter, BanditRouter) override this to update their
    // latency/success statistics. Declaring it on the base interface lets the
    // gateway report outcomes polymorphically instead of dynamic_cast-ing to a
    // single concrete type — previously only a raw MLRouter received feedback,
    // so a wrapped/decorated router (e.g. BanditRouter) never learned. Default
    // is a no-op for stateless routers (Basic/CostAware/Geo/ABTest).
    virtual void reportOutcome(const std::string& /*model*/,
                               double /*latency_ms*/,
                               bool /*success*/) {}
};

// P2-#6: the Phase 11.2 self-evolving router stack (RoutingStrategyCatalog +
// MultiObjectiveRouter + BanditRouter) is implemented and unit-tested but
// intentionally reachable only from the offline replay/shadow-evaluation path
// (replay CLI), never the live request hot path — bandit_mode defaults to
// "shadow". This classifies those config names so the runtime can warn an
// operator who sets `routing.type` to one of them, instead of silently
// degrading to CostAware. Returns false for the live-wired live types
// (ml/basic/cost_aware) and genuinely unknown typos.
inline bool isOfflineOnlyRouterType(const std::string& type) {
    return type == "multi_objective" || type == "bandit" ||
           type == "catalog" || type == "self_evolving";
}

class BasicRouter : public Router {
public:
    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;
};

class CostAwareRouter : public Router {
public:
    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;

private:
    size_t estimateComplexity(const RequestContext& ctx) const;
    std::string selectByCharCount(const RequestContext& ctx,
                                  const ConnectorRegistry& registry) const;
};

} // namespace aegisgate
