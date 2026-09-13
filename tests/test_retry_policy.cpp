// Covers: FR-605 (exponential backoff retry), AC-927 (Retry-After), AC-928 (3 retries),
//         AC-929 (401 halts), AC-930 (402 not deleted), AC-931 (400 permanent failure)
#include <gtest/gtest.h>
#include "RetryPolicy.hpp"
#include <chrono>

// FR-605: HTTP 429 is retryable
TEST(RetryPolicyTest, Http429IsRetryable) {
    beacon::internal::HttpResult result;
    result.status_code = 429;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_retryable(result));
}

// FR-605: HTTP 500 is retryable
TEST(RetryPolicyTest, Http500IsRetryable) {
    beacon::internal::HttpResult result;
    result.status_code = 500;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_retryable(result));
}

// FR-605: HTTP 502 is retryable
TEST(RetryPolicyTest, Http502IsRetryable) {
    beacon::internal::HttpResult result;
    result.status_code = 502;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_retryable(result));
}

// FR-605: HTTP 503 is retryable
TEST(RetryPolicyTest, Http503IsRetryable) {
    beacon::internal::HttpResult result;
    result.status_code = 503;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_retryable(result));
}

// FR-605: Network error is retryable
TEST(RetryPolicyTest, NetworkErrorIsRetryable) {
    beacon::internal::HttpResult result;
    result.status_code = 0;
    result.is_network_error = true;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_retryable(result));
}

// FR-605: HTTP 200 is not retryable
TEST(RetryPolicyTest, Http200NotRetryable) {
    beacon::internal::HttpResult result;
    result.status_code = 200;
    result.is_network_error = false;

    EXPECT_FALSE(beacon::internal::RetryPolicy::is_retryable(result));
}

// AC-931: HTTP 400 is a permanent failure (not retried, events deleted)
TEST(RetryPolicyTest, Http400IsPermanentFailure) {
    beacon::internal::HttpResult result;
    result.status_code = 400;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_permanent_failure(result));
    EXPECT_FALSE(beacon::internal::RetryPolicy::is_retryable(result));
}

// AC-929: HTTP 401 is a permanent failure
TEST(RetryPolicyTest, Http401IsPermanentFailure) {
    beacon::internal::HttpResult result;
    result.status_code = 401;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_permanent_failure(result));
    EXPECT_FALSE(beacon::internal::RetryPolicy::is_retryable(result));
}

// AC-930: HTTP 402 is a permanent failure (not retried, events stay in queue)
TEST(RetryPolicyTest, Http402IsPermanentFailure) {
    beacon::internal::HttpResult result;
    result.status_code = 402;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_permanent_failure(result));
    EXPECT_FALSE(beacon::internal::RetryPolicy::is_retryable(result));
}

// FR-605: HTTP 403 is a permanent failure
TEST(RetryPolicyTest, Http403IsPermanentFailure) {
    beacon::internal::HttpResult result;
    result.status_code = 403;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_permanent_failure(result));
}

// FR-605: HTTP 404 is a permanent failure
TEST(RetryPolicyTest, Http404IsPermanentFailure) {
    beacon::internal::HttpResult result;
    result.status_code = 404;
    result.is_network_error = false;

    EXPECT_TRUE(beacon::internal::RetryPolicy::is_permanent_failure(result));
}

// FR-605: HTTP 429 is NOT a permanent failure (it is retryable)
TEST(RetryPolicyTest, Http429NotPermanentFailure) {
    beacon::internal::HttpResult result;
    result.status_code = 429;
    result.is_network_error = false;

    EXPECT_FALSE(beacon::internal::RetryPolicy::is_permanent_failure(result));
}

// FR-605: Max retry attempts is 3
TEST(RetryPolicyTest, MaxRetriesIsThree) {
    EXPECT_EQ(beacon::internal::RetryPolicy::max_retries, 3);
}

// FR-605: Exponential backoff - attempt 0 base is 1000ms (+/- 20%)
TEST(RetryPolicyTest, FirstRetryDelayAroundOneSecond) {
    auto delay = beacon::internal::RetryPolicy::compute_delay(0);
    // 1000ms +/- 20% = [800ms, 1200ms]
    EXPECT_GE(delay.count(), 800);
    EXPECT_LE(delay.count(), 1200);
}

// FR-605: Exponential backoff - attempt 1 base is 2000ms (+/- 20%)
TEST(RetryPolicyTest, SecondRetryDelayAroundTwoSeconds) {
    auto delay = beacon::internal::RetryPolicy::compute_delay(1);
    // 2000ms +/- 20% = [1600ms, 2400ms]
    EXPECT_GE(delay.count(), 1600);
    EXPECT_LE(delay.count(), 2400);
}

// FR-605: Exponential backoff - attempt 2 base is 4000ms (+/- 20%)
TEST(RetryPolicyTest, ThirdRetryDelayAroundFourSeconds) {
    auto delay = beacon::internal::RetryPolicy::compute_delay(2);
    // 4000ms +/- 20% = [3200ms, 4800ms]
    EXPECT_GE(delay.count(), 3200);
    EXPECT_LE(delay.count(), 4800);
}

// FR-605: Maximum delay capped at 60000ms
TEST(RetryPolicyTest, DelayNeverExceedsSixtySeconds) {
    // After many retries, the delay should be capped at 60000ms +/- 20%
    auto delay = beacon::internal::RetryPolicy::compute_delay(20);
    // 60000ms +/- 20% = [48000ms, 72000ms]
    EXPECT_LE(delay.count(), 72000);
}

// AC-927: Retry-After header takes precedence
TEST(RetryPolicyTest, RetryAfterHeaderUsedAsDelay) {
    auto delay = beacon::internal::RetryPolicy::compute_delay(0, 60);
    EXPECT_EQ(delay.count(), 60000); // 60 seconds in ms
}

// FR-605: Retry-After of 0 seconds
TEST(RetryPolicyTest, RetryAfterZeroSeconds) {
    auto delay = beacon::internal::RetryPolicy::compute_delay(0, 0);
    EXPECT_EQ(delay.count(), 0);
}

// FR-605: Retry-After large value
TEST(RetryPolicyTest, RetryAfterLargeValue) {
    auto delay = beacon::internal::RetryPolicy::compute_delay(0, 300);
    EXPECT_EQ(delay.count(), 300000); // 300 seconds in ms
}

// --- FR-2366: rate-limit cooldown ------------------------------------------------------
//
// compute_cooldown_seconds answers a different question from compute_delay. compute_delay
// asks "how long before retrying THIS batch"; after a 429 that is the wrong question, because
// every other queued batch is charged against the same exhausted per-minute budget, so
// retrying one batch and moving on to the next both fail for the same reason.

// AC-3451: a missing Retry-After falls back to the server's own one-minute bucket.
TEST(RetryPolicyTest, CooldownWithoutRetryAfterUsesTheServerBucket) {
    EXPECT_EQ(beacon::internal::RetryPolicy::compute_cooldown_seconds(-1),
              beacon::internal::RetryPolicy::default_cooldown_seconds);
}

// AC-3451: a normal Retry-After is honoured as sent.
TEST(RetryPolicyTest, CooldownHonoursAServerSuppliedRetryAfter) {
    EXPECT_EQ(beacon::internal::RetryPolicy::compute_cooldown_seconds(45), 45);
}

// AC-3452: Retry-After: 0 must still produce a real pause. Treating it literally would set a
// deadline of "now" — no cooldown at all — and the next tick walks back into the same limit.
TEST(RetryPolicyTest, CooldownFloorsAZeroRetryAfter) {
    EXPECT_GE(beacon::internal::RetryPolicy::compute_cooldown_seconds(0), 1);
}

// AC-3452: same for a negative value that is not the -1 sentinel.
TEST(RetryPolicyTest, CooldownFloorsANonsensicalNegativeRetryAfter) {
    EXPECT_GE(beacon::internal::RetryPolicy::compute_cooldown_seconds(-999), 1);
}

// AC-3453: a day-long Retry-After is not a real instruction. Obeying it verbatim would let a
// single bad response silence the SDK for the rest of the process lifetime.
TEST(RetryPolicyTest, CooldownClampsAnAbsurdRetryAfter) {
    EXPECT_EQ(beacon::internal::RetryPolicy::compute_cooldown_seconds(86400),
              beacon::internal::RetryPolicy::max_cooldown_seconds);
}

// The ceiling must stay above the server's own bucket, or a legitimate Retry-After would be
// truncated and the client would resume while still over the limit.
TEST(RetryPolicyTest, CooldownCeilingExceedsTheServerBucket) {
    EXPECT_GT(beacon::internal::RetryPolicy::max_cooldown_seconds,
              beacon::internal::RetryPolicy::default_cooldown_seconds);
}
