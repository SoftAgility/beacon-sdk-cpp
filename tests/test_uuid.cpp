#include <gtest/gtest.h>
#include "UuidV7.hpp"
#include <set>
#include <string>

// AC-939: UUID v7 format validation
TEST(UuidV7Test, FormatIsCorrect) {
    std::string uuid = beacon::internal::new_uuid_v7();

    // Length: 36 (32 hex chars + 4 hyphens)
    EXPECT_EQ(uuid.size(), 36u);

    // Hyphens at correct positions
    EXPECT_EQ(uuid[8], '-');
    EXPECT_EQ(uuid[13], '-');
    EXPECT_EQ(uuid[18], '-');
    EXPECT_EQ(uuid[23], '-');

    // Version nibble at position 14 (0-indexed) should be '7'
    EXPECT_EQ(uuid[14], '7');

    // Variant bits: first hex char of 4th group should be 8, 9, a, or b
    char variant = uuid[19];
    EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b')
        << "Variant char was: " << variant;
}

// AC-939: 10000 unique UUIDs
TEST(UuidV7Test, TenThousandUniqueValues) {
    std::set<std::string> uuids;
    for (int i = 0; i < 10000; ++i) {
        uuids.insert(beacon::internal::new_uuid_v7());
    }
    EXPECT_EQ(uuids.size(), 10000u);
}

// UUID v7 is time-ordered (subsequent UUIDs have same or later timestamp prefix)
TEST(UuidV7Test, TimeOrdering) {
    std::string prev = beacon::internal::new_uuid_v7();
    for (int i = 0; i < 100; ++i) {
        std::string next = beacon::internal::new_uuid_v7();
        // First 8 hex chars represent the most significant timestamp bits
        // Subsequent UUIDs should be >= previous (time-ordered)
        EXPECT_GE(next.substr(0, 8), prev.substr(0, 8));
        prev = next;
    }
}

// All characters are valid hex or hyphens
TEST(UuidV7Test, AllCharsValid) {
    std::string uuid = beacon::internal::new_uuid_v7();
    for (size_t i = 0; i < uuid.size(); ++i) {
        char c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            EXPECT_EQ(c, '-');
        } else {
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                << "Invalid char at pos " << i << ": " << c;
        }
    }
}
