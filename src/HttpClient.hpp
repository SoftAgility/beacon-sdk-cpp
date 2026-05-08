#pragma once

#include "beacon/ILogger.hpp"
#include "beacon/internal/Types.hpp"

#include <memory>
#include <string>

typedef void CURL;

namespace beacon {
namespace internal {

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Initialize libcurl handle. Returns false on failure.
    // The optional logger is retained and used to surface transport-level
    // errors (TLS handshake failures, connection timeouts, DNS errors,
    // unsupported protocol, etc.). Without a logger, errors are silent —
    // which makes CA-bundle / no-TLS-backend / firewall failures invisible
    // to the integrator.
    bool init(std::shared_ptr<ILogger> logger = nullptr);

    // POST JSON to the given URL with the given API key.
    // Optionally attaches X-Environment-Data header.
    HttpResult post_json(const std::string& url, const std::string& api_key,
                         const std::string& json_body,
                         const std::string& env_data_header = "");

    // Returns true if the curl handle was successfully initialized.
    bool is_initialized() const;

private:
    CURL* curl_ = nullptr;
    std::shared_ptr<ILogger> logger_;
};

} // namespace internal
} // namespace beacon
