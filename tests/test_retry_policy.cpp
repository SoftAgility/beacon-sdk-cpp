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
