#include "RetryPolicy.hpp"

#include <algorithm>
#include <random>

namespace beacon {
namespace internal {

bool RetryPolicy::is_retryable(const HttpResult& result) {
    if (result.is_network_error) return true;
    if (result.status_code == 429) return true;
    if (result.status_code >= 500) return true;
    return false;
}

bool RetryPolicy::is_permanent_failure(const HttpResult& result) {
    // 400, 401, 402, 403, 404 are non-retryable
    if (result.status_code >= 400 && result.status_code < 500 &&
        result.status_code != 429) {
        return true;
    }
    return false;
}

std::chrono::milliseconds RetryPolicy::compute_delay(int attempt, int retry_after_seconds) {
    if (retry_after_seconds >= 0) {
        return std::chrono::milliseconds(retry_after_seconds * 1000);
    }

    // Exponential backoff: 1000ms * 2^attempt, max 60000ms
    int base_ms = 1000;
    for (int i = 0; i < attempt; ++i) {
        base_ms *= 2;
    }
    base_ms = std::min(base_ms, 60000);

    // Jitter: +/- 20%
    double jitter_range = base_ms * 0.2;

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-jitter_range, jitter_range);
    double jitter = dist(rng);

    int delay_ms = static_cast<int>(base_ms + jitter);
    delay_ms = std::max(delay_ms, 1);

    return std::chrono::milliseconds(delay_ms);
}

} // namespace internal
} // namespace beacon
