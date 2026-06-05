#pragma once

#include "beacon/ILogger.hpp"
#include "beacon/internal/Types.hpp"

#include <memory>
#include <string>

typedef void CURL;

namespace beacon {
namespace internal {

// Default per-request curl connect + transfer timeout (seconds) used when a
// caller does not pass an explicit override to post_json. Matches the
// historical hardcoded value so existing call paths are unchanged.
constexpr long kDefaultHttpTimeoutSeconds = 10L;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Process-wide libcurl global init/cleanup ownership. libcurl's
    // curl_global_init is NOT thread-safe and must be called exactly once
    // before any curl_easy_* use, and curl_global_cleanup exactly once after
    // the last use. The SDK previously relied on curl_easy_init's implicit
    // lazy global init — fine for steady-state, but a destruct-time send may
    // be the last libcurl call in the process, so global lifecycle must be
    // owned explicitly. These are reference-counted and thread-safe: each
    // Tracker calls global_init() in its constructor and global_cleanup() in
    // its destructor; the underlying curl_global_init/cleanup fire only on the
    // 0->1 and 1->0 transitions.
    static void global_init();
    static void global_cleanup();

    // Initialize libcurl handle. Returns false on failure.
    // The optional logger is retained and used to surface transport-level
    // errors (TLS handshake failures, connection timeouts, DNS errors,
    // unsupported protocol, etc.). Without a logger, errors are silent —
    // which makes CA-bundle / no-TLS-backend / firewall failures invisible
    // to the integrator.
    bool init(std::shared_ptr<ILogger> logger = nullptr);

    // POST JSON to the given URL with the given API key.
    // Optionally attaches X-Environment-Data header.
    // timeout_seconds bounds BOTH CURLOPT_CONNECTTIMEOUT and CURLOPT_TIMEOUT
    // for this request; it lets the destructor's bounded session-end send
    // honor Options.shutdown_flush_timeout_seconds instead of the historical
    // hardcoded 10s. Defaults to kDefaultHttpTimeoutSeconds for all existing
    // callers (event/identify/exception delivery), preserving prior behavior.
    HttpResult post_json(const std::string& url, const std::string& api_key,
                         const std::string& json_body,
                         const std::string& env_data_header = "",
                         long timeout_seconds = kDefaultHttpTimeoutSeconds);

    // Returns true if the curl handle was successfully initialized.
    bool is_initialized() const;

private:
    CURL* curl_ = nullptr;
    std::shared_ptr<ILogger> logger_;
};

} // namespace internal
} // namespace beacon
