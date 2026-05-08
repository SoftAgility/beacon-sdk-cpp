// Covers: FR-610 (event manifest export), Data Model validation for EventDefinitionBuilder
#include <gtest/gtest.h>
#include <beacon/EventDefinitionBuilder.hpp>
#include <stdexcept>
#include <string>
#include <vector>

// EventDefinitionBuilder: empty category throws std::invalid_argument
TEST(EventDefinitionBuilderTest, EmptyCategoryThrows) {
    beacon::EventDefinitionBuilder builder;
    EXPECT_THROW(builder.define("", "name"), std::invalid_argument);
}

// EventDefinitionBuilder: empty name throws std::invalid_argument
TEST(EventDefinitionBuilderTest, EmptyNameThrows) {
    beacon::EventDefinitionBuilder builder;
    EXPECT_THROW(builder.define("category", ""), std::invalid_argument);
}

// EventDefinitionBuilder: build returns sorted vector (by category then name)
TEST(EventDefinitionBuilderTest, BuildReturnsSortedEntries) {
    beacon::EventDefinitionBuilder builder;
    builder.define("inventory", "item_added")
           .define("app", "launched")
           .define("app", "closed")
           .define("inventory", "search_performed");

    auto result = builder.build();
    ASSERT_EQ(result.size(), 4u);

    EXPECT_EQ(result[0].first, "app");
    EXPECT_EQ(result[0].second, "closed");
    EXPECT_EQ(result[1].first, "app");
    EXPECT_EQ(result[1].second, "launched");
    EXPECT_EQ(result[2].first, "inventory");
    EXPECT_EQ(result[2].second, "item_added");
    EXPECT_EQ(result[3].first, "inventory");
    EXPECT_EQ(result[3].second, "search_performed");
}

// EventDefinitionBuilder: duplicates are deduplicated
TEST(EventDefinitionBuilderTest, DuplicatesAreDeduplicated) {
    beacon::EventDefinitionBuilder builder;
    builder.define("cat", "name")
           .define("cat", "name")
           .define("cat", "name");

    auto result = builder.build();
    EXPECT_EQ(result.size(), 1u);
}

// EventDefinitionBuilder: build on empty builder returns empty vector
TEST(EventDefinitionBuilderTest, EmptyBuilderReturnsEmpty) {
    beacon::EventDefinitionBuilder builder;
    auto result = builder.build();
    EXPECT_TRUE(result.empty());
}

// EventDefinitionBuilder: fluent interface returns reference
TEST(EventDefinitionBuilderTest, FluentInterface) {
    beacon::EventDefinitionBuilder builder;
    auto& ref = builder.define("cat1", "name1");
    EXPECT_EQ(&ref, &builder);

    auto& ref2 = ref.define("cat2", "name2");
    EXPECT_EQ(&ref2, &builder);
}

// EventDefinitionBuilder: build is const and can be called multiple times
TEST(EventDefinitionBuilderTest, BuildIsIdempotent) {
    beacon::EventDefinitionBuilder builder;
    builder.define("cat", "name");

    auto result1 = builder.build();
    auto result2 = builder.build();

    EXPECT_EQ(result1.size(), result2.size());
    EXPECT_EQ(result1[0], result2[0]);
}
