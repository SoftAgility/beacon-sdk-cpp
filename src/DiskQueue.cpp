#include "DiskQueue.hpp"
#include "UuidV7.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

namespace beacon {
namespace internal {

namespace {

std::string utc_now_iso8601() {
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

std::string extract_event_id(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (j.contains("event_id") && j["event_id"].is_string()) {
            return j["event_id"].get<std::string>();
        }
    } catch (...) {}
    return new_uuid_v7();
}

} // anonymous namespace

DiskQueue::DiskQueue() = default;

DiskQueue::~DiskQueue() {
    close();
}

bool DiskQueue::open(const std::string& path) {
    path_ = path;

    int rc = sqlite3_open_v2(path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (rc != SQLITE_OK) {
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
        return false;
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
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_close_v2(db_);
        db_ = nullptr;
        return false;
    }

    // Enable WAL mode
    rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK && err_msg) {
        sqlite3_free(err_msg);
    }

    return true;
}

void DiskQueue::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

bool DiskQueue::is_open() const {
    return db_ != nullptr;
}

void DiskQueue::enqueue(const std::vector<std::string>& event_jsons) {
    if (!db_ || event_jsons.empty()) return;

    const char* insert_sql =
        "INSERT INTO queued_events (event_id, payload_json, queued_at, retry_count) "
        "VALUES (?, ?, ?, 0);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return;

    std::string now = utc_now_iso8601();

    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    for (const auto& json_str : event_jsons) {
        std::string event_id = extract_event_id(json_str);

        sqlite3_bind_text(stmt, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, json_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_finalize(stmt);
}

std::vector<QueuedEvent> DiskQueue::dequeue_up_to(int limit) {
    std::vector<QueuedEvent> result;
    if (!db_) return result;

    const char* select_sql =
        "SELECT id, event_id, payload_json, queued_at, retry_count "
        "FROM queued_events ORDER BY id ASC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QueuedEvent event;
        event.id = sqlite3_column_int64(stmt, 0);
        event.event_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        event.payload_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        event.queued_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        event.retry_count = sqlite3_column_int(stmt, 4);
        result.push_back(std::move(event));
    }

    sqlite3_finalize(stmt);
    return result;
}

void DiskQueue::delete_by_ids(const std::vector<int64_t>& ids) {
    if (!db_ || ids.empty()) return;

    // Build comma-separated list
    std::ostringstream oss;
    oss << "DELETE FROM queued_events WHERE id IN (";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) oss << ",";
        oss << ids[i];
    }
    oss << ");";

    sqlite3_exec(db_, oss.str().c_str(), nullptr, nullptr, nullptr);
}

bool DiskQueue::enforce_max_size(int64_t max_bytes) {
    if (!db_) return true;

    int64_t current_size = get_file_size_bytes();
    if (current_size < max_bytes) return true;

    // Delete oldest 100 events at a time
    while (current_size >= max_bytes) {
        const char* delete_sql =
            "DELETE FROM queued_events WHERE id IN "
            "(SELECT id FROM queued_events ORDER BY id ASC LIMIT 100);";

        int rc = sqlite3_exec(db_, delete_sql, nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) break;

        // Check if any rows were deleted
        int changes = sqlite3_changes(db_);
        if (changes == 0) break;

        // VACUUM to reclaim space
        sqlite3_exec(db_, "VACUUM;", nullptr, nullptr, nullptr);

        current_size = get_file_size_bytes();
    }

    return current_size < max_bytes;
}

int64_t DiskQueue::get_file_size_bytes() const {
    if (path_.empty()) return 0;

#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    if (GetFileAttributesExA(path_.c_str(), GetFileExInfoStandard, &attrs)) {
        LARGE_INTEGER size;
        size.HighPart = attrs.nFileSizeHigh;
        size.LowPart = attrs.nFileSizeLow;
        return size.QuadPart;
    }
    return 0;
#else
    struct stat st = {};
    if (stat(path_.c_str(), &st) == 0) {
        return st.st_size;
    }
    return 0;
#endif
}

} // namespace internal
} // namespace beacon
