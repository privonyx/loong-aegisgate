#include "fallback.h"
#include <spdlog/spdlog.h>
#include <utility>

namespace aegisgate {

FallbackManager::FallbackManager(ConnectorRegistry& registry)
    : registry_(registry), circuit_breaker_{} {}

FallbackManager::FallbackManager(ConnectorRegistry& registry, CircuitConfig circuit_config)
    : registry_(registry), circuit_breaker_{std::move(circuit_config)} {}

void FallbackManager::setChain(const std::string& primary,
                                const std::vector<std::string>& fallbacks) {
    FallbackChain chain;
    chain.models = fallbacks;
    chains_[primary] = std::move(chain);
}

std::vector<std::string> FallbackManager::getChain(const std::string& model) const {
    auto it = chains_.find(model);
    if (it != chains_.end()) {
        return it->second.models;
    }
    return {};
}

ChatResponse FallbackManager::executeWithFallback(
    const ChatRequest& req,
    const std::string& primary_model) {

    // Build ordered list: primary + fallbacks
    std::vector<std::string> candidates = {primary_model};
    auto it = chains_.find(primary_model);
    if (it != chains_.end()) {
        candidates.insert(candidates.end(),
                          it->second.models.begin(),
                          it->second.models.end());
    }

    std::string last_error;
    bool any_attempt = false;
    bool all_no_keys = true;  // P0-3: preserve NoHealthyKeys when every attempt
                              // failed solely due to exhausted key pools.
    bool any_circuit_open = false;  // P1-A: a candidate was skipped (circuit open)
    bool saw_upstream_status = false;  // P1-A: at least one definitive upstream status
    int last_upstream_status = 0;
    for (const auto& model : candidates) {
        if (!circuit_breaker_.allowRequest(model)) {
            spdlog::warn("Fallback: circuit open for {}, skipping", model);
            any_circuit_open = true;
            continue;
        }

        auto* connector = registry_.findByModel(model);
        if (!connector) {
            spdlog::warn("Fallback: model {} not found, skipping", model);
            continue;
        }

        any_attempt = true;
        try {
            ChatRequest modified = req;
            modified.model = model;
            auto resp = connector->complete(modified);
            circuit_breaker_.recordSuccess(model);
            if (model != primary_model) {
                spdlog::info("Fallback succeeded: {} -> {}", primary_model, model);
            }
            return resp;
        } catch (const std::exception& e) {
            last_error = e.what();
            if (auto* use = dynamic_cast<const UpstreamStatusError*>(&e)) {
                saw_upstream_status = true;
                last_upstream_status = use->upstreamStatus();
                all_no_keys = false;
            } else if (!dynamic_cast<const NoHealthyKeysError*>(&e)) {
                all_no_keys = false;
            }
            circuit_breaker_.recordFailure(model);
            spdlog::warn("Model {} failed: {}, trying next fallback", model, last_error);
        }
    }

    // P0-3: if we actually attempted upstreams and every one reported an
    // exhausted key pool, surface AEGIS-4007 (503) so operators can tell a
    // capacity/config problem apart from a generic upstream failure (502).
    if (any_attempt && all_no_keys) {
        throw NoHealthyKeysError(
            toAegisCode(ErrorCode::NoHealthyKeys) + ": " +
            std::string(toDefaultMessage(ErrorCode::NoHealthyKeys)) +
            ". Last error: " + last_error);
    }

    // P1-A: surface the real upstream status (429/5xx/4xx/timeout) if any
    // candidate produced one, instead of collapsing into a generic 502.
    if (saw_upstream_status) {
        throw UpstreamStatusError(
            last_upstream_status,
            toAegisCode(ErrorCode::UpstreamError) + ": upstream returned " +
            std::to_string(last_upstream_status) + ". Last error: " + last_error);
    }

    // P1-A: nothing was attempted and at least one candidate was skipped because
    // its circuit breaker is open → AEGIS-4002 (503), not a generic 502.
    if (!any_attempt && any_circuit_open) {
        throw CircuitBreakerOpenError(
            toAegisCode(ErrorCode::CircuitBreakerOpen) + ": " +
            std::string(toDefaultMessage(ErrorCode::CircuitBreakerOpen)) +
            " for " + primary_model);
    }

    throw std::runtime_error(
        toAegisCode(ErrorCode::UpstreamError) + ": " +
        std::string(toDefaultMessage(ErrorCode::UpstreamError)) +
        ". Last error: " + last_error);
}

void FallbackManager::streamWithFallback(
    const ChatRequest& req,
    const std::string& primary_model,
    std::function<void(const StreamDelta&)> onDelta,
    std::function<void(const TokenUsage&)> onDone,
    std::function<void(const GatewayError&)> onError) {

    auto state = std::make_shared<StreamFallbackState>();
    state->base_req = req;
    state->onDelta = std::move(onDelta);
    state->onDone = std::move(onDone);
    state->onError = std::move(onError);

    state->candidates = {primary_model};
    auto it = chains_.find(primary_model);
    if (it != chains_.end()) {
        state->candidates.insert(state->candidates.end(),
                                 it->second.models.begin(),
                                 it->second.models.end());
    }

    tryStreamModel(state, 0);
}

void FallbackManager::tryStreamModel(
    std::shared_ptr<StreamFallbackState> state, size_t idx) {

    while (idx < state->candidates.size()) {
        const auto& model = state->candidates[idx];

        if (!circuit_breaker_.allowRequest(model)) {
            spdlog::warn("Stream fallback: circuit open for {}, skipping", model);
            state->any_circuit_open.store(true, std::memory_order_relaxed);
            ++idx;
            continue;
        }

        auto* connector = registry_.findByModel(model);
        if (!connector) {
            spdlog::warn("Stream fallback: model {} not found, skipping", model);
            ++idx;
            continue;
        }

        ChatRequest modified = state->base_req;
        modified.model = model;
        std::string primary = state->candidates[0];
        state->any_attempt.store(true, std::memory_order_relaxed);

        connector->streamComplete(modified,
            [state](const StreamDelta& delta) {
                state->chunks_sent.store(true, std::memory_order_relaxed);
                state->onDelta(delta);
            },
            [state, model, primary, this](const TokenUsage& usage) {
                circuit_breaker_.recordSuccess(model);
                if (model != primary) {
                    spdlog::info("Stream fallback succeeded: {} -> {}", primary, model);
                }
                state->onDone(usage);
            },
            [state, model, idx, this](const GatewayError& err) {
                circuit_breaker_.recordFailure(model);
                state->last_err = err;
                spdlog::warn("Stream model {} failed: {}", model, err.message);

                if (state->chunks_sent.load(std::memory_order_relaxed)) {
                    state->onError(err);
                    return;
                }
                tryStreamModel(state, idx + 1);
            });
        return;
    }

    // P1-D: all candidates exhausted. If nothing was ever attempted because
    // every candidate's circuit was open, surface a typed 503 circuit-breaker
    // error (parity with the non-streaming CircuitBreakerOpenError path) rather
    // than the generic 502 default.
    if (!state->any_attempt.load(std::memory_order_relaxed) &&
        state->any_circuit_open.load(std::memory_order_relaxed)) {
        GatewayError err{
            toHttpStatus(ErrorCode::CircuitBreakerOpen),
            toAegisCode(ErrorCode::CircuitBreakerOpen),
            toErrorType(ErrorCode::CircuitBreakerOpen),
            toDefaultMessage(ErrorCode::CircuitBreakerOpen),
            "All fallback candidates skipped (circuit breaker open)"};
        state->onError(err);
        return;
    }

    state->onError(state->last_err);
}

} // namespace aegisgate
