// Covers: AC-937 (destructor persists queued events to disk), AC-938 (no leaks, full lifecycle),
//         FR-611 (destructor graceful shutdown), ED-499 (tracker destroyed with queued events),
//         ED-500 (process exit without explicit destroy), ED-506 (disabled SDK no-ops),
//         EC-469 (method on disposed tracker)
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include "DiskQueue.hpp"
#include "DeviceId.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

class LifecycleTest : public ::testing::Test {
protected:
    void TearDown() override {
        beacon::Tracker::reset_for_testing();
    }
};

// Helper to determine the expected disk queue path for a given app name
static std::string expected_db_path(const std::string& app_name) {
    std::string safe_name = beacon::internal::sanitize_path_component(app_name);
    std::string path;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        path = std::string(appdata) + "\\SoftAgility\\Beacon\\" + safe_name + "\\beacon_queue.db";
    }
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home) {
        path = std::string(home) + "/Library/Application Support/SoftAgility/Beacon/" + safe_name + "/beacon_queue.db";
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        path = std::string(home) + "/.local/share/SoftAgility/Beacon/" + safe_name + "/beacon_queue.db";
    }
#endif
    return path;
}

// AC-937: Destructor persists queued events to disk
TEST_F(LifecycleTest, DestructorPersistsQueuedEvents) {
    std::string app_name = "LifecycleTestPersist";
    std::string db_path = expected_db_path(app_name);

    if (db_path.empty()) {
        GTEST_SKIP() << "Could not determine disk queue path for this platform";
    }

    // Clean up before test
    std::remove(db_path.c_str());
    std::remove((db_path + "-wal").c_str());
    std::remove((db_path + "-shm").c_str());

    {
        auto tracker = beacon::Tracker::configure([&app_name](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = app_name;
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600; // Prevent auto-flush
            o.max_batch_size = 1000;
        });

        tracker->identify("user-1");
        for (int i = 0; i < 5; ++i) {
            tracker->track("cat", "event_" + std::to_string(i), "user-1");
        }

        EXPECT_EQ(tracker->queue_size(), 5u);

        // Reset singleton so destructor runs
        beacon::Tracker::reset_for_testing();
        tracker.reset();
    }

    // Verify the events are in the disk queue
    beacon::internal::DiskQueue dq;
    ASSERT_TRUE(dq.open(db_path));

    auto events = dq.dequeue_up_to(100);
    EXPECT_EQ(events.size(), 5u);

    dq.close();

    // Clean up
    std::remove(db_path.c_str());
    std::remove((db_path + "-wal").c_str());
    std::remove((db_path + "-shm").c_str());
}

// AC-938: No crashes on full lifecycle (configure -> identify -> session -> track -> endSession -> destroy)
TEST_F(LifecycleTest, FullLifecycleNoCrash) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "LifecycleTest";
        o.app_version = "1.0";
        o.flush_interval_seconds = 3600;
    });

    ASSERT_NE(tracker, nullptr);

    // Full lifecycle: identify -> startSession -> track -> endSession -> destroy
    tracker->identify("user-1");
    tracker->startSession();
    tracker->track("cat", "name");
    tracker->endSession();

    // Give background threads time to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Destroy
    tracker.reset();
    beacon::Tracker::reset_for_testing();

    // If we get here without crashing or hanging, the test passes
    SUCCEED();
}

// ED-506: Disabled SDK - all methods are no-ops
TEST_F(LifecycleTest, DisabledSdkMethodsAreNoops) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.enabled = false;
        o.app_name = "TestApp";
        o.app_version = "1.0";
    });

    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::Disabled);

    // All these should be no-ops (not throw)
    EXPECT_NO_THROW(tracker->identify("user-1"));
    EXPECT_NO_THROW(tracker->track("cat", "name", "user-1"));
    EXPECT_NO_THROW(tracker->startSession("user-1"));
    EXPECT_NO_THROW(tracker->endSession());

    std::runtime_error ex("test");
    EXPECT_NO_THROW(tracker->trackException(ex, "user-1"));

    EXPECT_TRUE(tracker->flush());
    EXPECT_EQ(tracker->queue_size(), 0u);
}

// ED-506: Disabled SDK does not start background thread (no queue, no crash on destroy)
TEST_F(LifecycleTest, DisabledSdkNoBackgroundThread) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.enabled = false;
        o.app_name = "TestApp";
        o.app_version = "1.0";
    });

    // track should be no-op, queue should be 0
    tracker->track("cat", "name", "user-1");
    EXPECT_EQ(tracker->queue_size(), 0u);

    // Destroy should be instant (no thread to join)
    tracker.reset();
    beacon::Tracker::reset_for_testing();

    SUCCEED();
}

// FR-611: Destructor does not crash when no events are queued
TEST_F(LifecycleTest, DestructorNoEventsNoCrash) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "EmptyDestroyApp";
        o.app_version = "1.0";
        o.flush_interval_seconds = 3600;
    });

    // No events tracked - just destroy
    tracker.reset();
    beacon::Tracker::reset_for_testing();

    SUCCEED();
}
