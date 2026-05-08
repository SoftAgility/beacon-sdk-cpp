#include "beacon/Tracker.hpp"
#include "beacon/Options.hpp"

#include "Base64.hpp"
#include "BreadcrumbBuffer.hpp"
#include "DeviceId.hpp"
#include "DiskQueue.hpp"
#include "EnvironmentCollector.hpp"
#include "HttpClient.hpp"
#include "PropertySanitizer.hpp"
#include "RetryPolicy.hpp"
#include "StackTrace.hpp"
#include "UuidV7.hpp"
#include "beacon/internal/Types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <sys/stat.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace beacon {

// ---------- Static singleton members ----------

std::mutex Tracker::singleton_mutex_;
std::shared_ptr<Tracker> Tracker::singleton_;
bool Tracker::configured_ = false;

// ---------- Helper: ISO 8601 UTC timestamp ----------

namespace {

std::string utc_iso8601_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm utc_tm = {};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &time_t_now);
#else
    gmtime_r(&time_t_now, &utc_tm);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc_tm);

    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

} // anonymous namespace

// ---------- Static factory methods ----------

std::shared_ptr<Tracker> Tracker::configure(std::function<void(Options&)> configurator) {
    Options opts;
    if (configurator) {
        configurator(opts);
    }
    return configure(std::move(opts));
}

std::shared_ptr<Tracker> Tracker::configure(Options options) {
    std::lock_guard<std::mutex> lock(singleton_mutex_);

    if (configured_) {
        throw std::logic_error("beacon::Tracker is already configured.");
    }

    // Validate required fields; disable if invalid
    auto warn_and_disable = [&](const std::string& field) {
        std::string msg = "beacon: " + field + " is missing or invalid. SDK disabled.";
        if (options.logger) {
            options.logger->log(LogLevel::Warning, msg);
        } else {
            std::cerr << msg << std::endl;
        }
        options.enabled = false;
    };

    if (options.api_key.empty()) {
        warn_and_disable("api_key");
    }
    if (options.api_base_url.empty() ||
        (options.api_base_url.substr(0, 7) != "http://" &&
         options.api_base_url.substr(0, 8) != "https://")) {
        warn_and_disable("api_base_url");
    }
    if (options.app_name.empty()) {
        warn_and_disable("app_name");
    }
    if (options.app_version.empty()) {
        warn_and_disable("app_version");
    }

    // Strip trailing slash from URL
    while (!options.api_base_url.empty() && options.api_base_url.back() == '/') {
        options.api_base_url.pop_back();
    }

    // Truncate app_name and app_version
    if (options.app_name.size() > 128) {
        options.app_name.resize(128);
    }
    if (options.app_version.size() > 256) {
        options.app_version.resize(256);
    }

    // Clamp numeric values
    options.flush_interval_seconds = std::max(1, std::min(3600, options.flush_interval_seconds));
    options.max_batch_size = std::max(1, std::min(1000, options.max_batch_size));
    options.max_queue_size_mb = std::max(1, std::min(1000, options.max_queue_size_mb));
    options.max_breadcrumbs = std::max(0, std::min(200, options.max_breadcrumbs));

    auto tracker = std::shared_ptr<Tracker>(new Tracker(std::move(options)));
    singleton_ = tracker;
    configured_ = true;

    return tracker;
}

std::shared_ptr<Tracker> Tracker::instance() {
    std::lock_guard<std::mutex> lock(singleton_mutex_);
    return singleton_;
}

void Tracker::reset_for_testing() {
    std::lock_guard<std::mutex> lock(singleton_mutex_);
    singleton_.reset();
    configured_ = false;
}

// ---------- Constructor ----------

Tracker::Tracker(Options opts)
    : options_(std::move(opts))
{
    // Build event definitions for manifest export — works even when disabled
    event_definitions_ = options_.events.build();

    // Initialize device ID and data directory BEFORE checking Enabled (FR-1128).
    // These are needed by reset(), opt_out(), opt_in() which operate regardless of Enabled.
    try {
        device_id_ = internal::get_or_create_device_id(options_.app_name);
    } catch (...) {
        device_id_ = internal::new_uuid_v7();
        log(LogLevel::Warning, "beacon: device ID creation failed, using transient ID.");
    }

    try {
        data_directory_ = internal::get_data_directory(options_.app_name);
    } catch (...) {
        log(LogLevel::Warning, "beacon: failed to resolve data directory.");
    }

    // Check opt-out sentinel file (FR-1128)
    if (!data_directory_.empty()) {
        try {
            std::string opt_out_path = data_directory_;
#if defined(_WIN32)
            opt_out_path += "\\beacon_opted_out";
#else
            opt_out_path += "/beacon_opted_out";
#endif
            std::ifstream test(opt_out_path);
            if (test.good()) {
                opted_out_.store(true);
            }
        } catch (...) {
            // Cannot check — assume opted in
        }
    }

    if (!options_.enabled) {
        flush_status_.store(FlushStatus::Disabled);
        return;
    }

    // Set initial flush status based on opt-out state
    if (opted_out_.load()) {
        flush_status_.store(FlushStatus::OptedOut);
    } else {
        flush_status_.store(FlushStatus::NotConnected);
    }

    // Collect environment data
    try {
        std::string env_json = internal::collect_environment_json();
        if (!env_json.empty()) {
            environment_data_base64_ = internal::base64_encode(env_json);
        }
    } catch (...) {
        log(LogLevel::Warning, "beacon: environment collection failed.");
    }

    // Initialize disk queue
    init_disk_queue();

    // Start background flush thread
    flush_thread_ = std::thread(&Tracker::flush_thread_loop, this);
}

// ---------- Destructor ----------

Tracker::~Tracker() {
    disposed_.store(true);

    if (!options_.enabled) return;

    // Signal shutdown
    shutdown_requested_.store(true);
    flush_cv_.notify_all();

    // Wait for flush thread to finish (it owns the curl handle)
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }

    // Persist remaining in-memory events to disk (no network I/O)
    // Safe to access SQLite now because the flush thread has exited.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!memory_queue_.empty()) {
            std::vector<std::string> remaining(memory_queue_.begin(), memory_queue_.end());
            memory_queue_.clear();
            enqueue_to_disk(remaining);
        }
    }

    // Clear session
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_.clear();
    }

    // Close disk queue
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }

    // flush_http_ is cleaned up by unique_ptr destructor
}

// ---------- identify() ----------

void Tracker::identify(std::string actor_id) {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: identify() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    if (actor_id.empty()) {
        throw std::invalid_argument("actorId must not be null or empty.");
    }
    if (actor_id.size() > 512) {
        throw std::invalid_argument("actorId must not exceed 512 characters.");
    }

    std::string previous_actor_id;
    std::string device_id_copy;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        previous_actor_id = actor_id_;
        actor_id_ = actor_id;
        device_id_copy = device_id_;
    }

    // Fire best-effort identify POST if this is a new identification (not re-identify)
    if (previous_actor_id != actor_id && !device_id_copy.empty()) {
        std::string api_key = options_.api_key;
        std::string base_url = options_.api_base_url;
        std::string app_name = options_.app_name;
        std::string app_version = options_.app_version;
        auto logger = options_.logger;

        std::thread([device_id_copy, actor_id, api_key, base_url, app_name, app_version, logger]() {
            try {
                nlohmann::json body;
                body["anonymous_actor_id"] = device_id_copy;
                body["identified_actor_id"] = actor_id;
                body["identified_at"] = utc_iso8601_now();
                body["source_app"] = app_name;
                if (!app_version.empty()) {
                    body["source_version"] = app_version;
                }

                internal::HttpClient http;
                if (http.init(logger)) {
                    std::string url = base_url + "/v1/actors/identify";
                    auto result = http.post_json(url, api_key, body.dump());

                    if (result.status_code == 409) {
                        std::string msg = "beacon: device ID " + device_id_copy +
                            " is already linked to a different user. Identity link not recorded.";
                        if (logger) {
                            logger->log(LogLevel::Warning, msg);
                        }
                    }
                }
            } catch (...) {
                // Best-effort — swallow all exceptions
            }
        }).detach();
    }
}

// ---------- track() overloads ----------

void Tracker::track(std::string category, std::string name,
                    std::optional<std::unordered_map<std::string, std::string>> properties) {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: track() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    // Reserved category check
    if (!category.empty() && category[0] == '_') {
        log(LogLevel::Warning, "beacon: track() called with reserved category '" + category + "' -- ignored. Categories starting with '_' are reserved for SDK-internal use.");
        return;
    }

    std::string actor;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        actor = actor_id_;
    }
    if (actor.empty()) {
        // Fall back to device ID (FR-1132)
        if (!device_id_.empty()) {
            actor = device_id_;
        } else {
            log(LogLevel::Warning, "beacon: no actor ID available -- call identify() or check device ID initialization.");
            return;
        }
    }

    track_impl(std::move(category), std::move(name), std::move(actor), std::move(properties));
}

void Tracker::track(std::string category, std::string name, std::string actor_id,
                    std::optional<std::unordered_map<std::string, std::string>> properties) {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: track() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    // Reserved category check
    if (!category.empty() && category[0] == '_') {
        log(LogLevel::Warning, "beacon: track() called with reserved category '" + category + "' -- ignored. Categories starting with '_' are reserved for SDK-internal use.");
        return;
    }

    validate_actor_id(actor_id);
    track_impl(std::move(category), std::move(name), std::move(actor_id), std::move(properties));
}

// ---------- track_impl ----------

void Tracker::track_impl(std::string category, std::string name, std::string actor_id,
                         std::optional<std::unordered_map<std::string, std::string>> properties) {
    try {
        // Truncate category/name
        if (category.size() > 128) category.resize(128);
        if (name.size() > 256) name.resize(256);

        std::string timestamp = utc_iso8601_now();
        std::string event_id = internal::new_uuid_v7();

        // Build JSON
        nlohmann::json j;
        j["event_id"] = event_id;
        j["category"] = category;
        j["name"] = name;
        j["timestamp"] = timestamp;
        j["actor_id"] = actor_id;
        j["source_app"] = options_.app_name;
        j["source_version"] = options_.app_version;

        // Session ID
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_id_.empty()) {
                j["session_id"] = session_id_;
            }
        }

        // Properties
        if (properties.has_value() && !properties->empty()) {
            auto sanitized = internal::sanitize_properties(*properties);
            if (!sanitized.empty()) {
                nlohmann::json props_json = nlohmann::json::object();
                for (const auto& [k, v] : sanitized) {
                    props_json[k] = v;
                }
                j["properties"] = props_json;
            }
        }

        std::string json_str = j.dump();

        // Enqueue to memory queue
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            memory_queue_.push_back(json_str);
        }

        // Add breadcrumb
        if (options_.max_breadcrumbs > 0) {
            std::unordered_map<std::string, std::string> bc_props;
            if (properties.has_value()) {
                auto sanitized = internal::sanitize_properties(*properties);
                for (auto& [k, v] : sanitized) {
                    bc_props[k] = v;
                }
            }
            add_breadcrumb(category, name, timestamp, bc_props);
        }

        // Signal flush thread if batch is full
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (static_cast<int>(memory_queue_.size()) >= options_.max_batch_size) {
                flush_cv_.notify_one();
            }
        }

        // Trigger preflight on first track/startSession call
        trigger_preflight();

    } catch (const std::exception& ex) {
        log(LogLevel::Warning, std::string("beacon: track() internal error: ") + ex.what());
    } catch (...) {
        log(LogLevel::Warning, "beacon: track() internal error (unknown).");
    }
}

// ---------- Session management ----------

void Tracker::startSession() {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: startSession() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    std::string actor;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        actor = actor_id_;
    }
    if (actor.empty()) {
        // Fall back to device ID (FR-1133)
        if (!device_id_.empty()) {
            actor = device_id_;
        } else {
            log(LogLevel::Warning, "beacon: no actor ID available -- call identify() or check device ID initialization.");
            return;
        }
    }

    start_session_impl(std::move(actor));
}

void Tracker::startSession(std::string actor_id) {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: startSession() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    validate_actor_id(actor_id);

    // Store actor_id
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        actor_id_ = actor_id;
    }

    start_session_impl(std::move(actor_id));
}

void Tracker::start_session_impl(std::string actor_id) {
    try {
        std::string old_session_id;
        std::string new_session_id = internal::new_uuid_v7();
        std::string started_at = utc_iso8601_now();

        {
            std::lock_guard<std::mutex> lock(session_mutex_);

            // If a session is already active, end it first
            if (!session_id_.empty()) {
                old_session_id = session_id_;
            }

            session_id_ = new_session_id;
            environment_sent_.store(false); // Reset for new session
        }

        // End old session in background (fire-and-forget)
        // Capture all needed values by value to avoid accessing 'this' from detached thread.
        if (!old_session_id.empty()) {
            std::string api_key_end = options_.api_key;
            std::string base_url_end = options_.api_base_url;
            auto logger_end = options_.logger;
            std::thread([old_session_id, api_key_end, base_url_end, logger_end]() {
                try {
                    nlohmann::json body;
                    body["session_id"] = old_session_id;
                    body["ended_at"] = utc_iso8601_now();
                    body["end_reason"] = "normal";

                    internal::HttpClient http;
                    if (http.init(logger_end)) {
                        std::string url = base_url_end + "/v1/events/sessions/end";
                        http.post_json(url, api_key_end, body.dump());
                    }
                } catch (...) {}
            }).detach();
        }

        // Start new session in background (fire-and-forget)
        std::string app_name = options_.app_name;
        std::string app_version = options_.app_version;
        std::string api_key = options_.api_key;
        std::string base_url = options_.api_base_url;
        auto logger_start = options_.logger;

        std::thread([new_session_id, actor_id, app_name, app_version,
                     started_at, api_key, base_url, logger_start]() {
            try {
                nlohmann::json body;
                body["session_id"] = new_session_id;
                body["actor_id"] = actor_id;
                body["source_app"] = app_name;
                body["source_version"] = app_version;
                body["started_at"] = started_at;

                internal::HttpClient http;
                if (http.init(logger_start)) {
                    std::string url = base_url + "/v1/events/sessions";
                    http.post_json(url, api_key, body.dump());
                }
            } catch (...) {}
        }).detach();

        // Trigger preflight on first track/startSession call
        trigger_preflight();

    } catch (const std::exception& ex) {
        log(LogLevel::Warning, std::string("beacon: startSession() internal error: ") + ex.what());
    } catch (...) {
        log(LogLevel::Warning, "beacon: startSession() internal error (unknown).");
    }
}

void Tracker::endSession() {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: endSession() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    try {
        std::string sid;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (session_id_.empty()) return; // No-op
            sid = session_id_;
            session_id_.clear();
        }

        std::string api_key = options_.api_key;
        std::string base_url = options_.api_base_url;
        auto logger = options_.logger;
        std::thread([sid, api_key, base_url, logger]() {
            try {
                nlohmann::json body;
                body["session_id"] = sid;
                body["ended_at"] = utc_iso8601_now();
                body["end_reason"] = "normal";

                internal::HttpClient http;
                if (http.init(logger)) {
                    std::string url = base_url + "/v1/events/sessions/end";
                    http.post_json(url, api_key, body.dump());
                }
            } catch (...) {}
        }).detach();

    } catch (const std::exception& ex) {
        log(LogLevel::Warning, std::string("beacon: endSession() internal error: ") + ex.what());
    } catch (...) {
        log(LogLevel::Warning, "beacon: endSession() internal error (unknown).");
    }
}

// ---------- Consent API (FR-1129, FR-1130) ----------

void Tracker::opt_out() {
    try {
        // Idempotent — if already opted out, do nothing (ED-735)
        bool expected = false;
        if (!opted_out_.compare_exchange_strong(expected, true)) {
            return;
        }

        // Clear in-memory queue under queue mutex (FR-1129)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            memory_queue_.clear();
        }

        // Set flush status
        flush_status_.store(FlushStatus::OptedOut);

        // Persist opt-out to disk (EC-659)
        if (!data_directory_.empty()) {
            try {
                std::string opt_out_path = data_directory_;
#if defined(_WIN32)
                opt_out_path += "\\beacon_opted_out";
#else
                opt_out_path += "/beacon_opted_out";
#endif
                std::ofstream ofs(opt_out_path, std::ios::trunc);
                // Empty file — just create it
                if (ofs.is_open()) {
                    ofs.close();
                }
            } catch (...) {
                log(LogLevel::Warning,
                    "beacon: failed to persist opt-out flag to disk -- opted out in-memory only. State will not survive restart.");
            }
        }
    } catch (...) {
        log(LogLevel::Warning, "beacon: opt_out() internal error.");
    }
}

void Tracker::opt_in() {
    try {
        // Idempotent — if not opted out, do nothing (ED-732)
        bool expected = true;
        if (!opted_out_.compare_exchange_strong(expected, false)) {
            return;
        }

        // Delete opt-out file (EC-660)
        if (!data_directory_.empty()) {
            try {
                std::string opt_out_path = data_directory_;
#if defined(_WIN32)
                opt_out_path += "\\beacon_opted_out";
#else
                opt_out_path += "/beacon_opted_out";
#endif
                std::remove(opt_out_path.c_str());
            } catch (...) {
                log(LogLevel::Warning,
                    "beacon: failed to remove opt-out flag file -- opted in in-memory only. Opt-out may re-apply on next restart.");
            }
        }

        // Reset flush status
        flush_status_.store(FlushStatus::NotConnected);

        // Signal the flush thread condition variable to wake and check for work
        flush_cv_.notify_one();
    } catch (...) {
        log(LogLevel::Warning, "beacon: opt_in() internal error.");
    }
}

// ---------- Reset (FR-1131) ----------

void Tracker::reset() {
    try {
        // Operates regardless of enabled or opt-out state

        // Step 1: End active session (fire-and-forget)
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_id_.empty()) {
                std::string old_session_id = session_id_;
                session_id_.clear();

                // Fire-and-forget session end in background thread
                std::string api_key = options_.api_key;
                std::string base_url = options_.api_base_url;
                auto logger = options_.logger;
                std::thread([old_session_id, api_key, base_url, logger]() {
                    try {
                        nlohmann::json body;
                        body["session_id"] = old_session_id;
                        body["ended_at"] = utc_iso8601_now();
                        body["end_reason"] = "normal";

                        internal::HttpClient http;
                        if (http.init(logger)) {
                            std::string url = base_url + "/v1/events/sessions/end";
                            http.post_json(url, api_key, body.dump());
                        }
                    } catch (...) {}
                }).detach();
            }

            // Step 2: Clear actor ID
            actor_id_.clear();
        }

        // Step 3: Clear in-memory queue
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            memory_queue_.clear();
        }

        // Step 4: Clear breadcrumbs
        {
            std::lock_guard<std::mutex> lock(breadcrumb_mutex_);
            breadcrumbs_.clear();
        }

        // Step 5 & 6: Generate new device ID and persist
        std::string new_device_id = internal::new_uuid_v7();

        try {
            internal::write_device_id(options_.app_name, new_device_id);
        } catch (...) {
            log(LogLevel::Warning,
                "beacon: failed to write new device ID to disk -- anonymous ID is ephemeral for this session.");
        }

        device_id_ = new_device_id;

        // Step 7: Opt-out state is NOT changed
    } catch (...) {
        log(LogLevel::Warning, "beacon: reset() internal error.");
    }
}

// ---------- Exception tracking ----------

void Tracker::trackException(const std::exception& ex, ExceptionSeverity severity) {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: trackException() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    std::string actor;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        actor = actor_id_;
    }

    if (actor.empty()) {
        // Fall back to device ID (FR-1134)
        if (!device_id_.empty()) {
            actor = device_id_;
        } else {
            log(LogLevel::Warning, "beacon: no actor ID available -- call identify() or check device ID initialization.");
            return;
        }
    }

    track_exception_impl(ex, std::move(actor), severity);
}

void Tracker::trackException(const std::exception& ex, std::string actor_id,
                             ExceptionSeverity severity) {
    if (disposed_.load()) {
        log(LogLevel::Warning, "beacon: trackException() called on disposed tracker -- ignored.");
        return;
    }
    if (!options_.enabled) return;
    if (opted_out_.load()) return;

    validate_actor_id(actor_id);
    track_exception_impl(ex, std::move(actor_id), severity);
}

void Tracker::track_exception_impl(const std::exception& ex, std::string actor_id,
                                   ExceptionSeverity severity) {
    try {
        // Capture data on calling thread
        std::string exception_id = internal::new_uuid_v7();
        std::string exception_type = internal::demangle_type_name(typeid(ex).name());
        std::string occurred_at = utc_iso8601_now();

        std::string message;
        if (ex.what()) {
            message = std::string(ex.what());
            if (message.size() > 1000) message.resize(1000);
        }

        std::string stack_trace = internal::capture_stack_trace();
        if (stack_trace.size() > 32768) stack_trace.resize(32768);

        std::string current_session;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            current_session = session_id_;
        }

        // Snapshot breadcrumbs
        auto bc_snapshot = snapshot_breadcrumbs();

        // Build JSON
        nlohmann::json body;
        body["exception_id"] = exception_id;
        body["exception_type"] = exception_type;
        body["severity"] = (severity == ExceptionSeverity::Fatal) ? "fatal" : "non_fatal";
        body["occurred_at"] = occurred_at;
        body["actor_id"] = actor_id;
        body["source_app"] = options_.app_name;
        body["source_version"] = options_.app_version;

        if (!message.empty()) {
            body["message"] = message;
        }

        if (!stack_trace.empty()) {
            body["stack_trace"] = stack_trace;
        }

        if (!current_session.empty()) {
            body["session_id"] = current_session;
        }

        if (!bc_snapshot.empty()) {
            nlohmann::json bc_array = nlohmann::json::array();
            for (const auto& bc : bc_snapshot) {
                nlohmann::json bc_json;
                bc_json["timestamp"] = bc.timestamp;
                bc_json["category"] = bc.category;
                bc_json["name"] = bc.name;
                if (!bc.properties.empty()) {
                    nlohmann::json props = nlohmann::json::object();
                    for (const auto& [k, v] : bc.properties) {
                        props[k] = v;
                    }
                    bc_json["properties"] = props;
                }
                bc_array.push_back(bc_json);
            }
            body["breadcrumbs"] = bc_array;
        }

        // Dispatch in background thread (fire-and-forget)
        std::string json_body = body.dump();

        // Store for test accessor
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            last_exception_json_ = json_body;
        }
        std::string api_key = options_.api_key;
        std::string base_url = options_.api_base_url;
        auto logger = options_.logger;

        std::thread([json_body, api_key, base_url, logger]() {
            try {
                internal::HttpClient http;
                if (http.init(logger)) {
                    std::string url = base_url + "/v1/events/exceptions";
                    http.post_json(url, api_key, json_body);
                }
            } catch (...) {}
        }).detach();

    } catch (const std::exception& e) {
        log(LogLevel::Warning, std::string("beacon: trackException() internal error: ") + e.what());
    } catch (...) {
        log(LogLevel::Warning, "beacon: trackException() internal error (unknown).");
    }
}

// ---------- flush() ----------

bool Tracker::flush() {
    if (!options_.enabled) return true;
    if (disposed_.load()) return false;
    if (opted_out_.load()) return true;

    flush_requested_.store(true);
    flush_completed_.store(false);
    flush_cv_.notify_one();

    // Wait for flush to complete or timeout
    std::unique_lock<std::mutex> lock(flush_sync_mutex_);
    bool result = flush_done_cv_.wait_for(lock, std::chrono::seconds(30),
        [this]() { return flush_completed_.load(); });

    flush_requested_.store(false);
    return result;
}

// ---------- exportEventManifest() ----------

void Tracker::exportEventManifest(std::string file_path) {
    nlohmann::json manifest;
    manifest["schema_version"] = "1";
    manifest["generated_at"] = utc_iso8601_now();
    manifest["source_app"] = options_.app_name;
    manifest["source_version"] = options_.app_version;

    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [cat, nm] : event_definitions_) {
        entries.push_back({{"category", cat}, {"name", nm}});
    }
    manifest["entries"] = entries;

    std::ofstream ofs(file_path);
    if (!ofs.is_open()) {
        throw std::runtime_error(
            "beacon: failed to write event manifest to " + file_path + ": unable to open file.");
    }

    ofs << manifest.dump(2);
    ofs.close();

    if (ofs.fail()) {
        throw std::runtime_error(
            "beacon: failed to write event manifest to " + file_path + ": write failed.");
    }
}

// ---------- last_flush_status() ----------

FlushStatus Tracker::last_flush_status() const {
    return flush_status_.load();
}

// ---------- Testing accessors ----------

size_t Tracker::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return memory_queue_.size();
}

std::string Tracker::last_enqueued_json() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (memory_queue_.empty()) return {};
    return memory_queue_.back();
}

std::string Tracker::last_exception_json() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return last_exception_json_;
}

const Options& Tracker::options() const {
    return options_;
}

std::string Tracker::session_id() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_id_;
}

// ---------- Private helpers ----------

void Tracker::log(LogLevel level, const std::string& message) const {
    if (options_.logger) {
        options_.logger->log(level, message);
    }
}

void Tracker::validate_actor_id(const std::string& actor_id) const {
    if (actor_id.empty()) {
        throw std::invalid_argument("actorId must not be null or empty.");
    }
    if (actor_id.size() > 512) {
        throw std::invalid_argument("actorId must not exceed 512 characters.");
    }
}

void Tracker::trigger_preflight() {
    // Use shared_from_this() to keep the Tracker alive during the preflight request.
    // If the Tracker is already being destroyed, weak_from_this() expires and we skip.
    std::call_once(preflight_flag_, [this]() {
        std::string api_key = options_.api_key;
        std::string base_url = options_.api_base_url;
        auto logger = options_.logger;
        std::weak_ptr<Tracker> weak_self;
        try {
            weak_self = shared_from_this();
        } catch (...) {
            // shared_from_this() may throw if no shared_ptr owns this.
            return;
        }

        std::thread([weak_self, api_key, base_url, logger]() {
            try {
                internal::HttpClient http;
                if (!http.init(logger)) return;

                std::string url = base_url + "/v1/events";
                auto result = http.post_json(url, api_key, "[]");

                if (result.status_code == 401) {
                    if (auto self = weak_self.lock()) {
                        self->halted_.store(true);
                        self->flush_status_.store(FlushStatus::Offline);
                        self->log(LogLevel::Error,
                            "beacon: API key rejected (401). Check Options::api_key. Event delivery halted.");
                    }
                }
            } catch (...) {}
        }).detach();
    });
}

// ---------- Breadcrumb helpers ----------

void Tracker::add_breadcrumb(const std::string& category, const std::string& name,
                             const std::string& timestamp,
                             const std::unordered_map<std::string, std::string>& properties) {
    std::lock_guard<std::mutex> lock(breadcrumb_mutex_);

    internal::BreadcrumbEntry entry;
    entry.category = category;
    entry.name = name;
    entry.timestamp = timestamp;
    entry.properties = properties;

    breadcrumbs_.push_back(std::move(entry));

    while (static_cast<int>(breadcrumbs_.size()) > options_.max_breadcrumbs) {
        breadcrumbs_.pop_front();
    }
}

std::vector<internal::BreadcrumbEntry> Tracker::snapshot_breadcrumbs() const {
    std::lock_guard<std::mutex> lock(breadcrumb_mutex_);
    return std::vector<internal::BreadcrumbEntry>(breadcrumbs_.begin(), breadcrumbs_.end());
}

// ---------- Disk queue helpers ----------

void Tracker::init_disk_queue() {
    try {
        // Determine path for disk queue
        std::string safe_name = internal::sanitize_path_component(options_.app_name);

#if defined(_WIN32)
        char appdata[260] = {};
        if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) > 0) {
            db_path_ = std::string(appdata) + "\\SoftAgility\\Beacon\\" + safe_name + "\\beacon_queue.db";
        } else {
            db_path_ = "beacon_queue.db";
        }
#elif defined(__APPLE__)
        const char* home = std::getenv("HOME");
        if (home) {
            db_path_ = std::string(home) + "/Library/Application Support/SoftAgility/Beacon/" + safe_name + "/beacon_queue.db";
        } else {
            db_path_ = "beacon_queue.db";
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            db_path_ = std::string(home) + "/.local/share/SoftAgility/Beacon/" + safe_name + "/beacon_queue.db";
        } else {
            db_path_ = "/var/lib/SoftAgility/Beacon/" + safe_name + "/beacon_queue.db";
        }
#endif

        // Create directory
        std::string dir = db_path_.substr(0, db_path_.find_last_of("/\\"));
#if defined(_WIN32)
        // Recursively create dirs on Windows
        std::string current;
        for (size_t i = 0; i < dir.size(); ++i) {
            current += dir[i];
            if ((dir[i] == '/' || dir[i] == '\\') && current.size() > 1) {
                CreateDirectoryA(current.c_str(), nullptr);
            }
        }
        CreateDirectoryA(dir.c_str(), nullptr);
#else
        std::string current;
        for (size_t i = 0; i < dir.size(); ++i) {
            current += dir[i];
            if (dir[i] == '/' && current.size() > 1) {
                mkdir(current.c_str(), 0755);
            }
        }
        mkdir(dir.c_str(), 0755);
#endif

        // Open SQLite
        int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);

        if (rc != SQLITE_OK) {
            log(LogLevel::Warning,
                "beacon: failed to initialize disk queue at " + db_path_ + ": " +
                (db_ ? sqlite3_errmsg(db_) : "unknown error") +
                ". Offline persistence unavailable.");
            if (db_) {
                sqlite3_close_v2(db_);
                db_ = nullptr;
            }
            return;
        }

        // Create table
        const char* create_sql =
            "CREATE TABLE IF NOT EXISTS queued_events ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    event_id TEXT NOT NULL,"
            "    payload_json TEXT NOT NULL,"
            "    queued_at TEXT NOT NULL,"
            "    retry_count INTEGER NOT NULL DEFAULT 0"
            ");";

        char* err_msg = nullptr;
        rc = sqlite3_exec(db_, create_sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            log(LogLevel::Warning,
                std::string("beacon: failed to create disk queue table: ") +
                (err_msg ? err_msg : "unknown error"));
            if (err_msg) sqlite3_free(err_msg);
        }

        // WAL mode
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    } catch (const std::exception& ex) {
        log(LogLevel::Warning,
            std::string("beacon: disk queue init failed: ") + ex.what());
    }
}

void Tracker::enqueue_to_disk(const std::vector<std::string>& events) {
    if (!db_ || events.empty()) return;

    try {
        // Enforce size cap
        int64_t max_bytes = static_cast<int64_t>(options_.max_queue_size_mb) * 1024 * 1024;
        int64_t current_size = get_disk_queue_file_size();

        if (current_size >= max_bytes) {
            // Evict oldest events
            bool evicted_any = false;
            while (current_size >= max_bytes) {
                const char* delete_sql =
                    "DELETE FROM queued_events WHERE id IN "
                    "(SELECT id FROM queued_events ORDER BY id ASC LIMIT 100);";

                int rc = sqlite3_exec(db_, delete_sql, nullptr, nullptr, nullptr);
                if (rc != SQLITE_OK) break;

                int changes = sqlite3_changes(db_);
                if (changes == 0) break;

                evicted_any = true;
                log(LogLevel::Warning, "beacon: disk queue at cap, evicted oldest events.");
                current_size = get_disk_queue_file_size();
            }

            // Vacuum once after all deletions
            if (evicted_any) {
                sqlite3_exec(db_, "VACUUM;", nullptr, nullptr, nullptr);
                current_size = get_disk_queue_file_size();
            }

            // If still at cap, drop the batch
            if (current_size >= max_bytes) {
                log(LogLevel::Warning, "beacon: disk queue still at cap after eviction, dropping batch.");
                return;
            }
        }

        // Insert events
        const char* insert_sql =
            "INSERT INTO queued_events (event_id, payload_json, queued_at, retry_count) "
            "VALUES (?, ?, ?, 0);";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return;

        std::string now = utc_iso8601_now();

        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        for (const auto& json_str : events) {
            std::string event_id;
            try {
                auto j = nlohmann::json::parse(json_str);
                if (j.contains("event_id") && j["event_id"].is_string()) {
                    event_id = j["event_id"].get<std::string>();
                }
            } catch (...) {}
            if (event_id.empty()) event_id = internal::new_uuid_v7();

            sqlite3_bind_text(stmt, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, json_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }

        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_finalize(stmt);

    } catch (...) {
        log(LogLevel::Warning, "beacon: failed to write events to disk queue.");
    }
}

std::vector<internal::QueuedEvent> Tracker::dequeue_from_disk(int limit) {
    std::vector<internal::QueuedEvent> result;
    if (!db_) return result;

    const char* select_sql =
        "SELECT id, event_id, payload_json, queued_at, retry_count "
        "FROM queued_events ORDER BY id ASC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        internal::QueuedEvent event;
        event.id = sqlite3_column_int64(stmt, 0);

        const char* text;
        text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (text) event.event_id = text;
        text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (text) event.payload_json = text;
        text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (text) event.queued_at = text;

        event.retry_count = sqlite3_column_int(stmt, 4);
        result.push_back(std::move(event));
    }

    sqlite3_finalize(stmt);
    return result;
}

void Tracker::delete_from_disk(const std::vector<int64_t>& ids) {
    if (!db_ || ids.empty()) return;

    std::ostringstream oss;
    oss << "DELETE FROM queued_events WHERE id IN (";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) oss << ",";
        oss << ids[i];
    }
    oss << ");";

    sqlite3_exec(db_, oss.str().c_str(), nullptr, nullptr, nullptr);
}

void Tracker::enforce_disk_queue_size() {
    if (!db_) return;

    int64_t max_bytes = static_cast<int64_t>(options_.max_queue_size_mb) * 1024 * 1024;
    int64_t current = get_disk_queue_file_size();
    bool evicted_any = false;

    while (current >= max_bytes) {
        const char* sql =
            "DELETE FROM queued_events WHERE id IN "
            "(SELECT id FROM queued_events ORDER BY id ASC LIMIT 100);";

        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        int changes = sqlite3_changes(db_);
        if (changes == 0) break;

        evicted_any = true;
        log(LogLevel::Warning, "beacon: disk queue at cap, evicted oldest events.");
        current = get_disk_queue_file_size();
    }

    // Vacuum once after all deletions to reclaim space
    if (evicted_any) {
        sqlite3_exec(db_, "VACUUM;", nullptr, nullptr, nullptr);
    }
}

int64_t Tracker::get_disk_queue_file_size() const {
    if (db_path_.empty()) return 0;

#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    if (GetFileAttributesExA(db_path_.c_str(), GetFileExInfoStandard, &attrs)) {
        LARGE_INTEGER size;
        size.HighPart = attrs.nFileSizeHigh;
        size.LowPart = attrs.nFileSizeLow;
        return size.QuadPart;
    }
    return 0;
#else
    struct stat st = {};
    if (stat(db_path_.c_str(), &st) == 0) {
        return st.st_size;
    }
    return 0;
#endif
}

// ---------- Flush thread ----------

void Tracker::flush_thread_loop() {
    // Initialize HTTP client for this thread
    flush_http_ = std::make_unique<internal::HttpClient>();
    if (!flush_http_->init(options_.logger)) {
        log(LogLevel::Error, "beacon: libcurl initialization failed. HTTP delivery unavailable.");
        halted_.store(true);
        return;
    }

    // Determine initial wait: min(5s, flush_interval_seconds)
    int initial_wait_seconds = std::min(5, options_.flush_interval_seconds);

    bool first_wake = true;

    while (!shutdown_requested_.load()) {
        // Wait on condition variable with timeout
        {
            std::unique_lock<std::mutex> lock(flush_cv_mutex_);

            auto timeout = first_wake
                ? std::chrono::seconds(initial_wait_seconds)
                : std::chrono::seconds(options_.flush_interval_seconds);

            flush_cv_.wait_for(lock, timeout, [this]() {
                return shutdown_requested_.load() || flush_requested_.load();
            });

            first_wake = false;
        }

        if (shutdown_requested_.load()) break;
        if (halted_.load()) continue;
        if (opted_out_.load()) continue;

        // Try to acquire flush semaphore
        bool is_sync_flush = flush_requested_.load();
        {
            std::lock_guard<std::mutex> lock(flush_in_progress_mutex_);
            if (flush_in_progress_ && !is_sync_flush) {
                continue; // Skip if async and flush already in progress
            }
            flush_in_progress_ = true;
        }

        // Drain disk queue first
        drain_disk_queue();

        // Then drain memory queue
        drain_memory_queue();

        // Release flush semaphore
        {
            std::lock_guard<std::mutex> lock(flush_in_progress_mutex_);
            flush_in_progress_ = false;
        }

        // Signal flush() if it was a sync request
        if (is_sync_flush) {
            flush_completed_.store(true);
            flush_done_cv_.notify_all();
        }
    }

    // Cleanup HTTP client on thread exit
    flush_http_.reset();
}

void Tracker::drain_disk_queue() {
    if (!db_ || halted_.load()) return;

    auto batch = dequeue_from_disk(options_.max_batch_size);
    if (batch.empty()) return;

    // Build JSON array from disk queue events
    nlohmann::json arr = nlohmann::json::array();
    std::vector<int64_t> ids;

    for (const auto& event : batch) {
        try {
            arr.push_back(nlohmann::json::parse(event.payload_json));
        } catch (...) {
            arr.push_back(nlohmann::json::parse("{}"));
        }
        ids.push_back(event.id);
    }

    std::string json_body = arr.dump();
    std::string url = options_.api_base_url + "/v1/events";

    // Determine if we should send environment header
    std::string env_header;
    if (!environment_sent_.load() && !environment_data_base64_.empty()) {
        env_header = environment_data_base64_;
    }

    // Send HTTP request via shared HttpClient
    internal::HttpResult result;
    if (flush_http_ && flush_http_->is_initialized()) {
        result = flush_http_->post_json(url, options_.api_key, json_body, env_header);
    } else {
        result.is_network_error = true;
    }

    if (result.success) {
        delete_from_disk(ids);
        if (!opted_out_.load())
            flush_status_.store(FlushStatus::Connected);
        if (!env_header.empty()) {
            environment_sent_.store(true);
        }
    } else if (result.status_code == 401) {
        halted_.store(true);
        flush_status_.store(FlushStatus::Offline);
        log(LogLevel::Error, "beacon: API key rejected (401). Event delivery halted.");
    } else if (result.status_code == 402) {
        flush_status_.store(FlushStatus::Offline);
        // Leave in queue
    } else if (internal::RetryPolicy::is_permanent_failure(result)) {
        // Permanent 4xx: delete from queue
        delete_from_disk(ids);
        log(LogLevel::Warning, "beacon: permanent HTTP error " + std::to_string(result.status_code) +
            " for disk queue batch. Events discarded.");
    } else {
        // Retryable or network error - leave in disk queue for next cycle
        flush_status_.store(FlushStatus::Offline);
    }
}

void Tracker::drain_memory_queue() {
    if (halted_.load()) return;

    while (!shutdown_requested_.load() && !halted_.load()) {
        std::vector<std::string> batch;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (memory_queue_.empty()) break;

            int count = std::min(static_cast<int>(memory_queue_.size()), options_.max_batch_size);
            for (int i = 0; i < count; ++i) {
                batch.push_back(std::move(memory_queue_.front()));
                memory_queue_.pop_front();
            }
        }

        if (batch.empty()) break;

        // Build JSON array
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& event_json : batch) {
            try {
                arr.push_back(nlohmann::json::parse(event_json));
            } catch (...) {
                arr.push_back(nlohmann::json::parse("{}"));
            }
        }

        std::string json_body = arr.dump();
        std::string url = options_.api_base_url + "/v1/events";

        // Determine if we should send environment header
        std::string env_header;
        if (!environment_sent_.load() && !environment_data_base64_.empty()) {
            env_header = environment_data_base64_;
        }

        // Attempt send with retry
        internal::HttpResult result;
        bool delivered = false;

        for (int attempt = 0; attempt <= internal::RetryPolicy::max_retries; ++attempt) {
            if (attempt > 0) {
                auto delay = internal::RetryPolicy::compute_delay(
                    attempt - 1, result.retry_after_seconds);
                std::this_thread::sleep_for(delay);
            }

            result = internal::HttpResult{};
            if (flush_http_ && flush_http_->is_initialized()) {
                result = flush_http_->post_json(url, options_.api_key, json_body, env_header);
            } else {
                result.is_network_error = true;
            }

            if (result.success) {
                delivered = true;
                if (!opted_out_.load())
                    flush_status_.store(FlushStatus::Connected);
                if (!env_header.empty()) {
                    environment_sent_.store(true);
                }
                break;
            }

            if (result.status_code == 401) {
                halted_.store(true);
                flush_status_.store(FlushStatus::Offline);
                log(LogLevel::Error, "beacon: API key rejected (401). Event delivery halted.");
                return;
            }

            if (result.status_code == 402) {
                // Write to disk queue, leave for later
                enqueue_to_disk(batch);
                flush_status_.store(FlushStatus::Offline);
                return;
            }

            if (internal::RetryPolicy::is_permanent_failure(result)) {
                // Permanent 4xx: discard
                log(LogLevel::Warning, "beacon: permanent HTTP error " +
                    std::to_string(result.status_code) + ". Events discarded.");
                return;
            }

            if (!internal::RetryPolicy::is_retryable(result) && !result.is_network_error) {
                break;
            }
        }

        if (!delivered) {
            // Write failed batch to disk queue
            enqueue_to_disk(batch);
            flush_status_.store(FlushStatus::Offline);
        }
    }
}

} // namespace beacon
