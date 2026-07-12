#pragma once
#include "multimodal/modality.h"
#include "multimodal/modality_handler.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aegisgate {

struct RoutingPolicy {
    enum class Strategy {
        Cheapest = 0,    // pick handler with the lowest estimateCost(req)
        RoundRobin = 1,  // rotating index
        FastestP99 = 2,  // future MetricsRegistry integration; N=1 stub returns front()
    };
    Strategy strategy = Strategy::Cheapest;
};

// CR2 scheme A: fat Router.
// - holds map<Modality, vector<unique_ptr<Handler>>>
// - selects via per-modality RoutingPolicy
// - N=1 short-circuit returns front() (zero-overhead happy path)
// - N>1 (future) iterates and picks per strategy
//
// Thread safety: registerHandler / setRoutingPolicy are NOT thread-safe;
// they are expected to run only during GatewayRuntime::initialize().
// route() / selectHandler() / handlerCount() are safe to call concurrently
// after initialization.
class ModalityRouter {
public:
    void registerHandler(std::unique_ptr<ModalityHandler> h);
    void setRoutingPolicy(Modality m, RoutingPolicy policy);

    // Returns ProxyResponse with http_status==504 + error body when no
    // handler is registered for the modality (or modality is Unknown).
    ProxyResponse route(Modality m, const ProxyRequest& req,
                        const std::string& api_key);

    size_t handlerCount(Modality m) const;
    std::vector<std::string> registeredProviders(Modality m) const;

    // Exposed for tests / metric sampling. Returns nullptr if no handler.
    ModalityHandler* selectHandler(Modality m, const ProxyRequest& req) const;

private:
    std::map<Modality, std::vector<std::unique_ptr<ModalityHandler>>> handlers_;
    std::map<Modality, RoutingPolicy> policies_;
    mutable std::atomic<size_t> round_robin_counter_{0};
};

} // namespace aegisgate
