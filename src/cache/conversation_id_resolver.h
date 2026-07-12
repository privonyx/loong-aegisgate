#pragma once
#include "aegisgate/types.h"
#include <string>

namespace aegisgate {

// Resolves a stable conversation id for partition-key generation (D2=C, hybrid mode).
// Priority:
//   1. client-provided metadata.conversation_id (sanitized, truncated)
//   2. multi-turn history hash ("hist-" prefix, sha256 truncated)
//   3. first-turn fallback ("first-" prefix, sha256 truncated)
//   4. empty request -> empty string
//
// SECURITY: This resolver only produces an opaque id string. Cross-tenant isolation
// is enforced by SemanticCache mixing tenant_id into the partition_key (SR1).
class ConversationIdResolver {
public:
    std::string resolve(const ChatRequest& req) const;

    static constexpr size_t kMaxIdLen = 256;

private:
    std::string sanitizeClientId(const std::string& raw) const;
    std::string hashHistory(const std::vector<Message>& msgs) const;
    std::string firstTurnId(const std::vector<Message>& msgs) const;
};

} // namespace aegisgate
