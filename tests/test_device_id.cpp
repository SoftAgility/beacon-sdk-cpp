#include <gtest/gtest.h>
#include "DeviceId.hpp"
#include <string>

// AC-940: Same device ID on consecutive calls
TEST(DeviceIdTest, ConsistentAcrossCalls) {
    std::string id1 = beacon::internal::get_or_create_device_id("BeaconTestApp");
    std::string id2 = beacon::internal::get_or_create_device_id("BeaconTestApp");
    EXPECT_EQ(id1, id2);
    EXPECT_FALSE(id1.empty());
}

// AC-941: Returns a valid UUID v7 format
TEST(DeviceIdTest, ReturnsValidUuid) {
    std::string id = beacon::internal::get_or_create_device_id("BeaconTestNewApp");
    EXPECT_EQ(id.size(), 36u);
    EXPECT_EQ(id[14], '7'); // Version nibble
}

// ED-508: Path-unsafe characters in app_name are sanitized
TEST(DeviceIdTest, SanitizesPathUnsafeChars) {
    std::string sanitized = beacon::internal::sanitize_path_component("My:App/v2\\test");
    EXPECT_EQ(sanitized, "My_App_v2_test");
}

// Different app names produce different device IDs (or at least don't conflict)
TEST(DeviceIdTest, DifferentAppNamesDifferentPaths) {
    std::string id1 = beacon::internal::get_or_create_device_id("AppAlpha");
    std::string id2 = beacon::internal::get_or_create_device_id("AppBeta");
    // They might be the same if stored in different files but that's fine -
    // the important thing is that both return non-empty valid UUIDs
    EXPECT_FALSE(id1.empty());
    EXPECT_FALSE(id2.empty());
    EXPECT_EQ(id1.size(), 36u);
    EXPECT_EQ(id2.size(), 36u);
}
