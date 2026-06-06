// Shared harness for the session-end durability tests.
//
// Two reusable pieces, both modeled on the existing test conventions:
//   * MockHttpServer — a minimal loopback HTTP/1.1 server that captures every
//     request the SDK sends (path + body) and replies with a per-path canned
//     response. The existing suite has no mock endpoint (it points the SDK at a
//     non-routable localhost:9999 and asserts via test hooks). The session-end
//     durability cases require asserting on the bytes that actually REACH the
//     server (recovery field names, end_reason, the synchronous endSession();
//     flush(); delivery), so a real capturing endpoint is required. It is
//     intentionally tiny and lives only in the test tree.
//   * PendingEndStore — a read-only SQLite view of the `pending_session_ends`
//     sibling table, opened the same way test_disk_queue.cpp inspects the
//     `queued_events` table, so a test can assert table contents directly.
//
// Per-test DB isolation reuses the SDK's own path logic: each test uses a
// UNIQUE product name, so beacon_queue.db lives in a product-specific data
// directory (internal::get_data_directory). UniqueProduct() + the RAII cleaner
// keep tests independent and order-free.
#pragma once

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "DeviceId.hpp" // beacon::internal::get_data_directory

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h> // select(), fd_set for the acceptor poll
#  include <sys/socket.h>
#  include <sys/time.h>   // struct timeval
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace beacon_test {

// A single captured HTTP request.
struct CapturedRequest {
    std::string method;
    std::string path;
    std::string body;
};

// Per-path canned response. status==0 means "drop the connection" (forces the
// SDK to see a transport/network error). delay_ms sleeps before replying so the
// destructor's bounded curl timeout can be exercised.
struct MockResponse {
    int status = 200;
    std::string body = "{}";
    int delay_ms = 0;
};

// Minimal loopback HTTP/1.1 server. Single-threaded acceptor; serves one
// request per accepted connection (the SDK opens a fresh easy handle per POST).
class MockHttpServer {
public:
    MockHttpServer() = default;
    ~MockHttpServer() { stop(); }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    // Starts listening on 127.0.0.1:<ephemeral>. Returns false on failure.
    bool start() {
#if defined(_WIN32)
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_started_ = true;
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == kInvalidSocket) return false;

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        // Make accept() time out periodically so the acceptor thread can observe
        // running_ == false and exit cleanly. close()ing the listen fd from
        // another thread does NOT reliably wake a blocked accept() on Linux
        // (POSIX leaves this unspecified), which would deadlock stop()'s join().
        // SO_RCVTIMEO is honored for accept() on Linux/macOS; on Windows the
        // existing closesocket() already wakes accept(), so this is belt-and-
        // suspenders there. The accept_loop treats the resulting EAGAIN/timeout
        // (fd == kInvalidSocket) as "re-check running_ and continue".
#if defined(_WIN32)
        DWORD accept_timeout_ms = 100;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&accept_timeout_ms), sizeof(accept_timeout_ms));
#else
        struct timeval accept_timeout {};
        accept_timeout.tv_sec = 0;
        accept_timeout.tv_usec = 100000; // 100ms
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO,
                     &accept_timeout, sizeof(accept_timeout));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return false;
        }
        port_ = ntohs(addr.sin_port);

        if (::listen(listen_fd_, 16) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return false;
        }

        running_.store(true);
        acceptor_ = std::thread(&MockHttpServer::accept_loop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            // Not running, but still tear down winsock if start() partially ran.
            cleanup_winsock();
            return;
        }
        if (listen_fd_ != kInvalidSocket) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
        }
        if (acceptor_.joinable()) acceptor_.join();
        cleanup_winsock();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    // Configure the reply for a given request path. Unconfigured paths default
    // to 200 {}.
    void set_response(const std::string& path, MockResponse resp) {
        std::lock_guard<std::mutex> lock(mu_);
        responses_[path] = std::move(resp);
    }

    // All requests captured so far.
    std::vector<CapturedRequest> requests() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }

    // Requests whose path == the given value.
    std::vector<CapturedRequest> requests_for(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<CapturedRequest> out;
        for (const auto& r : requests_) {
            if (r.path == path) out.push_back(r);
        }
        return out;
    }

    size_t request_count(const std::string& path) const {
        return requests_for(path).size();
    }

    // Block until at least `n` requests have arrived for `path`, or timeout.
    bool wait_for_requests(const std::string& path, size_t n,
                           std::chrono::milliseconds timeout) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (request_count(path) >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return request_count(path) >= n;
    }

private:
    void accept_loop() {
        while (running_.load()) {
            // Poll for a pending connection with a timeout so the acceptor
            // re-checks running_ every 100ms and exits cleanly on stop() — on
            // EVERY platform. A bare blocking accept() can't be portably woken
            // by close()/closesocket() from another thread (unspecified on
            // Linux, racy on Windows), which deadlocks join(). select() makes
            // the wakeup deterministic; accept() is only called once a
            // connection is ready, so it never blocks.
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd_, &rfds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms
            int sel = ::select(static_cast<int>(listen_fd_) + 1, &rfds, nullptr, nullptr, &tv);
            if (!running_.load()) break;
            if (sel <= 0) continue; // timeout or error → re-check running_

            sockaddr_in client{};
            socklen_t clen = sizeof(client);
            socket_t fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &clen);
            if (fd == kInvalidSocket) {
                if (!running_.load()) break;
                continue;
            }
            handle_connection(fd);
            close_socket(fd);
        }
    }

    void handle_connection(socket_t fd) {
        std::string raw;
        size_t content_length = 0;
        size_t header_end = std::string::npos;

        char buf[4096];
        // Read until headers complete, then until the full body has arrived.
        while (true) {
            int n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));

            if (header_end == std::string::npos) {
                header_end = raw.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    content_length = parse_content_length(raw.substr(0, header_end));
                }
            }
            if (header_end != std::string::npos) {
                size_t have = raw.size() - (header_end + 4);
                if (have >= content_length) break;
            }
        }

        CapturedRequest req;
        if (!raw.empty()) {
            size_t sp1 = raw.find(' ');
            size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : raw.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                req.method = raw.substr(0, sp1);
                req.path = raw.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            if (header_end != std::string::npos) {
                req.body = raw.substr(header_end + 4, content_length);
            }
        }

        MockResponse resp;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = responses_.find(req.path);
            if (it != responses_.end()) resp = it->second;
            requests_.push_back(req);
        }

        if (resp.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(resp.delay_ms));
        }

        if (resp.status == 0) {
            // Drop the connection without replying → SDK sees a transport error.
            return;
        }

        std::string status_text = (resp.status == 200) ? "OK"
                                 : (resp.status == 202) ? "Accepted"
                                 : (resp.status == 404) ? "Not Found"
                                 : (resp.status == 401) ? "Unauthorized"
                                 : (resp.status == 402) ? "Payment Required"
                                 : (resp.status == 500) ? "Internal Server Error"
                                 : "Status";
        std::string out =
            "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text + "\r\n" +
            "Content-Type: application/json\r\n" +
            "Content-Length: " + std::to_string(resp.body.size()) + "\r\n" +
            "Connection: close\r\n\r\n" + resp.body;

        ::send(fd, out.c_str(), static_cast<int>(out.size()), 0);
    }

    static size_t parse_content_length(const std::string& headers) {
        // Case-insensitive search for "content-length:".
        std::string lower;
        lower.reserve(headers.size());
        for (char c : headers) lower.push_back(static_cast<char>(::tolower(c)));
        size_t pos = lower.find("content-length:");
        if (pos == std::string::npos) return 0;
        pos += std::string("content-length:").size();
        while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
        size_t val = 0;
        while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
            val = val * 10 + static_cast<size_t>(headers[pos] - '0');
            ++pos;
        }
        return val;
    }

    static void close_socket(socket_t fd) {
#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void cleanup_winsock() {
#if defined(_WIN32)
        if (wsa_started_) {
            WSACleanup();
            wsa_started_ = false;
        }
#endif
    }

    socket_t listen_fd_ = kInvalidSocket;
    unsigned short port_ = 0;
    std::atomic<bool> running_{false};
    std::thread acceptor_;
    mutable std::mutex mu_;
    std::vector<CapturedRequest> requests_;
    std::map<std::string, MockResponse> responses_;
#if defined(_WIN32)
    bool wsa_started_ = false;
#endif
};

// Endpoint paths the SDK posts to.
inline const char* kSessionEndPath() { return "/v1/events/sessions/end"; }
inline const char* kSessionStartPath() { return "/v1/events/sessions"; }
inline const char* kEventsPath() { return "/v1/events"; }

// Read-only view of the pending_session_ends sibling table for a given product.
// Opened directly (like test_disk_queue.cpp opens queued_events), AFTER the
// tracker that owns the write connection has been destroyed.
class PendingEndStore {
public:
    explicit PendingEndStore(const std::string& product) {
        std::string dir = beacon::internal::get_data_directory(product);
        if (!dir.empty()) {
#if defined(_WIN32)
            db_path_ = dir + "\\beacon_queue.db";
#else
            db_path_ = dir + "/beacon_queue.db";
#endif
        }
    }

    struct Row {
        int64_t id = 0;
        std::string session_id;
        std::string actor_id;
        std::string source_app;
        std::string product_version;
        std::string started_at;
        std::string account_id; // "" if SQL NULL
        std::string license_id;
        std::string ended_at;
        std::string end_reason;
    };

    std::vector<Row> rows() const {
        std::vector<Row> result;
        sqlite3* db = nullptr;
        if (db_path_.empty()) return result;
        if (sqlite3_open_v2(db_path_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return result;
        }
        const char* sql =
            "SELECT id, session_id, actor_id, source_app, product_version, "
            "started_at, account_id, license_id, ended_at, end_reason "
            "FROM pending_session_ends ORDER BY id ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            auto col = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                return t ? std::string(t) : std::string();
            };
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Row r;
                r.id = sqlite3_column_int64(stmt, 0);
                r.session_id = col(1);
                r.actor_id = col(2);
                r.source_app = col(3);
                r.product_version = col(4);
                r.started_at = col(5);
                r.account_id = col(6);
                r.license_id = col(7);
                r.ended_at = col(8);
                r.end_reason = col(9);
                result.push_back(std::move(r));
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
        return result;
    }

    size_t count() const { return rows().size(); }

    // Count rows in the homogeneous event table (queued_events) — used to prove
    // a session-end never lands in the event queue.
    size_t queued_events_count() const {
        sqlite3* db = nullptr;
        if (db_path_.empty()) return 0;
        if (sqlite3_open_v2(db_path_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return 0;
        }
        size_t n = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM queued_events;", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) n = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
        return n;
    }

    const std::string& path() const { return db_path_; }

private:
    std::string db_path_;
};

// Generates a unique product name per test so each test gets an isolated
// beacon_queue.db. Includes a process-unique counter.
inline std::string unique_product(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return prefix + "_" + std::to_string(now) + "_" +
           std::to_string(counter.fetch_add(1));
}

// Deletes the beacon_queue.db (and journal/WAL siblings) for a product, so a
// test starts and ends with a clean store regardless of order.
inline void purge_product_db(const std::string& product) {
    std::string dir = beacon::internal::get_data_directory(product);
    if (dir.empty()) return;
#if defined(_WIN32)
    std::string base = dir + "\\beacon_queue.db";
#else
    std::string base = dir + "/beacon_queue.db";
#endif
    std::remove(base.c_str());
    std::remove((base + "-wal").c_str());
    std::remove((base + "-shm").c_str());
    std::remove((base + "-journal").c_str());
}

} // namespace beacon_test
