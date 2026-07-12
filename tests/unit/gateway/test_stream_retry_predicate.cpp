// REV20260707-I14 (D2 Option A) Layer 1 predicate unit tests.
// Family N=4: canRetryStream joins shouldWireRedisRateLimiter (TASK-20260708-02),
// shouldWireGuardExplanation (TASK-20260708-03), isAdvancedRoutingEnabled
// (TASK-20260711-01). Six truth-table cases target Mutation M1 (short-circuit
// `return false;`) which is expected to kill T1 + T5 + T6 while T2/T3/T4
// survive (expected-false semantics aligned with mutation).

#include <gtest/gtest.h>
#include "gateway/connector/base.h"

using aegisgate::canRetryStream;

TEST(StreamRetryPredicateTest, NoChunkYetRetryableStatusUnderMax_Retries_T1) {
    // Retryable 429 status, still under attempt budget, no chunks emitted.
    EXPECT_TRUE(canRetryStream(/*first_chunk_seen=*/false, /*last_status=*/429,
                                /*tried_size=*/1, /*max_attempts=*/3));
}

TEST(StreamRetryPredicateTest, ChunkSeenRetryableStatus_DoesNotRetry_T2) {
    // Client already received bytes; even a retryable status cannot retry.
    EXPECT_FALSE(canRetryStream(/*first_chunk_seen=*/true, /*last_status=*/429,
                                 /*tried_size=*/1, /*max_attempts=*/3));
}

TEST(StreamRetryPredicateTest, NoChunkYetNonRetryableStatus_DoesNotRetry_T3) {
    // 400 is a caller-fault (request payload) — another key won't help.
    EXPECT_FALSE(canRetryStream(/*first_chunk_seen=*/false, /*last_status=*/400,
                                 /*tried_size=*/1, /*max_attempts=*/3));
}

TEST(StreamRetryPredicateTest, NoChunkYetRetryableStatusAtMax_DoesNotRetry_T4) {
    // Budget exhausted: tried_size == max_attempts.
    EXPECT_FALSE(canRetryStream(/*first_chunk_seen=*/false, /*last_status=*/429,
                                 /*tried_size=*/3, /*max_attempts=*/3));
}

TEST(StreamRetryPredicateTest, NoChunkYetTransportError_Retries_T5) {
    // last_status == -1 sentinel encodes transport-level failure (curl error
    // etc.). Always retryable pre-stream.
    EXPECT_TRUE(canRetryStream(/*first_chunk_seen=*/false, /*last_status=*/-1,
                                /*tried_size=*/1, /*max_attempts=*/3));
}

TEST(StreamRetryPredicateTest, EmptyTriedSetFirstAttempt_Retries_T6) {
    // First attempt: no prior status (0), no prior tries.
    EXPECT_TRUE(canRetryStream(/*first_chunk_seen=*/false, /*last_status=*/0,
                                /*tried_size=*/0, /*max_attempts=*/3));
}
