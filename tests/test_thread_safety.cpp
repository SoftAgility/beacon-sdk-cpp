#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <thread>
#include <vector>

class ThreadSafetyTest : public ::testing::Test {
protected:
    void TearDown() override {
        beacon::Tracker::reset_for_testing();
    }
};

// AC-906: 10 threads x 1000 track() calls = 10000 items, no crashes
TEST_F(ThreadSafetyTest, ConcurrentTrackCalls) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "ThreadTestApp";
        o.app_version = "1.0.0";
        o.flush_interval_seconds = 3600; // Prevent auto-flush
        o.max_batch_size = 100000;       // Prevent batch-triggered flush
    });

    tracker->identify("user-1");

    constexpr int num_threads = 10;
    constexpr int calls_per_thread = 1000;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tracker, t, calls_per_thread]() {
            for (int i = 0; i < calls_per_thread; ++i) {
                tracker->track("thread_" + std::to_string(t),
                              "event_" + std::to_string(i),
                              "user-1");
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(tracker->queue_size(), static_cast<size_t>(num_threads * calls_per_thread));
}

// Concurrent identify and track - no deadlock
TEST_F(ThreadSafetyTest, ConcurrentIdentifyAndTrack) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "ThreadTestApp2";
        o.app_version = "1.0.0";
        o.flush_interval_seconds = 3600;
        o.max_batch_size = 100000;
    });

    tracker->identify("user-1");

    std::vector<std::thread> threads;

    // Thread that re-identifies
    threads.emplace_back([&tracker]() {
        for (int i = 0; i < 100; ++i) {
            tracker->identify("user-" + std::to_string(i));
        }
    });

    // Threads that track
    for (int t = 0; t < 5; ++t) {
        threads.emplace_back([&tracker]() {
            for (int i = 0; i < 200; ++i) {
                tracker->track("cat", "name", "user-1");
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // If we got here without deadlock or crash, the test passes
    EXPECT_GT(tracker->queue_size(), 0u);
}
