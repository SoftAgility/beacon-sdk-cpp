# Session-end durability (C++ SDK) — capture clean app closes instead of relying on the reaper

- **Status:** READY-TO-IMPLEMENT (post-codex revision 1; ABI decision settled at revision 2 — converged)
- **Owner:** Matthew Clendening
- **Created:** 2026-06-03
- **Slug:** `session-end-durability`
- **Repo:** `beacon-sdk-cpp` (client SDK)
- **Depends on:** `usage-tracking/docs/specs/session-end-server-reconciliation.prd.md` (the shared server foundation + the recovery wire contract this SDK targets). Ship the server PRD first.
- **Sibling:** `beacon-sdk-dotnet/docs/specs/session-end-durability.prd.md` — this PRD is the C++ port of that one (Parts A/B), which converged over three codex rounds. Same design, C++ idioms.

---

## Problem

Identical to the .NET SDK: a clean application close does not record a session end. The session stays open until the server's inactivity **reaper** closes it as `timeout`, so `completed_session_count` is ~always 0 for desktop apps, duration analytics are low-fidelity, and a clean close is indistinguishable from abandonment. The C++ SDK is structurally a twin of the .NET SDK (a SQLite `DiskQueue`, a background flush thread, `startSession`/`endSession`/`flush`), and it has the **same gaps**.

---

## Root cause (C++ SDK)

All refs verified against current source (`beacon-sdk-cpp`).

- **R1 — the destructor never attempts a session-end.** `Tracker::~Tracker()` (`src/Tracker.cpp` ~`232`) signals shutdown, **joins the flush thread**, persists remaining in-memory **events** to the disk queue (`enqueue_to_disk`), then clears `session_id_` under `session_mutex_`. No session-end is sent or persisted. (Direct analogue of .NET R1; the comment intent is the same — rely on the server reaper.)
- **R2 — every session-end path is a detached fire-and-forget thread.** `Tracker::endSession()` (`src/Tracker.cpp` ~`604`) clears `session_id_`, then launches `std::thread([...]{ ... http.post_json(base + "/v1/events/sessions/end", ...{end_reason:"normal"}); }).detach()`. On process exit a detached thread mid-POST is abandoned before completion — exactly the .NET R2 loss. `startSession()` (~`503`) is also detached, and when it **replaces** an active session it ends the prior one via the same detached pattern.
- **R3 — `flush()` does not cover session lifecycle.** `flush()` (`include/beacon/Tracker.hpp` ~`67`) is **synchronous and blocking** (drains the event queue, up to a 30s timeout). But `endSession`'s detached thread is *not* part of that queue, so a blocking `flush()` gives no guarantee the end was delivered. (Analogue of .NET R3 — and the synchronous `flush()` is what makes the C++ Part B simpler; see below.)

**Server dependency:** the reaper-default and the supersede/create-on-recovery **wire contract** live in the server PRD; this SDK targets that contract.

---

## Proposed fix

Two layers, mirroring the .NET design, in C++ idioms.

### Part A — Durable session-end in the destructor (and `endSession`)

On `~Tracker()` and on `endSession()`, when a session is active:

1. **Persist a durable, self-contained pending-end record first**, *before* clearing `session_id_`. In the destructor this is race-free because the flush thread has already been **joined** (~`232`), so the SQLite store is exclusively owned. The record carries the full start context (`session_id, actor_id, source_app, product_version, started_at, account_id?, license_id?, ended_at, end_reason`) with **`ended_at` stamped at write time** (last activity / destruct instant), not delivery time — so a next-launch delivery records the true end.
2. **Best-effort bounded send** — this is *cleaner in C++ than .NET*. After joining the flush thread (which owns the libcurl handle and has exited), do a **synchronous** `post_json` on a fresh `HttpClient` with a **per-request curl timeout = `Options.shutdown_flush_timeout_seconds`** (new field, default 2; `0` = skip the send, disk-only). A bounded synchronous request needs no detached thread and no cancellation dance — there is no "process exits before the thread finishes" hazard. On success (online), the end lands as `end_reason = "normal"` and the durable record is deleted.
   - **Prerequisite (review finding — Medium):** `HttpClient::post_json` currently **hardcodes `CURLOPT_TIMEOUT`/`CURLOPT_CONNECTTIMEOUT` to 10s** (`src/HttpClient.cpp` ~`100-101`) with no per-request override. A **timeout-taking overload/parameter must be added** before `shutdown_flush_timeout_seconds` is enforceable — otherwise the destructor can block up to 10s regardless of the option.
   - **libcurl global lifecycle (review finding — Medium):** the SDK uses `curl_easy_init`/`curl_easy_cleanup` only — there is **no `curl_global_init`/`curl_global_cleanup`** ownership. A fresh `HttpClient` in the destructor avoids easy-handle sharing, but process-wide libcurl global init/cleanup ownership must be defined as an **implementation requirement** (not just an open question), since a destruct-time send is a late, potentially-last libcurl use.
   - **No-throw destructor (review finding — Medium):** `~Tracker()` (~`232`) has no top-level `try/catch`. Adding JSON build, SQLite writes, HTTP setup, and logging inside the destructor risks `std::terminate` if anything escapes. The entire pending-end **write → send → delete** path in the destructor MUST swallow all exceptions (wrap in `try { … } catch (...) {}`), matching the existing detached-thread error handling.
3. **Next-launch delivery** — on `Tracker` construction, drain the pending-end store (alongside the existing `drain_disk_queue()`) as `end_reason = "sdk_recovery"`, using the **original** persisted `ended_at`.

**Self-contained record (the start dependency).** `startSession` is also fire-and-forget (`src/Tracker.cpp` ~`576`), and the server returns `404 session_not_found` for an end on a missing session. So if the C++ app was offline at session start — or the start simply hasn't landed yet — the end would 404. Carrying the start context lets the server's **create-on-recovery** (server PRD) materialize the absent session.

**Generalize `session_not_found` handling — even a same-run `normal` end can race the start (review finding — High):** because `startSession` stays detached, a fast `startSession(); endSession(); flush();` can deliver the end synchronously *before* the start row exists → `404 session_not_found`. So a **`404` carrying the structured `session_not_found` error** must be **non-terminal**: instead of dropping it, **persist (or retain) the self-contained record and re-deliver as `sdk_recovery`**, which the create-on-recovery path resolves. (Optionally also order delivery so a pending start drains before its end, but the recovery fallback is the guarantee.) Scope this strictly to the `session_not_found` body — a bare `404` from a wrong `api_base_url`, a missing route, or an un-upgraded server must NOT be retained as a recovery (that would retain forever); treat those as ordinary transport failures. Net: the SDK never drops an end on a genuine `session_not_found`; it converts that to the recovery path.

**Storage choice:** do NOT reuse the event `DiskQueue` — `enqueue(std::vector<std::string> event_jsons)` / `dequeue_up_to` is homogeneous event JSON delivered to the *events* endpoint (`src/DiskQueue.hpp` ~`29-32`). Add a **typed sibling store** — a separate `pending_session_ends` table in the same `beacon_queue.db` file.
   - **`busy_timeout` is per-connection, not per-table (review finding — Medium):** `Tracker` already owns one SQLite connection (`db_`, `src/Tracker.cpp` ~`1218`) with a **5s busy timeout** (~`1262`). If the sibling table uses that connection, a destruct-time write **inherits the 5s** block — contradicting "non-blocking." Two options, pick one in implementation: (a) use the existing connection and **accept a bounded ≤5s worst-case** destruct write (simplest; the flush thread is already joined so contention is low), or (b) open a **second connection** with a short `busy_timeout` for the pending-end table — but then explicitly handle **same-file single-writer locking** against the event-queue connection (SQLite rollback-journal mode serializes writers; WAL would relax it). Default recommendation: (a) — joined flush thread means the writer contention that motivated 5s is largely absent at destruct time.

**Multiple pending ends — a queue, not one row:** `startSession` ends the prior session before starting a new one, so one offline run can produce several unsent ends. The store must be a **queue keyed by `session_id`**, all delivered on next launch.

**Opt-out / consent:** `optOut()` persists an opt-out file and clears the memory queue; `reset()` clears identity/session/queue/breadcrumbs. Both must **purge pending session-ends** (do not deliver) — consistent with the SDK's consent posture.

### Part B — Synchronous delivery guarantee (no new async surface)

C++ `flush()` is **already synchronous and blocking**, so there is no async/interface problem like .NET's (no `IBeaconTracker`, no default-interface-method limitation). The clean design:

- `endSession()` **stops being a detached fire-and-forget thread** — it enqueues the durable pending-end record (Part A) and lets the background flush thread (and an explicit `flush()`) drain it.
- **`flush()` also drains the pending-session-end store** after the event queues. So `tracker.endSession(); tracker.flush();` gives a synchronous "the end is delivered when online" guarantee — simpler than the .NET awaitable. Caveat: if the just-started session's detached start hasn't landed, the synchronous end can `404`; per the generalized 404 handling in Part A, that end is then retained and re-delivered as `sdk_recovery` rather than lost — so the guarantee is "delivered (as `normal`, or as `sdk_recovery` if it raced the start)," not "delivered as `normal`."

No new public method is strictly required; if one is added for ergonomics it is additive at source level.

**ABI / versioning (decided).** The SDK ships as a `SHARED` library (`CMakeLists.txt` ~`124`) and `Tracker` is not pImpl (`Options options_`, mutexes, `std::thread`, `sqlite3*` are exposed in the public header ~`202-257`; `Options` is by-value in `configure(Options)` ~`37`), so adding state to `Options`/`Tracker` is an ABI break for already-compiled clients. **Decision (project owner, 2026-06-03): breaking changes are acceptable for the C++ SDK.** Therefore: **append the new `Options` field and the new `Tracker` state directly** (no pImpl refactor), document "recompile required," and bump the **major** version → **`4.0.0`**. Source changes stay clean; binary compatibility is explicitly not a goal.

### Server foundation (dependency, not implemented here)

Reaper-default shortening and the supersede/create-on-recovery contract live in the server PRD. Ship it first.

---

## Test plan (C++ unit/integration)

- Destructor with an active session writes a durable pending-end record into the sibling store (assert table contents).
- Bounded send success deletes the record; timeout/failure leaves it persisted.
- Next-construction drain delivers a persisted end as `sdk_recovery` with the **original** `ended_at`.
- The record is **self-contained**; a `404` on delivery is non-terminal (left for create-on-recovery), not dropped.
- `endSession(); flush();` delivers the end synchronously when online (no longer a detached thread).
- `shutdown_flush_timeout_seconds = 0` skips the send but still persists.
- **Multiple pending ends:** `startSession → startSession → ~Tracker()` offline persists two records, both delivered next launch.
- `optOut()` / `reset()` purge pending ends; disabled / no active session → no record written.
- The sibling store is separate from the event `DiskQueue` (event delivery unaffected).
- Bounded send honors the curl timeout (does not hang destruction beyond `shutdown_flush_timeout_seconds`).

Server-side behavior (supersede, create-on-recovery, reaper) is tested in the server PRD.

---

## Alternatives considered & rejected

- **Keep the detached-thread send, just add it to the destructor.** Rejected: same abandonment-on-exit hazard as today; the bounded synchronous send + durable record is reliable and, in C++, simpler (curl per-request timeout, no thread to join/cancel).
- **Reuse the event `DiskQueue` for the end record.** Rejected: it is homogeneous event JSON delivered to the events endpoint; a session-lifecycle record breaks that batch contract. Use a typed sibling table.
- **Single pending-end slot.** Rejected: `startSession` replacement can leave multiple unsent ends; must be a queue keyed by `session_id`.

---

## Risks & open questions

- **Destructor send latency.** The bounded send adds up to `shutdown_flush_timeout_seconds` to destruction when offline (curl connect/read timeout). Default **2s** (mirrors .NET); `0` opts out.
- **libcurl handle ownership.** The bounded send must use a **fresh `HttpClient`** created after the flush thread is joined — the flush thread owns the original curl handle and has exited by then. Confirm no shared global curl state issue.
- **ABI / shared-lib.** Resolved: breaking changes are acceptable for the C++ SDK → add state directly, recompile required, **major bump 4.0.0** (no pImpl refactor).
- **Same-run start race.** `endSession(); flush();` can `404` if the detached start hasn't landed; resolved by the generalized 404→`sdk_recovery` fallback, not dropped.
- **Destructor exception safety.** All pending-end work in `~Tracker()` must be `noexcept`-in-effect (swallow everything) to avoid `std::terminate`.
- **HttpClient timeout + libcurl globals.** A per-request timeout overload and defined `curl_global_*` ownership are prerequisites, not nice-to-haves.
- **Crash before the durable write.** Reaper-only — acceptable, unchanged.
- **Cross-platform.** SQLite sibling store and curl timeouts are portable across the SDK's supported platforms; mirror `DiskQueue`'s existing guards.

---

## Implementation order

Ship the server PRD (D → C) first. Then:

1. **Part A — durable end** (`beacon-sdk-cpp`): sibling `pending_session_ends` table + persist-in-destructor/`endSession` + bounded synchronous send + next-construction `sdk_recovery` drain.
2. **Part B — `flush()` drains pending ends**; `endSession()` stops detaching. Add the `HttpClient` timeout overload + define libcurl global init/cleanup ownership as part of this work.

Released as **4.0.0** (ABI break accepted — see Part B; no pImpl refactor).

---

## Iteration log

### Revision 0 — initial draft (2026-06-03)
Port of the converged .NET PRD (`beacon-sdk-dotnet`, 3 codex rounds) to the C++ SDK, after confirming the C++ SDK has the identical gaps: destructor persists events but not the end (~`232`); `endSession`/`startSession` are detached fire-and-forget (~`604`/`503`); `flush()` is synchronous but excludes session lifecycle (~`67`); it has a SQLite `DiskQueue` so the durable-end pattern maps almost 1:1. C++-specific design decisions vs. the .NET PRD: (1) the best-effort send is a **bounded synchronous** curl request in the destructor (cleaner than .NET's CTS dance, since the flush thread is already joined); (2) Part B needs **no async surface** — `flush()` is already blocking, so `endSession(); flush();` is the guarantee; (3) ABI considerations for the `SHARED` library. Pending first reviewer (codex) pass on these C++-specific deltas.

### Revision 1 — post-codex round 1 (2026-06-03)
Codex confirmed R1–R3 against source and reviewed the C++-specific deltas: 6 findings (2 High, 4 Medium), all verified and incorporated:
- **High (ABI):** `Tracker` is **not** pImpl — `Options options_`, mutexes, `std::thread`, `sqlite3*` are exposed in the public header (~`202-257`) and `Options` is passed by value (~`37`). Appending to `Options`/`Tracker` **breaks ABI** for the SHARED lib. → Removed the "append-only, minor bump" framing; now an explicit decision between **major (4.0.0, recompile)** or a **pImpl refactor (keep minor)**.
- **High (start race):** `startSession` stays detached (~`576`), so even a same-run `endSession(); flush();` can `404` before the start lands. → A `404` carrying the structured `session_not_found` error is non-terminal — retain + re-deliver as `sdk_recovery` (create-on-recovery resolves it); never drop. (Round-2 refinement: scoped to `session_not_found`, not any bare `404`, so a wrong URL / missing route / old server isn't retained forever.)
- **Medium:** `HttpClient::post_json` hardcodes a 10s timeout (~`100-101`); no per-request override. → Added as a prerequisite (timeout overload) before `shutdown_flush_timeout_seconds` is enforceable.
- **Medium:** no `curl_global_init`/`cleanup` ownership. → Promoted from open question to implementation requirement.
- **Medium:** `~Tracker()` has no top-level `try/catch`; destruct-time JSON/SQLite/HTTP risks `std::terminate`. → Required the pending-end path to swallow all exceptions.
- **Medium:** `busy_timeout` is per-connection (existing `db_` has 5s, ~`1262`), not per-table. → Clarified: either accept a bounded ≤5s destruct write on the existing connection (recommended — flush thread already joined) or a second connection with explicit same-file single-writer handling.

### Revision 2 — owner decision: accept ABI break (2026-06-03, CONVERGED)
The single load-bearing open item from round 1 — the ABI/versioning choice — was resolved by the project owner: **breaking changes are acceptable for the C++ SDK.** So the design adds state directly to `Options`/`Tracker` (no pImpl refactor) and ships as **4.0.0** (recompile required). With that decided, the remaining round-1 findings are all *specified mechanics* (HttpClient timeout overload, `curl_global_*` ownership, no-throw destructor path, per-connection `busy_timeout` handling) — not open design questions. The inherited server/lifecycle design was already converged on the .NET sibling. No architectural questions remain.

**Materiality check:** PASS — genuine durability/correctness feature ported to C++, not a reorganization. Status → READY-TO-IMPLEMENT.

### Revision 3 — post-codex round 2 (2026-06-04, CONVERGED)
Final confirmatory pass. Codex verified ABI/versioning is now internally consistent (4.0.0, direct state, recompile — coherent across the doc) and that the 404→recovery path can't enable cross-tenant abuse (server gates create-on-recovery on auth-derived tenant/api_key + start validation). 2 findings (1 Medium, 1 Low), both precision, incorporated; "Everything else looks ready":
- **Medium:** scope the non-terminal handling to a structured **`session_not_found`** 404, not any bare `404` — a wrong `api_base_url` / missing route / un-upgraded server also returns 404 and must NOT be retained forever as a recovery. → Reworded throughout.
- **Low:** stale open-question on the `shutdown_flush_timeout_seconds` default (already specified as 2). → Removed.

