// Covers: FR-594 (property sanitization), AC-903 (properties in event JSON),
//         AC-905 (max 20 properties), Data Model validation for event properties
#include <gtest/gtest.h>
#include "PropertySanitizer.hpp"
#include <string>
#include <unordered_map>
#include <vector>

// FR-594: Keys truncated to 64 chars
TEST(PropertySanitizerTest, KeysTruncatedTo64Chars) {
    std::unordered_map<std::string, std::string> props;
    std::string long_key(100, 'k');
    props[long_key] = "value";

    auto result = beacon::internal::sanitize_properties(props);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].first.size(), 64u);
}

// FR-594: Values truncated to 256 chars
TEST(PropertySanitizerTest, ValuesTruncatedTo256Chars) {
    std::unordered_map<std::string, std::string> props;
    props["key"] = std::string(500, 'v');

    auto result = beacon::internal::sanitize_properties(props);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].second.size(), 256u);
}

// FR-594: Empty values are removed
TEST(PropertySanitizerTest, EmptyValuesRemoved) {
    std::unordered_map<std::string, std::string> props;
    props["key1"] = "value1";
    props["key2"] = "";
    props["key3"] = "value3";

    auto result = beacon::internal::sanitize_properties(props);

    // Should have 2 entries (key2 removed because empty value)
    EXPECT_EQ(result.size(), 2u);

    // Verify the empty-value entry is not present
    for (const auto& [k, v] : result) {
        EXPECT_NE(k, "key2");
        EXPECT_FALSE(v.empty());
    }
}

// AC-905: Max 20 properties kept
TEST(PropertySanitizerTest, MaxTwentyPropertiesKept) {
    std::unordered_map<std::string, std::string> props;
    for (int i = 0; i < 30; ++i) {
        props["key" + std::to_string(i)] = "val" + std::to_string(i);
    }

    auto result = beacon::internal::sanitize_properties(props);
    EXPECT_EQ(result.size(), 20u);
}

// Normal case: all properties valid, nothing truncated
TEST(PropertySanitizerTest, ValidPropertiesPassThrough) {
    std::unordered_map<std::string, std::string> props;
    props["key1"] = "value1";
    props["key2"] = "value2";

    auto result = beacon::internal::sanitize_properties(props);
    EXPECT_EQ(result.size(), 2u);
}

// Edge case: empty map
TEST(PropertySanitizerTest, EmptyMapReturnsEmpty) {
    std::unordered_map<std::string, std::string> props;

    auto result = beacon::internal::sanitize_properties(props);
    EXPECT_TRUE(result.empty());
}

// Boundary: exactly 20 properties all valid
TEST(PropertySanitizerTest, ExactlyTwentyPropertiesAllKept) {
    std::unordered_map<std::string, std::string> props;
    for (int i = 0; i < 20; ++i) {
        props["key" + std::to_string(i)] = "val" + std::to_string(i);
    }

    auto result = beacon::internal::sanitize_properties(props);
    EXPECT_EQ(result.size(), 20u);
}

// Boundary: key exactly 64 chars is not truncated
TEST(PropertySanitizerTest, KeyExactly64CharsNotTruncated) {
    std::unordered_map<std::string, std::string> props;
    std::string key64(64, 'k');
    props[key64] = "value";

    auto result = beacon::internal::sanitize_properties(props);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].first.size(), 64u);
    EXPECT_EQ(result[0].first, key64);
}

// Boundary: value exactly 256 chars is not truncated
TEST(PropertySanitizerTest, ValueExactly256CharsNotTruncated) {
    std::unordered_map<std::string, std::string> props;
    std::string val256(256, 'v');
    props["key"] = val256;

    auto result = beacon::internal::sanitize_properties(props);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].second.size(), 256u);
    EXPECT_EQ(result[0].second, val256);
}

// Boundary: key at 65 chars is truncated to 64
TEST(PropertySanitizerTest, KeyAt65CharsTruncated) {
    std::unordered_map<std::string, std::string> props;
    std::string key65(65, 'k');
    props[key65] = "value";

    auto result = beacon::internal::sanitize_properties(props);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].first.size(), 64u);
}

// Boundary: value at 257 chars is truncated to 256
TEST(PropertySanitizerTest, ValueAt257CharsTruncated) {
    std::unordered_map<std::string, std::string> props;
    props["key"] = std::string(257, 'v');

    auto result = beacon::internal::sanitize_properties(props);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].second.size(), 256u);
}
