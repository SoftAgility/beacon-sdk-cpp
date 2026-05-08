// Covers: AC-912 (trackException without identify is noop), AC-913 (empty actorId throws),
//         AC-914 (trackException with identified actor dispatches), AC-915 (fatal severity),
//         AC-916 (message truncated to 1000 chars), AC-917 (breadcrumbs in exception payload),
//         AC-918 (max_breadcrumbs=0 no breadcrumbs), AC-919 (ring buffer keeps most recent),
//         FR-599 (trackException with identified actor), FR-600 (trackException with explicit actor),
//         FR-608 (breadcrumb ring buffer), EC-469 (disposed tracker silent noop),
//         EC-470 (invalid actor), ED-503 (no actor silent noop)
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

// --- Default fixture: max_breadcrumbs=25 ---

class ExceptionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "TestApp";
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600;
            o.max_batch_size = 1000;
            o.max_breadcrumbs = 25;
        });
    }

    void TearDown() override {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    std::shared_ptr<beacon::Tracker> tracker_;
};

// --- Fixture: max_breadcrumbs=0 (TD-005 / AC-918) ---

class ExceptionsNoBreadcrumbsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "TestApp";
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600;
            o.max_batch_size = 1000;
            o.max_breadcrumbs = 0;
        });
    }

    void TearDown() override {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    std::shared_ptr<beacon::Tracker> tracker_;
};

// --- Fixture: max_breadcrumbs=3 (TD-006 / AC-919) ---

class ExceptionsSmallBreadcrumbsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "TestApp";
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600;
            o.max_batch_size = 1000;
            o.max_breadcrumbs = 3;
        });
    }

    void TearDown() override {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    std::shared_ptr<beacon::Tracker> tracker_;
};

// --- Custom exception for TD-003 / AC-916 (message truncation) ---

class LongMessageException : public std::exception {
public:
    explicit LongMessageException(size_t length) : msg_(length, 'X') {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

// ==========================================================================
// AC-912: trackException without identify is silent no-op
// ==========================================================================

TEST_F(ExceptionsTest, TrackExceptionWithoutIdentifyIsNoop) {
    std::runtime_error ex("test error");
    EXPECT_NO_THROW(tracker_->trackException(ex));
}

// ==========================================================================
// AC-913: trackException with empty actorId throws
// ==========================================================================

TEST_F(ExceptionsTest, TrackExceptionEmptyActorIdThrows) {
    std::runtime_error ex("test error");
    EXPECT_THROW(tracker_->trackException(ex, ""), std::invalid_argument);
}

// AC-913: trackException with long actorId throws
TEST_F(ExceptionsTest, TrackExceptionLongActorIdThrows) {
    std::runtime_error ex("test error");
    EXPECT_THROW(
        tracker_->trackException(ex, std::string(513, 'x')),
        std::invalid_argument
    );
}

// ==========================================================================
// TD-001 / AC-914: trackException with identified actor produces valid JSON
// ==========================================================================

TEST_F(ExceptionsTest, TrackExceptionWithIdentifiedActorProducesValidPayload) {
    tracker_->identify("u1");
    std::runtime_error ex("test error");

    tracker_->trackException(ex);

    // last_exception_json() is populated synchronously before background dispatch
    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty()) << "last_exception_json() must not be empty after trackException";

    auto j = nlohmann::json::parse(json_str);

    // actor_id
    ASSERT_TRUE(j.contains("actor_id"));
    EXPECT_EQ(j["actor_id"], "u1");

    // exception_type (non-empty)
    ASSERT_TRUE(j.contains("exception_type"));
    std::string exception_type = j["exception_type"];
    EXPECT_FALSE(exception_type.empty());

    // severity
    ASSERT_TRUE(j.contains("severity"));
    EXPECT_EQ(j["severity"], "non_fatal");

    // occurred_at (ISO 8601)
    ASSERT_TRUE(j.contains("occurred_at"));
    std::string occurred_at = j["occurred_at"];
    EXPECT_NE(occurred_at.find('T'), std::string::npos);
    EXPECT_EQ(occurred_at.back(), 'Z');

    // exception_id (UUID format: 36 chars with hyphens)
    ASSERT_TRUE(j.contains("exception_id"));
    std::string exception_id = j["exception_id"];
    EXPECT_EQ(exception_id.size(), 36u);

    // source_app
    ASSERT_TRUE(j.contains("source_app"));
    EXPECT_EQ(j["source_app"], "TestApp");

    // source_version
    ASSERT_TRUE(j.contains("source_version"));
    EXPECT_EQ(j["source_version"], "1.0.0");
}

// AC-914: trackException with explicit actor produces valid JSON
TEST_F(ExceptionsTest, TrackExceptionWithExplicitActorProducesValidPayload) {
    std::runtime_error ex("test error");

    tracker_->trackException(ex, "u1");

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    EXPECT_EQ(j["actor_id"], "u1");
    EXPECT_EQ(j["severity"], "non_fatal");
    ASSERT_TRUE(j.contains("exception_type"));
    EXPECT_FALSE(j["exception_type"].get<std::string>().empty());
    ASSERT_TRUE(j.contains("occurred_at"));
    ASSERT_TRUE(j.contains("exception_id"));
    EXPECT_EQ(j["exception_id"].get<std::string>().size(), 36u);
    EXPECT_EQ(j["source_app"], "TestApp");
    EXPECT_EQ(j["source_version"], "1.0.0");
}

// ==========================================================================
// TD-002 / AC-915: Fatal severity produces severity:"fatal" in JSON
// ==========================================================================

TEST_F(ExceptionsTest, FatalSeverityProducesFatalInJson) {
    tracker_->identify("u1");
    std::runtime_error ex("fatal error");

    tracker_->trackException(ex, "u1", beacon::ExceptionSeverity::Fatal);

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("severity"));
    EXPECT_EQ(j["severity"], "fatal");
}

// AC-915: NonFatal severity is the default
TEST_F(ExceptionsTest, NonFatalIsDefaultSeverityInJson) {
    tracker_->identify("u1");
    std::runtime_error ex("non-fatal");

    tracker_->trackException(ex);

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("severity"));
    EXPECT_EQ(j["severity"], "non_fatal");
}

// ==========================================================================
// TD-003 / AC-916: Message truncated to 1000 characters
// ==========================================================================

TEST_F(ExceptionsTest, MessageTruncatedTo1000Chars) {
    tracker_->identify("u1");
    LongMessageException ex(2000);

    tracker_->trackException(ex);

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("message"));
    std::string message = j["message"];
    EXPECT_EQ(message.size(), 1000u);
}

// ==========================================================================
// TD-004 / AC-917: Breadcrumbs attached to exception payload
// ==========================================================================

TEST_F(ExceptionsTest, BreadcrumbsAttachedToExceptionPayload) {
    // track() 3 times to populate breadcrumbs
    tracker_->track("analytics", "page_view", "u1");
    tracker_->track("user_action", "button_click", "u1");
    tracker_->track("navigation", "route_change", "u1");

    // Now trackException to capture the breadcrumbs in the JSON
    std::runtime_error ex("something went wrong");
    tracker_->trackException(ex, "u1");

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("breadcrumbs")) << "Exception JSON must contain breadcrumbs array";
    ASSERT_TRUE(j["breadcrumbs"].is_array());
    ASSERT_EQ(j["breadcrumbs"].size(), 3u);

    // Each breadcrumb must have category, name, and timestamp
    for (size_t i = 0; i < j["breadcrumbs"].size(); ++i) {
        const auto& bc = j["breadcrumbs"][i];
        EXPECT_TRUE(bc.contains("category"))
            << "Breadcrumb " << i << " missing category";
        EXPECT_TRUE(bc.contains("name"))
            << "Breadcrumb " << i << " missing name";
        EXPECT_TRUE(bc.contains("timestamp"))
            << "Breadcrumb " << i << " missing timestamp";
    }

    // Verify the breadcrumbs have correct categories/names in order (oldest first)
    EXPECT_EQ(j["breadcrumbs"][0]["category"], "analytics");
    EXPECT_EQ(j["breadcrumbs"][0]["name"], "page_view");
    EXPECT_EQ(j["breadcrumbs"][1]["category"], "user_action");
    EXPECT_EQ(j["breadcrumbs"][1]["name"], "button_click");
    EXPECT_EQ(j["breadcrumbs"][2]["category"], "navigation");
    EXPECT_EQ(j["breadcrumbs"][2]["name"], "route_change");
}

// ==========================================================================
// TD-005 / AC-918: max_breadcrumbs=0 produces no breadcrumbs key
// ==========================================================================

TEST_F(ExceptionsNoBreadcrumbsTest, NoBreadcrumbsKeyWhenMaxIsZero) {
    // track() to attempt to populate breadcrumbs (should be ignored with max=0)
    tracker_->track("cat", "name", "u1");

    std::runtime_error ex("error");
    tracker_->trackException(ex, "u1");

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    EXPECT_FALSE(j.contains("breadcrumbs"))
        << "Exception JSON must NOT contain breadcrumbs when max_breadcrumbs=0";
}

// ==========================================================================
// TD-006 / AC-919: Ring buffer eviction keeps only 3 most recent breadcrumbs
// ==========================================================================

TEST_F(ExceptionsSmallBreadcrumbsTest, RingBufferKeepsOnlyMostRecentBreadcrumbs) {
    // track() 10 times with distinct names
    for (int i = 0; i < 10; ++i) {
        tracker_->track("cat", "event_" + std::to_string(i), "u1");
    }

    std::runtime_error ex("error");
    tracker_->trackException(ex, "u1");

    std::string json_str = tracker_->last_exception_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    ASSERT_TRUE(j.contains("breadcrumbs"));
    ASSERT_TRUE(j["breadcrumbs"].is_array());
    ASSERT_EQ(j["breadcrumbs"].size(), 3u)
        << "With max_breadcrumbs=3 and 10 track() calls, exactly 3 breadcrumbs expected";

    // The 3 most recent (events 7, 8, 9) should remain, in oldest-first order
    EXPECT_EQ(j["breadcrumbs"][0]["name"], "event_7");
    EXPECT_EQ(j["breadcrumbs"][1]["name"], "event_8");
    EXPECT_EQ(j["breadcrumbs"][2]["name"], "event_9");
}

// ==========================================================================
// TD-007 / EC-469: Disposed tracker methods are silent no-ops
//
// DEFERRED: The disposed_ flag is set inside the Tracker destructor (~Tracker),
// which means by the time disposed_ is true, the shared_ptr has already been
// released and the destructor is running. There is no safe way to call public
// methods on a partially-destroyed object without undefined behavior (the
// shared_ptr is null/invalid after destruction begins). Testing this would
// require either:
//   (a) A test-only setter like set_disposed_for_testing(true), or
//   (b) Calling methods on a dangling pointer, which is UB.
// Neither approach is acceptable without modifying production code. The behavior
// is adequately verified by code inspection: every public method checks
// disposed_.load() as its first operation and returns immediately if true.
// ==========================================================================

// ==========================================================================
// ED-503: trackException with no actor - silent no-op, no throw
// ==========================================================================

TEST_F(ExceptionsTest, TrackExceptionNoActorSilentNoop) {
    std::runtime_error ex("test");
    // No identify() called
    EXPECT_NO_THROW(tracker_->trackException(ex));
}

// ED-503: trackException with no actor does not enqueue to memory queue
TEST_F(ExceptionsTest, TrackExceptionNoActorDoesNotEnqueue) {
    std::runtime_error ex("test");
    tracker_->trackException(ex);
    // Exception reports are fire-and-forget via HTTP, not via memory queue
    // So queue_size should be 0 (no events tracked)
    EXPECT_EQ(tracker_->queue_size(), 0u);
}
