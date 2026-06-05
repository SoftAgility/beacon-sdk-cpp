# Changelog

All notable changes to the SoftAgility Beacon C++ SDK are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [4.0.0] - 2026-06-05

### Added

- **Durable session-end on shutdown — clean app closes are no longer lost.** Previously, closing the app or calling `endSession()` fired the session end on a detached thread, so the request was usually abandoned when the process exited; the server only closed the session via its inactivity timeout (recorded as a `timeout`, up to a day later). Now the SDK persists a self-contained session-end record (session id, actor, product/version, started-at, account/license, ended-at) to a `pending_session_ends` table in the existing local SQLite queue file **before** the destructor returns, then makes a best-effort **bounded synchronous** send (per-request timeout = the new `Options::shutdown_flush_timeout_seconds`). Anything not delivered at shutdown is sent on the **next launch** as an `sdk_recovery` end carrying the *original* end time — so session duration and completion are recorded faithfully across offline closes, crashes, and force-kills. Multiple sessions ended in one run are each delivered. (The recovery/supersede behavior requires the matching backend support; a live `normal` end while online works against the existing endpoint.)
- **`Options::shutdown_flush_timeout_seconds`** (default 2; clamped to `[0, 30]`; `0` skips the blocking send and persists-only) — bounds the best-effort send in `~Tracker()`.
- `HttpClient::post_json` gained an optional per-request `timeout_seconds` parameter (default 10, so existing call sites are unchanged); libcurl global init/cleanup is now reference-counted and owned by the SDK.

### Changed

- **BREAKING (ABI) — `Options` and `Tracker` gained new members.** Because the library ships as a SHARED library and exposes these types directly (no pImpl), the added state is binary-incompatible with 3.x: **consumers must recompile** against the 4.0.0 headers. No source-level breakage — existing code compiles unchanged. Major bump + `SOVERSION 4`.
- `endSession()` is no longer a detached fire-and-forget — it enqueues the durable record and lets `flush()` (and the background flush thread) deliver it, so `endSession(); flush();` now guarantees delivery when online.
- `optOut()` and `reset()` purge any pending session-end records (consent-respecting — pending ends are not delivered after opt-out).

## [3.1.0] - 2026-06-01

### Changed

- **Disk queue: graceful multi-instance contention + consistent journaling (non-breaking).** Set `sqlite3_busy_timeout(db, 5000)` at both open sites so two instances of the same app (same `product`) sharing one queue file serialize gracefully — a contending writer waits up to 5s for the lock instead of erroring with `SQLITE_BUSY`. Also switched the offline queue from WAL to the default rollback journal (`PRAGMA journal_mode=DELETE`), matching the .NET SDK: WAL added nothing for this single-connection, write-on-failure-only queue and made the `max_queue_size_mb` cap under-count disk usage (writes lived in the uncounted `-wal` sidecar). Existing WAL databases from an earlier version migrate back automatically on open. Off the user-facing path: `track()` is non-blocking; only the background flush touches the disk queue.

## [3.0.0] - 2026-06-01

### Changed

- **BREAKING — renamed the version config field `Options::app_version` → `Options::product_version`.** Call sites must update `opts.app_version = ...` to `opts.product_version = ...`. No compatibility alias is provided. The value flowing is unchanged — still the application version string.
- **BREAKING — renamed the wire field `source_version` → `product_version`** on every JSON payload: event, session-start, exception, actor-identify, and the exported event manifest. The backend now reads the `product_version` wire key.

## [2.0.0] - 2026-05-31

### Changed

- **BREAKING — renamed the app-identity config field `Options::app_name` → `Options::product`.** Call sites must update `opts.app_name = ...` to `opts.product = ...`. No compatibility alias is provided. The value flowing is unchanged — still the registered product slug from the portal.
- **BREAKING — renamed the wire field `source_app` → `product`** on every JSON payload: event, session-start, exception, actor-identify, and the exported event manifest. The backend now reads the `product` wire key. `app_version` / `source_version` are unchanged.

## [1.1.0] - 2026-05-26

### Added

- **`Tracker::setAccount(account_id)` and `Tracker::clearAccount()`** — attach a per-customer account identifier to subsequent events, sessions, and exceptions. Enables Beacon's account-grain analytics (Account Detail page, account-grain segments / funnels / retention) for vendors with multi-tenant or multi-customer apps.
- **`Tracker::setLicense(license_id)` and `Tracker::clearLicense()`** — attach a per-contract license identifier. **Prefer per-contract IDs over per-user IDs** for richer License Detail analytics. See the `setLicense` doc comment for guidance.
- `account_id` and `license_id` are added to event, session-start, and exception JSON payloads when set. When unset, the fields are **omitted entirely** from the JSON — the ingestion validator distinguishes "absent" from "present but invalid".
- Validation matches the .NET and JS SDKs: 1-256 chars after trim, no whitespace-only, no control characters (including U+2028 / U+2029). Invalid input is silently ignored at `Warning` log level and does NOT overwrite a previously valid value. `setAccount` / `setLicense` are no-ops while opted out; `clearAccount` / `clearLicense` are always safe to call.
- **`Tracker::actorId()`** — read-only accessor for the currently identified actor ID. Returns an empty string before `identify()` has been called. Mirrors the .NET SDK's `ActorId` property and the JS SDK's `getActorId()`.

### Changed

- `Tracker::reset()` now clears the account and license context in addition to clearing actor and session state.
- **Renamed `opt_out()` → `optOut()` and `opt_in()` → `optIn()`** on `Tracker` for consistency with the other multi-word public methods (`startSession`, `endSession`, `setAccount`, `setLicense`, `trackException`, `exportEventManifest`). The README already documented the camelCase form, so this brings the code in line with the docs. **Breaking** — call sites must be updated; no compatibility alias is provided.

## [1.0.1] - 2026-05-08

### Removed

- **Init-time preflight check.** The SDK previously fired an empty-batch POST to `/v1/events` on first `track()` / `startSession()` to detect a 401 response from a bad API key before any real events were queued. This was a workaround for not having a logger — the only way to surface a wrong API key. With the `ILogger` introduced in 1.0.0, a wrong API key now surfaces naturally on the first real flush via the `Warning`-level `HTTP POST .../v1/events returned 401 — ...` log line. The preflight added one HTTP request per app start, polluted the backend's logs with a benign 400 `batch_empty` response, and produced a confusing Warning in customer-side logs (the backend correctly rejects empty batches with 400). Removing it aligns the C++ SDK with the JS SDK (which never had preflight) and the rest of the analytics-SDK industry (Segment, Amplitude, Mixpanel, PostHog all queue eagerly and surface auth failures on first real flush).

### Changed

- **`User-Agent` header now derives from `BEACON_VERSION_STRING`** instead of being hardcoded, so it auto-tracks future version bumps and never drifts.

## [1.0.0] - 2026-05-08

### Added

- Initial public release
- `beacon::Tracker::configure(fn)` singleton factory; `instance()` accessor
- `track`, `trackException`, `identify`, `startSession`, `endSession`, `flush`, `reset`, `optOut`, `optIn` instance methods
- `events.define(category, name)` + `exportEventManifest(path)` for portal manifest upload
- Anonymous-by-default device identity (UUIDv7) with deterministic linking on `identify`
- Idle-timeout session lifecycle
- Breadcrumb ring buffer auto-attached to exception reports
- Platform-specific stack trace capture (Windows DbgHelp, Linux libunwind / glibc backtrace)
- SQLite disk queue for offline durability across crashes and restarts
- Strict TLS (peer cert + hostname verification, no redirects)
- Schannel TLS backend on Windows; OpenSSL on Linux + macOS
- `User-Agent: beacon-sdk-cpp/1.0.0` on every HTTP request
- `BEACON_VERSION_MAJOR/MINOR/PATCH` macros and `beacon::version()` runtime accessor
- CMake install rules: `cmake --install` produces a clean prefix with headers + lib + CMake config package; consumers integrate via `find_package(beacon_sdk CONFIG REQUIRED)`
- ILogger interface — surfaces transport-level errors (TLS handshake failures, DNS, connection timeouts), non-2xx HTTP responses with backend error body, and libcurl init failures. Without a logger the SDK is silent on the failure path; **strongly recommended to wire up during initial integration**.
- Targets C++17; CMake 3.25+; MSVC 2022 / GCC 11+ / Clang 14+
