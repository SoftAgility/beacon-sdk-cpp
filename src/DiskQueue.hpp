#pragma once

#include "beacon/internal/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace beacon {
namespace internal {

class DiskQueue {
public:
    DiskQueue();
    ~DiskQueue();

    // Opens the SQLite database at the given path. Returns true on success.
    bool open(const std::string& path);

    // Closes the database connection.
    void close();

    // Returns true if the database is open and usable.
    bool is_open() const;

    // Enqueues events into the disk queue.
    void enqueue(const std::vector<std::string>& event_jsons);

    // Dequeues up to `limit` events from the disk queue, ordered by id ASC.
    std::vector<QueuedEvent> dequeue_up_to(int limit);

    // Deletes events by their row IDs.
    void delete_by_ids(const std::vector<int64_t>& ids);

    // Enforces the maximum queue size in bytes. Deletes oldest events in
    // batches of 100 until below the cap, with VACUUM after each batch.
    // Returns true if below cap after enforcement.
    bool enforce_max_size(int64_t max_bytes);

    // Returns the file size of the SQLite database in bytes.
    int64_t get_file_size_bytes() const;

    // Returns the database file path.
    const std::string& path() const { return path_; }

private:
    sqlite3* db_ = nullptr;
    std::string path_;
};

} // namespace internal
} // namespace beacon
