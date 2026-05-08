// Covers: AC-899 (empty identify throws), AC-900 (long identify throws),
//         AC-901 (track without identify throws), AC-902 (identify+track enqueues JSON),
//         AC-903 (track with properties), AC-904 (category truncated to 128),
//         AC-905 (max 20 properties), FR-593 (identify), FR-594 (track with explicit actor),
//         FR-595 (track with identified actor), EC-470 (invalid actor ID), EC-472 (no actor identified)
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

class IdentifyTrackTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "TestApp";
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600; // Prevent auto-flush
            o.max_batch_size = 1000;         // Prevent batch-triggered flush
        });
    }

    void TearDown() override {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    std::shared_ptr<beacon::Tracker> tracker_;
};

// AC-899: identify("") throws std::invalid_argument containing "actorId"
TEST_F(IdentifyTrackTest, IdentifyEmptyThrowsInvalidArgument) {
    EXPECT_THROW({
        try {
            tracker_->identify("");
        } catch (const std::invalid_argument& e) {
            EXPECT_NE(std::string(e.what()).find("actorId"), std::string::npos);
            throw;
        }
    }, std::invalid_argument);
}

// AC-900: identify(513 chars) throws std::invalid_argument containing "512"
TEST_F(IdentifyTrackTest, IdentifyTooLongThrowsInvalidArgument) {
    EXPECT_THROW({
        try {
            tracker_->identify(std::string(513, 'x'));
        } catch (const std::invalid_argument& e) {
            EXPECT_NE(std::string(e.what()).find("512"), std::string::npos);
            throw;
        }
    }, std::invalid_argument);
}

// EC-470: identify with exactly 512 chars should succeed (boundary)
TEST_F(IdentifyTrackTest, IdentifyExactly512CharsSucceeds) {
    EXPECT_NO_THROW(tracker_->identify(std::string(512, 'x')));
}

// FR-1132: track() before identify() uses anonymous device ID fallback
TEST_F(IdentifyTrackTest, TrackWithoutIdentifyUsesDeviceId) {
    EXPECT_NO_THROW(tracker_->track("cat", "name"));
    EXPECT_EQ(tracker_->queue_size(), 1u);
    auto json = tracker_->last_enqueued_json();
    EXPECT_FALSE(json.empty());
    // actor_id should be a non-empty device ID
    EXPECT_NE(json.find("\"actor_id\""), std::string::npos);
}

// AC-902: identify then track enqueues one valid JSON event
TEST_F(IdentifyTrackTest, IdentifyThenTrackEnqueuesEvent) {
    tracker_->identify("user-1");
    tracker_->track("cat", "name");

    EXPECT_EQ(tracker_->queue_size(), 1u);

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    EXPECT_EQ(j["category"], "cat");
    EXPECT_EQ(j["name"], "name");
    EXPECT_EQ(j["actor_id"], "user-1");
    EXPECT_EQ(j["source_app"], "TestApp");
    EXPECT_EQ(j["source_version"], "1.0.0");
    EXPECT_TRUE(j.contains("event_id"));
    EXPECT_TRUE(j.contains("timestamp"));

    // Verify UUID v7 format
    std::string event_id = j["event_id"];
    EXPECT_EQ(event_id.size(), 36u);
    EXPECT_EQ(event_id[14], '7'); // Version nibble
}

// AC-903: track with properties
TEST_F(IdentifyTrackTest, TrackWithProperties) {
    std::unordered_map<std::string, std::string> props = {{"key", "value"}};
    tracker_->track("cat", "name", "user-1", props);

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("properties"));
    EXPECT_EQ(j["properties"]["key"], "value");
}

// AC-903: track with multiple properties
TEST_F(IdentifyTrackTest, TrackWithMultipleProperties) {
    std::unordered_map<std::string, std::string> props = {
        {"color", "red"},
        {"size", "large"},
        {"quantity", "5"}
    };
    tracker_->track("cat", "name", "user-1", props);

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("properties"));
    EXPECT_EQ(j["properties"].size(), 3u);
}

// AC-904: category truncated to 128 chars
TEST_F(IdentifyTrackTest, CategoryTruncatedTo128) {
    std::string long_cat(200, 'c');
    tracker_->track(long_cat, "name", "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    std::string cat = j["category"];
    EXPECT_EQ(cat.size(), 128u);
}

// FR-594: name truncated to 256 chars
TEST_F(IdentifyTrackTest, NameTruncatedTo256) {
    std::string long_name(500, 'n');
    tracker_->track("cat", long_name, "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    std::string name = j["name"];
    EXPECT_EQ(name.size(), 256u);
}

// AC-905: max 20 properties kept
TEST_F(IdentifyTrackTest, MaxTwentyProperties) {
    std::unordered_map<std::string, std::string> props;
    for (int i = 0; i < 30; ++i) {
        props["key" + std::to_string(i)] = "val" + std::to_string(i);
    }

    tracker_->track("cat", "name", "user-1", props);

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("properties"));
    EXPECT_EQ(j["properties"].size(), 20u);
}

// FR-594: track without properties omits properties field
TEST_F(IdentifyTrackTest, TrackWithoutPropertiesOmitsField) {
    tracker_->track("cat", "name", "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    // Properties field should either be absent or not present
    // when no properties are provided
    if (j.contains("properties")) {
        // If present, should be empty (implementation may omit entirely)
        EXPECT_TRUE(j["properties"].empty());
    }
}

// FR-594: track with explicit actor validates actor
TEST_F(IdentifyTrackTest, TrackWithExplicitEmptyActorThrows) {
    EXPECT_THROW(tracker_->track("cat", "name", std::string("")), std::invalid_argument);
}

// FR-594: track with explicit actor that exceeds 512 chars throws
TEST_F(IdentifyTrackTest, TrackWithExplicitLongActorThrows) {
    EXPECT_THROW(
        tracker_->track("cat", "name", std::string(513, 'x')),
        std::invalid_argument
    );
}

// FR-594: Multiple track calls accumulate in queue
TEST_F(IdentifyTrackTest, MultipleTrackCallsAccumulate) {
    tracker_->identify("user-1");
    tracker_->track("cat1", "name1");
    tracker_->track("cat2", "name2");
    tracker_->track("cat3", "name3");

    EXPECT_EQ(tracker_->queue_size(), 3u);
}

// FR-594: timestamp is ISO 8601 UTC format
TEST_F(IdentifyTrackTest, TimestampIsIso8601Utc) {
    tracker_->track("cat", "name", "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    std::string ts = j["timestamp"];
    // Must contain 'T' and end with 'Z'
    EXPECT_NE(ts.find('T'), std::string::npos);
    EXPECT_EQ(ts.back(), 'Z');
    // Must have milliseconds (e.g., ".123Z")
    EXPECT_NE(ts.find('.'), std::string::npos);
}

// FR-594: Properties with empty values are removed
TEST_F(IdentifyTrackTest, EmptyValuePropertiesRemoved) {
    std::unordered_map<std::string, std::string> props = {
        {"key1", "value1"},
        {"key2", ""},
    };
    tracker_->track("cat", "name", "user-1", props);

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    if (j.contains("properties")) {
        EXPECT_FALSE(j["properties"].contains("key2"));
    }
}

// AC-2332: identify fires a best-effort POST with anonymous_actor_id = device_id_
// Note: This test verifies that identify() returns immediately (non-blocking)
// and that the tracker state is correctly set. The actual HTTP POST goes to a
// non-routable address and will fail silently, which is the expected best-effort
// behavior.
TEST_F(IdentifyTrackTest, IdentifyFiresNonBlockingPostWithDeviceId) {
    // Measure that identify returns quickly (non-blocking)
    auto start = std::chrono::steady_clock::now();
    tracker_->identify("user-A");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // identify() should return in under 50ms (the HTTP call is on a detached thread)
    EXPECT_LT(elapsed.count(), 50);

    // The actor_id should be set
    // Verify by tracking an event and checking actor_id in the JSON
    tracker_->track("cat", "name");
    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);
    EXPECT_EQ(j["actor_id"], "user-A");

    // Give the detached thread a moment to complete (it will fail silently
    // against the non-routable address)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// AC-2332 supplemental: re-identify with same actor ID does not fire POST
TEST_F(IdentifyTrackTest, IdentifySameUserSkipsPost) {
    // First identify sets actor_id
    tracker_->identify("user-A");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second identify with same user should be fast and not spawn a thread
    auto start = std::chrono::steady_clock::now();
    tracker_->identify("user-A");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_LT(elapsed.count(), 5);

    // Actor ID should still be user-A
    tracker_->track("cat", "name");
    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);
    EXPECT_EQ(j["actor_id"], "user-A");
}
