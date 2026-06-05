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

// A durable, self-contained pending session-end record persisted to the
// `pending_session_ends` sibling table in beacon_queue.db. It carries the
// full session-start context (snapshotted at session start, NOT read from the
// live tracker members at end time) so a next-launch recovery delivery can
// drive the server's create-on-recovery path even if the original start POST
// never landed. Empty account_id/license_id mean "absent" and are omitted from
// the wire payload. id is the SQLite rowid (0 = not yet persisted).
struct PendingSessionEnd {
    int64_t id = 0;
    std::string session_id;
    std::string actor_id;
    std::string source_app;       // == Options.product
    std::string product_version;
    std::string started_at;       // ISO-8601 UTC, snapshotted at session start
    std::string account_id;       // optional ("" => omit)
    std::string license_id;       // optional ("" => omit)
    std::string ended_at;         // ISO-8601 UTC, stamped at end/destruct time
    std::string end_reason;       // "normal" (live) or "sdk_recovery" (recovery)
};

} // namespace internal
} // namespace beacon
