// Covers: FR-589 (Options defaults), FR-590 (option clamping), Data Model validation
//         for beacon::Options fields. EC-465 (invalid options disable SDK)
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <string>

// FR-589: Default-constructed Options have expected default values
TEST(OptionsTest, DefaultValues) {
    beacon::Options opts;
    EXPECT_EQ(opts.api_key, "");
    EXPECT_EQ(opts.api_base_url, "");
    EXPECT_EQ(opts.app_name, "");
    EXPECT_EQ(opts.app_version, "");
    EXPECT_EQ(opts.enabled, true);
    EXPECT_EQ(opts.flush_interval_seconds, 60);
    EXPECT_EQ(opts.max_batch_size, 25);
    EXPECT_EQ(opts.max_queue_size_mb, 10);
    EXPECT_EQ(opts.max_breadcrumbs, 25);
    EXPECT_EQ(opts.logger, nullptr);
}

class OptionsClampingTest : public ::testing::Test {
protected:
    void TearDown() override {
        beacon::Tracker::reset_for_testing();
    }
};

// FR-590: flush_interval_seconds clamped to [1, 3600]
TEST_F(OptionsClampingTest, FlushIntervalClampedToMin) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.flush_interval_seconds = 0;
    });
    EXPECT_EQ(tracker->options().flush_interval_seconds, 1);
}

TEST_F(OptionsClampingTest, FlushIntervalClampedToMax) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.flush_interval_seconds = 5000;
    });
    EXPECT_EQ(tracker->options().flush_interval_seconds, 3600);
}

// FR-590: max_batch_size clamped to [1, 1000]
TEST_F(OptionsClampingTest, MaxBatchSizeClampedToMin) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_batch_size = 0;
    });
    EXPECT_EQ(tracker->options().max_batch_size, 1);
}

TEST_F(OptionsClampingTest, MaxBatchSizeClampedToMax) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_batch_size = 2000;
    });
    EXPECT_EQ(tracker->options().max_batch_size, 1000);
}

// FR-590: max_queue_size_mb clamped to [1, 1000]
TEST_F(OptionsClampingTest, MaxQueueSizeClampedToMin) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_queue_size_mb = -1;
    });
    EXPECT_EQ(tracker->options().max_queue_size_mb, 1);
}

TEST_F(OptionsClampingTest, MaxQueueSizeClampedToMax) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_queue_size_mb = 2000;
    });
    EXPECT_EQ(tracker->options().max_queue_size_mb, 1000);
}

// FR-590: max_breadcrumbs clamped to [0, 200]
TEST_F(OptionsClampingTest, MaxBreadcrumbsClampedToMin) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_breadcrumbs = -5;
    });
    EXPECT_EQ(tracker->options().max_breadcrumbs, 0);
}

TEST_F(OptionsClampingTest, MaxBreadcrumbsClampedToMax) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_breadcrumbs = 500;
    });
    EXPECT_EQ(tracker->options().max_breadcrumbs, 200);
}

// FR-590: app_name truncated to 128 chars
TEST_F(OptionsClampingTest, AppNameTruncatedTo128) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = std::string(200, 'a');
        o.app_version = "1.0";
    });
    EXPECT_EQ(tracker->options().app_name.size(), 128u);
}

// FR-590: app_version truncated to 256 chars
TEST_F(OptionsClampingTest, AppVersionTruncatedTo256) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = std::string(300, 'v');
    });
    EXPECT_EQ(tracker->options().app_version.size(), 256u);
}

// FR-590: Trailing slash stripped from api_base_url
TEST_F(OptionsClampingTest, TrailingSlashStripped) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999///";
        o.app_name = "App";
        o.app_version = "1.0";
    });
    EXPECT_EQ(tracker->options().api_base_url, "http://localhost:9999");
}

// EC-465: Empty app_name disables SDK
TEST_F(OptionsClampingTest, EmptyAppNameDisablesSdk) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "";
        o.app_version = "1.0";
    });
    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::Disabled);
}

// EC-465: Empty app_version does NOT disable SDK (not a required field per PRD)
// Note: The PRD specifies api_key, api_base_url, and app_name as required.
// app_version is required — empty disables the SDK (parity with .NET SDK).
TEST_F(OptionsClampingTest, EmptyAppVersionDisablesSdk) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "";
    });
    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::Disabled);
}

// EC-465: http URL accepted
TEST_F(OptionsClampingTest, HttpUrlAccepted) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
    });
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::NotConnected);
}

// EC-465: https URL accepted
TEST_F(OptionsClampingTest, HttpsUrlAccepted) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "https://example.com";
        o.app_name = "App";
        o.app_version = "1.0";
    });
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::NotConnected);
}

// EC-465: ftp URL disables SDK
TEST_F(OptionsClampingTest, FtpUrlDisablesSdk) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "ftp://example.com";
        o.app_name = "App";
        o.app_version = "1.0";
    });
    ASSERT_NE(tracker, nullptr);
    EXPECT_EQ(tracker->last_flush_status(), beacon::FlushStatus::Disabled);
}

// Boundary: flush_interval_seconds exactly 1 (min) stays as 1
TEST_F(OptionsClampingTest, FlushIntervalExactlyAtMin) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.flush_interval_seconds = 1;
    });
    EXPECT_EQ(tracker->options().flush_interval_seconds, 1);
}

// Boundary: flush_interval_seconds exactly 3600 (max) stays as 3600
TEST_F(OptionsClampingTest, FlushIntervalExactlyAtMax) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.flush_interval_seconds = 3600;
    });
    EXPECT_EQ(tracker->options().flush_interval_seconds, 3600);
}

// Boundary: max_batch_size exactly 1 (min) stays as 1
TEST_F(OptionsClampingTest, MaxBatchSizeExactlyAtMin) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_batch_size = 1;
    });
    EXPECT_EQ(tracker->options().max_batch_size, 1);
}

// Boundary: max_breadcrumbs exactly 0 (disables breadcrumbs)
TEST_F(OptionsClampingTest, MaxBreadcrumbsExactlyZero) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "k";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "App";
        o.app_version = "1.0";
        o.max_breadcrumbs = 0;
    });
    EXPECT_EQ(tracker->options().max_breadcrumbs, 0);
}
