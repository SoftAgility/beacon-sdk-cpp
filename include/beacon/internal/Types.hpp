#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace beacon {
namespace internal {

struct BreadcrumbEntry {
    std::string timestamp;
    std::string category;
    std::string name;
    std::unordered_map<std::string, std::string> properties;
};

struct HttpResult {
    int status_code = 0;
    bool success = false;
    bool is_network_error = false;
    std::string body;
    int retry_after_seconds = -1; // -1 means no Retry-After header
};

struct QueuedEvent {
    int64_t id = 0;
    std::string event_id;
    std::string payload_json;
    std::string queued_at;
    int retry_count = 0;
};

} // namespace internal
} // namespace beacon
