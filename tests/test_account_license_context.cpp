// Slice 3c — account & license context API parity with .NET (Slice 1A) and
// JS (Slice 3b) SDKs. Covers setAccount / clearAccount / setLicense /
// clearLicense and verifies the resulting JSON payloads on events, sessions
// (session-start dispatch is fire-and-forget so we cover that path indirectly
// via track() — payload assembly uses the same snapshot), and exceptions.
//
// Validation rules (mirrors validateContextId in JS, ValidateAndTrimContextId
// in .NET): 1-256 chars after trim, no whitespace-only, no control chars
// (including U+2028 / U+2029). Invalid input is silently ignored (Warning
// log) — it does NOT overwrite a previously valid value.
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

class AccountLicenseContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        tracker_ = beacon::Tracker::configure([](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = "http://localhost:9999";
            o.app_name = "TestApp";
            o.app_version = "1.0.0";
            o.flush_interval_seconds = 3600; // Prevent auto-flush
            o.max_batch_size = 1000;         // Prevent batch-triggered flush
            o.max_breadcrumbs = 0;           // Keep exception payloads compact
        });
        tracker_->identify("user-1");
    }

    void TearDown() override {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    nlohmann::json LastEventJson() {
        auto raw = tracker_->last_enqueued_json();
        EXPECT_FALSE(raw.empty()) << "no event was enqueued";
        return nlohmann::json::parse(raw);
    }

    std::shared_ptr<beacon::Tracker> tracker_;
};

// ── Basic happy paths ─────────────────────────────────────────────────────

TEST_F(AccountLicenseContextTest, SetAccount_ThenTrack_IncludesAccountIdInPayload) {
    tracker_->setAccount("acc_123");
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    ASSERT_TRUE(j.contains("account_id"));
    EXPECT_EQ(j["account_id"], "acc_123");
    EXPECT_FALSE(j.contains("license_id"));
}

TEST_F(AccountLicenseContextTest, SetLicense_ThenTrack_IncludesLicenseIdInPayload) {
    tracker_->setLicense("lic_abc");
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["license_id"], "lic_abc");
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetBothAccountAndLicense_IncludesBothFieldsInPayload) {
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    ASSERT_TRUE(j.contains("account_id"));
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["account_id"], "acc_1");
    EXPECT_EQ(j["license_id"], "lic_1");
}

TEST_F(AccountLicenseContextTest, TrackWithoutAnyContext_OmitsBothFields) {
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    EXPECT_FALSE(j.contains("account_id"));
    EXPECT_FALSE(j.contains("license_id"));
}

// ── Clear behavior ────────────────────────────────────────────────────────

TEST_F(AccountLicenseContextTest, ClearAccount_OmitsAccountIdFromSubsequentPayload_PreservesLicenseId) {
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");
    tracker_->clearAccount();

    tracker_->track("cat", "name");
    auto j = LastEventJson();

    EXPECT_FALSE(j.contains("account_id"));
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["license_id"], "lic_1");
}

TEST_F(AccountLicenseContextTest, ClearLicense_OmitsLicenseIdFromSubsequentPayload_PreservesAccountId) {
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");
    tracker_->clearLicense();

    tracker_->track("cat", "name");
    auto j = LastEventJson();

    ASSERT_TRUE(j.contains("account_id"));
    EXPECT_EQ(j["account_id"], "acc_1");
    EXPECT_FALSE(j.contains("license_id"));
}

TEST_F(AccountLicenseContextTest, Reset_ClearsBothAccountAndLicenseContext) {
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");

    tracker_->reset();
    // After reset, actor_id is cleared too — re-identify to enqueue cleanly.
    tracker_->identify("user-2");
    tracker_->track("cat", "name");

    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
    EXPECT_FALSE(j.contains("license_id"));
}

// ── Validation: empty / whitespace ────────────────────────────────────────

TEST_F(AccountLicenseContextTest, SetAccount_WithEmptyString_IsIgnored) {
    tracker_->setAccount(std::string(""));
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_WithWhitespaceOnly_IsIgnored) {
    tracker_->setAccount("   \t  ");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_TrimsLeadingAndTrailingWhitespace) {
    tracker_->setAccount("   acc_trimmed   ");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    ASSERT_TRUE(j.contains("account_id"));
    EXPECT_EQ(j["account_id"], "acc_trimmed");
}

// ── Validation: length ────────────────────────────────────────────────────

TEST_F(AccountLicenseContextTest, SetAccount_With257Chars_IsIgnored) {
    tracker_->setAccount(std::string(257, 'a'));
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_With256Chars_IsAccepted) {
    std::string id(256, 'a');
    tracker_->setAccount(id);
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    ASSERT_TRUE(j.contains("account_id"));
    EXPECT_EQ(j["account_id"], id);
}

TEST_F(AccountLicenseContextTest, SetAccount_With1Char_IsAccepted) {
    tracker_->setAccount("x");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    ASSERT_TRUE(j.contains("account_id"));
    EXPECT_EQ(j["account_id"], "x");
}

// ── Validation: control characters ────────────────────────────────────────

TEST_F(AccountLicenseContextTest, SetAccount_WithEmbeddedNewline_IsIgnored) {
    tracker_->setAccount("acc\n123");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_WithCarriageReturn_IsIgnored) {
    tracker_->setAccount("acc\r123");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_WithEmbeddedTab_IsIgnored) {
    tracker_->setAccount("acc\t123");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_WithNullByte_IsIgnored) {
    std::string id = "acc";
    id.push_back('\0');
    id += "123";
    tracker_->setAccount(id);
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_WithU2028_IsIgnored) {
    // U+2028 LINE SEPARATOR in UTF-8 is 0xE2 0x80 0xA8.
    std::string id = "acc";
    id.push_back(static_cast<char>(0xE2));
    id.push_back(static_cast<char>(0x80));
    id.push_back(static_cast<char>(0xA8));
    id += "x";
    tracker_->setAccount(id);
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, SetAccount_WithU2029_IsIgnored) {
    // U+2029 PARAGRAPH SEPARATOR in UTF-8 is 0xE2 0x80 0xA9.
    std::string id = "acc";
    id.push_back(static_cast<char>(0xE2));
    id.push_back(static_cast<char>(0x80));
    id.push_back(static_cast<char>(0xA9));
    id += "x";
    tracker_->setAccount(id);
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("account_id"));
}

// ── Validation: prior valid value is preserved when rejection occurs ──────

TEST_F(AccountLicenseContextTest, SetAccount_PreservesValidValueWhenSubsequentInvalidInputRejected) {
    tracker_->setAccount("acc_valid");
    tracker_->setAccount("");          // rejected
    tracker_->setAccount("   ");        // rejected
    tracker_->setAccount("bad\nvalue"); // rejected
    tracker_->setAccount(std::string(300, 'x')); // rejected

    tracker_->track("cat", "name");
    auto j = LastEventJson();
    ASSERT_TRUE(j.contains("account_id"));
    EXPECT_EQ(j["account_id"], "acc_valid");
}

TEST_F(AccountLicenseContextTest, SetLicense_PreservesValidValueWhenSubsequentInvalidInputRejected) {
    tracker_->setLicense("lic_valid");
    tracker_->setLicense("");
    tracker_->setLicense("\t\r\n");
    // Build a string with an embedded null byte. Using a C-string literal
    // here would truncate at the \0 and yield a valid 3-char "bad" string.
    std::string bad_nul = "bad";
    bad_nul.push_back('\0');
    bad_nul += "nul";
    tracker_->setLicense(bad_nul);
    tracker_->setLicense(std::string(300, 'x'));

    tracker_->track("cat", "name");
    auto j = LastEventJson();
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["license_id"], "lic_valid");
}

// ── Symmetric license validation ──────────────────────────────────────────

TEST_F(AccountLicenseContextTest, SetLicense_WithEmptyString_IsIgnored) {
    tracker_->setLicense(std::string(""));
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("license_id"));
}

TEST_F(AccountLicenseContextTest, SetLicense_WithWhitespaceOnly_IsIgnored) {
    tracker_->setLicense("   ");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("license_id"));
}

TEST_F(AccountLicenseContextTest, SetLicense_With257Chars_IsIgnored) {
    tracker_->setLicense(std::string(257, 'l'));
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("license_id"));
}

TEST_F(AccountLicenseContextTest, SetLicense_With256Chars_IsAccepted) {
    std::string id(256, 'l');
    tracker_->setLicense(id);
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["license_id"], id);
}

TEST_F(AccountLicenseContextTest, SetLicense_WithEmbeddedNewline_IsIgnored) {
    tracker_->setLicense("lic\nbad");
    tracker_->track("cat", "name");
    auto j = LastEventJson();
    EXPECT_FALSE(j.contains("license_id"));
}

// ── Exception path ────────────────────────────────────────────────────────

TEST_F(AccountLicenseContextTest, TrackException_AfterSetAccountAndSetLicense_IncludesBothFieldsInPayload) {
    tracker_->setAccount("acc_exc");
    tracker_->setLicense("lic_exc");

    try {
        throw std::runtime_error("boom");
    } catch (const std::exception& ex) {
        tracker_->trackException(ex);
    }

    auto raw = tracker_->last_exception_json();
    ASSERT_FALSE(raw.empty());
    auto j = nlohmann::json::parse(raw);

    ASSERT_TRUE(j.contains("account_id"));
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["account_id"], "acc_exc");
    EXPECT_EQ(j["license_id"], "lic_exc");
}

TEST_F(AccountLicenseContextTest, TrackException_WithoutContext_OmitsBothFieldsFromPayload) {
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception& ex) {
        tracker_->trackException(ex);
    }

    auto raw = tracker_->last_exception_json();
    ASSERT_FALSE(raw.empty());
    auto j = nlohmann::json::parse(raw);

    EXPECT_FALSE(j.contains("account_id"));
    EXPECT_FALSE(j.contains("license_id"));
}

TEST_F(AccountLicenseContextTest, TrackException_AfterClearAccount_OmitsAccountIdButKeepsLicense) {
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");
    tracker_->clearAccount();

    try {
        throw std::runtime_error("boom");
    } catch (const std::exception& ex) {
        tracker_->trackException(ex);
    }

    auto raw = tracker_->last_exception_json();
    ASSERT_FALSE(raw.empty());
    auto j = nlohmann::json::parse(raw);

    EXPECT_FALSE(j.contains("account_id"));
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["license_id"], "lic_1");
}

// ── Opt-out gating ────────────────────────────────────────────────────────

TEST_F(AccountLicenseContextTest, SetAccount_WhileOptedOut_IsNoOp) {
    tracker_->opt_out();
    tracker_->setAccount("acc_should_not_apply");

    // Bring back into a state where we can enqueue and inspect a payload.
    tracker_->opt_in();
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    EXPECT_FALSE(j.contains("account_id"))
        << "setAccount during opt-out should not persist account_id once opted back in.";
}

TEST_F(AccountLicenseContextTest, SetLicense_WhileOptedOut_IsNoOp) {
    tracker_->opt_out();
    tracker_->setLicense("lic_should_not_apply");

    tracker_->opt_in();
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    EXPECT_FALSE(j.contains("license_id"))
        << "setLicense during opt-out should not persist license_id once opted back in.";
}

TEST_F(AccountLicenseContextTest, ClearAccount_WhileOptedOut_StillWorks) {
    // Set account, then opt out, then clear — clear must always be safe.
    tracker_->setAccount("acc_will_be_cleared");
    tracker_->opt_out();
    tracker_->clearAccount();  // should not throw, should clear the value

    tracker_->opt_in();
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    EXPECT_FALSE(j.contains("account_id"));
}

TEST_F(AccountLicenseContextTest, ClearLicense_WhileOptedOut_StillWorks) {
    tracker_->setLicense("lic_will_be_cleared");
    tracker_->opt_out();
    tracker_->clearLicense();

    tracker_->opt_in();
    tracker_->track("cat", "name");
    auto j = LastEventJson();

    EXPECT_FALSE(j.contains("license_id"));
}

// ── Session-start payload ─────────────────────────────────────────────────
// The session-start POST is fire-and-forget against a non-routable address.
// We cannot intercept the request body without a test transport, but we can
// confirm that startSession() does not throw or otherwise clobber the
// account/license context. The track() following startSession() still emits
// both fields, which exercises the same snapshot-under-session-mutex path
// used to build the start payload.

TEST_F(AccountLicenseContextTest, StartSession_DoesNotClobberAccountOrLicense) {
    tracker_->setAccount("acc_session");
    tracker_->setLicense("lic_session");

    tracker_->startSession();
    // Give the detached start-session thread a moment to complete (it will
    // fail silently against the non-routable address).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    tracker_->track("cat", "name");
    auto j = LastEventJson();

    ASSERT_TRUE(j.contains("session_id"));
    ASSERT_TRUE(j.contains("account_id"));
    ASSERT_TRUE(j.contains("license_id"));
    EXPECT_EQ(j["account_id"], "acc_session");
    EXPECT_EQ(j["license_id"], "lic_session");
}
