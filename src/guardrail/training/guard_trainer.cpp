#include "guardrail/training/guard_trainer.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>
#include <utility>

namespace aegisgate::guard {

namespace {

int labelToY(GuardFeedbackLabel l) {
    switch (l) {
        case GuardFeedbackLabel::FalsePositive:  return 0;
        case GuardFeedbackLabel::ConfirmedAllow: return 0;
        case GuardFeedbackLabel::FalseNegative:  return 1;
        case GuardFeedbackLabel::ConfirmedBlock: return 1;
    }
    return 0;
}

}  // namespace

std::string sanitizeFreeText(std::string in) {
    // Defense-in-depth: drop bare email patterns even if upstream forgot.
    static const std::regex kEmail(
        R"([a-zA-Z0-9._%+\-]+@[a-zA-Z0-9.\-]+\.[a-zA-Z]{2,})");
    return std::regex_replace(in, kEmail, "[EMAIL]");
}

TrainingFeatures extractTrainingFeatures(const GuardFeedbackPayload& payload) {
    TrainingFeatures out;
    out["request_id"] = payload.request_id;
    out["trace_id"] = payload.trace_id;
    out["label"] = std::string(labelToString(payload.label));
    out["label_y"] = std::to_string(labelToY(payload.label));
    out["reviewer_role"] = payload.reviewer_role;
    out["text"] = sanitizeFreeText(payload.original_text_redacted);
    out["comment"] = sanitizeFreeText(payload.comment);
    return out;
}

GuardTrainer::GuardTrainer(std::size_t max_buffer_rows)
    : max_buffer_rows_(max_buffer_rows) {}

void GuardTrainer::captureFromPayload(const GuardFeedbackPayload& payload,
                                       const std::string& tenant_id) {
    std::lock_guard lock(mu_);
    auto row = extractTrainingFeatures(payload);
    row["tenant_id"] = tenant_id;
    if (rows_.size() < max_buffer_rows_) {
        rows_.push_back(std::move(row));
    }
}

std::size_t GuardTrainer::bufferedRows() const {
    std::lock_guard lock(mu_);
    return rows_.size();
}

bool GuardTrainer::snapshotJsonl(const std::string& out_path) const {
    std::ofstream out(out_path, std::ios::app);
    if (!out.is_open()) return false;
    std::lock_guard lock(mu_);
    for (const auto& row : rows_) {
        nlohmann::json j(row);
        out << j.dump() << '\n';
    }
    return static_cast<bool>(out);
}

void GuardTrainer::clear() {
    std::lock_guard lock(mu_);
    rows_.clear();
}

}  // namespace aegisgate::guard
