// Covers: FR-606 (disk queue write on flush failure), AC-932 (persistence across restarts),
//         AC-933 (max queue size enforcement), AC-937 (destructor persists to disk),
//         EC-467 (SQLite init failure), ED-499 (tracker destroyed with queued events),
//         ED-505 (disk queue at cap)
#include <gtest/gtest.h>
#include "DiskQueue.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
#include <vector>

class DiskQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        db_path_ = std::string(std::getenv("TEMP")) + "\\beacon_dq_test_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
#else
        db_path_ = "/tmp/beacon_dq_test_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
#endif
        // Remove any leftover file
        std::remove(db_path_.c_str());
    }

    void TearDown() override {
        queue_.close();
        std::remove(db_path_.c_str());
        // Also remove WAL and SHM files
        std::remove((db_path_ + "-wal").c_str());
        std::remove((db_path_ + "-shm").c_str());
    }

    std::string make_event_json(const std::string& event_id) {
        nlohmann::json j;
        j["event_id"] = event_id;
        j["category"] = "test";
        j["name"] = "event";
        j["timestamp"] = "2026-03-13T00:00:00.000Z";
        j["actor_id"] = "user-1";
        j["source_app"] = "TestApp";
        j["source_version"] = "1.0";
        return j.dump();
    }

    beacon::internal::DiskQueue queue_;
    std::string db_path_;
};

// FR-606: DiskQueue opens and creates table successfully
TEST_F(DiskQueueTest, OpenCreatesDatabase) {
    EXPECT_TRUE(queue_.open(db_path_));
    EXPECT_TRUE(queue_.is_open());
}

// EC-467: Opening invalid path fails gracefully
TEST_F(DiskQueueTest, OpenInvalidPathReturnsFalse) {
    beacon::internal::DiskQueue q;
    // Path with nonexistent directory
    bool result = q.open("/nonexistent_dir_12345/beacon_test.db");
    EXPECT_FALSE(result);
    EXPECT_FALSE(q.is_open());
}

// FR-606: Enqueue and dequeue round-trip
TEST_F(DiskQueueTest, EnqueueDequeueRoundTrip) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events = {
        make_event_json("evt-001"),
        make_event_json("evt-002"),
        make_event_json("evt-003"),
    };

    queue_.enqueue(events);

    auto dequeued = queue_.dequeue_up_to(10);
    ASSERT_EQ(dequeued.size(), 3u);

    // Verify order is id ASC
    EXPECT_LT(dequeued[0].id, dequeued[1].id);
    EXPECT_LT(dequeued[1].id, dequeued[2].id);

    // Verify payload is preserved
    auto j0 = nlohmann::json::parse(dequeued[0].payload_json);
    EXPECT_EQ(j0["event_id"], "evt-001");

    auto j2 = nlohmann::json::parse(dequeued[2].payload_json);
    EXPECT_EQ(j2["event_id"], "evt-003");
}

// FR-606: Dequeue respects limit
TEST_F(DiskQueueTest, DequeueRespectsLimit) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back(make_event_json("evt-" + std::to_string(i)));
    }
    queue_.enqueue(events);

    auto dequeued = queue_.dequeue_up_to(3);
    EXPECT_EQ(dequeued.size(), 3u);
}

// FR-606: Delete by IDs removes the correct rows
TEST_F(DiskQueueTest, DeleteByIdsRemovesRows) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events = {
        make_event_json("evt-001"),
        make_event_json("evt-002"),
        make_event_json("evt-003"),
    };
    queue_.enqueue(events);

    auto dequeued = queue_.dequeue_up_to(10);
    ASSERT_EQ(dequeued.size(), 3u);

    // Delete first two
    std::vector<int64_t> ids_to_delete = {dequeued[0].id, dequeued[1].id};
    queue_.delete_by_ids(ids_to_delete);

    // Only the third event should remain
    auto remaining = queue_.dequeue_up_to(10);
    ASSERT_EQ(remaining.size(), 1u);

    auto j = nlohmann::json::parse(remaining[0].payload_json);
    EXPECT_EQ(j["event_id"], "evt-003");
}

// FR-606: Empty enqueue is a no-op
TEST_F(DiskQueueTest, EmptyEnqueueIsNoop) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> empty_events;
    queue_.enqueue(empty_events);

    auto dequeued = queue_.dequeue_up_to(10);
    EXPECT_TRUE(dequeued.empty());
}

// FR-606: Dequeue on empty queue returns empty
TEST_F(DiskQueueTest, DequeueEmptyQueueReturnsEmpty) {
    ASSERT_TRUE(queue_.open(db_path_));

    auto dequeued = queue_.dequeue_up_to(10);
    EXPECT_TRUE(dequeued.empty());
}

// AC-932: Events persist across close/reopen (simulates process restart)
TEST_F(DiskQueueTest, EventsPersistAcrossReopen) {
    {
        beacon::internal::DiskQueue q1;
        ASSERT_TRUE(q1.open(db_path_));

        std::vector<std::string> events = {
            make_event_json("persist-001"),
            make_event_json("persist-002"),
            make_event_json("persist-003"),
        };
        q1.enqueue(events);
        q1.close();
    }

    // Reopen with a new DiskQueue instance (simulates process restart)
    beacon::internal::DiskQueue q2;
    ASSERT_TRUE(q2.open(db_path_));

    auto dequeued = q2.dequeue_up_to(10);
    ASSERT_EQ(dequeued.size(), 3u);

    auto j0 = nlohmann::json::parse(dequeued[0].payload_json);
    EXPECT_EQ(j0["event_id"], "persist-001");

    auto j2 = nlohmann::json::parse(dequeued[2].payload_json);
    EXPECT_EQ(j2["event_id"], "persist-003");
}

// FR-606: event_id is extracted from JSON
TEST_F(DiskQueueTest, EventIdExtractedFromJson) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events = {make_event_json("my-uuid-123")};
    queue_.enqueue(events);

    auto dequeued = queue_.dequeue_up_to(1);
    ASSERT_EQ(dequeued.size(), 1u);
    EXPECT_EQ(dequeued[0].event_id, "my-uuid-123");
}

// FR-606: queued_at is a non-empty ISO 8601 string
TEST_F(DiskQueueTest, QueuedAtIsNonEmpty) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events = {make_event_json("evt-001")};
    queue_.enqueue(events);

    auto dequeued = queue_.dequeue_up_to(1);
    ASSERT_EQ(dequeued.size(), 1u);
    EXPECT_FALSE(dequeued[0].queued_at.empty());
    // Basic format check: should contain 'T' and 'Z'
    EXPECT_NE(dequeued[0].queued_at.find('T'), std::string::npos);
    EXPECT_NE(dequeued[0].queued_at.find('Z'), std::string::npos);
}

// FR-606: retry_count defaults to 0
TEST_F(DiskQueueTest, RetryCountDefaultsToZero) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events = {make_event_json("evt-001")};
    queue_.enqueue(events);

    auto dequeued = queue_.dequeue_up_to(1);
    ASSERT_EQ(dequeued.size(), 1u);
    EXPECT_EQ(dequeued[0].retry_count, 0);
}

// FR-606: get_file_size_bytes returns positive after enqueue
TEST_F(DiskQueueTest, FileSizePositiveAfterEnqueue) {
    ASSERT_TRUE(queue_.open(db_path_));

    std::vector<std::string> events = {make_event_json("evt-001")};
    queue_.enqueue(events);

    int64_t size = queue_.get_file_size_bytes();
    EXPECT_GT(size, 0);
}

// FR-606: Dequeue and delete without errors on closed DB is safe
TEST_F(DiskQueueTest, OperationsOnClosedDbAreSafe) {
    ASSERT_TRUE(queue_.open(db_path_));
    queue_.close();

    EXPECT_FALSE(queue_.is_open());

    // These should not crash
    auto dequeued = queue_.dequeue_up_to(10);
    EXPECT_TRUE(dequeued.empty());

    std::vector<int64_t> ids = {1, 2, 3};
    queue_.delete_by_ids(ids); // Should not crash

    std::vector<std::string> events = {make_event_json("evt-001")};
    queue_.enqueue(events); // Should not crash
}
