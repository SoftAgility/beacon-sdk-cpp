#include "HttpClient.hpp"
#include "beacon/Version.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#include <curl/curl.h>

namespace beacon {
namespace internal {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* response = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    response->append(ptr, total);
    return total;
}

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* result = static_cast<HttpResult*>(userdata);
    size_t total = size * nitems;
    std::string header(buffer, total);

    // Parse Retry-After header (case-insensitive)
    if (header.size() > 13) {
        std::string lower = header.substr(0, 12);
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "retry-after:") {
            std::string value = header.substr(12);
            // Trim whitespace
            size_t start = value.find_first_not_of(" \t\r\n");
            size_t end = value.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos) {
                value = value.substr(start, end - start + 1);
            }
            try {
                int seconds = std::stoi(value);
                if (seconds > 300) seconds = 300; // Cap at 300s per PRD
                result->retry_after_seconds = seconds;
            } catch (...) {
                // Not a valid integer, ignore
            }
        }
    }

    return total;
}

} // anonymous namespace

HttpClient::HttpClient() = default;

HttpClient::~HttpClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
}

bool HttpClient::init(std::shared_ptr<ILogger> logger) {
    logger_ = std::move(logger);
    curl_ = curl_easy_init();
    if (curl_ == nullptr && logger_) {
        logger_->log(LogLevel::Error,
            "beacon: curl_easy_init() failed — HTTP transport unavailable. "
            "All event delivery will be queued and retried.");
    }
    return curl_ != nullptr;
}

bool HttpClient::is_initialized() const {
    return curl_ != nullptr;
}

HttpResult HttpClient::post_json(const std::string& url, const std::string& api_key,
                                  const std::string& json_body,
                                  const std::string& env_data_header) {
    HttpResult result;

    if (!curl_) {
        result.is_network_error = true;
        return result;
    }

    curl_easy_reset(curl_);

    std::string response_body;

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &result);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

    // Identify the SDK in HTTP request logs so backend operators can
    // distinguish official-SDK traffic from raw-curl/custom integrations.
    // Derived from BEACON_VERSION_STRING so it auto-tracks the version bump.
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, "beacon-sdk-cpp/" BEACON_VERSION_STRING);

    // Strict TLS verification (peer cert chain + hostname). Defaults to ON
    // in libcurl 7.x+ but a future contributor toggling these silently
    // would compromise transport security with no visible signal — set
    // explicitly so the intent is documented.
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

    // Disable redirects. Analytics ingestion endpoints never redirect; a
    // 301/302 response would be a sign of misconfiguration or DNS hijack
    // and should fail loud, not follow silently.
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 0L);

    // Headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string auth_header = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth_header.c_str());

    if (!env_data_header.empty()) {
        std::string env_header = "X-Environment-Data: " + env_data_header;
        headers = curl_slist_append(headers, env_header.c_str());
    }

    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl_);

    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        result.is_network_error = true;
        result.body = curl_easy_strerror(res);
        if (logger_) {
            logger_->log(LogLevel::Warning,
                std::string("beacon: HTTP POST ") + url + " failed: " +
                curl_easy_strerror(res) +
                " (event queued for retry). Common causes: missing CA bundle, "
                "no TLS backend compiled into libcurl, firewall blocking "
                "outbound HTTPS, or DNS failure.");
        }
        return result;
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    result.status_code = static_cast<int>(http_code);
    result.body = std::move(response_body);
    result.success = (http_code >= 200 && http_code < 300);

    if (!result.success && logger_) {
        // 401/402/422/etc — surface to the integrator. Body often contains
        // the structured error from the backend, e.g. "Product 'foo' is
        // not registered" or "API key rejected".
        const auto level = (http_code == 401 || http_code == 402)
            ? LogLevel::Error
            : LogLevel::Warning;
        std::string msg = "beacon: HTTP POST " + url + " returned " +
            std::to_string(http_code);
        if (!result.body.empty()) {
            // Truncate body to avoid log spam from large 500-page error responses.
            const std::string body_preview =
                result.body.size() > 500
                    ? result.body.substr(0, 500) + "...(truncated)"
                    : result.body;
            msg += " — " + body_preview;
        }
        logger_->log(level, msg);
    }

    return result;
}

} // namespace internal
} // namespace beacon
