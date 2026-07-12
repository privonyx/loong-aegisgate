#include "guardrail/inbound/pii_filter.h"
#include "guardrail/re2_replacement_escape.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <shared_mutex>

namespace aegisgate {

PIIFilter::PIIFilter() {
    addDefaultPatterns();
}

void PIIFilter::addDefaultPatterns() {
    // Longer/more specific patterns first to avoid partial matches
    addPattern("id_card",
               "[1-9]\\d{5}(?:19|20)\\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\\d|3[01])\\d{3}[\\dXx]",
               "[ID_CARD]");
    addPattern("bank_card", "\\b(?:62|4|5[1-5])\\d{14,17}\\b", "[BANK_CARD]");
    addPattern("jwt",
               "eyJ[a-zA-Z0-9_\\-]+\\.eyJ[a-zA-Z0-9_\\-]+\\.[a-zA-Z0-9_\\-]+",
               "[JWT]");
    addPattern("api_key",
               "(?:sk|pk|api|key|token|secret)[_\\-]?[a-zA-Z0-9]{20,}",
               "[API_KEY]");
    addPattern("phone", "\\b1[3-9]\\d{9}\\b", "[PHONE]");
    addPattern("email",
               "[a-zA-Z0-9._%+\\-]+@[a-zA-Z0-9.\\-]+\\.[a-zA-Z]{2,}",
               "[EMAIL]");
}

void PIIFilter::loadPatterns(const std::string& yaml_path) {
    std::unique_lock<std::shared_mutex> lock(patterns_mutex_);
    std::vector<PIIPattern> backup = std::move(patterns_);
    try {
        auto root = YAML::LoadFile(yaml_path);
        std::vector<PIIPattern> new_patterns;

        auto addToVec = [&new_patterns](const std::string& n,
                                         const std::string& pat,
                                         const std::string& repl) {
            PIIPattern pp;
            pp.name = n;
            std::string escaped;
            escaped.reserve(repl.size() * 2);
            for (char c : repl) {
                if (c == '\\') escaped += "\\\\";
                else escaped += c;
            }
            pp.replacement = escaped;
            pp.regex = std::make_unique<RE2>(pat);
            if (pp.regex->ok()) {
                new_patterns.push_back(std::move(pp));
            } else {
                spdlog::warn("Invalid PII regex '{}': {}", n, pp.regex->error());
            }
        };

        auto loadLocale = [&addToVec](const YAML::Node& locale) {
            if (!locale) return;
            for (const auto& p : locale) {
                addToVec(p["name"].as<std::string>(),
                         p["pattern"].as<std::string>(),
                         p["replacement"].as<std::string>());
            }
        };

        loadLocale(root["locale_zh_CN"]);
        loadLocale(root["locale_global"]);

        for (auto& d : backup) {
            bool overridden = false;
            for (const auto& p : new_patterns) {
                if (p.name == d.name) {
                    overridden = true;
                    break;
                }
            }
            if (!overridden) {
                new_patterns.push_back(std::move(d));
            }
        }

        patterns_ = std::move(new_patterns);
        spdlog::info("Loaded {} PII patterns", patterns_.size());
    } catch (const YAML::Exception& e) {
        patterns_ = std::move(backup);
        spdlog::error("Failed to load PII patterns: {}", e.what());
    }
}

void PIIFilter::reloadPatterns(const std::string& yaml_path) {
    std::vector<PIIPattern> new_patterns;

    try {
        auto root = YAML::LoadFile(yaml_path);

        auto addToVec = [&new_patterns](const std::string& n,
                                         const std::string& pat,
                                         const std::string& repl) {
            PIIPattern pp;
            pp.name = n;
            pp.replacement = repl;
            // Escape backslashes for RE2 replacement strings
            std::string escaped;
            escaped.reserve(repl.size() * 2);
            for (char c : repl) {
                if (c == '\\') escaped += "\\\\";
                else escaped += c;
            }
            pp.replacement = escaped;
            pp.regex = std::make_unique<RE2>(pat);
            if (pp.regex->ok()) {
                new_patterns.push_back(std::move(pp));
            } else {
                spdlog::warn("Invalid PII regex '{}': {}", n, pp.regex->error());
            }
        };

        auto loadLocale = [&addToVec](const YAML::Node& locale) {
            if (!locale) return;
            for (const auto& p : locale) {
                addToVec(p["name"].as<std::string>(),
                         p["pattern"].as<std::string>(),
                         p["replacement"].as<std::string>());
            }
        };

        loadLocale(root["locale_zh_CN"]);
        loadLocale(root["locale_global"]);

        // Merge with defaults: build default set, skip those overridden by YAML
        std::vector<PIIPattern> defaults;
        {
            auto addDefault = [&defaults](const std::string& n,
                                           const std::string& pat,
                                           const std::string& repl) {
                PIIPattern pp;
                pp.name = n;
                std::string escaped;
                escaped.reserve(repl.size() * 2);
                for (char c : repl) {
                    if (c == '\\') escaped += "\\\\";
                    else escaped += c;
                }
                pp.replacement = escaped;
                pp.regex = std::make_unique<RE2>(pat);
                if (pp.regex->ok()) defaults.push_back(std::move(pp));
            };
            addDefault("id_card",
                       "[1-9]\\d{5}(?:19|20)\\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\\d|3[01])\\d{3}[\\dXx]",
                       "[ID_CARD]");
            addDefault("bank_card", "\\b(?:62|4|5[1-5])\\d{14,17}\\b", "[BANK_CARD]");
            addDefault("jwt",
                       "eyJ[a-zA-Z0-9_\\-]+\\.eyJ[a-zA-Z0-9_\\-]+\\.[a-zA-Z0-9_\\-]+",
                       "[JWT]");
            addDefault("api_key",
                       "(?:sk|pk|api|key|token|secret)[_\\-]?[a-zA-Z0-9]{20,}",
                       "[API_KEY]");
            addDefault("phone", "\\b1[3-9]\\d{9}\\b", "[PHONE]");
            addDefault("email",
                       "[a-zA-Z0-9._%+\\-]+@[a-zA-Z0-9.\\-]+\\.[a-zA-Z]{2,}",
                       "[EMAIL]");
        }

        for (auto& d : defaults) {
            bool overridden = false;
            for (const auto& p : new_patterns) {
                if (p.name == d.name) { overridden = true; break; }
            }
            if (!overridden) new_patterns.push_back(std::move(d));
        }

        std::unique_lock<std::shared_mutex> lock(patterns_mutex_);
        patterns_ = std::move(new_patterns);
        spdlog::info("Reloaded {} PII patterns", patterns_.size());
    } catch (const YAML::Exception& e) {
        spdlog::error("Failed to reload PII patterns: {}", e.what());
    }
}

void PIIFilter::addPattern(const std::string& name, const std::string& pattern,
                            const std::string& replacement) {
    PIIPattern pp;
    pp.name = name;
    pp.replacement = escapeRe2Replacement(replacement);
    pp.regex = std::make_unique<RE2>(pattern);
    if (pp.regex->ok()) {
        patterns_.push_back(std::move(pp));
    } else {
        spdlog::warn("Invalid PII regex '{}': {}", name, pp.regex->error());
    }
}

std::string PIIFilter::mask(const std::string& text) const {
    std::shared_lock<std::shared_mutex> lock(patterns_mutex_);
    std::string result = text;
    for (const auto& pat : patterns_) {
        RE2::GlobalReplace(&result, *pat.regex, pat.replacement);
    }
    return result;
}

StageResult PIIFilter::process(RequestContext& ctx) {
    if (outbound_) {
        // P1-3 / SR-2: redact PII in the non-streaming model response. The
        // gateway reflects accumulated_response into the response body via
        // finalizeNonStreamingResponse(), so masking here closes the leak.
        if (ctx.accumulated_response.empty()) return StageResult::Continue;
        std::string masked = mask(ctx.accumulated_response);
        if (masked != ctx.accumulated_response) {
            spdlog::info("PII masked in response {}", ctx.request_id);
            ctx.accumulated_response = std::move(masked);
        }
        return StageResult::Continue;
    }
    // P1-C: consume the InputPreprocessor's normalized view so PII hidden behind
    // homoglyph / full-width / encoding obfuscation is still caught. We stay
    // conservative: the raw payload is only rewritten when PII is actually
    // redacted — pure normalization (no PII) does NOT alter the upstream content.
    auto& messages = ctx.chat_request.messages;
    const bool use_normalized =
        ctx.input_preprocessed &&
        ctx.normalized_messages.size() == messages.size();
    for (size_t i = 0; i < messages.size(); ++i) {
        auto& msg = messages[i];
        if (msg.role == "system" || isToolMessage(msg)) continue;

        // TASK-20260707-03 / REV20260707-N19: image references are a detection-
        // only side channel. PII hidden in an image_url / data: URI can't be
        // masked in place (the bytes go upstream verbatim), so reject the whole
        // request when PII surfaces there.
        std::string image_ref = ctx.scanImageText(i);
        if (!image_ref.empty() && mask(image_ref) != image_ref) {
            spdlog::warn("PII detected in image reference of request {} (role={})",
                         ctx.request_id, msg.role);
            return StageResult::Reject;
        }

        if (use_normalized && ctx.normalized_messages[i] != msg.content) {
            // Obfuscated content: redact PII visible only after normalization,
            // and forward the masked normalized form so the raw never leaks.
            std::string masked_norm = mask(ctx.normalized_messages[i]);
            if (masked_norm != ctx.normalized_messages[i]) {
                spdlog::info("PII masked (normalized) in request {} (role={})",
                             ctx.request_id, msg.role);
                msg.content = std::move(masked_norm);
                continue;
            }
        }

        std::string masked = mask(msg.content);
        if (masked != msg.content) {
            spdlog::info("PII masked in request {} (role={})",
                         ctx.request_id, msg.role);
            msg.content = std::move(masked);
        }
    }
    return StageResult::Continue;
}

StageResult PIIFilter::processChunk(RequestContext& ctx,
                                     std::string_view chunk) {
    std::string combined = prev_tail_ + std::string(chunk);
    std::string masked = mask(combined);  // mask() acquires shared_lock internally

    size_t overlap_start = prev_tail_.size();
    if (masked.size() >= overlap_start) {
        ctx.chunk_output = masked.substr(overlap_start);
    } else {
        ctx.chunk_output = masked;
    }

    if (chunk.size() >= kChunkOverlap) {
        prev_tail_ = std::string(chunk.substr(chunk.size() - kChunkOverlap));
    } else {
        prev_tail_ = std::string(chunk);
    }
    return StageResult::Continue;
}

} // namespace aegisgate
