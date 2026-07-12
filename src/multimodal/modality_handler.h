#pragma once
#include "aegisgate/types.h"
#include "multimodal/modality.h"
#include <string>

namespace aegisgate {

// CR2 scheme A: thin Handler — one (Modality, Provider) tuple.
//
// Each Handler concretely implements the upstream call for exactly one
// modality + provider combination. Provider selection is the Router's
// responsibility (see modality_router.h).
class ModalityHandler {
public:
    virtual ~ModalityHandler() = default;

    virtual ProxyResponse handle(const ProxyRequest& req,
                                  const std::string& api_key) = 0;
    virtual Modality modality() const = 0;
    virtual std::string provider() const = 0;        // e.g. "openai"
    virtual double estimateCost(const ProxyRequest& req) const = 0;
    virtual std::string name() const = 0;            // e.g. "openai_embedding"
};

} // namespace aegisgate
