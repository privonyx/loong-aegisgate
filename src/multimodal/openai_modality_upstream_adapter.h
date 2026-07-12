#pragma once
#include "gateway/connector/base.h"
#include "multimodal/modality_upstream.h"

namespace aegisgate {

// Adapter that exposes a ModelConnector (typically OpenAIConnector) as a
// ModalityUpstream so the multimodal handlers built in CR2 §4.3 can be
// composed against the existing connector + key balancer + retry stack
// without introducing a parallel HTTP path.
//
// Ownership: the adapter borrows the underlying connector (non-owning). The
// caller must guarantee the connector outlives the adapter; in production
// both live inside GatewayRuntime, so the adapter member must be declared
// AFTER connector_registry_ (so it is destroyed first at runtime shutdown).
//
// The api_key parameter on ModalityUpstream::proxy is intentionally ignored:
// authentication is already managed by the connector's internal
// KeyBalancer + ProviderConfig.api_keys, matching how every other
// modality-agnostic call (chat completions, etc.) flows today. Passing the
// key through would create a second, conflicting source of truth for
// OpenAI-compatible providers.
class OpenAIModalityUpstreamAdapter : public ModalityUpstream {
public:
    explicit OpenAIModalityUpstreamAdapter(ModelConnector* connector)
        : connector_(connector) {}

    ProxyResponse proxy(const ProxyRequest& req,
                        const std::string& /*api_key*/) override {
        if (!connector_) {
            ProxyResponse r;
            r.http_status = 503;
            r.body = R"({"error":{"code":"upstream_unavailable",)"
                      R"("message":"openai connector not registered"}})";
            return r;
        }
        return connector_->proxyRequest(req);
    }

private:
    ModelConnector* connector_;  // non-owning; lifetime guaranteed by caller
};

} // namespace aegisgate
