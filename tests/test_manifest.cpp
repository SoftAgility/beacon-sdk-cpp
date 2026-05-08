#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

class ManifestTest : public ::testing::Test {
protected:
    void TearDown() override {
        beacon::Tracker::reset_for_testing();
        // Clean up temp file
        if (!temp_path_.empty()) {
            std::remove(temp_path_.c_str());
        }
    }

    std::string temp_path_;
};

// AC-934: exportEventManifest writes valid JSON with expected structure
TEST_F(ManifestTest, ExportManifestWritesValidJson) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "2.0";
        o.events
            .define("app", "launched")
            .define("inventory", "item_added")
            .define("app", "closed");
    });

#ifdef _WIN32
    temp_path_ = std::string(std::getenv("TEMP")) + "\\beacon_test_manifest.json";
#else
    temp_path_ = "/tmp/beacon_test_manifest.json";
#endif

    EXPECT_NO_THROW(tracker->exportEventManifest(temp_path_));

    std::ifstream ifs(temp_path_);
    ASSERT_TRUE(ifs.is_open());

    auto j = nlohmann::json::parse(ifs);
    EXPECT_EQ(j["schema_version"], "1");
    EXPECT_EQ(j["source_app"], "TestApp");
    EXPECT_EQ(j["source_version"], "2.0");
    EXPECT_TRUE(j.contains("generated_at"));
    ASSERT_TRUE(j.contains("entries"));

    auto& entries = j["entries"];
    EXPECT_EQ(entries.size(), 3u);

    // Entries should be sorted by category then name
    EXPECT_EQ(entries[0]["category"], "app");
    EXPECT_EQ(entries[0]["name"], "closed");
    EXPECT_EQ(entries[1]["category"], "app");
    EXPECT_EQ(entries[1]["name"], "launched");
    EXPECT_EQ(entries[2]["category"], "inventory");
    EXPECT_EQ(entries[2]["name"], "item_added");
}

// AC-935: exportEventManifest works on disabled tracker
TEST_F(ManifestTest, ExportManifestWorksWhenDisabled) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.enabled = false;
        o.app_name = "TestApp";
        o.app_version = "1.0";
        o.events.define("cat", "name");
    });

#ifdef _WIN32
    temp_path_ = std::string(std::getenv("TEMP")) + "\\beacon_test_manifest_disabled.json";
#else
    temp_path_ = "/tmp/beacon_test_manifest_disabled.json";
#endif

    EXPECT_NO_THROW(tracker->exportEventManifest(temp_path_));

    std::ifstream ifs(temp_path_);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    EXPECT_EQ(j["entries"].size(), 1u);
}

// AC-936: exportEventManifest to invalid path throws runtime_error
TEST_F(ManifestTest, ExportManifestInvalidPathThrows) {
    auto tracker = beacon::Tracker::configure([](beacon::Options& o) {
        o.api_key = "test-key";
        o.api_base_url = "http://localhost:9999";
        o.app_name = "TestApp";
        o.app_version = "1.0";
    });

    EXPECT_THROW({
        try {
            tracker->exportEventManifest("/nonexistent/path/manifest.json");
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("/nonexistent/path/manifest.json"),
                       std::string::npos);
            throw;
        }
    }, std::runtime_error);
}
