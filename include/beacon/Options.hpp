#pragma once

#include <memory>
#include <string>

#include "beacon/EventDefinitionBuilder.hpp"
#include "beacon/ILogger.hpp"

namespace beacon {

struct Options {
    std::string api_key;
    std::string api_base_url;
    std::string product;
    std::string product_version;
    bool enabled = true;
    int flush_interval_seconds = 60;
    int max_batch_size = 25;
    int max_queue_size_mb = 10;
    int max_breadcrumbs = 25;

    // Maximum number of seconds the destructor will block on a best-effort,
    // synchronous session-end POST before giving up and relying on the
    // next-launch recovery delivery. Applied as the per-request libcurl
    // connect + transfer timeout. Default 2. A value of 0 SKIPS the blocking
    // send entirely (disk-only: the durable record is persisted and delivered
    // on the next launch as end_reason="sdk_recovery"). Clamped to [0, 30].
    int shutdown_flush_timeout_seconds = 2;

    std::shared_ptr<ILogger> logger;
    EventDefinitionBuilder events;
};

} // namespace beacon
