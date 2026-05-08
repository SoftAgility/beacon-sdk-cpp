// Covers: AC-895 (double configure throws), AC-896 (empty api_key disables + logs WARNING),
//         AC-897 (invalid URL disables), AC-898 (valid config = NotConnected),
//         FR-590 (option clamping), FR-591 (double config guard),
//         FR-592 (instance accessor), EC-465 (invalid options), EC-466 (double config)
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <stdexcept>
#include <string>
#include <vector>

// Test logger that records log messages for assertion
class TestLogger : public beacon::ILogger {
public:
    struct LogEntry {
        beacon::LogLevel level;
        std::string message;
    };

    void log(beacon::LogLevel level, const std::string& message) override {
        entries.push_back({level, message});
    }

    std::vector<LogEntry> entries;
};

class ConfigureTest : public ::testing::Test {
protected:
    void TearDown() override {
        beacon::Tracker::reset_for_testing();
    }
};

// AC-895: configure() called twice throws std::logic_error with "already configured"
TEST_F(ConfigureTest, DoubleConfigureThrowsLogicError) {
    beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "1.0";
    });

    EXPECT_THROW({
        try {
            beacon::Tracker::configure([](beacon::Options& o) {
                o.api_key = "another-key";
                o.api_base_url = "http://localhost:9999";
                o.app_name = "TestApp2";
                o.app_version = "1.0";
            });
        } catch (const std::logic_error& e) {
            EXPECT_NE(std::string(e.what()).find("already configured"), std::string::npos);
            throw;
        }
    }, std::logic_error);
}

// AC-895: Double configure with Options struct overload also throws
TEST_F(ConfigureTest, DoubleConfigureWithOptionsStructThrows) {
    beacon::Options opts;
    opts.api_key = "k";
    opts.api_base_url = "http://localhost:9999";
    opts.app_name = "App";
    opts.app_version = "1.0";
    beacon::Tracker::configure(opts);

    EXPECT_THROW(beacon::Tracker::configure(opts), std::logic_error);
}

// AC-896: Empty api_key returns non-null tracker, logs WARNING containing "api_key", status is Disabled
TEST_F(ConfigureTest, EmptyApiKeyDisablesSdkAndLogsWarning) {
    auto logger = std::make_shared<TestLogger>();

    auto tracker = beacon::Tracker::configure([&logger](beacon::Options& o) {
        o.api_key = "";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "1.0";
        o.logger = logger;
    });

    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::Disabled);

    // Verify WARNING was logged containing "api_key"
    bool found_warning = false;
    for (const auto& entry : logger->entries) {
        if (entry.level == beacon::LogLevel::Warning &&
            entry.message.find("api_key") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_warning) << "Expected WARNING log containing 'api_key'";
}

// AC-897: Invalid URL disables SDK
TEST_F(ConfigureTest, InvalidUrlDisablesSdk) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "not-a-url";
        o.app_name = "app";
        o.app_version = "1.0";
    });

    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::Disabled);
}

// AC-897: Invalid URL logs WARNING containing "api_base_url"
TEST_F(ConfigureTest, InvalidUrlLogsWarning) {
    auto logger = std::make_shared<TestLogger>();

    auto tracker = beacon::Tracker::configure([&logger](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "not-a-url";
        o.app_name = "app";
        o.app_version = "1.0";
        o.logger = logger;
    });

    bool found_warning = false;
    for (const auto& entry : logger->entries) {
        if (entry.level == beacon::LogLevel::Warning &&
            entry.message.find("api_base_url") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_warning) << "Expected WARNING log containing 'api_base_url'";
}

// AC-898: Valid config has NotConnected status
TEST_F(ConfigureTest, ValidConfigHasNotConnectedStatus) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "1.0";
    });

    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::NotConnected);
}

// FR-590: Options clamped to valid ranges
TEST_F(ConfigureTest, OptionsClampedToValidRanges) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "1.0";
        o.flush_interval_seconds = -5;
        o.max_batch_size = 5000;
        o.max_queue_size_mb = 0;
        o.max_breadcrumbs = 500;
    });

    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->options().flush_interval_seconds, 1);
    EXPECT_EQ(tracker->options().max_batch_size, 1000);
    EXPECT_EQ(tracker->options().max_queue_size_mb, 1);
    EXPECT_EQ(tracker->options().max_breadcrumbs, 200);
}

// FR-592: instance() returns nullptr before configure
TEST_F(ConfigureTest, InstanceReturnsNullptrBeforeConfigure) {
    EXPECT_EQ(beacon::Tracker::instance(), nullptr);
}

// FR-592: instance() returns the singleton after configure
TEST_F(ConfigureTest, InstanceReturnsSingletonAfterConfigure) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "1.0";
    });

    auto instance = beacon::Tracker::instance();
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(tracker.get(), instance.get());
}

// FR-590: configure() with Options struct (not lambda) works
TEST_F(ConfigureTest, ConfigureWithOptionsStruct) {
    beacon::Options opts;
    opts.api_key = "test-key";
    opts.api_base_url = "http://localhost:9999";
    opts.app_name = "TestApp";
    opts.app_version = "1.0";

    auto tracker = beacon::Tracker::configure(std::move(opts));
    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::NotConnected);
}

// EC-466: After double configure, existing singleton is not modified
TEST_F(ConfigureTest, DoubleConfigureDoesNotModifySingleton) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "original-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "OriginalApp";
        o.app_version = "1.0";
    });

    try {
        beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "new-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "NewApp";
            o.app_version = "2.0";
        });
    } catch (const std::logic_error&) {
        // Expected
    }

    // Original singleton should be unchanged
    auto instance = beacon::Tracker::instance();
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->options().app_name, "OriginalApp");
}
