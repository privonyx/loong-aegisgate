#include "guardrail/outbound/hallucination.h"
#include "observe/metrics.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace aegisgate {

HallucinationDetector::HallucinationDetector() = default;

void HallucinationDetector::setThreshold(double threshold) {
    threshold_ = threshold;
}

void HallucinationDetector::setGroundTruthConfig(const GroundTruthConfig& config) {
    ground_truth_config_ = config;
}

double HallucinationDetector::measureGroundedness(
    const std::string& output,
    const std::vector<std::string>& reference_texts) const {
    if (output.empty() || reference_texts.empty()) return 0.0;

    std::vector<std::string> sentences;
    std::string current;
    for (char c : output) {
        current += c;
        if (c == '.' || c == '!' || c == '?') {
            std::string trimmed;
            for (char ch : current) {
                if (!std::isspace(static_cast<unsigned char>(ch)) || !trimmed.empty()) {
                    trimmed += ch;
                }
            }
            if (trimmed.size() > 5) {
                sentences.push_back(std::move(trimmed));
            }
            current.clear();
        }
    }
    if (sentences.empty()) {
        sentences.push_back(output);
    }

    auto extract_words = [](const std::string& text) {
        std::set<std::string> words;
        std::string word;
        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                word += std::tolower(static_cast<unsigned char>(c));
            } else if (!word.empty()) {
                if (word.size() > 2) words.insert(word);
                word.clear();
            }
        }
        if (!word.empty() && word.size() > 2) words.insert(word);
        return words;
    };

    std::set<std::string> ref_words;
    for (const auto& ref : reference_texts) {
        auto w = extract_words(ref);
        ref_words.insert(w.begin(), w.end());
    }

    if (ref_words.empty()) return 0.0;

    double total_score = 0.0;
    for (const auto& sentence : sentences) {
        auto sent_words = extract_words(sentence);
        if (sent_words.empty()) continue;

        int overlap = 0;
        for (const auto& w : sent_words) {
            if (ref_words.count(w)) ++overlap;
        }
        total_score += static_cast<double>(overlap) / sent_words.size();
    }

    return total_score / sentences.size();
}

double HallucinationDetector::countSpecificClaims(const std::string& text) const {
    int claim_count = 0;
    int total_sentences = 0;

    // Count sentences (rough approximation)
    for (char c : text) {
        if (c == '.' || c == '!' || c == '?') total_sentences++;
    }
    if (total_sentences == 0) total_sentences = 1;

    re2::StringPiece input_sp(text);

    re2::StringPiece sp1 = input_sp;
    while (RE2::FindAndConsume(&sp1, date_re_)) claim_count++;

    re2::StringPiece sp2 = input_sp;
    while (RE2::FindAndConsume(&sp2, url_re_)) claim_count++;

    re2::StringPiece sp3 = input_sp;
    while (RE2::FindAndConsume(&sp3, num_re_)) claim_count++;

    return static_cast<double>(claim_count) / total_sentences;
}

double HallucinationDetector::measureInputRelevance(
    const std::string& output, const std::string& input) const {
    // Extract words from input
    std::set<std::string> input_words;
    std::string word;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += std::tolower(static_cast<unsigned char>(c));
        } else if (!word.empty()) {
            if (word.size() > 3) input_words.insert(word);
            word.clear();
        }
    }
    if (!word.empty() && word.size() > 3) input_words.insert(word);

    if (input_words.empty()) return 1.0;

    // Check how many input words appear in output
    int matches = 0;
    for (const auto& iw : input_words) {
        std::string lower_out = output;
        std::transform(lower_out.begin(), lower_out.end(), lower_out.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_out.find(iw) != std::string::npos) {
            matches++;
        }
    }

    return static_cast<double>(matches) / input_words.size();
}

HallucinationResult HallucinationDetector::analyze(
    const std::string& output, const std::string& input) const {
    HallucinationResult result;

    double claim_density = countSpecificClaims(output);
    double relevance = measureInputRelevance(output, input);

    // High claim density + low relevance = potential hallucination
    // Score: 1.0 = fully confident (no hallucination), 0.0 = likely hallucination
    result.confidence_score = std::max(0.0, std::min(1.0,
        1.0 - (claim_density * 0.3) + (relevance * 0.3)));

    if (result.confidence_score < threshold_) {
        result.flagged = true;
        if (claim_density > 0.5) {
            result.suspicious_claims.push_back("high_claim_density");
        }
        if (relevance < 0.3) {
            result.suspicious_claims.push_back("low_input_relevance");
        }
        result.reason = "Low confidence score: " +
                         std::to_string(result.confidence_score);
    }

    return result;
}

StageResult HallucinationDetector::process(RequestContext& ctx) {
    if (ctx.accumulated_response.empty()) return StageResult::Continue;
    if (ctx.has_tools && ctx.accumulated_response.empty()) return StageResult::Continue;

    std::string input;
    for (const auto& msg : ctx.chat_request.messages) {
        if (msg.role != "system" && !isToolMessage(msg)) input += msg.content + " ";
    }

    auto result = analyze(ctx.accumulated_response, input);
    ctx.hallucination_score = result.confidence_score;
    ctx.hallucination_flagged = result.flagged;

    if (result.flagged) {
        spdlog::warn("Hallucination flagged: req={} score={:.3f}",
                     ctx.request_id, result.confidence_score);
    }

    if (ground_truth_config_.enabled && !ctx.retrieval_sources.empty()) {
        std::vector<std::string> ref_texts;
        ref_texts.reserve(ctx.retrieval_sources.size());
        for (const auto& src : ctx.retrieval_sources) {
            ref_texts.push_back(src.content);
        }
        double groundedness = measureGroundedness(ctx.accumulated_response, ref_texts);
        ctx.groundedness_score = static_cast<float>(groundedness);
        // P1-B: record into the groundedness_score histogram (previously a
        // registered-but-never-observed metric → empty admin distribution).
        MetricsRegistry::instance().groundednessScore().observe(groundedness);
        spdlog::debug("Groundedness score: req={} score={:.3f}",
                      ctx.request_id, groundedness);
    }

    return StageResult::Continue;
}

} // namespace aegisgate
