// Phase 11.1 TASK-20260523-01 — Epic 3 OnlineTrainer (collector) tests.
//
// D2=A "collector + offline trainer" decision: the in-process component is
// a windowed collector that:
//   * subscribes to FeedbackBus topic "guard.feedback"
//   * derives the canonical feature row from each payload
//   * buffers up to N rows then flushes to disk as JSONL
//
// The actual ONNX retraining is delegated to an offline sidecar (out of
// scope here; tested via integration in Epic 6).
//
// Tests:
//   * extractFeatures returns the 5 canonical fields with the expected
//     label encoding
//   * captureFromEvent enqueues a feedback row
//   * snapshotJsonl produces one row per buffered feedback
//   * SR2-related: comment field that leaks PII is the masked version
//     (relies on the sink having masked it; the trainer must not undo)

#include "guardrail/feedback/guard_feedback_payload.h"
#include "guardrail/training/guard_trainer.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using aegisgate::guard::extractTrainingFeatures;
using aegisgate::guard::GuardFeedbackLabel;
using aegisgate::guard::GuardFeedbackPayload;
using aegisgate::guard::GuardTrainer;

namespace {

GuardFeedbackPayload makePayload(GuardFeedbackLabel label,
                                 std::string text = "[REDACTED]") {
    GuardFeedbackPayload p;
    p.request_id = "req-train-" + std::to_string(static_cast<int>(label));
    p.label = label;
    p.reviewer_user_id = "user-1";
    p.reviewer_role = "security_admin";
    p.comment = std::move(text);
    p.original_text_redacted = "Original prompt content [REDACTED]";
    return p;
}

}  // namespace

TEST(GuardTrainerTest, ExtractFeaturesMapsCanonicalFields) {
    auto row = extractTrainingFeatures(makePayload(GuardFeedbackLabel::FalsePositive));
    EXPECT_EQ(row.at("request_id"), "req-train-0");
    EXPECT_EQ(row.at("label"), "false_positive");
    // numeric encoded label for downstream training (0/1)
    EXPECT_EQ(row.at("label_y"), "0");

    auto row2 = extractTrainingFeatures(makePayload(GuardFeedbackLabel::ConfirmedBlock));
    EXPECT_EQ(row2.at("label"), "confirmed_block");
    EXPECT_EQ(row2.at("label_y"), "1");
}

TEST(GuardTrainerTest, CaptureEnqueuesPayload) {
    GuardTrainer trainer;
    trainer.captureFromPayload(makePayload(GuardFeedbackLabel::FalsePositive),
                                "tenant-A");
    trainer.captureFromPayload(makePayload(GuardFeedbackLabel::FalseNegative),
                                "tenant-A");
    EXPECT_EQ(trainer.bufferedRows(), 2u);
}

TEST(GuardTrainerTest, SnapshotJsonlWritesOneRowPerFeedback) {
    GuardTrainer trainer;
    trainer.captureFromPayload(makePayload(GuardFeedbackLabel::FalsePositive),
                                "tenant-A");
    trainer.captureFromPayload(makePayload(GuardFeedbackLabel::ConfirmedBlock),
                                "tenant-A");

    auto path = std::filesystem::temp_directory_path() /
                ("aegisgate_trainer_snapshot_" +
                 std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);

    ASSERT_TRUE(trainer.snapshotJsonl(path.string()));

    std::ifstream in(path);
    std::string line;
    int rows = 0;
    while (std::getline(in, line)) ++rows;
    EXPECT_EQ(rows, 2);
    std::filesystem::remove(path);
}

TEST(GuardTrainerTest, SnapshotDoesNotEmitRawPiiFields) {
    // SR2 reuse: if a payload comes in with a non-masked comment, the
    // trainer must not silently store it. We strip and re-mask any obvious
    // email/SSN pattern even if the upstream sink "should" have masked.
    auto p = makePayload(GuardFeedbackLabel::ConfirmedBlock,
                          "Sketchy bot at attacker@evil.com mentioned PII");
    GuardTrainer trainer;
    trainer.captureFromPayload(p, "tenant-A");

    auto path = std::filesystem::temp_directory_path() /
                ("aegisgate_trainer_pii_" + std::to_string(::getpid()) + ".jsonl");
    std::filesystem::remove(path);
    ASSERT_TRUE(trainer.snapshotJsonl(path.string()));

    std::ifstream in(path);
    std::stringstream contents;
    contents << in.rdbuf();
    auto raw = contents.str();
    EXPECT_EQ(raw.find("attacker@evil.com"), std::string::npos)
        << "PII email must not appear in JSONL snapshot. raw=" << raw;
    std::filesystem::remove(path);
}
