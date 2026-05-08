#pragma once

#include <memory>
#include <string>

#include "beacon/EventDefinitionBuilder.hpp"
#include "beacon/ILogger.hpp"

namespace beacon {

struct Options {
    std::string api_key;
    std::string api_base_url;
    std::string app_name;
    std::string app_version;
    bool enabled = true;
    int flush_interval_seconds = 60;
    int max_batch_size = 25;
    int max_queue_size_mb = 10;
    int max_breadcrumbs = 25;
    std::shared_ptr<ILogger> logger;
    EventDefinitionBuilder events;
};

} // namespace beacon
