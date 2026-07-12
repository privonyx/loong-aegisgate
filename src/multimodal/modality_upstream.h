#pragma once
#include "aegisgate/types.h"
#include <string>

namespace aegisgate {

// Abstract upstream callable for multimodal handlers (CR2 §4.3).
//
// Handlers do not directly couple to OpenAIConnector / ModelConnector;
// instead they depend on this thin interface so the production wiring can
// adapt an existing ModelConnector and unit tests can inject a fake.
class ModalityUpstream {
public:
    virtual ~ModalityUpstream() = default;
    virtual ProxyResponse proxy(const ProxyRequest& req,
                                const std::string& api_key) = 0;
};

} // namespace aegisgate
