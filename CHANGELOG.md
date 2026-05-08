# Changelog

All notable changes to the SoftAgility Beacon C++ SDK are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
