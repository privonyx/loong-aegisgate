#include "multimodal/modality_router.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace aegisgate {

void ModalityRouter::registerHandler(std::unique_ptr<ModalityHandler> h) {
    if (!h) return;
    const Modality m = h->modality();
    handlers_[m].push_back(std::move(h));
}

void ModalityRouter::setRoutingPolicy(Modality m, RoutingPolicy policy) {
    policies_[m] = policy;
}

size_t ModalityRouter::handlerCount(Modality m) const {
    auto it = handlers_.find(m);
    return it == handlers_.end() ? 0u : it->second.size();
}

std::vector<std::string> ModalityRouter::registeredProviders(Modality m) const {
    std::vector<std::string> out;
    auto it = handlers_.find(m);
    if (it == handlers_.end()) return out;
    out.reserve(it->second.size());
    for (const auto& h : it->second) out.push_back(h->provider());
    return out;
}

ModalityHandler* ModalityRouter::selectHandler(Modality m,
                                                const ProxyRequest& req) const {
    auto it = handlers_.find(m);
    if (it == handlers_.end() || it->second.empty()) return nullptr;

    const auto& vec = it->second;
    if (vec.size() == 1) return vec.front().get();   // N=1 fast path

    auto strat = RoutingPolicy::Strategy::Cheapest;
    auto pit = policies_.find(m);
    if (pit != policies_.end()) strat = pit->second.strategy;

    switch (strat) {
        case RoutingPolicy::Strategy::Cheapest: {
            ModalityHandler* best = vec.front().get();
            double best_cost = best->estimateCost(req);
            for (size_t i = 1; i < vec.size(); ++i) {
                const double c = vec[i]->estimateCost(req);
                if (c < best_cost) { best = vec[i].get(); best_cost = c; }
            }
            return best;
        }
        case RoutingPolicy::Strategy::RoundRobin: {
            const size_t idx = round_robin_counter_.fetch_add(
                1, std::memory_order_relaxed) % vec.size();
            return vec[idx].get();
        }
        case RoutingPolicy::Strategy::FastestP99:
            // N=1 stub: wire up to MetricsRegistry.per_provider_p99 once N>1
            // backends are deployed. For now we degrade to "first registered".
            return vec.front().get();
    }
    return vec.front().get();
}

ProxyResponse ModalityRouter::route(Modality m,
                                    const ProxyRequest& req,
                                    const std::string& api_key) {
    auto* h = selectHandler(m, req);
    if (!h) {
        spdlog::warn("ModalityRouter: no handler registered for modality={} endpoint={}",
                     modalityToString(m), req.endpoint);
        ProxyResponse resp;
        resp.http_status = 504;
        resp.content_type = "application/json";
        const auto err = nlohmann::json{
            {"error", {
                {"code", "AEGIS-1601"},
                {"type", "modality_not_configured"},
                {"message", "No handler registered for this modality"},
                {"modality", modalityToString(m)}
            }}
        };
        resp.body = err.dump();
        return resp;
    }
    return h->handle(req, api_key);
}

} // namespace aegisgate
