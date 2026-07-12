#include "cache/conversation_id_resolver.h"
#include "core/crypto.h"
#include <algorithm>

namespace aegisgate {

std::string ConversationIdResolver::sanitizeClientId(const std::string& raw) const {
    std::string out;
    out.reserve(std::min(raw.size(), kMaxIdLen));
    for (unsigned char c : raw) {
        if (out.size() >= kMaxIdLen) break;
        // Keep printable ASCII (0x20..0x7E) and high bytes (UTF-8 multibyte).
        if (c >= 0x20 && c != 0x7F) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string ConversationIdResolver::hashHistory(const std::vector<Message>& msgs) const {
    // Exclude the trailing user message (treated as "current turn"), hash the rest.
    std::string combined;
    combined.reserve(1024);
    size_t end = msgs.size();
    if (end > 0 && msgs.back().role == "user") --end;
    for (size_t i = 0; i < end; ++i) {
        combined.append(msgs[i].role);
        combined.push_back('\x1f');
        combined.append(msgs[i].content);
        combined.push_back('\x1e');
    }
    return "hist-" + crypto::sha256(combined).substr(0, 32);
}

std::string ConversationIdResolver::firstTurnId(const std::vector<Message>& msgs) const {
    if (msgs.empty()) return "";
    const auto& m = msgs.back();
    return "first-" + crypto::sha256(m.role + ":" + m.content).substr(0, 32);
}

std::string ConversationIdResolver::resolve(const ChatRequest& req) const {
    if (req.messages.empty()) return "";

    if (req.metadata) {
        auto it = req.metadata->find("conversation_id");
        if (it != req.metadata->end() && !it->second.empty()) {
            auto sanitized = sanitizeClientId(it->second);
            if (!sanitized.empty()) return sanitized;
        }
    }

    // Multi-message conversation -> stable hash over history.
    // Single-message request (regardless of role) is "first turn" by definition.
    if (req.messages.size() >= 2) return hashHistory(req.messages);
    return firstTurnId(req.messages);
}

} // namespace aegisgate
