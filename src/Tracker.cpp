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

namespace {

// Per-request timeout for background (flush-thread) session-end delivery.
// This is NOT the destructor's bounded send — that uses
// Options.shutdown_flush_timeout_seconds. The background drain runs off the
// flush thread where a normal 10s transport timeout is appropriate.
constexpr long kSessionEndDeliveryTimeoutSeconds = 10L;

} // anonymous namespace

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
    if (options.product.empty()) {
        warn_and_disable("product");
    }
    if (options.product_version.empty()) {
        warn_and_disable("product_version");
    }

    // Strip trailing slash from URL
    while (!options.api_base_url.empty() && options.api_base_url.back() == '/') {
        options.api_base_url.pop_back();
    }

    // Truncate product and product_version
    if (options.product.size() > 128) {
        options.product.resize(128);
    }
    if (options.product_version.size() > 256) {
        options.product_version.resize(256);
    }

    // Clamp numeric values
    options.flush_interval_seconds = std::max(1, std::min(3600, options.flush_interval_seconds));
    options.max_batch_size = std::max(1, std::min(1000, options.max_batch_size));
    options.max_queue_size_mb = std::max(1, std::min(1000, options.max_queue_size_mb));
    options.max_breadcrumbs = std::max(0, std::min(200, options.max_breadcrumbs));
    options.shutdown_flush_timeout_seconds =
        std::max(0, std::min(30, options.shutdown_flush_timeout_seconds));

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
    // These are needed by reset(), optOut(), optIn() which operate regardless of Enabled.
    try {
        device_id_ = internal::get_or_create_device_id(options_.product);
    } catch (...) {
        device_id_ = internal::new_uuid_v7();
        log(LogLevel::Warning, "beacon: device ID creation failed, using transient ID.");
    }

    try {
        data_directory_ = internal::get_data_directory(options_.product);
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

    // Take a reference on libcurl's process-wide global state. Paired with
    // global_cleanup() in the destructor. Owning the global lifecycle
    // explicitly (rather than relying on curl_easy_init's lazy init) is
    // required because a destruct-time bounded send may be the last libcurl
    // use in the process.
    internal::HttpClient::global_init();

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

    // Any pending session-ends persisted by a PRIOR process are recovery
    // deliveries: rewrite their end_reason to "sdk_recovery" before the flush
    // thread starts draining them. Records enqueued during THIS run keep
    // "normal". This is the next-launch recovery delivery (Part A step 5):
    // the flush thread's first tick will deliver them with the ORIGINAL
    // ended_at via drain_pending_session_ends().
    mark_pending_session_ends_as_recovery();

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

    // Durable session-end on clean close (Part A). The entire write -> send ->
    // delete path is wrapped so nothing escapes the destructor (no
    // std::terminate). The flush thread is already joined above, so the SQLite
    // store is exclusively owned here. Skipped when opted out (consent) — the
    // pending-end store is purged instead.
    try {
        internal::PendingSessionEnd rec;
        bool has_session = false;
        std::string ended_at = utc_iso8601_now();

        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            has_session = build_pending_end_from_snapshot(ended_at, "normal", rec);
            // Clear session AFTER snapshotting (race-free: flush thread joined).
            session_id_.clear();
            session_actor_id_.clear();
            session_account_id_.clear();
            session_license_id_.clear();
            session_started_at_.clear();
        }

        if (has_session && !opted_out_.load()) {
            // 1) Persist the durable record first so a failed/skipped send is
            //    still recovered on next launch with the true ended_at.
            int64_t rowid = persist_pending_session_end(rec);

            // 2) Best-effort BOUNDED synchronous send on a FRESH HttpClient.
            //    A timeout of 0 means "skip the send, disk-only".
            int timeout = options_.shutdown_flush_timeout_seconds;
            if (timeout > 0 && rowid != 0) {
                internal::HttpClient http;
                if (http.init(options_.logger)) {
                    bool retain_as_recovery = false;
                    bool should_delete = deliver_session_end(
                        http, rec, /*recovery=*/false,
                        static_cast<long>(timeout), retain_as_recovery);
                    if (should_delete) {
                        delete_pending_session_end(rowid);
                    }
                    // If retained (network failure, or session_not_found that
                    // raced the start), the record stays in the store. The NEXT
                    // launch's constructor runs mark_pending_session_ends_as_
                    // recovery(), flipping it to end_reason="sdk_recovery"
                    // before the flush thread drains it with the ORIGINAL
                    // ended_at — so no rewrite is needed here.
                }
            }
        }
    } catch (...) {
        // No-throw destructor: swallow everything (logging may itself throw).
    }

    // Close disk queue
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }

    // Release our reference on libcurl's global state (paired with the
    // global_init() in the constructor). The fresh HttpClient used above has
    // already been destroyed, so this is safe.
    internal::HttpClient::global_cleanup();

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
        std::string product = options_.product;
        std::string product_version = options_.product_version;
        auto logger = options_.logger;

        std::thread([device_id_copy, actor_id, api_key, base_url, product, product_version, logger]() {
            try {
                nlohmann::json body;
                body["anonymous_actor_id"] = device_id_copy;
                body["identified_actor_id"] = actor_id;
                body["identified_at"] = utc_iso8601_now();
                body["product"] = product;
                if (!product_version.empty()) {
                    body["product_version"] = product_version;
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
        j["product"] = options_.product;
        j["product_version"] = options_.product_version;

        // Session ID + account/license context
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_id_.empty()) {
                j["session_id"] = session_id_;
            }
            // Omit when unset — backend distinguishes "absent" from "present but invalid".
            if (!account_id_.empty()) {
                j["account_id"] = account_id_;
            }
            if (!license_id_.empty()) {
                j["license_id"] = license_id_;
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
        std::string new_session_id = internal::new_uuid_v7();
        std::string started_at = utc_iso8601_now();

        // Snapshot account/license context for the NEW session's start payload.
        std::string account_snapshot;
        std::string license_snapshot;

        // Durable end for any PRIOR session, built from its OLD snapshots
        // (.NET port lesson — never from the live actor/account/license, which
        // startSession(actor) may already have overwritten).
        internal::PendingSessionEnd prior_end;
        bool has_prior = false;
        std::string prior_ended_at = utc_iso8601_now();

        {
            std::lock_guard<std::mutex> lock(session_mutex_);

            // If a session is already active, build its durable end FIRST,
            // from the prior session's snapshots, BEFORE repointing live state.
            if (!session_id_.empty()) {
                has_prior = build_pending_end_from_snapshot(
                    prior_ended_at, "normal", prior_end);
            }

            // Now repoint live + snapshot state to the NEW session.
            session_id_ = new_session_id;
            environment_sent_.store(false); // Reset for new session

            account_snapshot = account_id_;
            license_snapshot = license_id_;

            // Capture the NEW session's snapshots so a later end/destruct uses
            // the context that was live at THIS session's start.
            session_actor_id_ = actor_id;
            session_account_id_ = account_id_;
            session_license_id_ = license_id_;
            session_started_at_ = started_at;
        }

        // Persist + enqueue the prior session's durable end. It is delivered by
        // the background flush thread / flush() (Part B) — NOT a detached
        // fire-and-forget thread. Note we already hold no lock here.
        if (has_prior) {
            persist_pending_session_end(prior_end);
            // Wake the flush thread so the end is delivered promptly.
            flush_cv_.notify_one();
        }

        // Start new session in background (fire-and-forget)
        std::string product = options_.product;
        std::string product_version = options_.product_version;
        std::string api_key = options_.api_key;
        std::string base_url = options_.api_base_url;
        auto logger_start = options_.logger;

        std::thread([new_session_id, actor_id, product, product_version,
                     started_at, api_key, base_url, logger_start,
                     account_snapshot, license_snapshot]() {
            try {
                nlohmann::json body;
                body["session_id"] = new_session_id;
                body["actor_id"] = actor_id;
                body["product"] = product;
                body["product_version"] = product_version;
                body["started_at"] = started_at;
                if (!account_snapshot.empty()) body["account_id"] = account_snapshot;
                if (!license_snapshot.empty()) body["license_id"] = license_snapshot;

                internal::HttpClient http;
                if (http.init(logger_start)) {
                    std::string url = base_url + "/v1/events/sessions";
                    http.post_json(url, api_key, body.dump());
                }
            } catch (...) {}
        }).detach();

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
        internal::PendingSessionEnd rec;
        bool has_session = false;
        std::string ended_at = utc_iso8601_now();

        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (session_id_.empty()) return; // No-op
            has_session = build_pending_end_from_snapshot(ended_at, "normal", rec);
            // Clear live + snapshot session state AFTER snapshotting the end.
            session_id_.clear();
            session_actor_id_.clear();
            session_account_id_.clear();
            session_license_id_.clear();
            session_started_at_.clear();
        }

        // Persist the durable end and let the background flush thread / an
        // explicit flush() deliver it (Part B). No detached fire-and-forget
        // thread — endSession(); flush(); now gives a synchronous "delivered
        // when online" guarantee.
        if (has_session) {
            persist_pending_session_end(rec);
            flush_cv_.notify_one();
        }

    } catch (const std::exception& ex) {
        log(LogLevel::Warning, std::string("beacon: endSession() internal error: ") + ex.what());
    } catch (...) {
        log(LogLevel::Warning, "beacon: endSession() internal error (unknown).");
    }
}

// ---------- Account / License context API ----------

void Tracker::setAccount(std::string account_id) {
    if (disposed_.load() || opted_out_.load()) return;
    std::string validated;
    if (!validate_and_trim_context_id(account_id, "account_id", validated)) return;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        account_id_ = std::move(validated);
    }
    log(LogLevel::Debug, "beacon: setAccount applied.");
}

void Tracker::clearAccount() {
    if (disposed_.load()) return;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        account_id_.clear();
    }
    log(LogLevel::Debug, "beacon: clearAccount applied.");
}

void Tracker::setLicense(std::string license_id) {
    if (disposed_.load() || opted_out_.load()) return;
    std::string validated;
    if (!validate_and_trim_context_id(license_id, "license_id", validated)) return;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        license_id_ = std::move(validated);
    }
    log(LogLevel::Debug, "beacon: setLicense applied.");
}

void Tracker::clearLicense() {
    if (disposed_.load()) return;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        license_id_.clear();
    }
    log(LogLevel::Debug, "beacon: clearLicense applied.");
}

// ---------- Consent API (FR-1129, FR-1130) ----------

void Tracker::optOut() {
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

        // Clear active session + per-session snapshots, and purge any
        // persisted pending session-ends (do not deliver) — consent posture.
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_id_.clear();
            session_actor_id_.clear();
            session_account_id_.clear();
            session_license_id_.clear();
            session_started_at_.clear();
        }
        purge_pending_session_ends();

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
        log(LogLevel::Warning, "beacon: optOut() internal error.");
    }
}

void Tracker::optIn() {
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
        log(LogLevel::Warning, "beacon: optIn() internal error.");
    }
}

// ---------- Reset (FR-1131) ----------

void Tracker::reset() {
    try {
        // Operates regardless of enabled or opt-out state

        // Step 1: Clear active session + per-session snapshots.
        // reset() discards identity, so any active or pending session-end is
        // PURGED (not delivered) — consistent with the SDK's consent posture
        // (mirrors optOut). The durable end is intentionally NOT persisted.
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_id_.clear();
            session_actor_id_.clear();
            session_account_id_.clear();
            session_license_id_.clear();
            session_started_at_.clear();

            // Step 2: Clear actor ID + account/license context.
            actor_id_.clear();
            account_id_.clear();
            license_id_.clear();
        }

        // Purge any persisted pending session-ends (do not deliver).
        purge_pending_session_ends();

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
            internal::write_device_id(options_.product, new_device_id);
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
        std::string current_account;
        std::string current_license;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            current_session = session_id_;
            current_account = account_id_;
            current_license = license_id_;
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
        body["product"] = options_.product;
        body["product_version"] = options_.product_version;

        if (!message.empty()) {
            body["message"] = message;
        }

        if (!stack_trace.empty()) {
            body["stack_trace"] = stack_trace;
        }

        if (!current_session.empty()) {
            body["session_id"] = current_session;
        }

        if (!current_account.empty()) {
            body["account_id"] = current_account;
        }

        if (!current_license.empty()) {
            body["license_id"] = current_license;
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
    manifest["product"] = options_.product;
    manifest["product_version"] = options_.product_version;

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

std::string Tracker::actorId() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return actor_id_;
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

bool Tracker::validate_and_trim_context_id(const std::string& input, const char* field_name,
                                           std::string& out) const {
    if (input.empty()) {
        log(LogLevel::Warning, std::string("beacon: ") + field_name +
            " must be a non-empty string -- ignored.");
        return false;
    }

    // Trim leading + trailing ASCII whitespace.
    auto begin = input.find_first_not_of(" \t\r\n\f\v");
    auto end = input.find_last_not_of(" \t\r\n\f\v");
    if (begin == std::string::npos) {
        log(LogLevel::Warning, std::string("beacon: ") + field_name +
            " cannot be whitespace-only -- ignored.");
        return false;
    }
    std::string trimmed = input.substr(begin, end - begin + 1);

    if (trimmed.size() > 256) {
        log(LogLevel::Warning, std::string("beacon: ") + field_name +
            " exceeds 256 characters -- ignored.");
        return false;
    }

    // Reject any control character. We iterate as bytes since these IDs are
    // ASCII-typical (subscription IDs, UUIDs, vendor strings). Code points
    // < 32 cover \r, \n, \t, \0, \f, \v, all C0 controls. U+2028 (E2 80 A8)
    // and U+2029 (E2 80 A9) are checked as their UTF-8 byte sequences.
    for (size_t i = 0; i < trimmed.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(trimmed[i]);
        if (c < 32) {
            log(LogLevel::Warning, std::string("beacon: ") + field_name +
                " contains a control character -- ignored.");
            return false;
        }
        // Detect U+2028 (E2 80 A8) and U+2029 (E2 80 A9) as 3-byte UTF-8 sequences.
        if (c == 0xE2 && i + 2 < trimmed.size() &&
            static_cast<unsigned char>(trimmed[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(trimmed[i + 2]) == 0xA8 ||
             static_cast<unsigned char>(trimmed[i + 2]) == 0xA9)) {
            log(LogLevel::Warning, std::string("beacon: ") + field_name +
                " contains a line-separator (U+2028/U+2029) -- ignored.");
            return false;
        }
    }

    out = std::move(trimmed);
    return true;
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
        std::string safe_name = internal::sanitize_path_component(options_.product);

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

        // Sibling table for durable pending session-ends. SEPARATE from the
        // homogeneous event queue (queued_events) — session-lifecycle records
        // are delivered to /v1/events/sessions/end, not /v1/events, so they
        // must not be mixed into the event batch. A queue keyed by session_id:
        // one offline run can produce multiple unsent ends (startSession ends
        // the prior session before starting a new one).
        const char* create_pending_sql =
            "CREATE TABLE IF NOT EXISTS pending_session_ends ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    session_id TEXT NOT NULL,"
            "    actor_id TEXT NOT NULL,"
            "    source_app TEXT NOT NULL,"
            "    product_version TEXT NOT NULL,"
            "    started_at TEXT NOT NULL,"
            "    account_id TEXT,"
            "    license_id TEXT,"
            "    ended_at TEXT NOT NULL,"
            "    end_reason TEXT NOT NULL"
            ");";

        char* err_msg2 = nullptr;
        rc = sqlite3_exec(db_, create_pending_sql, nullptr, nullptr, &err_msg2);
        if (rc != SQLITE_OK) {
            log(LogLevel::Warning,
                std::string("beacon: failed to create pending_session_ends table: ") +
                (err_msg2 ? err_msg2 : "unknown error"));
            if (err_msg2) sqlite3_free(err_msg2);
        }

        // Default rollback journal (NOT WAL): keeps the main .db reflecting the queue
        // size for the max_queue_size_mb cap, and WAL buys nothing for this single-
        // connection, write-on-failure-only queue. Explicit DELETE also migrates any
        // WAL database from an earlier SDK version back to rollback-journal mode.
        sqlite3_exec(db_, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr);

        // Wait (up to 5s) for the lock instead of erroring with SQLITE_BUSY when another
        // instance sharing the same Product holds it — THIS is what lets multiple instances
        // of the same app coexist on one queue file (graceful serialization).
        sqlite3_busy_timeout(db_, 5000);

    } catch (const std::exception& ex) {
        log(LogLevel::Warning,
            std::string("beacon: disk queue init failed: ") + ex.what());
    }
}

void Tracker::enqueue_to_disk(const std::vector<std::string>& events) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
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
    std::lock_guard<std::mutex> db_lock(db_mutex_);
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
    std::lock_guard<std::mutex> db_lock(db_mutex_);
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
    std::lock_guard<std::mutex> db_lock(db_mutex_);
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

// ---------- Pending session-end (durable session-end) helpers ----------

bool Tracker::build_pending_end_from_snapshot(const std::string& ended_at,
                                              const std::string& end_reason,
                                              internal::PendingSessionEnd& out) const {
    // Caller must hold session_mutex_.
    if (session_id_.empty()) return false;

    out.id = 0;
    out.session_id = session_id_;
    // Read from SNAPSHOTS, never from the live actor_id_/account_id_/license_id_
    // (the .NET port lesson). Fall back to the live actor only if the snapshot
    // is somehow empty (defensive — should not happen for an active session).
    out.actor_id = !session_actor_id_.empty() ? session_actor_id_ : actor_id_;
    out.source_app = options_.product;
    out.product_version = options_.product_version;
    out.started_at = session_started_at_;
    out.account_id = session_account_id_;
    out.license_id = session_license_id_;
    out.ended_at = ended_at;
    out.end_reason = end_reason;
    return true;
}

int64_t Tracker::persist_pending_session_end(const internal::PendingSessionEnd& rec) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!db_ || rec.session_id.empty()) return 0;

    try {
        const char* insert_sql =
            "INSERT INTO pending_session_ends "
            "(session_id, actor_id, source_app, product_version, started_at, "
            " account_id, license_id, ended_at, end_reason) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return 0;
        }

        sqlite3_bind_text(stmt, 1, rec.session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, rec.actor_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, rec.source_app.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, rec.product_version.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, rec.started_at.c_str(), -1, SQLITE_TRANSIENT);
        // account_id / license_id: NULL when absent so the recovery payload
        // can distinguish "absent" from "present but empty".
        if (rec.account_id.empty()) {
            sqlite3_bind_null(stmt, 6);
        } else {
            sqlite3_bind_text(stmt, 6, rec.account_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (rec.license_id.empty()) {
            sqlite3_bind_null(stmt, 7);
        } else {
            sqlite3_bind_text(stmt, 7, rec.license_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_text(stmt, 8, rec.ended_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, rec.end_reason.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) return 0;
        return sqlite3_last_insert_rowid(db_);
    } catch (...) {
        return 0;
    }
}

std::vector<internal::PendingSessionEnd> Tracker::dequeue_pending_session_ends(int limit) {
    std::vector<internal::PendingSessionEnd> result;
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!db_) return result;

    const char* select_sql =
        "SELECT id, session_id, actor_id, source_app, product_version, "
        "       started_at, account_id, license_id, ended_at, end_reason "
        "FROM pending_session_ends ORDER BY id ASC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        internal::PendingSessionEnd rec;
        rec.id = sqlite3_column_int64(stmt, 0);

        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            return t ? std::string(t) : std::string();
        };

        rec.session_id = col(1);
        rec.actor_id = col(2);
        rec.source_app = col(3);
        rec.product_version = col(4);
        rec.started_at = col(5);
        rec.account_id = col(6); // empty if SQL NULL
        rec.license_id = col(7);
        rec.ended_at = col(8);
        rec.end_reason = col(9);
        result.push_back(std::move(rec));
    }

    sqlite3_finalize(stmt);
    return result;
}

void Tracker::delete_pending_session_end(int64_t id) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!db_) return;

    const char* delete_sql = "DELETE FROM pending_session_ends WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Tracker::purge_pending_session_ends() {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!db_) return;
    sqlite3_exec(db_, "DELETE FROM pending_session_ends;", nullptr, nullptr, nullptr);
}

void Tracker::mark_pending_session_ends_as_recovery() {
    // Called once at construction: every record that survived a prior process
    // is a recovery delivery. Records added during THIS run keep "normal".
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!db_) return;
    sqlite3_exec(db_,
        "UPDATE pending_session_ends SET end_reason = 'sdk_recovery';",
        nullptr, nullptr, nullptr);
}

std::string Tracker::build_session_end_payload(const internal::PendingSessionEnd& rec,
                                               bool recovery) const {
    nlohmann::json body;
    body["session_id"] = rec.session_id;
    body["ended_at"] = rec.ended_at;
    body["end_reason"] = rec.end_reason;

    if (recovery) {
        // Self-contained recovery payload — matches the server's
        // create-on-recovery reader and the session-START field names
        // (snake_case, omit-when-null).
        body["actor_id"] = rec.actor_id;
        body["product"] = rec.source_app;
        body["product_version"] = rec.product_version;
        body["started_at"] = rec.started_at;
        if (!rec.account_id.empty()) body["account_id"] = rec.account_id;
        if (!rec.license_id.empty()) body["license_id"] = rec.license_id;
    }
    // Live "normal" ends stay minimal (session_id/ended_at/end_reason).

    return body.dump();
}

bool Tracker::deliver_session_end(internal::HttpClient& http,
                                  const internal::PendingSessionEnd& rec,
                                  bool recovery,
                                  long timeout_seconds,
                                  bool& retain_as_recovery) {
    retain_as_recovery = false;

    std::string url = options_.api_base_url + "/v1/events/sessions/end";
    std::string payload = build_session_end_payload(rec, recovery);

    internal::HttpResult result =
        http.post_json(url, options_.api_key, payload, "", timeout_seconds);

    if (result.success) {
        return true; // delivered → delete
    }

    // Network / transport error: retain for the next attempt.
    if (result.is_network_error) {
        return false;
    }

    // Structured session_not_found (server PRD wire contract). A 404 that
    // carries this body means the start hasn't landed yet (same-run race) or
    // the session never existed server-side. It is NON-TERMINAL:
    //   - On a recovery delivery, the server's create-on-recovery owns it, so
    //     a session_not_found here means we should DROP it locally (it has been
    //     handed off / can't be resolved by re-sending the same recovery).
    //   - On a live "normal" delivery, RETAIN and re-deliver as sdk_recovery.
    // Scope strictly to status 404 WITH a session_not_found body — a bare 404
    // from a wrong api_base_url / missing route / un-upgraded server must NOT
    // be retained forever; treat those as ordinary permanent failures (drop).
    if (result.status_code == 404) {
        bool is_session_not_found = false;
        try {
            if (!result.body.empty()) {
                auto j = nlohmann::json::parse(result.body, nullptr, false);
                if (!j.is_discarded() && j.is_object()) {
                    // Accept either {"error":"session_not_found"} or
                    // {"error":{"code":"session_not_found"}} shapes.
                    if (j.contains("error")) {
                        const auto& err = j["error"];
                        if (err.is_string()) {
                            is_session_not_found = (err.get<std::string>() == "session_not_found");
                        } else if (err.is_object() && err.contains("code") &&
                                   err["code"].is_string()) {
                            is_session_not_found =
                                (err["code"].get<std::string>() == "session_not_found");
                        }
                    }
                    if (!is_session_not_found && j.contains("code") &&
                        j["code"].is_string()) {
                        is_session_not_found =
                            (j["code"].get<std::string>() == "session_not_found");
                    }
                }
            }
        } catch (...) {
            is_session_not_found = false;
        }

        if (is_session_not_found) {
            if (recovery) {
                // create-on-recovery owns it — drop locally.
                return true;
            }
            // Live end raced the start → retain + re-deliver as recovery.
            retain_as_recovery = true;
            return false;
        }
        // Bare 404 (wrong URL / missing route / old server): permanent, drop.
        log(LogLevel::Warning,
            "beacon: session-end POST returned 404 without session_not_found "
            "(check api_base_url / server version). Record discarded.");
        return true;
    }

    // 401: API key rejected — permanent for this credential. Drop (the event
    // path halts on 401; for a single session-end we discard rather than loop).
    if (result.status_code == 401) {
        log(LogLevel::Error,
            "beacon: API key rejected (401) on session-end. Record discarded.");
        return true;
    }

    // 402 (cap) and other retryable codes: retain for a later attempt.
    if (result.status_code == 402 || internal::RetryPolicy::is_retryable(result)) {
        return false;
    }

    // Any other permanent 4xx: drop.
    if (internal::RetryPolicy::is_permanent_failure(result)) {
        log(LogLevel::Warning,
            "beacon: permanent HTTP error " + std::to_string(result.status_code) +
            " on session-end. Record discarded.");
        return true;
    }

    // Unknown: retain conservatively.
    return false;
}

void Tracker::drain_pending_session_ends() {
    if (!db_ || halted_.load()) return;
    if (opted_out_.load()) return;
    if (!flush_http_ || !flush_http_->is_initialized()) return;

    // Deliver oldest-first. Each record carries its own end_reason: records
    // that survived a prior process were rewritten to "sdk_recovery" at
    // construction; records enqueued this run carry "normal".
    auto pending = dequeue_pending_session_ends(options_.max_batch_size);
    for (const auto& rec : pending) {
        if (shutdown_requested_.load() || halted_.load()) break;

        bool recovery = (rec.end_reason == "sdk_recovery");
        bool retain_as_recovery = false;
        bool should_delete = deliver_session_end(
            *flush_http_, rec, recovery,
            kSessionEndDeliveryTimeoutSeconds, retain_as_recovery);

        if (should_delete) {
            delete_pending_session_end(rec.id);
        } else if (retain_as_recovery) {
            // A live "normal" end raced the start → flip it to sdk_recovery in
            // the store so the next drain delivers the full self-contained
            // recovery payload that create-on-recovery resolves.
            std::lock_guard<std::mutex> db_lock(db_mutex_);
            if (db_) {
                sqlite3_stmt* stmt = nullptr;
                const char* upd =
                    "UPDATE pending_session_ends SET end_reason = 'sdk_recovery' "
                    "WHERE id = ?;";
                if (sqlite3_prepare_v2(db_, upd, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(stmt, 1, rec.id);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
        // else: retained as-is for a later attempt (network / 402 / retryable).
    }
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

        // Finally drain the pending session-end store (Part B). Doing this in
        // the flush thread loop covers BOTH the periodic async tick AND a
        // synchronous flush() (which wakes this loop), so
        // endSession(); flush(); delivers the end when online.
        drain_pending_session_ends();

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
