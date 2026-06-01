# SoftAgility Beacon — C++ SDK

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

The official C++ SDK for [SoftAgility Beacon](https://beacon.softagility.com) — a usage-tracking SaaS for desktop, server, and embedded C++ applications. Send events, manage sessions, capture exceptions, and queue events offline. Buffered, batched, durable, and safe to call from any thread.

---

## Requirements

- **C++17** or later
- **CMake 3.25+**
- A working compiler: MSVC 2022, GCC 11+, or Clang 14+
- Dependencies (auto-fetched if not found via `find_package`): libcurl, nlohmann_json, SQLite3
- TLS backend (libcurl is built with Schannel on Windows / OpenSSL on Linux+macOS — both included automatically)

---

## Installation

The SDK has three integration paths. Pick whichever fits your build.

### CMake FetchContent (recommended)

Add to your `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    beacon_sdk
    GIT_REPOSITORY https://github.com/SoftAgility/beacon-sdk-cpp.git
    GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(beacon_sdk)

target_link_libraries(my_app PRIVATE beacon_sdk)

# IMPORTANT (Windows only): copy beacon_sdk.dll + libcurl.dll next to your
# exe at build time so your app can launch. The SDK does not bundle these
# DLLs into your install tree — that's your build's job.
if(WIN32)
    add_custom_command(TARGET my_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy -t
                $<TARGET_FILE_DIR:my_app>
                $<TARGET_RUNTIME_DLLS:my_app>
        COMMAND_EXPAND_LISTS
    )
endif()
```

### Build from source + install

```bash
git clone https://github.com/SoftAgility/beacon-sdk-cpp.git
cd beacon-sdk-cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --build build --config Release
cmake --install build --config Release
```

After installing, find the package from your project:

```cmake
find_package(beacon_sdk CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE beacon_sdk::beacon_sdk)
```

### vcpkg

A vcpkg port is planned for a follow-up release. For now, prefer the CMake FetchContent path.

---

## Quick start

```cpp
#include <beacon/beacon.hpp>
#include <stdexcept>

int main() {
    beacon::Tracker::configure([](beacon::Options& opts) {
        opts.api_key      = "pk_your_api_key";
        opts.api_base_url = "https://api.beacon.softagility.com";
        opts.product      = "my-app";        // the registered product slug, sent as product on every event
        opts.product_version  = "1.4.2";     // becomes product_version on every event

        // Optional but recommended — declare the events your app emits.
        // The manifest can be exported for the portal's Allowlists Import flow.
        opts.events
            .define("auth",    "user.signed-in")
            .define("billing", "checkout.completed");
    });

    auto tracker = beacon::Tracker::instance();
    tracker->identify("user-12345");
    tracker->startSession();

    tracker->track("auth", "user.signed-in");
    tracker->track("billing", "checkout.completed",
                   std::unordered_map<std::string, std::string>{
                       {"amount", "1990"}, {"currency", "USD"}
                   });

    try { /* risky work */ throw std::runtime_error("db offline"); }
    catch (const std::exception& ex) {
        tracker->trackException(ex, beacon::ExceptionSeverity::NonFatal);
    }

    // On shutdown:
    tracker->endSession();
    tracker->flush();   // blocks until queued events deliver or 30s timeout
    return 0;
}
```

---

## Account & License Context

For vendors with multi-customer or multi-tenant applications, Beacon supports two optional context fields that flow through every event, session, and exception report: `account_id` (the customer / organization the user belongs to) and `license_id` (the contract / subscription / entitlement under which usage is occurring).

```cpp
auto tracker = beacon::Tracker::instance();

// After resolving the current user's account + license at sign-in / startup:
tracker->setAccount("acct_42");           // pseudonymous, opaque ID — not an email
tracker->setLicense("sub_9p2KqXyz");      // see "Recommended vs avoid" below

tracker->identify("user-12345");
tracker->track("auth", "user.signed-in"); // payload includes account_id + license_id
```

All subsequent events, the next `startSession()` payload, and any `trackException()` reports will include the two fields. Set once at sign-in; clear via `clearAccount()` / `clearLicense()` or wipe everything via `reset()` (used on logout). Validation rules: 1-256 chars after trim, no whitespace-only, no control characters. Invalid input is silently ignored and does NOT overwrite a previously valid value.

### Recommended vs avoid (license IDs)

**Prefer per-contract IDs.** A *single* license string shared across all of a customer's users — typically your subscription ID, site key, or bundle SKU — unlocks the most useful License Detail analytics in Beacon.

```cpp
// Recommended: per-contract license — same value for every user on this subscription
tracker->setLicense(currentSubscription.id);          // e.g. "sub_9p2KqXyz"

// Avoid: per-user license — each user gets a distinct license ID
tracker->setLicense(currentUser.id);                  // duplicates the Actor view
```

With per-contract IDs:
- The License Detail page surfaces meaningful per-license usage rollups.
- The "Seen under multiple accounts" governance signal becomes actionable (a license used by users from two different `account_id`s is a useful integrity warning).

With per-user IDs the License Detail view becomes a near-duplicate of the Actor Identities view and the multi-account-sharing warning is disabled — the SDK still works, you just lose the richer analytics tier.

Note: the `/accounts` page in the Beacon portal is a Business+ feature, but ingestion is tier-blind — sending `account_id` / `license_id` on a Starter or Pro account is harmless and the values flow through immediately if you later upgrade.

---

## API surface

| Method | Purpose |
|---|---|
| `Tracker::configure(fn)` | One-time singleton initialization. Throws `std::logic_error` if called twice. Silent no-op disable on bad config (missing api_key / api_base_url / product / product_version) — does NOT throw. |
| `Tracker::instance()` | Returns the configured tracker, or `nullptr` if `configure()` was never called or disabled itself. |
| `tracker->identify(actor_id)` | Set actor id for subsequent calls. Async-links the anonymous device id on first call. |
| `tracker->actorId()` | Returns the current identified actor id (empty before `identify()`). |
| `tracker->track(category, name, properties?)` | Track an event. Returns immediately; batched + flushed in background. |
| `tracker->trackException(exception, severity)` | Report an exception with optional breadcrumb trail. Severity: `Fatal` or `NonFatal`. |
| `tracker->startSession()` / `endSession()` | Open / close a session for grouped event analytics. |
| `tracker->setAccount(account_id)` / `clearAccount()` | Attach / detach a customer account ID on every event, session, and exception. |
| `tracker->setLicense(license_id)` / `clearLicense()` | Attach / detach a contract / license ID. **Prefer per-contract IDs.** |
| `tracker->flush()` | Force-flush queued events. Blocks up to 30s. Use at shutdown. |
| `tracker->exportEventManifest(path)` | Write declared events as a JSON manifest. Upload to the portal's Allowlists Import page. |
| `tracker->optOut()` / `optIn()` | Persist consent. Opted-out tracker is a no-op. |
| `tracker->reset()` | Clear actor + session + account + license + queue + breadcrumbs. Used on logout. |

---

## Configuration

| Field | Default | Range | Notes |
|---|---|---|---|
| `api_key` | required | — | API key from the Beacon portal. Sent as `Authorization: Bearer`. |
| `api_base_url` | required | URL | `https://api.beacon.softagility.com` (or self-hosted). |
| `product` | required | ≤128 chars | The registered product slug, sent as `product` on every event. Must match a registered product in the portal. |
| `product_version` | required | ≤256 chars | Sent as `product_version` on every event. Auto-registers on first event. |
| `flush_interval_seconds` | `60` | 1-3600 | Background flush cadence. |
| `max_batch_size` | `25` | 1-1000 | Events per HTTP batch. |
| `max_queue_size_mb` | `10` | 1-1000 | SQLite disk queue cap (events persist across crashes / restarts). |
| `max_breadcrumbs` | `25` | 0-200 | Breadcrumb ring buffer; `0` disables. |
| `logger` | `nullptr` | — | Implement `beacon::ILogger` to receive transport-level diagnostics. **Strongly recommended for the first integration** — see below. |

---

## Diagnostics: hook up the logger

The SDK is fire-and-forget by design — `track()` returns immediately, errors don't propagate to the caller. Without a logger, transport-level failures are completely silent. For first integration we strongly recommend implementing `beacon::ILogger`:

```cpp
class SimpleLogger : public beacon::ILogger {
public:
    void log(beacon::LogLevel level, const std::string& message) override {
        std::cerr << "[beacon] " << message << "\n";
    }
};

opts.logger = std::make_shared<SimpleLogger>();
```

The logger surfaces:
- libcurl initialization failures (no TLS backend, etc.)
- Per-request transport errors (DNS failure, TLS handshake error, connection timeout)
- Non-2xx HTTP responses with the backend's error body (e.g. `Product 'foo' is not registered`, `API key rejected`, `Account hard-capped`)

---

## What the SDK does for you

| Behaviour | Detail |
|---|---|
| **Buffered & batched** | Events queue in memory then flush every `flush_interval_seconds` (default 60 s) in batches of up to `max_batch_size` (default 25). Background thread, non-blocking. |
| **Offline durable** | Events overflow to a SQLite disk queue capped at `max_queue_size_mb`. They survive crashes and restart cleanly. |
| **Anonymous-by-default** | A UUIDv7 device id is generated on first run. `identify()` later links the device to the actor. |
| **Exception capture** | `trackException` includes the most recent breadcrumbs, exception type, message, and platform-specific stack trace (Windows DbgHelp, Linux libunwind / glibc backtrace). |
| **Thread safety** | All public methods are safe to call from any thread. |
| **Shutdown safety** | `flush()` blocks up to 30 s waiting for queued events to deliver; the destructor joins the background thread cleanly. |
| **TLS strict** | Peer cert + hostname verification enabled, no redirects, explicit `User-Agent: beacon-sdk-cpp/<version>`. |

---

## Versioning

The SDK follows [Semantic Versioning](https://semver.org/). Compile-time + runtime version constants:

```cpp
#include <beacon/beacon.hpp>

#if BEACON_VERSION_MAJOR < 1
    #error "Requires beacon_sdk 1.0.0 or later"
#endif

std::cout << "beacon_sdk version: " << beacon::version() << "\n";
```

---

## Related

- **Beacon portal:** https://beacon.softagility.com
- **Other SDKs:** [.NET](https://www.nuget.org/packages/SoftAgility.Beacon), [JS / TypeScript](https://www.npmjs.com/package/@softagility/beacon-js)
- **REST API:** integrate without an SDK by POSTing to `/v1/events` directly
- **Source:** https://github.com/SoftAgility/beacon-sdk-cpp

## License

MIT — see [LICENSE](LICENSE).
