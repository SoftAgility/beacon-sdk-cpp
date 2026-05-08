#pragma once

#include "beacon/internal/Types.hpp"

#include <chrono>

namespace beacon {
namespace internal {

class RetryPolicy {
public:
    // Determines if a response is retryable.
    static bool is_retryable(const HttpResult& result);

    // Determines if a response is a permanent failure (should delete, not retry).
    static bool is_permanent_failure(const HttpResult& result);

    // Computes the delay before the next retry attempt.
    // attempt is 0-based (0 = first retry).
    // If retry_after_seconds >= 0, uses that value instead.
    static std::chrono::milliseconds compute_delay(int attempt, int retry_after_seconds = -1);

    // Maximum number of retry attempts.
    static constexpr int max_retries = 3;
};

} // namespace internal
} // namespace beacon
