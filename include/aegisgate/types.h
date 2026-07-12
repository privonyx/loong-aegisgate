#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace aegisgate {

struct ToolCall {
    std::string id;
    std::string type = "function";
    nlohmann::json function;  // {"name": "...", "arguments": "..."}
};

struct Message {
    std::string role;
    std::string content;
    // P2-#4: original multimodal content array for vision requests, e.g. OpenAI
    // [{type:"text",...},{type:"image_url",...}]. Null/empty for plain text.
    // `content` holds the concatenated text parts so the guardrail pipeline can
    // scan and rewrite them; serialization re-emits this array with the text
    // refreshed from `content`, so images survive the trip upstream and any
    // text mutation (PII masking, normalization) is not bypassed.
    nlohmann::json content_parts;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
    std::string name;

    Message() = default;
    Message(std::string r, std::string c)
        : role(std::move(r)), content(std::move(c)) {}
    Message(std::string r, std::string c, std::vector<ToolCall> tc,
            std::string tcid, std::string n)
        : role(std::move(r)), content(std::move(c)), tool_calls(std::move(tc)),
          tool_call_id(std::move(tcid)), name(std::move(n)) {}
};

// P2-#4: concatenate the text segments of a multimodal content array so the
// guardrail pipeline (which operates on a single string) can scan/modify them.
inline std::string extractMultimodalText(const nlohmann::json& parts) {
    std::string text;
    if (!parts.is_array()) return text;
    for (const auto& part : parts) {
        if (part.is_object() && part.value("type", "") == "text") {
            if (!text.empty()) text += "\n";
            text += part.value("text", "");
        }
    }
    return text;
}

// P2-#4: rebuild a multimodal content array preserving non-text parts (images
// etc.) in their original positions, while collapsing the text segments into a
// single part carrying the (possibly guardrail-rewritten) `text`. The text part
// takes the slot of the first original text part; if there was none it is
// prepended so vision models still receive the prompt.
inline nlohmann::json rebuildMultimodalContent(const nlohmann::json& parts,
                                               const std::string& text) {
    nlohmann::json out = nlohmann::json::array();
    bool text_emitted = false;
    if (parts.is_array()) {
        for (const auto& part : parts) {
            const bool is_text =
                part.is_object() && part.value("type", "") == "text";
            if (is_text) {
                if (!text_emitted) {
                    out.push_back({{"type", "text"}, {"text", text}});
                    text_emitted = true;
                }
                continue;  // drop extra text parts; merged into the single one
            }
            out.push_back(part);
        }
    }
    if (!text_emitted) {
        out.insert(out.begin(), {{"type", "text"}, {"text", text}});
    }
    return out;
}

// TASK-20260707-03 / REV20260707-N19: default cap for decoding data: URI text
// payloads in the image-reference scan surface (256 KB/part, DoS guard SR-4).
inline constexpr size_t kDefaultImageScanDecodeBytes = 256 * 1024;

namespace detail {
// RFC 4648 base64 decode, output-capped. Non-alphabet chars (whitespace/newline)
// are skipped; '=' terminates. On exceeding `max_bytes` the caller treats the
// result as oversized and scans only the URI prefix instead of the payload.
inline std::string decodeBase64Capped(const std::string& in, size_t max_bytes,
                                      bool& oversized) {
    oversized = false;
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = val(c);
        if (d < 0) continue;
        buf = (buf << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
            bits -= 8;
            if (out.size() > max_bytes) {
                oversized = true;
                return out;
            }
        }
    }
    return out;
}
}  // namespace detail

// TASK-20260707-03 / REV20260707-N19: surface the *scannable* text carried by a
// multimodal content array's `image_url` parts so inbound guardrail stages can
// detect PII/keywords/injection hidden in image references (the text channel is
// covered separately by extractMultimodalText/scanText).
//
//  - http(s) or other URL       -> the URL string literal (path/query scanned).
//  - data: URI, text-class mime -> the decoded payload (base64) or raw payload.
//  - data: URI, binary mime     -> only the "data:<mime>;base64" prefix literal
//                                  (binary bytes are never decoded -> no false
//                                  positives on clean images, DoS-safe).
//  - malformed data: URI        -> degrades to the URL literal (never throws).
//
// Parts are joined with '\n'. Non-array / empty input returns "".
inline std::string extractImageRefText(const nlohmann::json& parts,
                                       size_t max_decode_bytes) {
    std::string text;
    if (!parts.is_array()) return text;
    auto append = [&](const std::string& s) {
        if (s.empty()) return;
        if (!text.empty()) text += "\n";
        text += s;
    };
    for (const auto& part : parts) {
        if (!part.is_object() || part.value("type", "") != "image_url") continue;
        std::string url;
        if (part.contains("image_url")) {
            const auto& iu = part["image_url"];
            if (iu.is_string())
                url = iu.get<std::string>();
            else if (iu.is_object())
                url = iu.value("url", "");
        }
        if (url.empty()) continue;

        if (url.rfind("data:", 0) != 0) {
            append(url);  // http(s) or other scheme — scan the literal
            continue;
        }
        // data:[<mediatype>][;base64],<data>
        std::string after = url.substr(5);
        auto comma = after.find(',');
        std::string meta = (comma == std::string::npos) ? after : after.substr(0, comma);
        std::string mime = meta.substr(0, meta.find(';'));
        bool is_base64 = meta.find(";base64") != std::string::npos;
        bool text_class = mime.empty() || mime.rfind("text/", 0) == 0 ||
                          mime == "application/json" ||
                          mime == "application/xml" ||
                          mime == "application/xhtml+xml";
        // Binary payloads (image/audio/...) carry no model-readable text: a text
        // payload mislabeled as binary never reaches the model as text, so there
        // is nothing to scan. Skipping them also avoids false positives from the
        // "...;base64" meta literal tripping encoding-evasion heuristics.
        if (!text_class) continue;
        if (comma == std::string::npos) {
            append(url);  // malformed text data URI — degrade to literal
            continue;
        }
        std::string payload = after.substr(comma + 1);
        if (is_base64) {
            bool oversized = false;  // cap keeps output bounded even when set
            std::string decoded =
                detail::decodeBase64Capped(payload, max_decode_bytes, oversized);
            (void)oversized;
            append(decoded);  // bounded to <= max_decode_bytes (+1) — SR-4
        } else {
            append(payload.size() > max_decode_bytes ? payload.substr(0, max_decode_bytes)
                                                     : payload);
        }
    }
    return text;
}

struct ChatRequest {
    std::string model;
    std::vector<Message> messages;
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    bool stream = false;
    nlohmann::json extra;
    nlohmann::json tools;
    nlohmann::json tool_choice;
    // OpenAI 2024-04 metadata field; e.g., {"conversation_id": "xyz"} for multi-turn cache (D2=C).
    std::optional<std::map<std::string, std::string>> metadata;
};

struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};

struct ChatResponse {
    std::string id;
    std::string model;
    std::string content;
    TokenUsage usage;
    std::string finish_reason;
    std::vector<ToolCall> tool_calls;
};

struct StreamDelta {
    std::string content;
    nlohmann::json tool_calls_delta;
    std::string finish_reason;
};

struct GatewayError {
    int http_status;
    std::string error_code;    // AEGIS-xxxx structured code
    std::string error_type;    // category: authentication_error, rate_limit_error, etc.
    std::string message;
    std::string internal_detail;
};

struct ProxyRequest {
    std::string endpoint;      // e.g. "/v1/embeddings"
    std::string model;         // extracted from body for routing
    std::string raw_body;
    std::string content_type = "application/json";
};

struct ProxyResponse {
    int http_status = 200;
    std::string body;
    std::string content_type = "application/json";
};

inline bool hasToolsRequest(const ChatRequest& req) {
    return !req.tools.is_null() && !req.tools.empty();
}

inline bool isToolMessage(const Message& msg) {
    return msg.role == "tool" || !msg.tool_calls.empty();
}

inline void to_json(nlohmann::json& j, const ToolCall& tc) {
    j = nlohmann::json{{"id", tc.id}, {"type", tc.type}, {"function", tc.function}};
}
inline void from_json(const nlohmann::json& j, ToolCall& tc) {
    tc.id = j.value("id", "");
    tc.type = j.value("type", "function");
    if (j.contains("function")) tc.function = j["function"];
}

inline void to_json(nlohmann::json& j, const Message& m) {
    j = nlohmann::json{{"role", m.role}};
    if (m.content_parts.is_array() && !m.content_parts.empty()) {
        // P2-#4: multimodal — re-emit parts with refreshed text so guardrail
        // mutations apply and images are preserved.
        j["content"] = rebuildMultimodalContent(m.content_parts, m.content);
    } else if (!m.content.empty() || m.tool_calls.empty()) {
        j["content"] = m.content;
    } else {
        j["content"] = nullptr;
    }
    if (!m.tool_calls.empty()) {
        j["tool_calls"] = nlohmann::json::array();
        for (const auto& tc : m.tool_calls) {
            nlohmann::json tcj;
            to_json(tcj, tc);
            j["tool_calls"].push_back(tcj);
        }
    }
    if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
    if (!m.name.empty()) j["name"] = m.name;
}
inline void from_json(const nlohmann::json& j, Message& m) {
    j.at("role").get_to(m.role);
    if (j.contains("content") && !j["content"].is_null()) {
        if (j["content"].is_string()) {
            m.content = j["content"].get<std::string>();
        } else if (j["content"].is_array()) {
            // P2-#4: preserve multimodal parts; extract text for guardrails.
            m.content_parts = j["content"];
            m.content = extractMultimodalText(j["content"]);
        }
    }
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& tc : j["tool_calls"]) {
            ToolCall tool_call;
            from_json(tc, tool_call);
            m.tool_calls.push_back(std::move(tool_call));
        }
    }
    m.tool_call_id = j.value("tool_call_id", "");
    m.name = j.value("name", "");
}

inline void to_json(nlohmann::json& j, const TokenUsage& u) {
    j = nlohmann::json{{"prompt_tokens", u.prompt_tokens},
                       {"completion_tokens", u.completion_tokens},
                       {"total_tokens", u.total_tokens}};
}
inline void from_json(const nlohmann::json& j, TokenUsage& u) {
    u.prompt_tokens = j.value("prompt_tokens", 0);
    u.completion_tokens = j.value("completion_tokens", 0);
    u.total_tokens = j.value("total_tokens", 0);
}

} // namespace aegisgate
