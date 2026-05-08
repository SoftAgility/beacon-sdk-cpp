// Covers: AC-917 (breadcrumbs in exception payload), AC-918 (max_breadcrumbs=0),
//         AC-919 (ring buffer eviction), FR-608 (breadcrumb ring buffer)
#include <gtest/gtest.h>
#include "BreadcrumbBuffer.hpp"
#include <string>
#include <unordered_map>
#include <vector>

// AC-919: Ring buffer keeps only the most recent entries when at capacity
TEST(BreadcrumbBufferTest, RingBufferEvictsOldestWhenFull) {
    beacon::internal::BreadcrumbBuffer buffer(3);
    std::unordered_map<std::string, std::string> empty_props;

    for (int i = 0; i < 10; ++i) {
        buffer.add("cat", "event_" + std::to_string(i),
                   "2026-03-13T00:00:0" + std::to_string(i) + ".000Z", empty_props);
    }

    EXPECT_EQ(buffer.size(), 3u);

    auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    // The 3 most recent events (7, 8, 9) should remain
    EXPECT_EQ(snap[0].name, "event_7");
    EXPECT_EQ(snap[1].name, "event_8");
    EXPECT_EQ(snap[2].name, "event_9");
}

// AC-918: max_breadcrumbs=0 means no breadcrumbs are ever stored
TEST(BreadcrumbBufferTest, ZeroCapacityStoresNothing) {
    beacon::internal::BreadcrumbBuffer buffer(0);
    std::unordered_map<std::string, std::string> empty_props;

    buffer.add("cat", "name", "2026-03-13T00:00:00.000Z", empty_props);
    buffer.add("cat", "name2", "2026-03-13T00:00:01.000Z", empty_props);

    EXPECT_EQ(buffer.size(), 0u);

    auto snap = buffer.snapshot();
    EXPECT_TRUE(snap.empty());
}

// AC-917: Breadcrumb entries contain category, name, timestamp, and properties
TEST(BreadcrumbBufferTest, EntriesContainExpectedFields) {
    beacon::internal::BreadcrumbBuffer buffer(5);
    std::unordered_map<std::string, std::string> props = {{"key1", "val1"}};

    buffer.add("analytics", "page_view", "2026-03-13T14:30:00.000Z", props);

    auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 1u);

    EXPECT_EQ(snap[0].category, "analytics");
    EXPECT_EQ(snap[0].name, "page_view");
    EXPECT_EQ(snap[0].timestamp, "2026-03-13T14:30:00.000Z");
    ASSERT_FALSE(snap[0].properties.empty());
    EXPECT_EQ(snap[0].properties.at("key1"), "val1");
}

// FR-608: Snapshot returns oldest-first ordering
TEST(BreadcrumbBufferTest, SnapshotReturnsOldestFirst) {
    beacon::internal::BreadcrumbBuffer buffer(10);
    std::unordered_map<std::string, std::string> empty_props;

    buffer.add("cat", "first", "2026-03-13T00:00:01.000Z", empty_props);
    buffer.add("cat", "second", "2026-03-13T00:00:02.000Z", empty_props);
    buffer.add("cat", "third", "2026-03-13T00:00:03.000Z", empty_props);

    auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0].name, "first");
    EXPECT_EQ(snap[1].name, "second");
    EXPECT_EQ(snap[2].name, "third");
}

// FR-608: Snapshot does not clear the buffer
TEST(BreadcrumbBufferTest, SnapshotDoesNotClearBuffer) {
    beacon::internal::BreadcrumbBuffer buffer(10);
    std::unordered_map<std::string, std::string> empty_props;

    buffer.add("cat", "name", "2026-03-13T00:00:00.000Z", empty_props);
    EXPECT_EQ(buffer.size(), 1u);

    auto snap1 = buffer.snapshot();
    EXPECT_EQ(snap1.size(), 1u);
    EXPECT_EQ(buffer.size(), 1u);

    auto snap2 = buffer.snapshot();
    EXPECT_EQ(snap2.size(), 1u);
}

// FR-608: Breadcrumb with empty properties stores empty map
TEST(BreadcrumbBufferTest, EmptyPropertiesStored) {
    beacon::internal::BreadcrumbBuffer buffer(5);
    std::unordered_map<std::string, std::string> empty_props;

    buffer.add("cat", "name", "2026-03-13T00:00:00.000Z", empty_props);

    auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_TRUE(snap[0].properties.empty());
}

// AC-917: Exactly 3 breadcrumbs after 3 track calls with max_breadcrumbs=5
TEST(BreadcrumbBufferTest, ExactCountWhenBelowCapacity) {
    beacon::internal::BreadcrumbBuffer buffer(5);
    std::unordered_map<std::string, std::string> empty_props;

    buffer.add("cat", "e1", "2026-03-13T00:00:01.000Z", empty_props);
    buffer.add("cat", "e2", "2026-03-13T00:00:02.000Z", empty_props);
    buffer.add("cat", "e3", "2026-03-13T00:00:03.000Z", empty_props);

    EXPECT_EQ(buffer.size(), 3u);

    auto snap = buffer.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0].name, "e1");
    EXPECT_EQ(snap[1].name, "e2");
    EXPECT_EQ(snap[2].name, "e3");
}
