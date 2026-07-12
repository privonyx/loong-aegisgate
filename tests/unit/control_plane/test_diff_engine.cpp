// Phase 9.3 Epic 3 Task 3.4 — DiffEngine unified-diff MVP.
//
// MVP delegates to system `diff -u` for correctness, falling back to a naive
// line-compare when `diff` is unavailable (see engine docs). Tests pin the
// externally-observable contract rather than the exact whitespace of diff's
// output, so future swaps (myers implementation, yaml tree diff) stay green.

#include "control_plane/diff_engine.h"

#include <gtest/gtest.h>

#include <string>

namespace aegisgate {
namespace {

TEST(DiffEngine, IdenticalYieldsEmptyDiff) {
    DiffEngine d;
    auto out = d.unifiedDiff("foo: 1\nbar: 2\n", "foo: 1\nbar: 2\n");
    EXPECT_TRUE(out.empty()) << "got: " << out;
}

TEST(DiffEngine, AddedLineSurfacesInDiff) {
    DiffEngine d;
    auto out = d.unifiedDiff("a\nb\n", "a\nb\nc\n");
    EXPECT_NE(out.find("+c"), std::string::npos)
        << "expected `+c` hunk, got: " << out;
}

TEST(DiffEngine, RemovedLineSurfacesInDiff) {
    DiffEngine d;
    auto out = d.unifiedDiff("a\nb\nc\n", "a\nc\n");
    EXPECT_NE(out.find("-b"), std::string::npos) << out;
}

TEST(DiffEngine, ChangedLineShowsBothSides) {
    DiffEngine d;
    auto out = d.unifiedDiff("foo: 1\n", "foo: 2\n");
    EXPECT_NE(out.find("-foo: 1"), std::string::npos) << out;
    EXPECT_NE(out.find("+foo: 2"), std::string::npos) << out;
}

TEST(DiffEngine, EmptyVersusEmptyIsEmpty) {
    DiffEngine d;
    EXPECT_TRUE(d.unifiedDiff("", "").empty());
}

TEST(DiffEngine, HandlesLargeInputWithoutDeadlock) {
    // Sanity regression: we feed both sides via temp files so deadlocking on
    // pipe buffers is impossible, but exercise the size anyway.
    std::string left, right;
    left.reserve(4096 * 10);
    right.reserve(4096 * 10);
    for (int i = 0; i < 500; ++i) {
        left  += "line_" + std::to_string(i) + "\n";
        right += "line_" + std::to_string(i) + "\n";
    }
    right += "extra\n";
    DiffEngine d;
    auto out = d.unifiedDiff(left, right);
    EXPECT_NE(out.find("+extra"), std::string::npos);
}

TEST(DiffEngine, TempFileContentsDoNotLeakPaths) {
    // The output must not contain the workspace-specific temp path so that a
    // future CLI user copy-pasting the diff doesn't expose absolute host paths
    // of the server process.
    DiffEngine d;
    auto out = d.unifiedDiff("foo: 1\n", "foo: 2\n");
    // `diff -u` prints headers like `--- /tmp/XXXX.yaml`. We strip them so the
    // output starts directly with the hunk marker or is hunk-only.
    EXPECT_EQ(out.find("/tmp/"), std::string::npos)
        << "output still contains /tmp path: " << out;
}

} // namespace
} // namespace aegisgate
