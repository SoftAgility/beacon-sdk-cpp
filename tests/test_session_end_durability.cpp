// Covers the session-end-durability PRD (beacon-sdk-cpp) "Test plan" section:
//   T1  Destructor with an active session writes a durable pending-end record
//       (self-contained fields; ended_at ~ write time).
//   T2  Bounded send success deletes the record; timeout/failure retains it.
//   T3  Next-construction drain delivers a persisted end as sdk_recovery with
//       the ORIGINAL ended_at and full start context.
//   T4  Self-contained record; structured session_not_found 404 is non-terminal
//       (record removed locally) — and a BARE 404 is a permanent drop (round-2
//       scoping), distinct from session_not_found.
//   T5  endSession(); flush(); delivers the end synchronously when online.
//   T6  shutdown_flush_timeout_seconds = 0 skips the send but still persists.
//   T7  Multiple pending ends: startSession -> startSession -> ~Tracker offline
//       persists two records, both delivered next launch.
//   T8  optOut() / reset() purge pending ends; disabled / no active session
//       writes no record.
//   T9  The pending-end store is SEPARATE from the event queue (a session-end
//       never lands in queued_events).
//   T10 Bounded destructor send honors the curl timeout (no hang past
//       shutdown_flush_timeout_seconds when the endpoint is slow/unreachable).
//
// Regression guards (the .NET port's hard-won lessons):
//   RG-1 Per-session snapshot: two sessions ended in one offline run carry the
//        actor/account/license that was active at EACH session's start, not the
//        last value (the exact .NET defect).
//   RG-2 Recovery payload field names: the sdk_recovery body uses snake_case
//        actor_id/product/product_version/started_at/account_id/license_id.
//
// Harness: a real loopback MockHttpServer captures the bytes that reach the
// server; PendingEndStore reads the pending_session_ends sibling table directly
// (mirrors how test_disk_queue.cpp inspects queued_events). Per-test isolation
// uses a unique product name (own beacon_queue.db) + purge before/after.
#include <gtest/gtest.h>
#include <beacon/beacon.hpp>
#include <nlohmann/json.hpp>

#include "session_end_test_helpers.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using beacon_test::CapturedRequest;
using beacon_test::MockHttpServer;
using beacon_test::MockResponse;
using beacon_test::PendingEndStore;
using beacon_test::kEventsPath;
using beacon_test::kSessionEndPath;
using beacon_test::kSessionStartPath;

namespace {

// An address that refuses/black-holes connections fast, for "offline" runs.
// Port 1 on loopback is reserved and not listened to — connect() fails quickly.
constexpr const char* kUnreachableBaseUrl = "http://127.0.0.1:1";

class SessionEndDurabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        product_ = beacon_test::unique_product("SessEndDur");
        beacon_test::purge_product_db(product_);
    }

    void TearDown() override {
        // Make sure no tracker is holding the DB connection, then clean up.
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
        beacon_test::purge_product_db(product_);
    }

    // Configure a tracker for THIS test's product. A long flush interval keeps
    // the background tick from firing unless we explicitly flush(); callers that
    // want recovery-on-launch pass a short interval.
    std::shared_ptr<beacon::Tracker> make_tracker(const std::string& base_url,
                                                  int shutdown_timeout = 2,
                                                  int flush_interval = 3600) {
        std::string url = base_url;
        std::string prod = product_;
        return beacon::Tracker::configure([&](beacon::Options& o) {
            o.api_key = "test-key";
            o.api_base_url = url;
            o.product = prod;
            o.product_version = "9.9.9";
            o.flush_interval_seconds = flush_interval;
            o.max_batch_size = 1000;
            o.shutdown_flush_timeout_seconds = shutdown_timeout;
        });
    }

    // Destroy the active tracker (runs ~Tracker, the durable-end path) and clear
    // the singleton so a "next launch" can reconfigure the same product.
    void destroy_tracker() {
        tracker_.reset();
        beacon::Tracker::reset_for_testing();
    }

    std::string product_;
    std::shared_ptr<beacon::Tracker> tracker_;
};

// ─────────────────────────────────────────────────────────────────────────────
// T1 — destructor with an active session writes a durable, self-contained record
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, DestructorWithActiveSessionWritesDurablePendingEnd) {
    // Arrange — offline so the bounded send fails and the record survives for us
    // to inspect.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);
    std::string session_id = tracker_->session_id();
    ASSERT_FALSE(session_id.empty());

    // Act — destruct (runs the durable-end path in ~Tracker).
    destroy_tracker();

    // Assert — exactly one self-contained record with the start context.
    PendingEndStore store(product_);
    auto rows = store.rows();
    ASSERT_EQ(rows.size(), 1u);
    const auto& r = rows[0];
    EXPECT_EQ(r.session_id, session_id);
    EXPECT_EQ(r.actor_id, "user-1");
    EXPECT_EQ(r.source_app, product_); // source_app == Options.product
    EXPECT_EQ(r.product_version, "9.9.9");
    EXPECT_EQ(r.account_id, "acc_1");
    EXPECT_EQ(r.license_id, "lic_1");
    EXPECT_FALSE(r.started_at.empty());
    EXPECT_FALSE(r.ended_at.empty());
    // ended_at is stamped at destruct time (an ISO-8601 UTC instant).
    EXPECT_NE(r.ended_at.find('T'), std::string::npos);
    EXPECT_NE(r.ended_at.find('Z'), std::string::npos);
    // A "normal" record persisted by THIS run is not yet flagged recovery (the
    // flip to sdk_recovery happens at the NEXT construction).
    EXPECT_EQ(r.end_reason, "normal");
}

// ─────────────────────────────────────────────────────────────────────────────
// T2 — bounded send: success deletes the record; failure leaves it persisted
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, BoundedSendSuccessDeletesRecord) {
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", 0});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/5);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);

    // Act — destruct triggers the bounded synchronous send; mock 200s.
    destroy_tracker();

    // Assert — the end reached the server and the record was deleted.
    ASSERT_GE(server.request_count(kSessionEndPath()), 1u);
    PendingEndStore store(product_);
    EXPECT_EQ(store.count(), 0u);
    server.stop();
}

TEST_F(SessionEndDurabilityTest, BoundedSendNetworkFailureRetainsRecord) {
    // Offline endpoint → transport error → record retained.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);

    destroy_tracker();

    PendingEndStore store(product_);
    EXPECT_EQ(store.count(), 1u);
}

TEST_F(SessionEndDurabilityTest, BoundedSendTimeoutRetainsRecord) {
    // Server delays well beyond the 1s shutdown timeout → curl times out →
    // network error → record retained.
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", /*delay_ms=*/4000});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);

    destroy_tracker();

    PendingEndStore store(product_);
    EXPECT_EQ(store.count(), 1u);
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// T3 — next-construction drain delivers a persisted end as sdk_recovery with the
//      ORIGINAL ended_at and full start context.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, NextLaunchDrainsAsSdkRecoveryWithOriginalEndedAt) {
    // Arrange — launch 1 offline persists one record.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->setAccount("acc_orig");
    tracker_->setLicense("lic_orig");
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);
    destroy_tracker();

    PendingEndStore store(product_);
    ASSERT_EQ(store.count(), 1u);
    std::string original_ended_at = store.rows()[0].ended_at;
    std::string original_session_id = store.rows()[0].session_id;

    // Act — launch 2 with a reachable server + short interval so the recovery
    // drain fires promptly.
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", 0});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/1);
    ASSERT_TRUE(server.wait_for_requests(kSessionEndPath(), 1, 8s));
    destroy_tracker();

    // Assert — the recovery delivery used sdk_recovery + the ORIGINAL ended_at,
    // carried full start context, and the record was removed after delivery.
    auto ends = server.requests_for(kSessionEndPath());
    ASSERT_GE(ends.size(), 1u);
    auto j = nlohmann::json::parse(ends[0].body);
    EXPECT_EQ(j.value("end_reason", ""), "sdk_recovery");
    EXPECT_EQ(j.value("ended_at", ""), original_ended_at);
    EXPECT_EQ(j.value("session_id", ""), original_session_id);
    EXPECT_EQ(j.value("actor_id", ""), "user-1");
    EXPECT_FALSE(j.value("started_at", "").empty());
    EXPECT_EQ(j.value("account_id", ""), "acc_orig");
    EXPECT_EQ(j.value("license_id", ""), "lic_orig");

    PendingEndStore store2(product_);
    EXPECT_EQ(store2.count(), 0u);
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// T4 — session_not_found 404 (structured) is non-terminal; a BARE 404 is dropped
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, StructuredSessionNotFound404OnRecoveryRemovesRecord) {
    // Persist offline first.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);
    destroy_tracker();
    ASSERT_EQ(PendingEndStore(product_).count(), 1u);

    // Launch 2: server returns a STRUCTURED session_not_found 404. On a recovery
    // delivery the server's create-on-recovery owns it, so the record is removed
    // locally (queue does not stall).
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(),
                        MockResponse{404, R"({"error":"session_not_found"})", 0});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/1);
    ASSERT_TRUE(server.wait_for_requests(kSessionEndPath(), 1, 8s));
    // Allow the delete to apply.
    std::this_thread::sleep_for(200ms);
    destroy_tracker();

    EXPECT_EQ(PendingEndStore(product_).count(), 0u)
        << "structured session_not_found on a recovery delivery must remove the record";
    server.stop();
}

TEST_F(SessionEndDurabilityTest, Bare404IsPermanentDropNotRetainedForever) {
    // Persist offline first.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);
    destroy_tracker();
    ASSERT_EQ(PendingEndStore(product_).count(), 1u);

    // Launch 2: server returns a BARE 404 (no session_not_found body) — wrong
    // url / missing route / un-upgraded server. This must be a permanent drop,
    // NOT retained as a recovery forever.
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{404, R"({"error":"not_found"})", 0});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/1);
    ASSERT_TRUE(server.wait_for_requests(kSessionEndPath(), 1, 8s));
    std::this_thread::sleep_for(200ms);
    destroy_tracker();

    EXPECT_EQ(PendingEndStore(product_).count(), 0u)
        << "a bare 404 must be dropped, not retained forever";
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// T5 — endSession(); flush(); delivers the end synchronously when online
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, EndSessionThenFlushDeliversSynchronouslyWhenOnline) {
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", 0});
    server.set_response(kSessionStartPath(), MockResponse{200, "{}", 0});

    // Short interval keeps the loop responsive; flush() wakes it regardless.
    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);

    // Act — endSession() persists the durable record (no detached thread);
    // flush() drains the pending-end store after the event queues.
    tracker_->endSession();
    EXPECT_TRUE(tracker_->flush());

    // Assert — the end reached the server (drained by flush, not a detached
    // fire-and-forget thread).
    ASSERT_TRUE(server.wait_for_requests(kSessionEndPath(), 1, 8s));
    auto ends = server.requests_for(kSessionEndPath());
    ASSERT_GE(ends.size(), 1u);
    auto j = nlohmann::json::parse(ends[0].body);
    // Delivered this run as a live "normal" end (minimal payload).
    EXPECT_EQ(j.value("end_reason", ""), "normal");

    // And the record is gone after a successful synchronous delivery.
    destroy_tracker();
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// T6 — shutdown_flush_timeout_seconds = 0 skips the send but still persists
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, ZeroShutdownTimeoutSkipsSendButPersists) {
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", 0});

    // timeout 0 = disk-only on destruct. Long flush interval so the background
    // tick does not deliver before we inspect.
    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/3600);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);

    destroy_tracker();

    // No destructor send was attempted, but the record persists.
    EXPECT_EQ(server.request_count(kSessionEndPath()), 0u)
        << "shutdown_flush_timeout_seconds=0 must skip the bounded send";
    EXPECT_EQ(PendingEndStore(product_).count(), 1u);
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// T7 — multiple pending ends: start -> start -> ~Tracker offline persists two,
//      both delivered next launch.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, TwoSessionsOfflinePersistTwoRecordsBothDeliveredNextLaunch) {
    // Launch 1 offline: startSession ends the prior session, then ~Tracker ends
    // the second → two pending records.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(30ms);
    tracker_->startSession("user-2");
    std::this_thread::sleep_for(30ms);
    destroy_tracker();

    PendingEndStore store(product_);
    ASSERT_EQ(store.count(), 2u);

    // Launch 2 online: both delivered as sdk_recovery.
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", 0});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/1);
    ASSERT_TRUE(server.wait_for_requests(kSessionEndPath(), 2, 8s));
    destroy_tracker();

    EXPECT_GE(server.request_count(kSessionEndPath()), 2u);
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// T8 — optOut() / reset() purge pending ends; disabled / no-session → no record
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, OptOutPurgesPendingEnds) {
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(30ms);
    // Create a second pending record by superseding the session.
    tracker_->startSession("user-2");
    std::this_thread::sleep_for(30ms);
    ASSERT_GE(PendingEndStore(product_).count(), 1u);

    // optOut() must purge pending ends (consent) and not write the active end.
    tracker_->optOut();
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);

    destroy_tracker();
    // Destructor while opted out persists nothing either.
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);
}

TEST_F(SessionEndDurabilityTest, ResetPurgesPendingEnds) {
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(30ms);
    tracker_->startSession("user-2"); // supersede → one pending record
    std::this_thread::sleep_for(30ms);
    ASSERT_GE(PendingEndStore(product_).count(), 1u);

    tracker_->reset();
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);

    destroy_tracker();
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);
}

TEST_F(SessionEndDurabilityTest, DestructorWithNoActiveSessionWritesNoRecord) {
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    // No startSession.
    destroy_tracker();
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);
}

TEST_F(SessionEndDurabilityTest, DisabledTrackerWritesNoRecord) {
    // Disabled tracker: ~Tracker returns early, no DB, no record.
    std::string prod = product_;
    tracker_ = beacon::Tracker::configure([&](beacon::Options& o) {
        o.enabled = false;
        o.product = prod;
        o.product_version = "9.9.9";
        o.shutdown_flush_timeout_seconds = 1;
    });
    // startSession on a disabled tracker is a no-op.
    tracker_->startSession("user-1");
    destroy_tracker();
    EXPECT_EQ(PendingEndStore(product_).count(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T9 — the pending-end store is SEPARATE from the event queue
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, SessionEndNeverLandsInEventQueue) {
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1, /*flush_interval=*/3600);
    tracker_->identify("user-1");
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);
    // Enqueue some events too — they go to the memory queue, persisted to
    // queued_events on destruct.
    for (int i = 0; i < 3; ++i) tracker_->track("cat", "evt_" + std::to_string(i));

    destroy_tracker();

    PendingEndStore store(product_);
    // The session-end is in its own table...
    EXPECT_EQ(store.count(), 1u);
    // ...and the events are in queued_events — neither bled into the other.
    EXPECT_EQ(store.queued_events_count(), 3u);
}

// ─────────────────────────────────────────────────────────────────────────────
// T10 — bounded destructor send honors the curl timeout (no hang)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, DestructorSendDoesNotHangBeyondTimeoutWhenServerSlow) {
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    // Server accepts but never replies within the window (3s delay) while the
    // shutdown timeout is 1s → destruction must return promptly.
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", /*delay_ms=*/3000});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/1, /*flush_interval=*/3600);
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(50ms);

    auto t0 = std::chrono::steady_clock::now();
    destroy_tracker();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // Ceiling well below the 3s server delay: proves the curl timeout
    // (= shutdown_flush_timeout_seconds, 1s) bounded the destructor.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2500)
        << "destructor must not block on the slow endpoint beyond the curl timeout";

    // The record is retained for next-launch recovery.
    EXPECT_EQ(PendingEndStore(product_).count(), 1u);
    server.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// RG-1 — per-session SNAPSHOT: each persisted end carries the actor/account/
//        license that was active at THAT session's start (the .NET defect).
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, RG1_TwoSessionsCarryTheirOwnActorSnapshot) {
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);

    // Session 1: user-1.
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(30ms);
    // Session 2: user-2 (supersedes session 1; its end is built from session 1's
    // OLD snapshot BEFORE the live state repoints to user-2).
    tracker_->startSession("user-2");
    std::this_thread::sleep_for(30ms);

    destroy_tracker();

    auto rows = PendingEndStore(product_).rows();
    ASSERT_EQ(rows.size(), 2u);
    // Oldest first: row[0] is session 1 (user-1), row[1] is session 2 (user-2).
    EXPECT_EQ(rows[0].actor_id, "user-1");
    EXPECT_EQ(rows[1].actor_id, "user-2")
        << "second record must carry user-2, NOT user-1 — and the first must NOT "
           "be overwritten to user-2 (the .NET snapshot defect)";
    EXPECT_NE(rows[0].session_id, rows[1].session_id);
}

TEST_F(SessionEndDurabilityTest, RG1_AccountAndLicenseSnapshotPerSession) {
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);

    // Session 1 starts with acc_1 / lic_1.
    tracker_->setAccount("acc_1");
    tracker_->setLicense("lic_1");
    tracker_->startSession("user-1");
    std::this_thread::sleep_for(30ms);

    // Change account/license MID-LIFE, then start session 2 → session 2's
    // snapshot is acc_2 / lic_2; session 1's end must keep acc_1 / lic_1.
    tracker_->setAccount("acc_2");
    tracker_->setLicense("lic_2");
    tracker_->startSession("user-2");
    std::this_thread::sleep_for(30ms);

    destroy_tracker();

    auto rows = PendingEndStore(product_).rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].actor_id, "user-1");
    EXPECT_EQ(rows[0].account_id, "acc_1");
    EXPECT_EQ(rows[0].license_id, "lic_1");
    EXPECT_EQ(rows[1].actor_id, "user-2");
    EXPECT_EQ(rows[1].account_id, "acc_2");
    EXPECT_EQ(rows[1].license_id, "lic_2");
}

// ─────────────────────────────────────────────────────────────────────────────
// RG-2 — recovery payload field names are snake_case (no camelCase).
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SessionEndDurabilityTest, RG2_RecoveryPayloadUsesSnakeCaseFieldNames) {
    // Persist offline.
    tracker_ = make_tracker(kUnreachableBaseUrl, /*shutdown_timeout=*/1);
    tracker_->setAccount("acc_rg2");
    tracker_->setLicense("lic_rg2");
    tracker_->startSession("user-rg2");
    std::this_thread::sleep_for(50ms);
    destroy_tracker();
    ASSERT_EQ(PendingEndStore(product_).count(), 1u);

    // Recover online and capture the exact recovery body.
    MockHttpServer server;
    ASSERT_TRUE(server.start());
    server.set_response(kSessionEndPath(), MockResponse{200, "{}", 0});

    tracker_ = make_tracker(server.base_url(), /*shutdown_timeout=*/0, /*flush_interval=*/1);
    ASSERT_TRUE(server.wait_for_requests(kSessionEndPath(), 1, 8s));
    destroy_tracker();

    auto ends = server.requests_for(kSessionEndPath());
    ASSERT_GE(ends.size(), 1u);
    auto j = nlohmann::json::parse(ends[0].body);

    // Exact snake_case keys present.
    EXPECT_TRUE(j.contains("actor_id"));
    EXPECT_TRUE(j.contains("product"));
    EXPECT_TRUE(j.contains("product_version"));
    EXPECT_TRUE(j.contains("started_at"));
    EXPECT_TRUE(j.contains("account_id"));
    EXPECT_TRUE(j.contains("license_id"));
    EXPECT_TRUE(j.contains("session_id"));
    EXPECT_TRUE(j.contains("ended_at"));
    EXPECT_TRUE(j.contains("end_reason"));

    // No camelCase variants leaked in.
    EXPECT_FALSE(j.contains("actorId"));
    EXPECT_FALSE(j.contains("productVersion"));
    EXPECT_FALSE(j.contains("startedAt"));
    EXPECT_FALSE(j.contains("accountId"));
    EXPECT_FALSE(j.contains("licenseId"));
    EXPECT_FALSE(j.contains("sessionId"));
    EXPECT_FALSE(j.contains("endedAt"));
    EXPECT_FALSE(j.contains("endReason"));

    // Values match the persisted snapshot.
    EXPECT_EQ(j.value("actor_id", ""), "user-rg2");
    EXPECT_EQ(j.value("product", ""), product_);
    EXPECT_EQ(j.value("product_version", ""), "9.9.9");
    EXPECT_EQ(j.value("account_id", ""), "acc_rg2");
    EXPECT_EQ(j.value("license_id", ""), "lic_rg2");
    server.stop();
}

} // namespace
