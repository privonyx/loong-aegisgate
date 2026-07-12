#include "observe/quality_scorer.h"
#include "observe/metrics.h"
#include "observe/quality_monitor.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace aegisgate {

namespace {

std::string_view trimTrailingWhitespace(std::string_view s) {
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool endsWithSentenceLikePunctuation(std::string_view s) {
    static const std::vector<std::string> kSuffixes = {
        ".",  "?",  "!",  "。", "？", "！", "\"", "'", ")", "`", "}", "]"};
    for (const auto& suf : kSuffixes) {
        if (s.size() >= suf.size() &&
            s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
            return true;
        }
    }
    return false;
}

bool expectsJsonObject(const RequestContext& ctx) {
    const auto& extra = ctx.chat_request.extra;
    if (!extra.is_object() || !extra.contains("response_format")) {
        return false;
    }
    const auto& rf = extra["response_format"];
    return rf.is_string() && rf.get<std::string>() == "json_object";
}

}  // namespace

QualityScorer::QualityScorer() = default;

QualityScorer::QualityScorer(Config cfg) : cfg_(std::move(cfg)) {}

double QualityScorer::scoreLengthAdequacy(const std::string& response) const {
    if (response.empty()) {
        return 0.0;
    }
    const double len = static_cast<double>(response.size());
    if (cfg_.min_response_length > 0 &&
        len < static_cast<double>(cfg_.min_response_length)) {
        return (len / static_cast<double>(cfg_.min_response_length)) * 0.5;
    }
    if (cfg_.max_response_length > 0 &&
        len > static_cast<double>(cfg_.max_response_length)) {
        const double max_l = static_cast<double>(cfg_.max_response_length);
        return std::max(0.5, 1.0 - (len - max_l) / max_l);
    }
    return 1.0;
}

double QualityScorer::scoreCompleteness(const std::string& response) const {
    if (response.empty()) {
        return 0.0;
    }
    const std::string_view trimmed = trimTrailingWhitespace(response);
    if (trimmed.empty()) {
        return 0.0;
    }
    if (endsWithSentenceLikePunctuation(trimmed)) {
        return 1.0;
    }
    return 0.3;
}

double QualityScorer::scoreRepetition(const std::string& response) const {
    const int len = static_cast<int>(response.size());
    if (len < 20) {
        return 1.0;
    }
    if (len < 3) {
        return 1.0;
    }
    std::unordered_set<std::string> grams;
    grams.reserve(static_cast<size_t>(len));
    const int total = len - 2;
    for (int i = 0; i <= len - 3; ++i) {
        grams.insert(response.substr(static_cast<size_t>(i), 3));
    }
    if (total <= 0) {
        return 1.0;
    }
    const double unique_ratio =
        static_cast<double>(grams.size()) / static_cast<double>(total);

    const double half_max_rep = cfg_.max_repetition_ratio * 0.5;
    if (half_max_rep > 0.0 && unique_ratio < half_max_rep) {
        return (unique_ratio / half_max_rep) * 0.3;
    }
    if (unique_ratio >= 0.5) {
        return 1.0;
    }
    return unique_ratio / 0.5;
}

double QualityScorer::scoreFormatCompliance(const RequestContext& ctx) const {
    if (!expectsJsonObject(ctx)) {
        return 1.0;
    }
    try {
        [[maybe_unused]] const auto parsed =
            nlohmann::json::parse(ctx.accumulated_response);
        return 1.0;
    } catch (...) {
        return 0.0;
    }
}

StageResult QualityScorer::process(RequestContext& ctx) {
    // P0-5: attribute the sample to the model actually invoked (target_model),
    // falling back to the requested model when routing left it unset.
    const std::string& model =
        ctx.target_model.empty() ? ctx.chat_request.model : ctx.target_model;

    if (ctx.accumulated_response.empty()) {
        ctx.quality_score = 0.0;
        spdlog::debug("QualityScorer: empty response, quality_score=0.0");
        if (quality_monitor_) quality_monitor_->recordQuality(model, 0.0);
        return StageResult::Continue;
    }

    const double len_s = scoreLengthAdequacy(ctx.accumulated_response);
    const double comp_s = scoreCompleteness(ctx.accumulated_response);
    const double rep_s = scoreRepetition(ctx.accumulated_response);
    const double fmt_s = scoreFormatCompliance(ctx);

    ctx.quality_score = (len_s + comp_s + rep_s + fmt_s) / 4.0;
    MetricsRegistry::instance().qualityScore().observe(ctx.quality_score, {});
    spdlog::debug(
        "QualityScorer: quality_score={} (length={} completeness={} "
        "repetition={} format={})",
        ctx.quality_score, len_s, comp_s, rep_s, fmt_s);
    if (quality_monitor_) quality_monitor_->recordQuality(model, ctx.quality_score);
    return StageResult::Continue;
}

}  // namespace aegisgate
