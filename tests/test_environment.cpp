#include <gtest/gtest.h>
#include "EnvironmentCollector.hpp"
#include "Base64.hpp"
#include <nlohmann/json.hpp>
#include <string>

// AC-925: Environment JSON contains expected fields
TEST(EnvironmentTest, CollectsExpectedFields) {
    std::string json_str = beacon::internal::collect_environment_json();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    EXPECT_TRUE(j.contains("os_name"));
    EXPECT_TRUE(j.contains("cpu_core_count"));
    EXPECT_TRUE(j.contains("machine_name_hash"));
    EXPECT_TRUE(j.contains("runtime_name"));
    EXPECT_EQ(j["runtime_name"], "C++ (Beacon SDK)");
}

// AC-926: machine_name_hash is 64-char lowercase hex
TEST(EnvironmentTest, MachineNameHashFormat) {
    std::string json_str = beacon::internal::collect_environment_json();
    auto j = nlohmann::json::parse(json_str);

    if (j.contains("machine_name_hash")) {
        std::string hash = j["machine_name_hash"];
        EXPECT_EQ(hash.size(), 64u);
        for (char c : hash) {
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                << "Invalid hex char: " << c;
        }
    }
}

// Base64 round-trip
TEST(EnvironmentTest, Base64RoundTrip) {
    std::string original = "Hello, World!";
    std::string encoded = beacon::internal::base64_encode(original);
    std::string decoded = beacon::internal::base64_decode(encoded);
    EXPECT_EQ(decoded, original);
}

// AC-924/925: Environment data can be base64 encoded and decoded back to valid JSON
TEST(EnvironmentTest, EnvironmentDataBase64RoundTrip) {
    std::string json_str = beacon::internal::collect_environment_json();
    std::string encoded = beacon::internal::base64_encode(json_str);
    EXPECT_FALSE(encoded.empty());

    std::string decoded = beacon::internal::base64_decode(encoded);
    EXPECT_EQ(decoded, json_str);

    auto j = nlohmann::json::parse(decoded);
    EXPECT_TRUE(j.contains("os_name"));
}

// SHA-256 output format
TEST(EnvironmentTest, Sha256HexFormat) {
    std::string hash = beacon::internal::sha256_hex("test");
    EXPECT_EQ(hash.size(), 64u);
    for (char c : hash) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

// RAM bucket classification
TEST(EnvironmentTest, RamBuckets) {
    EXPECT_EQ(beacon::internal::ram_bucket(1024), "< 2 GB");
    EXPECT_EQ(beacon::internal::ram_bucket(3000), "2-4 GB");
    EXPECT_EQ(beacon::internal::ram_bucket(6000), "4-8 GB");
    EXPECT_EQ(beacon::internal::ram_bucket(12000), "8-16 GB");
    EXPECT_EQ(beacon::internal::ram_bucket(24000), "16-32 GB");
    EXPECT_EQ(beacon::internal::ram_bucket(64000), "> 32 GB");
}
