// Covers: AC-907 (startSession dispatches with correct fields), AC-908 (double startSession),
//         AC-909 (endSession noop), AC-910 (endSession dispatches), AC-911 (track includes session_id),
//         FR-596 (startSession with explicit actor), FR-597 (startSession with identified actor),
//         FR-598 (endSession), ED-502 (session started twice)
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

class SessionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "TestApp";
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600;
            o.max_batch_size = 1000;
        });
    }

    void TearDown() override {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    std::shared_ptr<beacon::Tracker> tracker_;
};

// AC-909: endSession() with no active session is a no-op
TEST_F(SessionsTest, EndSessionNoActiveSessionIsNoop) {
    // Should not throw and not dispatch any HTTP request
    EXPECT_NO_THROW(tracker_->endSession());
    // Session ID should still be empty
    EXPECT_TRUE(tracker_->session_id().empty());
}

// AC-907 / FR-596: startSession stores a valid UUID v7 session ID
TEST_F(SessionsTest, StartSessionSetsValidUuidSessionId) {
    tracker_->startSession("user-1");
    // Give background thread a moment to dispatch
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string sid = tracker_->session_id();
    EXPECT_FALSE(sid.empty());
    // UUID format check: 36 chars, version nibble '7' at position 14
    EXPECT_EQ(sid.size(), 36u);
    EXPECT_EQ(sid[14], '7');
}

// AC-907: startSession sets the actor
TEST_F(SessionsTest, StartSessionSetsActor) {
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // After startSession with explicit actor, track() without actor should work
    EXPECT_NO_THROW(tracker_->track("cat", "name"));
}

// AC-911: track after startSession includes session_id
TEST_F(SessionsTest, TrackIncludesSessionId) {
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string sid = tracker_->session_id();
    ASSERT_FALSE(sid.empty());

    tracker_->track("cat", "name", "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    EXPECT_TRUE(j.contains("session_id"));
    EXPECT_EQ(j["session_id"], sid);
}

// AC-911: track before startSession does not include session_id
TEST_F(SessionsTest, TrackBeforeSessionHasNoSessionId) {
    tracker_->track("cat", "name", "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    EXPECT_FALSE(j.contains("session_id"));
}

// FR-1133: startSession() without identify uses anonymous device ID fallback
TEST_F(SessionsTest, StartSessionWithoutIdentifyUsesDeviceId) {
    EXPECT_NO_THROW(tracker_->startSession());
    EXPECT_FALSE(tracker_->session_id().empty());
}

// FR-597: startSession() with identified actor
TEST_F(SessionsTest, StartSessionWithIdentifiedActor) {
    tracker_->identify("user-1");
    EXPECT_NO_THROW(tracker_->startSession());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_FALSE(tracker_->session_id().empty());
}

// AC-908 / ED-502: startSession() called twice ends first session and starts new one
TEST_F(SessionsTest, StartSessionTwiceEndsFirstAndStartsNew) {
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string first_sid = tracker_->session_id();
    ASSERT_FALSE(first_sid.empty());

    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string second_sid = tracker_->session_id();

    // New session should have a different ID
    EXPECT_NE(first_sid, second_sid);
    EXPECT_FALSE(second_sid.empty());
    // Both should be valid UUID v7 format
    EXPECT_EQ(first_sid.size(), 36u);
    EXPECT_EQ(second_sid.size(), 36u);
    EXPECT_EQ(first_sid[14], '7');
    EXPECT_EQ(second_sid[14], '7');
}

// AC-910: endSession clears session ID
TEST_F(SessionsTest, EndSessionClearsSessionId) {
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_FALSE(tracker_->session_id().empty());

    tracker_->endSession();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(tracker_->session_id().empty());
}

// AC-910: track after endSession does not include session_id
TEST_F(SessionsTest, TrackAfterEndSessionHasNoSessionId) {
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    tracker_->endSession();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    tracker_->track("cat", "name", "user-1");

    auto json_str = tracker_->last_enqueued_json();
    auto j = nlohmann::json::parse(json_str);

    EXPECT_FALSE(j.contains("session_id"));
}

// FR-596: startSession with empty actor throws
TEST_F(SessionsTest, StartSessionEmptyActorThrows) {
    EXPECT_THROW(tracker_->startSession(std::string("")), std::invalid_argument);
}

// FR-596: startSession with actor > 512 chars throws
TEST_F(SessionsTest, StartSessionLongActorThrows) {
    EXPECT_THROW(tracker_->startSession(std::string(513, 'x')), std::invalid_argument);
}

// AC-909: endSession called multiple times is safe
TEST_F(SessionsTest, EndSessionCalledMultipleTimesIsSafe) {
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_NO_THROW(tracker_->endSession());
    EXPECT_NO_THROW(tracker_->endSession());
    EXPECT_NO_THROW(tracker_->endSession());
}
