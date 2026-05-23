#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "beacon/EventDefinitionBuilder.hpp"
#include "beacon/ExceptionSeverity.hpp"
#include "beacon/Export.hpp"
#include "beacon/FlushStatus.hpp"
#include "beacon/ILogger.hpp"
#include "beacon/Options.hpp"

// Forward declarations for opaque C types
struct sqlite3;
namespace beacon { namespace internal { class HttpClient; } }

// Include internal types needed for private member declarations
#include "beacon/internal/Types.hpp"

namespace beacon {

class BEACON_API Tracker : public std::enable_shared_from_this<Tracker> {
public:
    // Singleton factory: configure with a lambda that populates Options.
    static std::shared_ptr<Tracker> configure(std::function<void(Options&)> configurator);

    // Singleton factory: configure with a pre-built Options struct.
    static std::shared_ptr<Tracker> configure(Options options);

    // Returns the singleton, or nullptr if not yet configured.
    static std::shared_ptr<Tracker> instance();

    // Resets the singleton (for testing only).
    static void reset_for_testing();

    ~Tracker();

    // Non-copyable, non-movable.
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;
    Tracker(Tracker&&) = delete;
    Tracker& operator=(Tracker&&) = delete;

    // Set the current actor ID. When the actor ID changes and a device ID is
    // available, fires a best-effort HTTP POST to /v1/actors/identify to link
    // the device ID (anonymous) to the identified actor ID. On 409 (already
    // linked to a different user), logs a warning. Re-identifying with the
    // same actor ID is a no-op (no POST fired).
    void identify(std::string actor_id);

    // Track an event using the identified actor.
    void track(std::string category, std::string name,
               std::optional<std::unordered_map<std::string, std::string>> properties = std::nullopt);

    // Track an event with an explicit actor ID.
    void track(std::string category, std::string name, std::string actor_id,
               std::optional<std::unordered_map<std::string, std::string>> properties = std::nullopt);

    // Start a session using the identified actor.
    void startSession();

    // Start a session with an explicit actor ID.
    void startSession(std::string actor_id);

    // End the current session.
    void endSession();

    /// Set the current account context. After this call, all subsequent events,
    /// sessions, and exception reports will include account_id until cleared or
    /// reset(). Use the customer's pseudonymous organization/account identifier;
    /// do not pass personally identifying strings like email addresses.
    ///
    /// account_id is silently ignored when:
    ///   - empty or whitespace-only
    ///   - longer than 256 characters
    ///   - contains control characters (< 32, U+2028, U+2029)
    /// An invalid input does NOT overwrite a previously valid value.
    void setAccount(std::string account_id);

    /// Clear the account context. Subsequent events emit with no account_id.
    void clearAccount();

    /// Set the current license context. After this call, all subsequent events,
    /// sessions, and exception reports will include license_id until cleared or
    /// reset().
    ///
    /// PREFER PER-CONTRACT IDs (a single string shared across all of a customer's
    /// users — e.g., a subscription ID, site key, or bundle SKU) over per-user
    /// IDs. Per-contract IDs give the richest Beacon analytics:
    ///   - License Detail page shows meaningful per-license usage
    ///   - "Seen under multiple accounts" governance warning (ED-1137) becomes
    ///     a useful signal
    /// Per-user license IDs work but reduce the License Detail page to a
    /// near-duplicate of the Actor Identities view and disable the multi-account
    /// sharing warning. See the Beacon docs section "Modeling licenses".
    ///
    /// license_id is silently ignored on the same validation rules as setAccount.
    void setLicense(std::string license_id);

    /// Clear the license context. Subsequent events emit with no license_id.
    void clearLicense();

    // Track an exception using the identified actor.
    void trackException(const std::exception& ex,
                        ExceptionSeverity severity = ExceptionSeverity::NonFatal);

    // Track an exception with an explicit actor ID.
    void trackException(const std::exception& ex, std::string actor_id,
                        ExceptionSeverity severity = ExceptionSeverity::NonFatal);

    // Synchronous flush - blocks until all events are sent or timeout (30s).
    bool flush();

    // Stop all tracking immediately, persist opt-out to disk, clear memory queue.
    // Idempotent. Never throws.
    void opt_out();

    // Resume tracking after a prior opt_out(). Deletes opt-out file from disk.
    // Idempotent. Never throws.
    void opt_in();

    // Clear actor identity, session state, queue, and breadcrumbs.
    // Generates a new anonymous device ID. Ends active session (fire-and-forget).
    // Operates regardless of enabled or opt-out state. Never throws.
    void reset();

    // Export event manifest to a JSON file.
    void exportEventManifest(std::string file_path);

    // Returns the most recent flush status.
    FlushStatus last_flush_status() const;

    // Access the internal memory queue size (for testing).
    size_t queue_size() const;

    // Access a copy of the most recently enqueued JSON (for testing).
    std::string last_enqueued_json() const;

    // Access a copy of the most recently built exception JSON (for testing).
    std::string last_exception_json() const;

    // Access the options (read-only, for testing).
    const Options& options() const;

    // Access the session ID (for testing).
    std::string session_id() const;

private:
    // Private constructor - use configure() factory.
    explicit Tracker(Options opts);

    void log(LogLevel level, const std::string& message) const;
    void validate_actor_id(const std::string& actor_id) const;

    // Validates an account_id or license_id against the ingest contract:
    // 1-256 chars after trim, no whitespace-only, no control chars.
    // Returns true (and writes the trimmed value into `out`) on success;
    // false (logs at Warning level) on failure. Matches the .NET SDK's
    // ValidateAndTrimContextId / JS SDK's validateContextId.
    bool validate_and_trim_context_id(const std::string& input, const char* field_name,
                                      std::string& out) const;

    void track_impl(std::string category, std::string name, std::string actor_id,
                    std::optional<std::unordered_map<std::string, std::string>> properties);

    void start_session_impl(std::string actor_id);

    void track_exception_impl(const std::exception& ex, std::string actor_id,
                              ExceptionSeverity severity);

    void flush_thread_loop();
    void drain_disk_queue();
    void drain_memory_queue();

    // Disk queue helpers
    void init_disk_queue();
    void enqueue_to_disk(const std::vector<std::string>& events);
    std::vector<internal::QueuedEvent> dequeue_from_disk(int limit);
    void delete_from_disk(const std::vector<int64_t>& ids);
    void enforce_disk_queue_size();
    int64_t get_disk_queue_file_size() const;

    // Breadcrumb helpers
    void add_breadcrumb(const std::string& category, const std::string& name,
                        const std::string& timestamp,
                        const std::unordered_map<std::string, std::string>& properties);
    std::vector<internal::BreadcrumbEntry> snapshot_breadcrumbs() const;

    // --- Member fields ---
    Options options_;
    std::atomic<bool> disposed_{false};
    std::atomic<bool> halted_{false};
    std::atomic<bool> opted_out_{false};
    std::atomic<FlushStatus> flush_status_{FlushStatus::NotConnected};

    // Actor & session state
    mutable std::mutex session_mutex_;
    std::string actor_id_;
    std::string session_id_;

    // Account / license context — set via setAccount/setLicense, cleared by reset().
    std::string account_id_;
    std::string license_id_;

    // Memory queue
    mutable std::mutex queue_mutex_;
    std::deque<std::string> memory_queue_;

    // Breadcrumb ring buffer
    mutable std::mutex breadcrumb_mutex_;
    std::deque<internal::BreadcrumbEntry> breadcrumbs_;

    // Flush thread
    std::thread flush_thread_;
    mutable std::mutex flush_cv_mutex_;
    std::condition_variable flush_cv_;
    std::atomic<bool> shutdown_requested_{false};

    // Flush synchronization for flush() method
    mutable std::mutex flush_sync_mutex_;
    std::condition_variable flush_done_cv_;
    std::atomic<bool> flush_requested_{false};
    std::atomic<bool> flush_completed_{false};

    // Flush-in-progress guard
    std::mutex flush_in_progress_mutex_;
    bool flush_in_progress_ = false;

    // Environment data
    std::string environment_data_base64_;
    std::atomic<bool> environment_sent_{false};

    // Device ID and data directory
    std::string device_id_;
    std::string data_directory_;

    // Event definitions (for manifest export)
    std::vector<std::pair<std::string, std::string>> event_definitions_;

    // Last exception JSON (for testing)
    std::string last_exception_json_;

    // SQLite disk queue
    sqlite3* db_ = nullptr;
    std::string db_path_;

    // HTTP client for flush thread (owned by flush thread lifetime)
    std::unique_ptr<internal::HttpClient> flush_http_;

    // Startup timing
    bool initial_flush_done_ = false;

    // Singleton
    static std::mutex singleton_mutex_;
    static std::shared_ptr<Tracker> singleton_;
    static bool configured_;
};

} // namespace beacon
