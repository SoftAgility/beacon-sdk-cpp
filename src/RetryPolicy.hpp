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

    // FR-2366: how long to suppress ALL sends after a 429, in seconds. Distinct from
    // compute_delay: that answers "how long before retrying this batch", which is the wrong
    // question for a rate limit, because every other queued batch is charged against the same
    // exhausted budget. Lives here because this class already owns Retry-After interpretation.
    static int compute_cooldown_seconds(int retry_after_seconds);

    // Used when the server sends a 429 with no Retry-After header. Matches the server's own
    // one-minute bucket.
    static constexpr int default_cooldown_seconds = 60;

    // Ceiling on a server-supplied Retry-After. Beyond this it is a misconfigured proxy or a
    // hostile endpoint, and obeying it verbatim would turn one bad response into an hours-long
    // silent telemetry outage.
    static constexpr int max_cooldown_seconds = 300;
};

} // namespace internal
} // namespace beacon
