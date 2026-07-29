# pubsub_itc_fw — Session Log

This file contains the full development session narrative extracted from
`pubsub_itc_fw_summary.md`. It records what was built, fixed, or investigated in
each session and is the primary source for "how did we get here" questions.

For current project state see `pubsub_itc_fw_summary.md`.
For design documentation see `docs/index.md`.

---

## Development Sessions

This project has been developed incrementally across a series of numbered work sessions. Each session focused on a specific area — a new feature, a bug fix, a performance investigation, or an infrastructure improvement. Many sections of this document refer to sessions by number (e.g. "session 16", "session 25") to indicate when a piece of work was completed or when a particular design decision was made. The full narrative of each session is recorded in the [Session Accomplishments](#session-accomplishments) section below, which forms a detailed change log.

---

## Session Accomplishments

### Session 27 (current)

**Catch-up entry covering all work since the MEP/TAP design session (26).** This spans the
completion of the Matching Engine HA work, the HA naming/architecture corrections and auth
fan-out fix, two Python test drivers, the Doxygen clickable architecture map, and the start
of a requirements-first pass on pub/sub. `pubsub_itc_fw_summary.md` holds the authoritative
per-item detail; this entry is the narrative index.

**Matching Engine HA (summary item 19) — DONE and verified.** Implemented as four slices:
role config + second ME instance; a book-replication channel (ME-primary streams `BookUpdate`
PDUs to ME-secondary); arbiter-mediated promotion; and WAL reconciliation
(`MePositionRequest`/`MePositionAck`) followed by cancel-on-failover, with the leader
sequencer promoting its standby connection so sequenced orders re-route to the promoted ME.
Two coupling bugs were fixed along the way: the arbiter is now keyed by
`(component-group, instance_id)` so the sequencer/ME/MEP pairs no longer alias onto shared
`{1,2}` slots (`dddb415`), and catch-up is served only by the leader sequencer, not a
follower (`b6451d8`). Verified by `ha_test.py` scenario 16, a live perf run through a
failover, and an orders-in-flight-during-the-gap run (~15,000 orders WAL-committed during the
gap and all replayed to the promoted ME — none dropped). Halt-on-failure remains the fallback
for irreconcilable failure modes.

**HA naming / architecture corrections.** Established the convention that `_primary/_secondary`
applies only to arbiter-elected components; `_a/_b` to active/active caller-selected
components; and no suffix to singletons. Applied it: the gateway became `order_gateway` (a
singleton — external FIX clients connect by unicast, so it cannot be a leader/follower pair),
and the authentication service became an active/active `_a`/`_b` pair. Fixed a real gap where
the admin service only notified one auth instance: `AuthServiceClient` now fans
credential updates to both instances best-effort (`171fac7`), live-verified (both auths logged
Set/RemoveCredential success for a test compID), and `ha_test.py` gained scenario 17
(auth-instance failover). Also reworded a misleading sequencer log line: an order with no ME
connected is WAL-committed with forwarding deferred, not "dropped" (`6fb31d8`).

**Python test drivers (summary items 15 and 17) — DONE.** `fix_client_smoke_test.py` drives
the fix-test-client REST API through a NOS/cancel/settle sequence and validates the blotter
(stdlib-only, pylint 10/10). `fix_client_burst_test.py` runs a large burst with WAL
replication active and scans the sequencer logs for distress — verified 50,000 orders at
~34,500 orders/s with zero drops and no slab exhaustion.

**Doxygen clickable architecture map (summary item 18) — DONE, committed `f01fc7d`.** Added
a clickable component-topology map to the Doxygen HTML: `docs/architecture.dot` is a digraph
whose nodes carry `URL="\ref <page-label>"`, embedded via `\dotfile` in
`docs/architecture_map.dox`, producing a client-side image map over the rendered PNG — click
any component box to open that component's documentation. The map links **directly to the
existing markdown docs** via their Doxygen-generated page labels
(e.g. `md_docs_2applications_2matching__engine`), so no per-component stub pages were needed
and the markdown stays pristine for GitHub. A new **System Architecture** section on the
mainpage links the map and a maintenance guide (`docs/architecture_map_howto.dox`) that
documents the `URL="\ref"` mechanism, the label-encoding rule, and an "adding a new component"
recipe. Four Doxygen warnings were fixed so the docs build cleanly under the existing
`WARN_AS_ERROR = FAIL_ON_WARNINGS` (setting unchanged): two unresolved `#anchor` links in
`authentication_service.md`, a `DESIGN.md` link outside the Doxygen input in
`fix_test_client.md`, and an undocumented `logger` constructor parameter in
`TlsRawBytesProtocolHandler.hpp`. Verified: `doxygen Doxyfile` exits 0 with 0 warnings.

**Pub/sub (summary item 7) — requirements-first pass started, no code.** Chosen as the next
long-pole item ahead of Prometheus. The existing `docs/design/mep_tap.md` (from session 26) is
a *solution* document; it is being treated as a **draft to challenge, not a settled baseline**.
The requirements are to be grounded in a reference system (a production pub/sub the author has
operated, with known problems and sub-optimal solutions but which works, so it provides real
requirements guidance).
Started `docs/design/pubsub_requirements.md` — a requirements note kept separate from the
solution doc — scaffolded with the framing and six open questions: (1) what's published and at
what granularity; (2) delivery & ordering guarantees; (3) durability & replay; (4) fanout &
backpressure; (5) failover semantics; (6) subscriber lifecycle & identity. Next session
resumes by capturing how the reference system's publisher (the MEP analog) works.

---

### Session 26

**MEP and TAP — design session. Full design in `docs/design/mep_tap.md`.**

Two new application components designed. No code was written this session; the entire output is the design document and the decisions recorded here.

**Motivation.** The system mirrors a production exchange. Two categories of downstream consumers exist that the existing WAL-follower-per-component pattern cannot serve cleanly at scale: a market data component (needs the order stream and ER stream) and TAP (Trade Activity Publisher, which publishes orders to an enterprise bus and maintains an L3 order book). With two named subscribers needing the same event streams, topic-based pub/sub is now justified.

**MEP (Matching Engine Publisher) — `applications/matching_engine_publisher/`.**

MEP is the topic publisher. It is a WAL follower on the sequencer (pure network protocol, separate TCP connection to the sequencer's new external WAL subscriber listener on ports 7030/7031; MEP is on its own machine and never touches the sequencer's WAL files). It maintains its own WAL of received records and fans them out to connected topic subscribers. It has no business logic; it routes by `pdu_id` without decoding payloads.

Two topics:

| Topic | PDU IDs | Subscribers |
|---|---|---|
| `orders` | 1000 (NOS), 1001 (OCR) | TAP, market data |
| `execution_reports` | 1002 (ER) | TAP (internal L3 book), market data |

Delivery uses `TopicPage` PDUs (id=109) with **page X of Y** pagination (`page_number`, `total_pages` fields). `total_pages` is calculated at the start of each delivery cycle and held fixed for that cycle. During catch-up a subscriber sees "page 3 of 47"; during live delivery it sees "page 1 of 1". Subscriber sends `TopicAck` (id=110) after each page; MEP sends the next page immediately if `page_number < total_pages` (catch-up), or waits for new records (live). `TOPIC_PAGE_SIZE = 16` records per page. Each `TopicRecord.payload` is variable-length (`bytes` type), carrying exactly the raw bytes from the WAL record — no fixed size limit.

MEP WAL truncation is gated on the minimum cursor across all *currently connected* subscribers, combined with a configurable minimum retention window (e.g. 24 hours). Cursor anchors are not retained for disconnected subscribers — the open subscriber API makes that untenable, as an anonymous subscriber that connects once and disappears would block truncation indefinitely. Subscribers are responsible for persisting their own cursor; MEP logs a warning and serves from its oldest available record if the cursor has been truncated past. A persistently-slow connected subscriber is disconnected when its lag exceeds a configurable per-subscriber threshold.

**The topic subscriber API is open.** Any process that can reach MEP's listener ports and speaks the topic protocol may subscribe without pre-registration or configuration change to MEP. `subscriber_id` is a logging label, not a registry key.

**Failover is transparent to subscriber application code.** `TopicSubscriberChannel` (a new class in `libraries/pubsub_itc_fw/`) is the client-side library that all subscriber applications link against. It owns connection management, endpoint selection, reconnect, and leader discovery. On MEP primary failure the channel detects the drop, tries the primary (fails), tries the secondary (now leader), sends `TopicSubscribeRequest` with the last acked cursor, and resumes delivery — all without any involvement from the application. The passive MEP secondary replies with `TopicNotLeader` (new PDU id=111) to any `TopicSubscribeRequest`, giving the channel an immediate redirect signal rather than a connect timeout.

MEP is a primary/secondary HA pair. Both instances independently follow the sequencer WAL; each maintains its own WAL. Only the leader serves topic subscribers.

**All direct topic subscribers are C++.** Low-latency is the reason any application connects directly to MEP rather than consuming via the enterprise bus. There is therefore no standalone client library, no C API, and no other-language mechanism. Applications needing downstream consumption without low-latency requirements subscribe to the enterprise bus via TAP (Kafka, Pulsar, or equivalent consumers in any language).

The framework provides two subscriber-side components in `libraries/pubsub_itc_fw/`:
- `TopicSubscriberChannel` — for applications that are already `ApplicationThread` subclasses (TAP, market data). Delegates into the owning thread's event callbacks.
- `TopicSubscriberThread` — a pre-built `ApplicationThread` subclass for external C++ applications that do not want to write their own. Accepts a record callback and a cursor-persistence callback; the caller instantiates it, registers it with a `Reactor`, and calls `reactor.run()`. No subclassing required.

**TAP (Trade Activity Publisher) — `applications/tap/`.**

TAP subscribes to **both** MEP topics. It maintains an L3 order book (all live orders tracked individually) and publishes order events to an enterprise bus. TAP does NOT publish ERs to the enterprise bus; it uses ERs internally to know when an order is filled, so it can remove it from the L3 book after the enterprise bus acknowledges receipt of the corresponding order event. This mirrors the behaviour of the equivalent component in the reference system.

The enterprise bus is abstracted behind a `BusPublisher` pure virtual interface. `StubBusPublisher` (used for framework validation) logs and counts. `KafkaBusPublisher` and `PulsarBusPublisher` are concrete implementations compiled in when `USE_KAFKA=ON` or `USE_PULSAR=ON` respectively; the enterprise bus has not been decided. TAP persists its cursor for each topic in a small local file so restarts and failovers are gap-free.

TAP is a primary/secondary HA pair with the same arbiter-mediated election as all other component pairs.

**`TopicPage` uses `list<TopicRecord>`.** The `page_number`/`total_pages` (X of Y) fields bound the delivery semantics at the protocol level — the subscriber always knows exactly how many records are in a page and where it falls in the batch. `list<TopicRecord>` is already supported by the DSL generator; no extension is needed. The `array<MessageType>[N]` DSL extension remains a worthwhile future framework improvement but is not on the critical path for MEP or TAP.

**New DSL files.**

`applications/topics.dsl` (new): defines the framework topic pub/sub protocol PDUs (ids 107–110: `TopicSubscribeRequest`, `TopicSubscribeAck`, `TopicPage`, `TopicAck`; inner type `TopicRecord`).

`applications/leader_follower.dsl` additions: `WalSubscribeRequest` (id=105) and `WalSubscribeAck` (id=106) for the MEP→sequencer subscription handshake.

**Sequencer changes (slice 10).**

New external WAL subscriber listener (ports 7030/7031). Handles `WalSubscribeRequest`; streams `WalRecord` to external subscribers; tracks per-subscriber cursor; WAL truncation updated to `min(all_subscriber_cursors, snapshot_anchor)`. `SequencerWal` extracted to a shared `Wal` class reused by MEP.

**New ports:** 7030–7047. See port table in `docs/mep_tap_design.md`.

**Implementation order:** `topics.dsl` → sequencer slice 10 (including `Wal` extraction) → MEP → TAP.

---

### Session 25

**ApplicationThread: BackoffWithYield replaced with eventfd-based blocking.**

Root-cause analysis established that `BackoffWithYield` tier-3 (entered after ~20ms of queue inactivity) calls `sleep_for(microseconds(10))`, which on this machine (CONFIG_HZ=1000, CONFIG_HIGH_RES_TIMERS=y) actually sleeps ~65µs. With the order pipeline passing through five ApplicationThread hops (OrderGatewayThread → SequencerThread → MatchingEngineThread → SequencerThread → OrderGatewayThread), each hop could be sleeping ~65µs when a message arrived, contributing ~325µs of avoidable overhead. ITC latency measured from heartbeat timer pairs in the logs confirmed ~140µs average wakeup time per hop when the thread was in tier-3 sleep.

Fix: each `ApplicationThread` now owns a non-blocking `eventfd` (`notify_fd_`). A new public `enqueue(EventMessage)` method enqueues to the MPSC queue and then writes 1 to `notify_fd_`. `run_internal()` drains the queue in a tight loop; when the queue is empty it calls `epoll_wait(notify_fd_, timeout=1s)` rather than spin-sleeping. The 1-second timeout is a safety net only — normal wakeup is immediate (kernel delivers the eventfd signal in <1µs). `shutdown()` also writes to `notify_fd_` so the thread exits promptly rather than waiting for the timeout. All 15 producer call sites across `Reactor.cpp`, `InboundConnectionManager.cpp`, `OutboundConnectionManager.cpp`, `PduParser.cpp`, `RawBytesProtocolHandler.cpp`, and `TlsRawBytesProtocolHandler.cpp` were updated from `thing->get_queue().enqueue(msg)` to `thing->enqueue(std::move(msg))`. All unit and integration tests pass.

**CPU registry bug: all processes claiming the same CPU set.**

Observed in logs: every process — matching engine (CPU 1, 2, 3), sequencer primary (CPU 1, 2, 3), sequencer secondary (CPU 1, 2, 3), order gateway (CPU 1, 2, 3) — was pinned to identical CPUs, completely defeating the purpose of the cross-process registry.

Root cause: `/dev/shm/pubsub_cpu_registry` was a stale file from a previous run with `active_entry_count = 256 = MAX_SYSTEM_CORES`. All 256 entries had `process_id = 0` or `process_id = -1`. `kill(0, 0)` sends to the calling process group (always returns 0) and `kill(-1, 0)` sends to all processes (also always returns 0), so every stale entry appeared alive. `get_available_cpu_ids()` therefore found no free CPUs — but that is not what the logs showed. The real mechanism: because none of the stale `core_id` values matched any valid CPU (they were all 0 or -1 from zero-initialised memory), the "busy" check was never triggered for CPUs 1–31, so `get_available_cpu_ids()` returned all of CPUs 1–31 as free. Every process then selected 1, 2, 3 (the first three). When they tried to write their claims to the registry, `active_entry_count >= MAX_SYSTEM_CORES` caused `claim_cpus()` to silently break from the write loop without recording anything. The next process found the same state and claimed the same CPUs.

Fixes:
- `CpuPinning.hpp` `get_available_cpu_ids()`: skip entries with `process_id <= 0` before the `kill()` check.
- `CpuRegistry.cpp` `claim_cpus()`: at the start of the flock-protected section, compact all entries whose PID is <= 0 or no longer alive (reuses the same `kill()` check), resetting `active_entry_count` before the availability scan. This prevents a full but corrupt table from blocking all future claims.
- `devenv.py` `cmd_start()`: delete `/dev/shm/pubsub_cpu_registry` (later moved — see below) on every start so each run begins with a clean registry.
- `deploy.py`: compute both `shared_reactor_cpu_registry_shm_path` and `shared_reactor_cpu_registry_lock_file` from `install_dir / "run"` and inject into the template namespace, overriding any env-TOML value. Creates `install_dir/run/` if it does not exist. This moves both registry files to under the install directory (principle: all runtime state under the install tree for container-readiness).
- `devenv.py`: updated to delete from `install_dir/run/pubsub_cpu_registry` and to `mkdir install_dir/run/` if absent.

Result after fix: matching engine on CPUs 10–12, sequencer primary on CPUs 13–15, sequencer secondary on CPUs 16–18, order gateway on CPUs 19–21. All processes on independent CPU sets.

**Partial — cpu_registry_shm_path not yet configurable from TOML.** `deploy.py` injects the correct absolute path for the lock file via the template namespace. However `ReactorConfiguration::cpu_registry_shm_path` still defaults to `/dev/shm/pubsub_cpu_registry` in C++ and is not yet read from the TOML. Making it configurable requires adding the field to six application `*Configuration.hpp` / `*ConfigurationLoader.cpp` / `*.cpp` wiring files and to all nine TOML templates. Deferred to next session.

**Blotter scrolling fix (fix-test-client GUI).**

Problem: 12 orders placed but only 9 visible; no auto-scroll.
- `style.css`: `max-height: 50vh` → `max-height: calc(100vh - 340px)` with `min-height: 100px`. The viewport-relative calculation uses the actual remaining height below the form controls rather than a fixed half-screen cap.
- `messages.html` `renderBlotter()`: added `wrap.scrollTop = wrap.scrollHeight` after populating the table body. The blotter now auto-scrolls to the most recent row after each poll update.

**Latency measurements and WAL jitter analysis.**

Before the eventfd fix (all processes sharing CPUs 1–3): gateway internal latency ~520–660µs. After the eventfd fix with broken pinning (still all sharing): ~600–700µs (similar; the CPU competition obscured the gain). After both fixes (dedicated CPUs): best observed ORD-010 at **389µs**, most orders 490–690µs.

Remaining variance (389–1769µs) was diagnosed as **WAL replication jitter**, described in detail below.

**WAL replication jitter: root cause, adversarial scenario, and options.**

The sequencer leader (sequencer primary) gates ER emission on two independent events completing in parallel:
1. ER arriving from the matching engine (path: sequencer primary → matching engine → sequencer primary)
2. WalAck arriving from the sequencer secondary after it appends the WalRecord to its own WAL (path: sequencer primary → sequencer secondary → sequencer primary)

The order pipeline stalls until both arrive (`max(ME_round_trip, WAL_round_trip)`). The WAL replication round-trip involves three `epoll_wait` wakeup events (secondary SequencerThread wakeup, secondary reactor wakeup for WalAck send, primary SequencerThread wakeup for WalAck receipt). On a non-realtime kernel each wakeup adds 10–50µs of scheduler jitter. Additionally, the primary SequencerThread handles heartbeat (every 2 seconds) and WAL snapshot (every 30 seconds) timer events from the same queue as WalAck events; if a timer event is queued just ahead of a WalAck it is processed first, adding timer overhead to the order path.

**Option A — decouple ER emission from WalAck (rejected).** Forward the ER to the gateway as soon as it arrives from the matching engine; continue WAL replication asynchronously. This eliminates the WAL round-trip from the critical path, giving a projected 150–250µs consistent latency.

This option is **not safe**. The adversarial scenario: the sequencer primary appends WAL record N, sends WalRecord N to the secondary (in transit, ~100–200µs), receives the ER from the ME, and forwards it to the gateway immediately. The primary then crashes before the WalRecord reaches the secondary. The secondary promotes to leader; its WAL ends at N−1; its `next_sequence_number` is therefore N. Two consequences:

1. **Sequence number N is reused.** The new leader assigns seq N to the next order that arrives after failover. Two completely different orders from different FIX clients now carry seq N in the system. Any downstream consumer that uses seq N as a unique record identifier finds a collision.
2. **The ER for the original order N cannot be routed after failover.** The routing table (seq_no → FIX session conn_id) is rebuilt from WAL replay. The new leader's WAL ends at N−1, so there is no routing entry for seq N. When the matching engine sends back the ER for the original order N (which the ME did execute), the new leader cannot route it to the FIX client. The trade happened; the client never receives the fill notification.

The secondary has no way to know record N existed. It cannot ask the primary (dead) and has no other source of truth.

**Option B — prioritise PDU events over timer events in SequencerThread (planned, not yet implemented).** Keep the WalAck gate intact (correctness preserved). Change the SequencerThread's event drain loop so that `FrameworkPdu` and connection events are drained to exhaustion before timer events are processed. This prevents a heartbeat or snapshot timer from adding its processing time to the WalAck path when both arrive in the queue simultaneously. Reduces worst-case latency spikes without touching the replication semantics.

Status: to be implemented next session.

---

### Session 24

**`ApplicationAnnouncer` — startup one-liner in each application.**

Every application now logs a single structured line at `Info` level as soon as its logger is ready:

```
sequencer: version=0.1.0 pid=12345 built=2026-06-02T10:14:07Z branch=main sha=654da89 host=seq-primary
```

**CMake build info generation.** Three new files:

- `cmake/BuildInfo.hpp.in` — template for the generated header; substitution variables are `@PROJECT_VERSION@`, `@GIT_SHA@`, `@GIT_BRANCH@`, `@BUILD_DATETIME@`.
- `cmake/GenerateBuildInfo.cmake` — cmake -P script run at build time. Queries `git rev-parse --short HEAD` and `git rev-parse --abbrev-ref HEAD`; stamps UTC datetime with `string(TIMESTAMP ... UTC)`; writes `${CMAKE_BINARY_DIR}/generated_build_info/pubsub_itc_fw/BuildInfo.hpp` using `copy_if_different` so an unchanged git state does not force recompilation.
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ApplicationAnnouncer.hpp` — header-only static class. `announce(QuillLogger& logger, const std::string& app_name)` calls `gethostname` + `getpid`, then `PUBSUB_LOG` at `Info` level with all fields.

`CMakeLists.txt` gains a `build_info_generated ALL` custom target (runs before `add_subdirectory(libraries/pubsub_itc_fw)`) and a `BUILD_INFO_INCLUDE_DIR` cache variable. `libraries/pubsub_itc_fw/CMakeLists.txt` adds `build_info_generated` to `add_dependencies` and `$<BUILD_INTERFACE:${BUILD_INFO_INCLUDE_DIR}>` to public include paths, so all downstream targets get `BuildInfo.hpp` transitively.

**Call sites in `main()`.** Two patterns:

- *Pattern A* (sequencer, arbiter, matching_engine, witness): announce immediately after `make_unique<QuillLogger>`, before config load. Logger is at its initial `Info` level; the announcement is always visible.
- *Pattern B* (authentication_service, order_gateway): announce after `load_and_init_logging` + `set_log_level` + `set_syslog_level`. Logger is fully configured before the announcement fires.

**Version management.** The semantic version is the `VERSION` field in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`. This is the source of truth; `release.py` already reads it by regex when naming the artefact, and the new `build_info_generated` target passes it to `GenerateBuildInfo.cmake` via `-DPROJECT_VERSION`. Release workflow: bump the version in `CMakeLists.txt`, commit, tag `v{version}`.

*Future enhancement:* derive the version from `git describe --tags --match 'v*'` at build time so the tag is the single source of truth. Preferred approach would be to examine the tag the current commit is reachable from (i.e. the nearest ancestor tag, resolving ambiguity via commit distance). Deferred for now because `git describe` returns nothing before the first tag exists and produces a non-semver suffix (`v1.2.3-5-gabc1234`) between releases; the manual bump is simpler and fully auditable.

**`ct.sh` — clang-tidy script hardened.**

`ct.sh` is the project's static analysis driver. Several issues were found and fixed:

- **DSL headers missing.** `ct.sh` wipes `build/` before running `sca.py`, but `sca.py` needs the generated DSL headers to compile. Fixed by adding explicit generator invocations before `sca.py`: `build/generated_dsl/` (three app-level headers: `fix_equity_orders.hpp`, `authentication.hpp`, `leader_follower.hpp` with namespace `pubsub_itc_fw_app`) and `build/libraries/pubsub_itc_fw/dsl/` (three framework-internal headers with namespace `pubsub_itc_fw`). `build/generated_dsl` and `build/libraries/pubsub_itc_fw/dsl` added to `includes.txt`.

- **`BuildInfo.hpp` missing.** `ApplicationAnnouncer.hpp` includes `pubsub_itc_fw/BuildInfo.hpp` which is generated at CMake build time. Fixed by running `cmake/GenerateBuildInfo.cmake` via `cmake -P` in `ct.sh`, extracting the version from `CMakeLists.txt` with `sed`. `build/generated_build_info` added to `includes.txt`.

- **toml++ not in include path.** `TomlConfiguration.cpp` includes `<toml++/toml.hpp>`. Added `/home/marlowa/mystuff/thirdparty/installed/tomlplusplus/3.4.0/include` to `includes.txt`.

- **`compile_commands.json` left in project root by `sca.py`, breaking Clangd.** `sca.py` writes `compile_commands.json` to the current directory after each batch. Clangd finds this file before the CMake-generated one in `cmake-build-debug/` and loses standard-header resolution. Fixed by deleting it at the end of `ct.sh`. A `.clangd` config file pointing Clangd at `cmake-build-debug/` was also added as a fallback for interrupted runs.

- **`thirdparty/` `.cpp` exclusion bug in `sca.py`.** `is_directory_excluded()` built the check pattern as `/{name}/` but paths from `os.walk` start with `./`, so `./thirdparty/` never matched `/{name}/`. Fixed by normalising the path (stripping the leading `./`) before the check and adding a `startswith(exclusion_dir + '/')` guard.

- **Thirdparty header violations in output.** Even with `.cpp` exclusion working, clang-tidy follows `#include` into thirdparty headers and reports violations there. The `/usr/include/` filter already existed in `tidy_up_clang_output`; added a matching `/thirdparty/` filter. Also added `--header-filter` support to `sca.py` (new `--header_filter` argparse argument, passed to clang-tidy), with `ct.sh` passing `$(realpath .)` so the project's real path (not the symlink path from `pwd`) is used.

**Clang-tidy fixes (session 2026-06-03/04).** After the SCA tooling was stabilised, a full `ct.sh` run was performed and several violation categories were fixed: `misc-const-correctness` (41 violations — variables that are never modified after initialisation), `hicpp-uppercase-literal-suffix` (25 — `u`/`ull` → `U`/`ULL`), `modernize-use-nodiscard` (43 — added `[[nodiscard]]` to accessor functions), `modernize-loop-convert` (1), `modernize-use-default-member-init` (10 — moved constructor initialisers to member defaults), `readability-redundant-casting` (12 — removed redundant `static_cast<int16_t>(message.pdu_id())`), `google-readability-braces-around-statements` (104 — 1TBS brace enforcement across 18 files). Build and all tests confirmed clean after each batch.

**HA scenario 15 — arbiter-mediated election (item 3 closed).** Added `VerifyStep` NamedTuple to `ha_test.py` enabling explicit intermediate log-marker checks mid-scenario. Scenario 15 (`arbiter_mediated_election`) verifies the full `ArbitrationReport` → `ArbitrationDecision` PDU exchange: secondary sends report (5.8 s after kill, within 15 s timeout), arbiter sends decision (0.0 s), secondary receives it (0.0 s), transitions to leader (0.0 s). Recovery orders flow. PASS confirmed.

**`RestoreCredentialRequest` PDU 514/515 (item 9 closed).** DSL messages added to `authentication.dsl`; `handle_restore_credential_request()` implemented in `AuthenticationThread` (decodes pre-derived binary SCRAM fields, validates 32-byte key sizes, installs into credentials map, persists). `AuthServiceClient.restoreCredential()` added in Java (hex→binary conversion; sends PDU 514, validates PDU 515 reply). `CompIdHandler.update()` calls `restoreCredential` when transitioning from disabled/locked → enabled+unlocked; `FirmHandler.update()` calls it for all enabled+unlocked comp_ids when a firm is re-enabled. Template credential-lifecycle warnings removed; README table updated. `auth_service_test.py` scenario 6 (`restore_credential_revoke_and_restore`) added and passes: baseline auth → PDU 512 remove → verify UnknownUser → PDU 514 restore → verify Granted again.

**FIX message capture (item 10 closed).** `FixCapture` class (`applications/order_gateway/FixCapture.hpp/cpp`): mutex queue + background writer thread; `capture(Direction, data, size, timestamp_ns)` on the gateway thread; no file I/O on hot path. Record format (LE): `uint32_t size | int64_t timestamp_ns | uint8_t direction | bytes`. Three capture points: inbound bytes (post-parse, consumed messages only), `send_fix_to_session`, ER encoder path. Config: mandatory `[fix_capture] enabled / file / queue_depth`. `FixCaptureTest.cpp` C++ unit tests (6 tests covering format, direction bytes, ordering, truncation, flush-on-destruction). `fix_capture_test.py` system test: starts auth_service + gateway with capture enabled, runs a Python FIX session (Logon/3×NOS/Logout), validates capture file contents end-to-end. `read_fix_capture.py` utility for inspecting capture files in production.

**`build.py` install directory fixed.** `staging_dir` was `(build_dir / "installed").resolve()` — install location varied with `--build-dir`, causing confusion when test scripts defaulted to `installed/`. Changed to `(source_dir / "installed").resolve()` so installs always land in the project-root `installed/` regardless of build directory.

**DSL codegen `ALL` targets.** `pubsub_itc_fw_dsl`, `fix_equity_orders_generated`, `leader_follower_generated`, `authentication_generated` all given `ALL` so every Python-based code generation step runs at the very start of the build before any C++ compilation begins.

**`auth_service_test.py` admin section fix.** The `[admin]` section (`listen_port`, TLS cert/key paths) is required by the auth service config loader. Previously omitted for non-TLS scenarios, breaking scenarios 1–3 against freshly built binaries. Fixed: a cert is always generated and the admin section is always written, regardless of `tls_mode`.

**FIX protocol version recorded.** BeginString must be `8=FIXT.1.1` (FIX 5.0 SP2 / FIXT 1.1), not `8=FIX.4.2`. The gateway enforces this with an immediate disconnect on preamble mismatch. Rule added to `coding-rules-for-ai-chatbots.txt` and the project memory.

**Hex-dump trace logging (item 4 closed).** All per-PDU hex dump log calls moved from `FwLogLevel::Debug` to `FwLogLevel::Trace`. Expensive string constructions (`hex_dump()` calls, the `hex_bytes` loop in `PduParser::dispatch_pdu`) are wrapped in `if (logger_.log_level() <= FwLogLevel{FwLogLevel::Trace})` guards so they are never evaluated in production (which runs at Info or Debug). Non-hex trace calls (decoded header fields, slab allocation counts) changed to Trace level but not guarded — they are integer format args with negligible evaluation cost.

---

### Session 23

**HA scenario 10 (`me_death_restart`) fixed — all 14 HA scenarios now pass.**

The scenario kills and restarts the matching engine during a live order run. The previous failure: `_ME_READY_MARKERS` fires at ~0.3 s when the secondary sequencer connects to the restarted ME; the primary sequencer only re-establishes its own ME connection at ~2.1 s. Phase 5 orders arriving in that gap were forwarded to ME over an invalid (unestablished) connection and dropped — approximately 21,000 drops.

Two-part fix in `ha_test.py`:
1. Capture `seq_primary_pos_pre_kill = file_end(seq_primary_log)` before Phase 4 (the kill/restart phase).
2. After Phase 4, when the scenario includes a ME restart, poll `seq_primary_log` from that position for the log message `"matching engine order connection established"` (10 s timeout) before proceeding to Phase 5.
3. In Phase 5, if ME was restarted, stop and restart the FIX session — the existing FIX session was blocked waiting for dropped in-flight order responses and would never drain.

**`devenv.py` — developer sandbox management script.**

New root-level script for starting, stopping, and monitoring all components on a dev machine.

- Subcommands: `start`, `stop`, `status`, `restart [name]`
- `--env PATH` selects the environment TOML (default: `environments/dev.toml`); `--no-ha` skips `ha_only=true` components; `--delay SECONDS` controls inter-start sleep.
- Components are launched in the order listed in `[startup_order]`, shut down in reverse order.
- Binary processes: `[binary_path, log_file, config_path]`, cwd = `install_dir/workdir`.
- JAR processes: `["java", "-jar", jar_path]`, cwd = `install_dir/workdir`. stdout/stderr go to `log_dir/<name>.stdout`.
- PID files: `run_dir/<name>.pid`. Stale PIDs cleaned on stop.
- Exports credentials (runs `db/export_credentials.py`) before starting any component; re-exports when restarting an auth service instance.

**`release.py` — versioned deployment artefact assembly.**

New root-level script that packages the build output into a `pubsub-<version>-<git-short-hash>.tar.gz`.

- Version read from `project(...VERSION x.y.z...)` in `CMakeLists.txt` via regex.
- Git hash from `git rev-parse --short HEAD`; `--no-git-hash` omits it.
- Deployment binaries derived from `[startup_order]` + `[components]` in the env TOML (set deduplication handles the shared `arbiter` binary).
- Staged layout: `bin/` (deployment binaries only; test/bench binaries skipped), `lib/` (`libpubsub_itc_fw.so` + jars from env TOML), `etc/` (config templates from `applications/`, not the installed tree; `credentials.toml` excluded), `db/` (Liquibase changelog + scripts), `environments/` (all `.toml` files), `devenv.py`, `deploy.py`, `release.json` (version, git hash, build timestamp).
- `--install-dir`, `--env`, `--version`, `--output-dir`, `--no-git-hash` options.

**Config template system — all 9 component TOML files converted.**

Component TOML files in `applications/` are now templates with `${placeholder}` syntax (Python `string.Template`). Placeholder names are the full flattened TOML path of the substitution value — for example `[arbiter_primary] peer_host` → `${arbiter_primary_peer_host}`. This avoids cryptic abbreviations.

**How to trace a placeholder back to its definition.**

When `${some_placeholder}` appears in an application TOML template (e.g. `applications/matching_engine/matching_engine.toml`), its value can be traced as follows:

1. Split the placeholder on the *first* underscore boundary that matches a section name. The part before the first `_` is the TOML section; everything after is the key within that section. In practice: `shared_reactor_cpu_pinning_reserve_cpu0` → section `[shared]`, key `reactor_cpu_pinning_reserve_cpu0`.

2. Open the appropriate environment file (e.g. `environments/dev.toml` for the sandbox, `environments/prod.toml` for production). Find the section and key. Example:

   ```toml
   # environments/dev.toml
   [shared]
   reactor_cpu_pinning_reserve_cpu0 = true   # ← this becomes ${shared_reactor_cpu_pinning_reserve_cpu0}
   ```

3. `deploy.py::flatten_toml(env)` recursively walks every section of the env TOML and produces a flat `{key: str}` dict where each key is `section_key` (section name + underscore + key name, with nested sections concatenated the same way). Booleans become `"true"`/`"false"`; lists are skipped. `string.Template.safe_substitute` then replaces every `${...}` in every `etc/**/*.toml` with the matching entry. An undefined placeholder causes a hard exit naming the file and the missing key.

4. A small number of placeholders are **injected programmatically** by `deploy.py` rather than read from the env TOML. Currently these are:

   | Placeholder | Injected value |
   |---|---|
   | `${paths_install_dir}` | The resolved install directory path |
   | `${shared_reactor_cpu_registry_shm_path}` | `<install_dir>/run/pubsub_cpu_registry` |
   | `${shared_reactor_cpu_registry_lock_file}` | `<install_dir>/run/pubsub_cpu_registry.lock` |

   These override any same-named entry that might appear in the env TOML. The `run/` subdirectory is created by `deploy.py` if absent.

**Rule of thumb**: every placeholder you see in an application template is defined either in the substitution section at the bottom of the env TOML (derived mechanically as `section_key`) or injected by `deploy.py` at expansion time. If a grep for the placeholder name finds no definition in the env TOML, look in `deploy.py` around the `namespace["..."] =` lines.

Files converted: `witness/witness.toml`, `arbiter/arbiter.toml`, `arbiter/arbiter_secondary.toml`, `authentication_service/authentication_service.toml`, `authentication_service/authentication_service_secondary.toml`, `matching_engine/matching_engine.toml`, `order_gateway/order_gateway.toml`, `sequencer/sequencer.toml`, `sequencer/sequencer_secondary.toml`.

`environments/dev.toml` extended with substitution sections at the end: `[shared]`, `[witness]`, `[arbiter_primary]`, `[arbiter_secondary]`, `[auth_service_primary]`, `[auth_service_secondary]`, `[matching_engine]`, `[order_gateway]`, `[sequencer_primary]`, `[sequencer_secondary]`. Values are placed in their logically-appropriate section, not a flat `[vars]` block.

**`cpu_pinning_dev_mode` → `cpu_pinning_reserve_cpu0` rename (C++ + TOML).**

`cpu_pinning_dev_mode` was ambiguous — "dev" could mean "development mode" generally. The flag means "exclude CPU 0 from pinning candidates (leave it for OS/interrupt use)"; `cpu_pinning_reserve_cpu0` says this precisely.

Renamed across 24 files:
- C++ struct field: `ReactorConfiguration::cpu_pinning_reserve_cpu0` and all six app-level `*Configuration` structs.
- C++ function parameter: `is_dev_mode` → `reserve_cpu0` in `get_available_cpu_ids()` (`CpuPinning.hpp`), `claim_cpus()` (`CpuRegistry.hpp`, `CpuRegistry.cpp`).
- TOML key in all 9 config templates: `cpu_pinning_reserve_cpu0 = ${shared_reactor_cpu_pinning_reserve_cpu0}`.
- `dev.toml` substitution key: `reactor_cpu_pinning_reserve_cpu0 = true` (set to `false` in `prod.toml`).
- `ReactorConfiguration.hpp` doc comment updated to remove "development machines" phrasing.

**`deploy.py` — deployment script.**

New root-level script; included in the release artefact.

Steps performed in order:
1. **Unpack** `--artefact pubsub-<ver>.tar.gz` into `install_dir`, stripping the top-level artefact directory (optional; skip if already unpacked).
2. **Expand templates**: `flatten_toml(env)` recursively flattens the env TOML into a `{key: str}` namespace (booleans → `"true"`/`"false"`; lists skipped), then `string.Template.substitute(namespace)` expands every `*.toml` in `install_dir/etc/`. Undefined placeholder → hard exit with file name and key.
3. **Generate TLS certs**: for each `[tls.*]` section, looks up the matching component's `workdir`, resolves cert/key paths, generates a 2048-bit self-signed RSA cert via `openssl req -x509` (3650-day validity, `/CN=localhost`). Deduplicates pairs sharing the same cert file. Skipped if cert+key already exist (use `--force-certs` to regenerate). Pass `--skip-certs` when deploying real CA-signed certs.
4. **Create database**: delegates to `db/create_db.py` with `[db]` section values. `--drop-db`, `--sudo-postgres`, `--liquibase-contexts` forwarded.
5. **Export credentials**: delegates to `db/export_credentials.py`.

Options: `--artefact`, `--env`, `--install-dir`, `--skip-certs`, `--force-certs`, `--skip-db`, `--drop-db`, `--sudo-postgres`, `--liquibase-contexts`.

**`environments/prod.toml` — production environment configuration.**

Mirrors `dev.toml` structure; every field that requires a real value is marked `# REPLACE`. Key differences:

| Setting | dev | prod |
|---|---|---|
| `paths.install_dir` | `installed` | `/opt/pubsub` |
| `paths.log_dir` | `installed/log` | `/var/log/pubsub` |
| `paths.run_dir` | `/var/tmp/pubsub/run` | `/var/run/pubsub` |
| WAL directories | `/var/tmp/pubsub/sequencer*_wal` | `/var/lib/pubsub/sequencer*_wal` |
| Listen hosts | `127.0.0.1` | `0.0.0.0` |
| Connect hosts | `127.0.0.1` | `*.exchange.internal` (REPLACE) |
| `reactor_cpu_pinning_reserve_cpu0` | `true` | `false` (isolated CPUs) |
| TLS `ca` field | `""` | `"ca.crt"` (REPLACE with CA cert) |
| SCRAM password | `"stubpassword"` | `"REPLACE_WITH_GATEWAY_SCRAM_PASSWORD"` |

Production deploy workflow: edit `prod.toml` with real hostnames, place CA-signed certs, set `PUBSUB_APP_DB_PASSWORD`, then:
```
./deploy.py --env environments/prod.toml \
            --artefact pubsub-<ver>.tar.gz \
            --install-dir /opt/pubsub \
            --skip-certs
```

**`environments/preprod.toml` and `environments/test-1.toml` — additional environment configurations.**

`preprod.toml` is structurally identical to `prod.toml` — same paths, CA-signed TLS, `cpu_pinning_reserve_cpu0 = false` — with hostnames in the `*.preprod.exchange.internal` namespace. Intended for final release validation under near-production conditions before promotion.

`test-1.toml` targets a dedicated test cluster with full HA enabled. Differs from preprod/prod in two ways: `cpu_pinning_reserve_cpu0 = true` (test machines are not CPU-isolated) and self-signed TLS (`ca = ""`, deploy.py generates certs — no CA required). Hostnames in the `*.test-1.exchange.internal` namespace. Additional test environments follow the same pattern (`test-2.toml`, etc.).

Environment comparison:

| Setting | `dev` | `test-1` | `preprod` | `prod` |
|---|---|---|---|---|
| `ha.enabled` | `true` (overridable with `--no-ha`) | `true` | `true` | `true` |
| Topology | single machine | multi-machine | multi-machine | multi-machine |
| `cpu_pinning_reserve_cpu0` | `true` | `true` | `false` | `false` |
| TLS CA verification | none (`ca=""`) | none (`ca=""`) | CA cert required | CA cert required |
| TLS certs | self-signed by deploy.py | self-signed by deploy.py | CA-signed, `--skip-certs` | CA-signed, `--skip-certs` |
| Hostnames | `127.0.0.1` | `*.test-1.exchange.internal` | `*.preprod.exchange.internal` | `*.exchange.internal` |

---

### Session 22

**`auth_service_test.py` — `--tls` flag.** Without `--tls`, the service starts with no TLS admin section and scenarios 1–3 (plain-PDU SCRAM) are the only valid choices. With `--tls`, all 5 scenarios are available; requesting scenario 4 or 5 without the flag exits with an error message. `_wait_for_service_ready` now takes an explicit `marker` argument; `_SERVICE_READY_MARKER_PDU` and `_SERVICE_READY_MARKER_TLS` are separate constants. `_write_test_toml` only emits the `[admin]` TLS section when `tls_mode=True`.

**PostgreSQL database setup and schema (`db/`).**

- `db/create_db.py` — idempotent Python script: (1) creates `pubsub_app` role with `DO $$ IF NOT EXISTS … CREATE ROLE … ELSE ALTER ROLE …`; (2) skips `CREATE DATABASE` if `pubsub` already exists (checked via `pg_database`); (3) runs `liquibase update`. Omits `--host` for localhost connections (peer auth; TCP password prompting avoided). Options: `--pg-superuser <name>` (default `postgres`), `--sudo-postgres`, `--drop-existing`. JDBC driver is expected at `/opt/liquibase/lib/postgresql*.jar`; script checks and exits with instructions if absent — no network downloads (appropriate for corporate environments). Liquibase is invoked with `--search-path=<script_dir>` and `--changeLogFile=changelog/db.changelog-root.xml` (Liquibase 5.x requires changeLogFile relative to searchPath).
- `db/liquibase.properties` — datasource config; password from `${env.PUBSUB_APP_DB_PASSWORD}`.
- `db/changelog/db.changelog-root.xml` — root changelog; property `tablePrefix = pubsub_`; includes `v1_initial_schema.xml`.
- `db/changelog/v1_initial_schema.xml` — three changesets, all using `${tablePrefix}`:
  - `v1-001-create-firm`: `firm_id varchar(32) PK`, `name varchar(255) NOT NULL`, `enabled boolean DEFAULT true NOT NULL`, `created_at/updated_at timestamptz DEFAULT now() NOT NULL`.
  - `v1-002-create-comp-id`: `comp_id varchar(64) PK`, `firm_id FK→firm NOT NULL`, SCRAM fields `stored_key/server_key/salt varchar(64) NOT NULL`, `iterations integer NOT NULL`, `enabled DEFAULT true`, `force_password_change DEFAULT true`, `consecutive_failed_logins DEFAULT 0`, `locked DEFAULT false`, `locked_reason varchar(255)` (nullable), `locked_at/last_login_at/password_changed_at timestamptz` (nullable), `created_at/updated_at NOT NULL`.
  - `v1-003-create-comp-id-gateway-permission`: `comp_id FK→comp_id`, `gateway_type varchar(64)`, `enabled DEFAULT true`, `created_at`; composite PK `(comp_id, gateway_type)`. Gateway type is free text (e.g. `order`, `drop_copy`, `risk`).

**Java admin service (`java/admin-service/`).**

A standalone Javalin 6 + Freemarker 2.3 + plain JDBC (HikariCP) web application. Technology choices: Javalin for HTTP (not Spring — never Spring), Pico.css classless CSS from CDN (superseded — Pico was removed on 2026-07-29 in favour of a bundled hand-written `static/desktop.css`), Freemarker for server-rendered templates, HikariCP + PostgreSQL JDBC for database access, Maven Shade plugin for a fat JAR. Java 17.

*Package layout:*
- `Config` — loads `application.properties` from classpath; overrides `db.url`, `db.username`, `db.password` from env vars `PUBSUB_DB_URL`, `PUBSUB_DB_USERNAME`, `PUBSUB_APP_DB_PASSWORD`. Fields include `tablePrefix`, `authServiceEnabled`, `authServiceHost`, `authServiceAdminPort`, `serverPort`.
- `model/`: `FirmRow`, `CompIdRow` (SCRAM fields present but never rendered in templates), `GatewayPermissionRow`.
- `exception/`: `NotFoundException` (404), `ConflictException` (409).
- `db/`: `Database` (HikariCP factory), `FirmDao`, `CompIdDao`, `GatewayPermissionDao` — plain JDBC, explicit column lists, `PreparedStatement` for all data. `CompIdDao.insert` requires a `ScramCredential`; `updateStatus` sets `enabled/locked/forcePasswordChange/lockedReason` and manages `locked_at` via a SQL CASE expression; `updateCredentials` resets `force_password_change=false`, `consecutive_failed_logins=0`, updates `password_changed_at`.
- `service/`: `ScramCredential` (record: `storedKey`, `serverKey`, `salt`, `iterations` — all hex strings matching the format used in `auth_service_test.py`). `ScramDerivation.derive(password, iterations)` — PBKDF2WithHmacSHA256 → HMAC-SHA256(saltedPassword, "Client Key") → SHA256(clientKey) for storedKey; random 16-byte salt encoded as 32-char hex. `AuthServiceClient.setCredential(compId, password, iterations)` sends `SetCredentialRequest` (PDU 510) over TLS: 24-byte big-endian header (canary `0xC0FFEE00`) + little-endian payload (i64 requestId, string compId, string password, i32 iterations); reads `SetCredentialResult` (PDU 511); validates requestId and outcome. One TLS connection per call; server cert not verified (admin channel, internal network only).
- `web/`: `FirmHandler`, `CompIdHandler`, `GatewayPermissionHandler`. Methods match Javalin's `Handler` interface (`throws Exception`). SCRAM iterations fixed at 4096. On password set: derive SCRAM → write DB → call auth service (when `authServiceEnabled=true`).
- `Main` — Javalin routes and global exception handlers.

*Routes:* `GET/POST /firms`, `/firms/new`, `/firms/{id}`, `/firms/{id}/delete`; `GET /comp-ids` (all or `?firmId=X`); `GET/POST /firms/{firmId}/comp-ids/new`, `/firms/{firmId}/comp-ids`; `GET/POST /comp-ids/{id}`, `/comp-ids/{id}/delete`, `/comp-ids/{id}/password`; `GET/POST /comp-ids/{id}/gateways`, `/comp-ids/{id}/gateways/{type}/delete`.

*Freemarker templates* (under `src/main/resources/templates/`): `layout.ftl` (nav macro; Pico.css at the time, replaced by `static/desktop.css` on 2026-07-29), `error.ftl`, `firms/list.ftl`, `firms/form.ftl` (create and edit via `<#if firm?>` branching), `comp-ids/list.ftl`, `comp-ids/form.ftl` (create and edit), `comp-ids/set-password.ftl`, `gateway-permissions/list.ftl` (list + inline add form).

*Build tooling added to `pom.xml`:*
- `maven-checkstyle-plugin:3.3.1` — bound to `validate` phase; custom `checkstyle.xml` (unused imports, need braces, empty catch exemption for `ignored`-named variables, etc.). Zero violations.
- `spotbugs-maven-plugin:4.8.6.0` — bound to `verify` phase; effort Max, threshold Medium.
- `dependency-check-maven:10.0.4` (OWASP Dependency Check) — **not** bound to the build lifecycle; run manually with `mvn dependency-check:check`. Requires NVD database download or a local mirror; `failBuildOnCVSS=7`.

Note on startup flow: the auth service never accesses the database directly. Before starting the auth service, a separate export script (not yet written — see item 8 in "What Is Not Yet Done") reads `pubsub_comp_id` from the DB and writes SCRAM credentials into the auth service TOML. Live credential changes made through the admin UI are pushed immediately via `SetCredentialRequest` (PDU 510) without restarting the auth service.

Build: `mvn compile` succeeds (17 source files, 0 errors); `mvn checkstyle:check` passes with 0 violations.

---

### Session 21

**Java DSL code generator — Java backend added.**

The DSL front end (lexer, parser, validator, AST) is language-agnostic and shared. Three new files add Java as a second code-generation target alongside C++:

- `python/dsl/generator_java.py` — `JavaGenerator(class_name, package_name)` dataclass. Emits a single public final outer class that acts as a namespace container for one public enum per DSL enum and one `public static final` inner class per DSL message. Wire format is little-endian throughout, matching the C++ generator; `ByteBuffer.order(ByteOrder.LITTLE_ENDIAN)` is applied at the start of every encode and decode call.

  Each message inner class contains: public fields with zero/empty defaults; `public static int encodedSize(MsgType msg)`; `public static int encode(MsgType msg, ByteBuffer buf)` (returns bytes written, or -1 if buffer too small — checked via a single `encodedSize` pre-flight); `public static MsgType decode(ByteBuffer buf)` (saves position, delegates to `_decodeFields`, resets position and returns null on `BufferUnderflowException`); `static MsgType _decodeFields(ByteBuffer buf)` (package-private, throws on underflow — called by nested-message decode paths so exceptions propagate to the outermost `decode()` catch without corrupting the position).

  Type mappings: DSL `char` → Java `byte` (not Java `char` which is 2-byte UTF-16); `i8`→`byte`, `i16`→`short`, `i32`→`int`, `i64`→`long`, `bool`→`boolean`, `datetime_ns`→`long`, `string`→`String`, `bytes`→`byte[]`. Lists map to Java arrays (`T[]`); nested lists give `T[][]` etc. with correct `new T[n][]` allocation syntax. Enums carry a `public final int value` field (or `long` for i64-backed enums), a `fromValue(v)` factory, and `wireSize()` returning the underlying type's byte count.

- `python/tools/generate_java_from_dsl.py` — thin wrapper script mirroring `generate_cpp_from_dsl.py`. Takes positional `input.dsl` and `output.java` arguments (class name inferred from the output file stem) plus `--package com.example.app` for an optional package declaration.

- `python/tests/test_generator_java.py` — 47 tests covering: outer class and package wrapping, all primitive type mappings, `char`→`byte` (not Java char), enums (entries, value, fromValue, wireSize, i64 long type), optional fields with `has_X` pattern, string/bytes fields, list and array fields, nested messages, nested lists, encode capacity check, `BufferUnderflowException` handling, `ByteOrder.LITTLE_ENDIAN`, and the `_decodeFields` / `decode` method split.

Build: `make all` passes pylint 10.00/10, 203/203 tests.

---

### Session 20

**SCRAM-SHA-256 authentication service — full end-to-end implementation.**

The system now requires every FIX client to complete a SCRAM-SHA-256 challenge-response exchange before a FIX session is established. The exchange happens over the internal PDU path (not over the FIX connection), using four new PDU types defined in `applications/authentication.dsl`:

| PDU ID | Name | Direction |
|---|---|---|
| 500 | `AuthenticationRequest` | gateway → auth service |
| 501 | `AuthenticationChallenge` | auth service → gateway |
| 502 | `AuthenticationProof` | gateway → auth service |
| 503 | `AuthenticationResult` | auth service → gateway |

`AuthenticationRequest` carries `request_id` (= the gateway's internal `ConnectionID` for the FIX session, used to correlate all four messages), `comp_id`, and a random 16-byte `client_nonce`. `AuthenticationChallenge` returns `server_nonce` (server-appended bytes concatenated with client_nonce), `salt`, and `iterations`. `AuthenticationProof` carries the SCRAM `ClientProof`. `AuthenticationResult` carries `outcome` (Granted/Denied), `server_signature` (32 bytes, for mutual authentication), and `force_password_change` flag.

**`ScramCrypto` static library** (`libraries/scram_crypto/`). Moved from `applications/authentication_service/` into a proper static library so both the authentication service and the gateway can link against it. Namespace `scram_crypto`. Exports: `hmac_sha256`, `sha256`, `pbkdf2_sha256`, `make_scram_credential`, `compute_auth_message`. Links against `OpenSSL::Crypto` (PRIVATE). `find_package(OpenSSL REQUIRED)` added to top-level `CMakeLists.txt`.

**Authentication service** (`applications/authentication_service/`). Stateless: each SCRAM exchange is fully self-contained with no server-side session state between Request and Proof. On receiving an `AuthenticationRequest`, the service generates a random 16-byte server nonce, derives SCRAM parameters from its stored credential for the comp_id, and replies with `AuthenticationChallenge`. On receiving `AuthenticationProof`, it verifies `ClientProof` via `StoredKey`, computes `ServerSignature` via `ServerKey`, and replies with `AuthenticationResult`. Currently uses a single stub credential (`stub_credential_`) for all comp_ids; credential database integration is the next major work item. Two instances are run for HA: primary on port 7070, secondary on port 7071. Both are stateless and independent — no synchronisation between them is needed.

**Gateway SCRAM integration** (`applications/order_gateway/`). On receiving a FIX Logon:
1. Cancels the logon timeout timer.
2. Selects the auth service connection: primary if connected, secondary as fallback (when `ha_enabled`).
3. If neither is connected, sends FIX Logout and disconnects.
4. Generates 16 random bytes as `client_nonce` (via `RAND_bytes`), sends `AuthenticationRequest`, sets `session.auth_pending = true`, and arms a `scram_auth_timeout` timer (default 10 s, configurable).
5. `handle_authentication_challenge`: derives `ClientProof` and `expected_server_signature` locally using `scram_crypto::pbkdf2_sha256`, `hmac_sha256`, `sha256`. Sends `AuthenticationProof` on the same connection the challenge arrived on (correct for HA routing).
6. `handle_authentication_result`: cancels the SCRAM timeout, verifies `ServerSignature` for mutual authentication, then either completes the FIX Logon reply or sends FIX Logout.
7. SCRAM timeout fires: sends FIX Logout and disconnects — session never hangs.

Key configuration additions to `FixGatewaySeqConfiguration`:
- `authentication_service_host / port` (primary, always required)
- `authentication_service_secondary_host / port` (required when `ha_enabled`)
- `scram_password` — the password sent to the auth service on behalf of FIX clients
- `scram_auth_timeout` — maximum time for a SCRAM exchange (default 10 s)

**HA test infrastructure update** (`ha_test.py`). Two authentication service instances (`authentication_service_primary` on port 7070, `authentication_service_secondary` on port 7071) added to the launch table before `order_gateway`. Binary preflight check extended to include `authentication_service`. Auth service log files added to stale-log cleanup. Startup order is now 9 processes: witness → arbiter_primary → arbiter_secondary → authentication_service_primary → authentication_service_secondary → order_gateway → sequencer_primary → sequencer_secondary → matching_engine.

**TLS subsystem added to the framework.** Two sessions of work (both within Session 20) added full TLS support to the raw-bytes connection layer.

*Inbound TLS* (`TlsRawBytesProtocolHandler`, `TlsContext`, `TlsState`, `TlsListenerConfiguration`, `ProtocolType::TlsRawBytes`). The `InboundListenerConfiguration` gains an optional `TlsListenerConfiguration`; when present the `Reactor` builds a `TlsContext` at init (loading certificates once) and constructs a `TlsRawBytesProtocolHandler` for each accepted connection. TLS is handled non-blockingly via OpenSSL memory BIOs — the reactor thread never blocks. `ConnectionEstablished` is not delivered until the handshake completes. Five integration tests cover the happy path, fragmented ciphertext, `close_notify`, mutual TLS, and deliberate handshake failure.

*Outbound TLS* (`TlsClientConfiguration`, `ServiceEndpoints::tls`, `OutboundConnection` and `OutboundConnectionManager` TLS paths). `ServiceEndpoints` gains `std::optional<TlsClientConfiguration> tls`. When present, the outbound manager calls `start_outbound_handshake()` after TCP connect and defers `ConnectionEstablished` until the TLS handshake completes. Four outbound integration tests cover the happy path, mutual TLS, server-initiated disconnect after handshake, and handshake failure with wrong trust anchor.

`ProtocolHandlerInterface` gains three new virtuals: `start_outbound_handshake()`, `is_handshake_complete()`, `is_reads_paused()`. Non-TLS handlers return safe defaults.

Status: framework complete, tested (9 integration tests). Not yet wired to any application — the gateway FIX listener and auth service still use plain TCP. See subsystem section 16 for full detail.

**`build.py` improvements.** Pylint always runs (on `python/dsl/`) before CMake. Pytest runs by default; suppressed by `--no-tests` or the new `--no-pytest` flag. This catches Python DSL regressions before the slower C++ build begins.

**All 13 HA scenarios pass.** `auth_service_test.py` (3 scenarios: single exchange, sequential exchanges on one connection, multiple independent clients) all pass. Build clean, all unit and integration tests pass.

**Database access — design discussion (not yet implemented).** See "Database Access Design" section below.

---

### Session 19

**Quill SPSC queue initial capacity raised to 32 MiB (`PubsubFrontendOptions`).** Under high-throughput runs the default 128 KiB Quill SPSC queue was doubling seven times (reaching 32 MiB) before stabilising, printing an INFO reallocation message on each doubling. A new `PubsubFrontendOptions` struct (`libraries/pubsub_itc_fw/include/pubsub_itc_fw/PubsubFrontendOptions.hpp`) sets `initial_queue_capacity = 32 MiB` to match the observed worst case and eliminates all reallocation. Queue type remains `UnboundedBlocking` so producers block rather than drop if the queue ever exceeds the 2 GiB cap. Physical memory is only touched as the queue fills — the 32 MiB is virtual address space reserved up-front. A companion `QuillLoggerFrontendOptions.hpp` wires the options into the logger type. Files: `PubsubFrontendOptions.hpp` (new), `QuillLoggerFrontendOptions.hpp` (new), `QuillLogger.hpp`, `QuillLogger.cpp`.

**SequencerThread WAL routing map removed from replay.** WAL replay on restart was rebuilding the `seq_no → gateway_session_conn_id` routing map. This was incorrect: after a restart the ME has already sent ERs for any in-flight orders from the previous run and those ERs will not be re-sent. Keeping stale entries caused unbounded heap growth under high throughput. Fix: the WAL `open()` replay callback is now `nullptr`; replay only recovers `next_sequence_number_`. ER PDUs whose seq_no is not in the routing map are handled gracefully by the existing "not in routing map" fallback. The log message on successful WAL recovery was simplified accordingly. File: `applications/sequencer/SequencerThread.cpp`.

**`FixErEncoder` — zero-allocation outbound ER encoder.** The outbound Execution Report path had been using `FixSerialiser`, which builds an `unordered_map<int, string>` inside a `FixMessage` and allocates on every ER sent. Profiling identified this as 3.34% of gateway CPU (heap allocation in libc). Replaced with a new `FixErEncoder` class (`FixErEncoder.cpp`, `FixErEncoder.hpp`) that writes directly to a caller-supplied fixed-size buffer via a `FixWireWriter` cursor/limit helper. No heap allocation on the hot path. `FixGatewaySeqThread` now calls `FixErEncoder::encode()` directly instead of constructing a `FixMessage`. Files: `FixErEncoder.cpp` (new), `FixErEncoder.hpp` (new), `FixGatewaySeqThread.cpp`.

**`ApplicationThreadTest.MessageProcessing` flakiness fix.** The test used a 200 ms startup wait before sending messages. On a loaded CI system this was too tight — the application thread had not yet reached its event loop. Increased to 5000 ms. File: `libraries/pubsub_itc_fw/tests/ApplicationThreadTest.cpp`.

**Performance test infrastructure.** Added `monitor_memory.py` (memory monitor for multi-process test runs) and `perf_run.sh` (shell driver for `perf record` runs). Various fixes to `perf_run.py`: call-graph mode switched from `fp` to `dwarf` (frame-pointer unwinding lost kernel frames), SIGKILL issued immediately on successful test completion.

**Burst=50 end-to-end test.** 50 fix8 clients × burst=50 × 1000 orders = **2,500,000 orders** processed end-to-end with no drops. `matching_engine.log` contained exactly 5,000,000 entries (NOS + ER per order). Pool-exhaustion warnings in the log are benign — they indicate slab chaining working correctly, not allocation failures. Initial "Connection refused" and end-of-run "gateway connection lost" messages are expected startup/shutdown races.

**Build and test status at session end:** all unit tests pass, all integration tests pass. Burst=50 test clean.

---

### Session 18

**Unit and integration test coverage improvements — PduParser, OutboundConnectionManager, and coverage tooling fix.**

**New unit tests: `PduParserTest.cpp`** — 6 tests covering PduParser error paths not exercised by the existing `PduFramerParserTest.cpp`:
- `ZeroLengthPayloadReturnsError` — `byte_count = 0` in header rejected before allocation.
- `OversizedPayloadReturnsError` — `byte_count > slab_size()` rejected before allocation.
- `DisconnectDuringPayloadCallsHandlerAndReturnsFalse` — valid header then peer disconnect calls the disconnect handler and returns `{false, ""}` (empty error = graceful close).
- `ReadErrorDuringPayloadReturnsFalse` — valid header then `ECONNRESET` returns `{false, non-empty}`.
- `EagainDuringPayloadResumesOnNextCall` — EAGAIN on first call returns `{true,""}` with no PDU dispatched; payload arrives on second call and PDU is dispatched correctly.
- `ReadErrorDuringHeaderReturnsError` — `ECONNRESET` with no bytes queued returns `{false, non-empty}`.

A `PduParserTestStream` stub with priority ordering (buffered bytes → disconnect → error → EAGAIN) was written to enable injecting errors at the payload phase after delivering the header. Added to `libraries/pubsub_itc_fw/tests/CMakeLists.txt`.

**New unit tests: `OutboundConnectionTest.cpp`** — 7 additional tests on the `OutboundConnectionManagerTest` fixture covering previously-uncovered paths:
- `RetryFailedConnectionsNoOpWhenEmpty` — calling `retry_failed_connections` with no pending retries is a no-op.
- `ProcessSendRawCommandReturnsFalse` — outbound connections reject SendRaw commands.
- `ProcessCommitRawBytesReturnsFalseForUnknownId` / `ReturnsTrueForKnownId` — commit path.
- `FindByIdReturnsNullForUnknownId` — unknown ConnectionID lookup.
- `ProcessDisconnectCommandTeardownsConnection` — teardown + second call returns false.
- `OnDataReadyParseErrorTeardownsConnection` — bad canary triggers teardown via `on_data_ready`.

**New integration test: `OutboundConnectionRetryIntegrationTest.cpp`** — end-to-end proof that `retry_failed_connections` re-issues `process_connect_command` after a connect timeout. A `RetryCountingThread` accumulates `connection_failed` events without shutting down; the test verifies `failed_count >= 2` (initial timeout + at least one retry). Uses `192.0.2.1:9999` (TEST-NET, non-routable) with a 50 ms `connect_timeout` and 1 ms `connect_retry_interval`. Added to `libraries/pubsub_itc_fw/integration_tests/CMakeLists.txt`.

Note: unit tests and integration tests go in separate directories (`tests/` and `integration_tests/` respectively). The retry test requires a live reactor event loop and belongs in `integration_tests/`.

**Coverage tooling fix — `--erase-functions FMT_COMPILE_STRING` in `build.py`.** The `lcov --omit-lines` option only removes *line* coverage entries (DA records); it does not touch *function* entries (FN/FNDA records). The `FMT_STRING(fmt)` macro used inside `PUBSUB_LOG` for compile-time format validation expands to an immediately-invoked lambda containing a `struct FMT_COMPILE_STRING`. GCC emits these as real callable functions in the object file, and gcov records them as uncovered functions (they live inside `if (false)` blocks). They inflate the function-coverage denominator significantly.

Fix: added `--erase-functions FMT_COMPILE_STRING` to the `lcov --remove` step in `build.py`. This removes all FN/FNDA records whose mangled name contains `FMT_COMPILE_STRING`, eliminating 253 phantom entries project-wide.

Results after fix:

| Class | Before | After |
|---|---|---|
| `TimerHandler` | 4/13 (30.8%) | 4/4 (100%) |
| `InboundConnectionManager` | 16/35 (45.7%) | 16/16 (100%) |
| `OutboundConnectionManager` | 16/35 (45.7%) | 16/16 (100%) |
| `PduParser` | 5/10 (50%) | 5/5 (100%) |
| **Overall** | **~74%** | **93.2% (894/959)** |

`--omit-lines` is retained in the filter step as it still removes phantom line coverage entries from the `if (false)` validate blocks.

Files changed: `libraries/pubsub_itc_fw/tests/CMakeLists.txt`, `libraries/pubsub_itc_fw/tests/PduParserTest.cpp` (new), `libraries/pubsub_itc_fw/tests/OutboundConnectionTest.cpp`, `libraries/pubsub_itc_fw/integration_tests/CMakeLists.txt`, `libraries/pubsub_itc_fw/integration_tests/OutboundConnectionRetryIntegrationTest.cpp` (new), `build.py`.

**Build and test status at session end:** 485 unit tests pass (1 pre-existing skip), 21 integration tests pass. Coverage report: 93.2% function coverage, 86.9% line coverage.

---

### Session 17

**`ExpandableSlabAllocator` SIGSEGV fix — segmented atomic array.** `ConcurrentSmallSlabHighChurn` was crashing with SIGSEGV under ASan / TSan. Root cause: `std::vector<std::unique_ptr<SlabAllocator>>::push_back()` in `append_new_slab()` triggers reallocation — allocates a new backing array, moves elements, frees the old — while worker threads concurrently read raw pointers out of the old array via `deallocate()`. Freed-memory access; undefined behaviour.

Fix: replaced the vector with a two-level segmented atomic array.
- `std::atomic<Page*> pages_[kMaxPages]` (1024 directory slots, in-object, never moves or reallocates).
- Each `Page` is heap-allocated once and holds `std::atomic<SlabAllocator*> slots[kPageSize]` (256 slots). Pages are never freed during the allocator's lifetime.
- Slab id N maps to `pages_[N >> 8]->slots[N & 0xFF]`. Page 0 allocated in constructor; further pages allocated on demand in `append_new_slab`.
- Workers in `deallocate()` load page ptr with `acquire`, slot ptr with `acquire`. Reactor's `release` stores in `append_new_slab` guarantee visibility.
- `drain_empty_slab_queue` and `load_slab_reactor` use `relaxed` loads (reactor-thread only).

The `ConcurrentSmallSlabHighChurn` test runs for `test_duration = std::chrono::seconds(5)` by design — 5-second runtime is intentional.

Files changed: `ExpandableSlabAllocator.hpp`, `ExpandableSlabAllocator.cpp`.

**Slices 1–5 of the WAL+HA plan implemented (spanning earlier sessions, documented here).**

- **Slice 1 — seqNo on wire and in `EventMessage`.** `PduHeader` gains an `int64_t seq_no` field; `EventMessage::create_framework_pdu_message` takes `seq_no`; `SequencerThread` routes `next_sequence_number_` into the field when re-encoding for forwarding. ME and gateway see seqNos.
- **Slice 2 — In-memory WAL.** Sequencer maintains an in-memory log (`SequencerWal`) of every committed order.
- **Slice 3 — mmap'd WAL on disk, segmented, no fsync.** `SequencerWal` writes to `wal_NNNNNN.log` segments. On restart, WAL is replayed from segment 0 (or from snapshot anchor) to rebuild sequencer state. `WalEntryHeader`: `magic(4) | payload_size(4) | seq_no(8) | pdu_id(2) | filler_a(2) | filler_b(4)`. Each entry ends with CRC32. Corrupt/truncated entry stops replay; entries beyond are treated as "did not happen".
- **Slice 4 — Snapshot (single, no rolling).** `SequencerWal::take_snapshot()` writes `snapshot.bin` atomically (write to `.tmp`, rename). `SnapshotHeader`: `magic | version | last_seq_no | record_count | wal_segment | wal_offset`. On open, valid snapshot causes WAL replay to start from the snapshot anchor, bounding restart time to post-snapshot WAL size.
- **Slice 5 — `cl_ord_id → SenderCompID` routing map in sequencer.** Sequencer stamps `routing_comp_id` (the originating FIX client's `SenderCompID`) onto every forwarded ER PDU. Gateway looks up comp-id → current ConnectionID in its small comp-id table and routes the FIX ER accordingly. WAL replay rebuilds the routing map on restart. Gateway's `cl_ord_id_to_session_` map now lives in the sequencer.

**Slice 6 — Leader-follower HA state machine.** Implemented in `SequencerThread`:
- `Role` enum: `unknown`, `leader`, `follower`.
- `adopt_role(role)`: transitions into the assigned role; logs the change. Only the leader forwards order PDUs to ME (`if (role_ != Role::leader) { release_pdu_payload(message); return; }`).
- Startup election via `StatusQuery`/`StatusResponse` on peer connect.
- `peer_heartbeat_timeout` timer: fires if peer does not complete election within the startup window; node self-promotes to leader.
- Epoch tracking: `epoch_` incremented on self-promotion; propagated in `StatusQuery`, `StatusResponse`, `Heartbeat`.
- Fence file written when becoming leader (single-host split-brain protection).
- New `SequencerConfiguration` fields: `ha_enabled{false}`, `startup_election_timeout_seconds{3}`, `heartbeat_timeout_seconds{15}`.

**Slice 6 regression fix — `ha_enabled` flag for single-node mode.** After Slice 6 landed, the sequencer started in `Role::unknown` and silently dropped every inbound order PDU until the election completed. With no secondary running, only the `peer_heartbeat_timeout` path fires — originally 15 seconds. Any NOS within that window received no reply.

Fix: `ha_enabled` flag (default false) added to both sequencer and gateway configs. When false:
- Sequencer `on_initial_event`: immediately `adopt_role(leader)` (epoch 1). No election timer. No arbiter/peer connects.
- Gateway `on_app_ready_event`: skips `connect_to_service("sequencer_secondary")`.
- Gateway `forward_pdu_to_sequencers`: secondary branch guarded by `config_.ha_enabled`.
- Gateway config loader: secondary host/port only parsed when `ha_enabled=true`.
- `SampleFixGatewaySeq.cpp`: secondary service-registry entry only added when `ha_enabled=true`.

Also: three connection-retry/failure log lines in `OutboundConnectionManager.cpp` demoted from `Warning` to `Info`.

Files changed (Slice 6 + regression fix):
- `applications/sequencer/SequencerConfiguration.hpp`
- `applications/sequencer/SequencerConfigurationLoader.cpp` (added `#include <tuple>` for `std::ignore`)
- `applications/sequencer/SequencerThread.cpp`
- `applications/sequencer/SequencerThread.hpp`
- `applications/order_gateway/FixGatewaySeqConfiguration.hpp`
- `applications/order_gateway/FixGatewaySeqConfigurationLoader.cpp`
- `applications/order_gateway/FixGatewaySeqThread.hpp`
- `applications/order_gateway/FixGatewaySeqThread.cpp`
- `applications/order_gateway/SampleFixGatewaySeq.cpp`
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp`

**Build and test status at session end:** all unit tests pass including `ConcurrentSmallSlabHighChurn`. End-to-end verified: fix8 NOS → gateway → sequencer (immediately leader with `ha_enabled=false`) → ME → sequencer → gateway → fix8 ER. No silent drops.

---

### Session 16

**Slab-queue race condition in `EmptySlabQueue` / `ExpandableSlabAllocator` diagnosed and fixed.** Under heavy fix8 T-burst load on the integrated gateway/sequencer, the reactor would wedge in `ExpandableSlabAllocator::drain_empty_slab_queue` with the tripwire firing after ~1 second. Diagnostic output showed a state that looked structurally impossible: `head_ = &slab_N's_queue_node` (a real slab), `tail_ = &dummy_`, `head_->next = nullptr` — persistent across millions of retry iterations. The consumer was stuck because `head_->next` would never become non-null (no producer can ever set `slab_N's_next` once `tail_` no longer points at `&slab_N's_node`).

**Diagnostic infrastructure added then removed.** Four `peek_*` diagnostic accessors on `EmptySlabQueue` (`peek_head`, `peek_head_next`, `peek_tail`, `peek_dummy`) were added to let the drain function and tests inspect the queue's state without mutating it. The accessors were retained in the final fix; the `fprintf` blocks inside `drain_empty_slab_queue` were used to capture the smoking-gun trace then removed. The trace showed the failing drain's ENTRY line read `head=&dummy_, head->next=&slab_9_node, tail=&dummy_` — proving a producer's `prev->next.store(node)` had set `dummy_.next` but the producer's `tail_.exchange(node)` had been clobbered back to `&dummy_`. Only `reset_to_empty` writes `&dummy_` to `tail_`, so the consumer's `reset_to_empty` must have interleaved with the producer's enqueue.

**Root cause.** `EmptySlabQueue::reset_to_empty()` did three non-atomic stores: `dummy_.next.store(nullptr); head_ = &dummy_; tail_.store(&dummy_)`. Between the consumer's Empty observation in `try_dequeue` and the `tail_.store(&dummy_)` inside `reset_to_empty`, a producer could complete its own `tail_.exchange(node)`. The consumer's subsequent store clobbered the producer's exchange. The producer's later `prev->next.store(node)` then wrote `dummy_.next = node` (because `prev` was `&dummy_` at the producer's exchange time), but `tail_` was back at `&dummy_`. Result: a "ghost-enqueued" slab visible via `head_->next` but unreachable from `tail_`. Over time, multiple such race-clobbered enqueues accumulated; when the consumer finally GotItem'd one, the resulting state was the wedged head=slab/tail=dummy pattern. The `is_enqueued_` one-shot CAS meant the ghost-enqueued slabs could never be re-enqueued either.

**Fix: classical Vyukov sentinel pattern with one-drain-deferred reclamation.**
- `EmptySlabQueue::reset_to_empty()` removed entirely — the function and its declaration. The classical Vyukov pattern doesn't need it.
- `ExpandableSlabAllocator` gained a `deferred_reclaim_slab_id_{-1}` member. The most-recently-popped slab in any drain stays alive as the queue's sentinel; head_ and tail_ point at it after the drain. On the NEXT drain, when a successful GotItem confirms head_ has advanced past the deferred slab, the deferred slab is finally destroyed and the new last-popped becomes the next deferred. This relies on the invariant that for head_ to advance past a node N, the producer who put N's successor in front of head_ must have completed its store (since head_->next.load returned non-null), and that producer is the *only* producer that ever held N's_node as `prev` (the Vyukov MPSC exchange guarantees this).
- The drain-loop tripwire was switched from iteration-count (100,000 — meaningless at modern CPU speeds; iteration time was nanoseconds, so the budget was microseconds) to wall-clock (one second) which is a true safety net for the now-much-rarer case of a genuinely stuck producer.

**Why this fix is correct (not just a workaround).** Producers never need `reset_to_empty` — they thread their nodes onto `tail_` and never read or write `head_`. The only reason `reset_to_empty` was there was to prevent head_/tail_ from pointing at memory that's about to be destroyed during reclamation. The Vyukov sentinel pattern handles that naturally by keeping the most-recently-popped node alive. The author of the original code worried about a self-loop hazard (same node enqueued twice while head_ still pointed at it), but that hazard is already prevented by the `is_enqueued_` one-shot CAS in `SlabAllocator::try_claim_enqueue` — a slab's node can only be enqueued once per slab lifetime, and after destruction a new slab gets a fresh node.

**Reproducer test written.** `ExpandableSlabAllocatorStressTest` (initially a separate fixture, later subsumed into `ExpandableSlabAllocatorTest`) was added: one reactor thread allocates and posts to a bounded blocking work queue; N worker threads pop and deallocate. Small slab and chunk sizes force frequent slab switches and empty-slab notifications, maximising pressure on the lock-free queue. Two test cases (`ConcurrentAllocateAndDeallocateMakesProgress` at slab/chunk 512/64 with 8 workers; `ConcurrentSmallSlabHighChurn` at 1024/32 with 16 workers); each runs for 5 wall-clock seconds and treats any allocator exception as a test failure. The test reproduced the bug in 1.1 seconds on the first run, providing a tight feedback loop for the fix.

**Three pre-existing tests broken by the new two-drain destruction lifecycle, all rewritten.** The fix means a slab destroyed by the queue is destroyed in the drain AFTER the one that popped it. Tests that previously asserted "deallocate against the popped slab's ID throws PreconditionAssertion" needed an additional allocate-deallocate cycle to push the slab through both drain phases. Affected tests: `ExpandableSlabAllocatorTest.OldSlabIsDestroyedAfterChaining`, `ExpandableSlabAllocatorTest.DeallocateDestroyedSlabThrows`, and the merged-in `DestroyedSlabIdThrowsOnDeallocateCrossThread` (formerly `ExpandableSlabAllocatorAdversarialTest.DestroyedSlabIdThrowsOnDeallocate`). All three now use `chunk_size == slab_size` so each `allocate()` chains a fresh slab deterministically, and walk through two drain cycles before asserting destruction.

**Tangential bug found and fixed in `ExpandablePoolAllocatorTest.AbaStressTest`.** Under Valgrind, the test was reporting "free-list corruption: unknown pointer 0x...". Initial theory (pool expansion firing because `pool_slots == num_threads` left no headroom) was partially right but the deeper bug was test-design: with `pool_slots = num_threads`, even after bumping by one to `num_threads + 1`, only `num_threads` slots are ever reachable by the workers (LIFO pop_back from a 5-slot pool with 4 threads holding at most one each leaves slot 0 perpetually untouched at the front of the vector). The drain phase still produced all 5 slots, one of which had never been recorded in `valid_addresses`, giving a phantom-address failure. Fix: pre-touch every slot at test setup (allocate all `pool_slots` in one go before stress so every slot's address is recorded), plus the `pool_slots = num_threads + 1` adjustment to ensure expansion never fires. Diagnosed by adding a one-line stderr print of `number_of_pools`, `total_capacity`, `valid_addresses.size`, and `behaviour_statistics.expansion_events` immediately before the assertion loop, which showed `expansion_events=0` and `valid_addresses.size=4` for a 5-slot pool — confirming the address-coverage gap rather than the expansion race. Test now passes under Valgrind with 500 repeats × 50,000 iterations (~hundred million allocate/deallocate pairs).

**Test file reorganisation.** Two structural rules were adopted: (1) one fixture per class under test, (2) one fixture per file. The previous layout was:
- `SlabAllocatorTest.cpp` contained three fixtures: `EmptySlabQueueTest`, `SlabAllocatorTest`, `ExpandableSlabAllocatorTest`.
- `SlabAllocatorAdversarialTest.cpp` contained three more: `EmptySlabQueueAdversarialTest`, `SlabAllocatorAdversarialTest`, `ExpandableSlabAllocatorAdversarialTest`.
- `ExpandableSlabAllocatorTest.cpp` contained `ExpandableSlabAllocatorTest` (a second copy, with different tests) and `ExpandableSlabAllocatorStressTest`.

After reorganisation:
- `EmptySlabQueueTest.cpp` — one fixture, 8 tests (6 original + 2 ex-adversarial).
- `SlabAllocatorTest.cpp` — one fixture, 20 tests (15 original + 5 ex-adversarial).
- `ExpandableSlabAllocatorTest.cpp` — one fixture, 37 tests (12 from the old `SlabAllocatorTest.cpp` + 21 from the previous `ExpandableSlabAllocatorTest.cpp` + 2 ex-stress + 4 ex-adversarial; one duplicate `DeallocateNullptrThrows` vs `DeallocateNullPtrThrows` dropped).
- `SlabAllocatorAdversarialTest.cpp` deleted entirely.

The colliding test names `AdversarialDestroyedSlabIdThrowsOnDeallocate` and `DeallocateDestroyedSlabThrows` were disambiguated by renaming the cross-thread variant to `DestroyedSlabIdThrowsOnDeallocateCrossThread`. All other ex-adversarial tests had the `Adversarial` prefix simply dropped; no other name collisions.

**Documentation correction in `FixedSizeMemoryPool.hpp`.** The `Slot<T>` docblocks (two of them: production and Valgrind paths) and the top-of-file CANARY DESIGN section claimed "Both the Valgrind and production Slot<T> definitions are identical in layout". They are NOT — the production path has `free_next` between `is_constructed` and `canary`; the Valgrind path doesn't (it uses a mutex-protected `std::vector` free list). The pointer arithmetic in `get_is_constructed_for_object` and `get_canary_for_object` works in each build path independently via `offsetof(SlotType, storage)` — it doesn't depend on the two layouts being byte-for-byte equivalent. Updated all three comments to describe the actual invariants: `is_constructed` is first, `canary` is immediately before `storage`, `storage` is last.

**Verification.** All ~65 tests across the three reorganised files pass. Verified under sustained load: the gateway and sequencer remained stable under heavy fix8 T-burst load with no tripwire firing.

**Files changed:**
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/EmptySlabQueue.hpp` — `reset_to_empty` removed; four `peek_*` diagnostic accessors added.
- `libraries/pubsub_itc_fw/src/EmptySlabQueue.cpp` — `reset_to_empty` implementation removed.
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ExpandableSlabAllocator.hpp` — `deferred_reclaim_slab_id_` member added; class-level docs updated.
- `libraries/pubsub_itc_fw/src/ExpandableSlabAllocator.cpp` — `reset_to_empty` call removed from drain loop; Vyukov sentinel deferred-reclamation logic added; iteration-count tripwire switched to wall-clock; `#include <chrono>` added.
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/FixedSizeMemoryPool.hpp` — three comment-only edits correcting the misleading "identical layout" claims about `Slot<T>`.
- `libraries/pubsub_itc_fw/tests/ExpandablePoolAllocatorTest.cpp` — `pool_slots = num_threads + 1` and pre-touch loop in `AbaStressTest`.
- `libraries/pubsub_itc_fw/tests/EmptySlabQueueTest.cpp` — new file containing the `EmptySlabQueueTest` fixture only.
- `libraries/pubsub_itc_fw/tests/SlabAllocatorTest.cpp` — replaced wholesale; now contains only the `SlabAllocatorTest` fixture.
- `libraries/pubsub_itc_fw/tests/ExpandableSlabAllocatorTest.cpp` — replaced wholesale; now contains the merged single `ExpandableSlabAllocatorTest` fixture with stress tests subsumed.
- `libraries/pubsub_itc_fw/tests/SlabAllocatorAdversarialTest.cpp` — deleted.

**Smaller deferred items noted during the session (none blocking):**
- `k`-prefix constants (`kSmallSlab`, `kLargeSlab`, `kPartial`) in the `ExpandableSlabAllocatorTest` fixture violate the project rule that constants are snake_case. Left alone in this session to avoid scope creep; mechanical search-and-replace when next convenient.
- The skipped `EmptySlabQueueTest.ReEnqueueOfSameNodeDoesNotCauseInfiniteSpin` test's comment was updated to remove the now-stale reference to `reset_to_empty` (which was supposed to mitigate the hazard but never did — the `is_enqueued_` one-shot CAS does).
- A few existing tests in the new `ExpandableSlabAllocatorTest.cpp` are functional near-duplicates (e.g. `OldSlabIsDestroyedAfterChaining` vs `OldEmptySlabIsDestroyed`; `DeallocateInvalidSlabIdThrows` vs `DeallocateOutOfRangeSlabIdThrows`). All kept — test coverage is cheap; names disambiguated where they collided.

### Session 15

**End-to-end ME ER fabrication.** The matching engine no longer logs `-- stub` and stops; it now decodes inbound `NewOrderSingle` PDUs and emits a fully-filled `ExecutionReport` PDU back to the sequencer. The ER populates every field that `SequencerThread` decodes during ER-forwarding (`order_id`, `exec_id`, `exec_type=Trade`, `ord_status=Filled`, `symbol`, `side`, `leaves_qty=0`, `cum_qty=order_qty`, `avg_px=price`, `transact_time` in nanoseconds, plus optional `cl_ord_id`, `order_qty`, `last_qty`, `last_px`, `price`, `ord_type`). No real order book or matching is performed — every order is fabricated as fully filled at its requested limit price. The ME has its own `order_id_counter_` and `exec_id_counter_` producing `ME-ORD-N` and `ME-EXEC-N` strings.

The ME's `handle_new_order_single` keeps fabricated `std::string` locals on the stack and assigns their `string_view`s into the ER struct, so the views are valid for the full duration of the immediately-following `send_pdu` call. This is a safer pattern than the existing `er.foo = std::string(view.foo);` style used in `SequencerThread`'s ER decoder (which assigns a temporary `std::string` to a `string_view`, leaving the view dangling at the end of the assignment statement; works in practice today only because the calling stack frame is not yet overwritten by the time `send_pdu` reads the bytes). The SequencerThread instances of that pattern have not been changed in this session — they should be cleaned up when next convenient.

**Sequencer-to-ME topology corrected.** Until this session the sequencer was using a single TCP connection bidirectionally: the ME's outbound to the sequencer's `inbound:7021` (the ER channel) was being repurposed by the sequencer to push order PDUs back the wrong way down the same socket. The proper topology is two unicast pipes — one each way — until pub/sub fanout replaces direct TCP. The fix:
- `SequencerConfiguration.hpp` gained `matching_engine_host`/`matching_engine_port` members defaulting to `127.0.0.1:7020`.
- `SequencerConfigurationLoader.cpp` now parses a `[matching_engine] host=... port=...` section.
- `Sequencer.cpp` registers the `matching_engine` service in the `ServiceRegistry`.
- `SequencerThread::on_app_ready_event` calls `connect_to_service("matching_engine")` so the sequencer opens its own outbound to the ME's order listener.
- `SequencerThread`'s previously-misnamed `matching_engine_conn_id_` was renamed to `me_outbound_order_conn_id_` to make the direction explicit. The `on_connection_established` branch that captured `inbound:7021` as "matching engine ER connection" was removed; ER PDUs are still routed correctly by `service_name == "inbound:7021"` matching in `on_framework_pdu_message` without needing the ID cached.
- `sequencer.toml` gained a `[matching_engine]` section.

**Secondary sequencer expunged from the gateway.** The gateway-side dual-publish was incomplete and was breaking the single-sequencer test setup: the gateway required `[sequencer.secondary_host]`/`[sequencer.secondary_port]` in its toml and would attempt to connect to a secondary that wasn't running, retrying forever. The secondary references were removed rather than carry broken half-configuration. The dual-publish concept is preserved in code (the function name `forward_pdu_to_sequencers` retained, with a comment explaining the plural will be reasserted when leader-follower lands) but the secondary endpoint, member, connect call, and toml entries are all gone:
- `order_gateway.toml` — `secondary_host`/`secondary_port` lines removed from `[sequencer]`.
- `FixGatewaySeqConfiguration.hpp` — `sequencer_secondary_host`/`sequencer_secondary_port` members removed; class doxygen rephrased.
- `FixGatewaySeqConfigurationLoader.cpp` — secondary parsing/validation removed.
- `FixGatewaySeqConfigurationLoader.hpp` — doxygen example updated.
- `SampleFixGatewaySeq.cpp` — `service_registry_.add("sequencer_secondary", ...)` removed; startup log updated.
- `SampleFixGatewaySeq.hpp` — class doxygen "two outbound PDU connections" → "one outbound PDU connection".
- `FixGatewaySeqThread.hpp` — `sequencer_secondary_conn_id_` member removed; `forward_pdu_to_sequencers` template body's secondary branch removed; class doxygen updated.
- `FixGatewaySeqThread.cpp` — secondary init list entry, `connect_to_service("sequencer_secondary")`, and the corresponding branches in `on_connection_established` and `on_connection_lost` all removed; bottom-of-file comment block rewritten.
- `start_fix_seq_system.py` — the `sequencer_secondary` launch step removed; docstring updated.

**Fix `EventMessage::create_framework_pdu_message` to plumb `pdu_id` through.** Diagnosed during the runtime debugging that drove much of this session: `EventMessage::pdu_id_` defaulted to `-1` and was never set by the factory, so SequencerThread saw `pdu_id=-1` on every PDU and dropped them with "unknown order PDU id -1". Three-file fix:
- `EventMessage.hpp` — factory signature gained `int16_t pdu_id` parameter; doxygen updated.
- `EventMessage.cpp` — implementation now assigns `msg.pdu_id_ = pdu_id`.
- `PduParser.cpp::dispatch_pdu` — passes `current_pdu_id_` through to the factory.

**`InboundConnectionManager::on_accept` populates ConnectionID once.** Diagnosed in the same debugging pass: two distinct `ConnectionID` objects were in flight per inbound connection — one populated with `inbound:<port>` for the `ConnectionEstablished` event, and a bare one with empty service_name passed to `PduProtocolHandler` and stamped on every `FrameworkPdu` event by `PduParser::connection_id_`. The fix builds a single `populated_id` near the top of `on_accept` and uses it everywhere downstream (handler ctor, `InboundConnection` ctor, `connections_` map key, `teardown_connection` path, success log line, `ConnectionEstablished` event).

**Trace logging in PduParser.** Two `Info`-level traces fire per header decode: one logging the decoded fields (canary, byte_count, pdu_id, version), one dumping the raw 16 header bytes in hex. Required re-adding a `QuillLogger&` member to `PduParser` and re-plumbing it through `PduProtocolHandler` (logger parameter forwarded to PduParser construction) and `OutboundConnection` (logger member, forwarded similarly). Plus updates to `InboundConnectionManager::on_accept` and `OutboundConnectionManager` construction sites, and three test files (`PduFramerParserTest.cpp` — seven constructor calls; `PduProtocolHandlerTest.cpp` — one; `OutboundConnectionTest.cpp` — two).

**Files changed (twenty):**
- `applications/sequencer/SequencerConfiguration.hpp`
- `applications/sequencer/SequencerConfigurationLoader.cpp`
- `applications/sequencer/Sequencer.cpp`
- `applications/sequencer/SequencerThread.hpp`
- `applications/sequencer/SequencerThread.cpp`
- `applications/sequencer/sequencer.toml`
- `applications/matching_engine/MatchingEngineThread.hpp`
- `applications/matching_engine/MatchingEngineThread.cpp`
- `applications/order_gateway/order_gateway.toml`
- `applications/order_gateway/FixGatewaySeqConfiguration.hpp`
- `applications/order_gateway/FixGatewaySeqConfigurationLoader.cpp`
- `applications/order_gateway/FixGatewaySeqConfigurationLoader.hpp`
- `applications/order_gateway/SampleFixGatewaySeq.cpp`
- `applications/order_gateway/SampleFixGatewaySeq.hpp`
- `applications/order_gateway/FixGatewaySeqThread.hpp`
- `applications/order_gateway/FixGatewaySeqThread.cpp`
- `scripts/start_fix_seq_system.py`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/EventMessage.hpp`
- `libraries/pubsub_itc_fw/src/EventMessage.cpp`
- `libraries/pubsub_itc_fw/src/PduParser.cpp` and the broader plumbing chain noted above

(plus the test files updated for the PduParser logger constructor change).

**Build and runtime status at session end:**
- All four applications build clean. Library tests pass (the `QuillLoggerTest.LogsAlertMessage` failure noted at session-13 end was fixed by the user during session 14).
- End-to-end pipeline verified: fix8 → gateway → sequencer (order on `inbound:7001`, `pdu_id=1000`) → ME (order received on connection 2, NewOrderSingle decoded, `ME-ORD-1`/`ME-EXEC-1` fabricated) → sequencer (ER on `inbound:7021`, `pdu_id=1002`, `byte_count=143`) → gateway → fix8. The fix8 client receives the ExecutionReport reply as expected. **Three sessions converged on this milestone; the comms framework is verified end-to-end with a real FIX client.**

**Session 15 continuation -- payload hex dump trace, dangling-string-view diagnosis and fix.**

A binary-garbage symptom appeared at first verification: the ME log showed `ClOrdID=\x0F\x00\x00\x00`, `Symbol=\x0F\x00\x00`, `OrderQty=\x0F\x00\x00\x00\x00\x00`. Lengths in the decoder were correct (4, 3, 6) but the field contents were SSO-bookkeeping bytes (`0x0F` is the libstdc++ short-string capacity marker). The wire-level forwarding was faithful; the corruption was happening *inside* one of the apps before it sent.

Diagnosis path:
1. Read the gateway log -- `ClOrdID=ord1` was correctly parsed from the FIX message at the gateway side.
2. Read the DSL `encode(NewOrderSingle, ...)` source -- length-prefixed-string format, encoder symmetric with decoder. So if both runs of the DSL on the same struct produce identical bytes, encode/decode is sound.
3. Added a payload hex dump trace to `PduParser::dispatch_pdu` (96 bytes, `Info` level, mirroring the existing header trace). This dumps the wire payload of every PDU at every hop.
4. Re-ran. The hex dump showed:
   - gateway → sequencer NOS: clean (`04 00 00 00 6f 72 64 31 ...` -- "ord1" followed by "BHP", "8517.0", "61.17677")
   - sequencer → ME NOS: corrupted (`04 00 00 00 0f 00 00 00 ...` -- length 4, then four bytes of SSO leakage)
5. Conclusion: the SequencerThread re-encode block was the bug. It used the `er.cl_ord_id = std::string(view.cl_ord_id);` pattern -- assigning a temporary `std::string` to a `string_view` field. The temporary dies at the semicolon, leaving the view pointing at stack memory that the next round of temporaries overwrites with their own SSO buffers (whose first byte is `0x0F`, the libstdc++ inline-capacity sentinel). Same UB the project summary had flagged as item 9 in "What Is Not Yet Done" -- but it was not "works in practice today"; it was *visible* corruption of every forwarded order PDU.

**Fix:** `view.X` fields point into the slab payload owned by the inbound `EventMessage`, which is alive until `release_pdu_payload(message)` is called *after* `send_pdu` returns. So the right pattern is to assign `view.X` directly to the outbound struct's `string_view` field -- no temporary, no copy, no UB. Three blocks fixed in `SequencerThread.cpp`:

1. NOS re-encode (forwarding gateway → ME order PDUs): direct view-assignment; also added propagation of all NOS optional fields (`has_stop_px`, `has_account`, `has_ex_destination`, `has_exec_inst`, `has_min_qty`, `has_max_floor`, `has_expire_time`, `has_text`) which the previous code dropped.
2. OCR re-encode: direct view-assignment; added `has_account`/`has_text` propagation.
3. ER re-encode (forwarding ME → gateway): direct view-assignment; **added `er.order_id = view.order_id;`** which the previous code was missing entirely (a separate latent bug that would have caused fix8 to receive an ER with empty OrderID); added every optional `has_*` flag and value alongside the required fields. The complete-propagation policy is now the rule for re-encode/forward blocks.

Re-run after the fix showed clean wire bytes at every hop. Item 9 in "What Is Not Yet Done" is now resolved; item 3 (the binary-garbage finding from the original Session 15 verification) went away with it.

**Session 15 continuation -- gateway ER routing implementation.**

With the ER bytes now arriving at the gateway intact, `FixGatewaySeqThread::on_framework_pdu_message` -- previously a TODO no-op that silently dropped slabs -- was implemented. Body:
- Validates `pdu_id == ExecutionReport`; drops with warning + slab release otherwise.
- Decodes ER via `BumpAllocator` over the inherited `decode_arena_buffer()`.
- Drops if `view.has_cl_ord_id` is false or empty (no routing key).
- Looks up `cl_ord_id` (named `std::string` local for safe map lookup) in `cl_ord_id_to_session_`. Drops with warning if absent (originating session may have disconnected).
- Looks up the `FixSession` by `ConnectionID`. If gone, erases the stale map entry and drops.
- Builds a FIX `ExecutionReport` populating `OrderID`, `ExecID`, `ExecType`, `OrdStatus`, `Symbol`, `Side`, `CumQty`, `LeavesQty`, `ClOrdID`, plus optionals `OrderQty`, `Price`, `OrdType` when their `has_*` flags are set. Single-character DSL enum fields convert via `static_cast<char>(view.X)` (the enums use FIX char values as their underlying representation).
- Sends via the existing `send_fix_to_session` helper.
- On terminal `OrdStatus` (Filled, Canceled, Rejected, Expired, DoneForDay, Replaced), erases the map entry. Non-terminal statuses (PartiallyFilled, PendingCancel, etc.) leave the entry alive for follow-up fills.
- `release_pdu_payload(message)` on every exit path.

A separate `cl_ord_id_to_session_` cleanup was added to `on_connection_lost`: when a FIX session disconnects, all map entries pointing at its `ConnectionID` are swept. Without this the map would accumulate stale entries indefinitely.

The gateway's `FixSerialiser` already supported every tag the ER routing populates (no serialiser changes needed).

**Files changed in the continuation work (eight, beyond the twenty earlier):**
- `libraries/pubsub_itc_fw/src/PduParser.cpp` (payload hex dump trace)
- `applications/sequencer/SequencerThread.cpp` (three re-encode blocks fixed; complete optional-field propagation)
- `applications/order_gateway/FixGatewaySeqThread.cpp` (`on_framework_pdu_message` implemented; `on_connection_lost` map sweep added; BumpAllocator include added)

(The summary's earlier "Files changed (twenty)" list at the top of the Session 15 entry covers the topology and ME-ER-fabrication work; the three above are the ones that closed the loop.)

### Session 14

**Use-after-free in `PduParser` and `PduProtocolHandler` fixed (option 2).** The disconnect_handler hook diagnosed at session 13 was removed entirely. `PduParser`, `PduProtocolHandler`, and `RawBytesProtocolHandler` no longer hold or invoke any disconnect-handler callback; failure now propagates up to the owning manager via the return value of the failure-capable methods. The owning manager is then responsible for tearing down the connection. This matches the framework's broader principle that connection lifecycle is owned by the Reactor and its managers, not by the parsers and handlers.

**Contract change on `ProtocolHandlerInterface`.** The three failure-capable virtuals now return `[[nodiscard]] std::tuple<bool, std::string>` instead of `void`:
- `on_data_ready()` — `{true, ""}` on a clean read (including no bytes available); `{false, ""}` on graceful peer disconnect; `{false, error_string}` on protocol failure
- `send_prebuilt(...)` — `{true, ""}` on progress or completion; `{false, error_string}` on unrecoverable send failure (slab chunk released before return)
- `continue_send()` — same convention as `send_prebuilt`

`has_pending_send`, `deallocate_pending_send`, and `commit_bytes` are unchanged.

**Logger removed from `PduProtocolHandler` and `RawBytesProtocolHandler`.** With the disconnect-handler gone, the handlers no longer log anything; all error strings flow up to the manager which logs them. The `QuillLogger&` constructor parameter and member were dropped from both classes. Manager-side logging covers all failure paths now: graceful peer disconnect logs at `Info`, protocol/send errors log at `Error` or `Warning` per existing conventions.

**`InboundConnection::handle_read()`** now returns `std::tuple<bool, std::string>` rather than void, propagating the handler's return up to the manager.

**`OutboundConnection::on_connected()`** no longer takes a `disconnect_handler` parameter. The `PduParser` it constructs internally is built without one too.

**Manager updates:**
- `InboundConnectionManager::on_accept` no longer constructs a disconnect_handler lambda; both handler constructors lose the disconnect_handler and logger arguments.
- `InboundConnectionManager::on_data_ready` inspects the tuple from `handle_read()` and calls `teardown_connection` on `!ok`. Reason string is `"peer 'X' closed connection"` for graceful (logged at `Info`) or `"protocol error on connection from 'X': <error>"` for failure (logged at `Error`).
- `InboundConnectionManager::on_write_ready`, `process_send_pdu_command`, and `process_send_raw_command` all inspect the tuple from `continue_send`/`send_prebuilt` and tear down on failure. The previous "may invoke disconnect handler synchronously, re-look up by cid" guard pattern is gone — the call no longer mutates the connections map, so the post-call lookup is unnecessary.
- `OutboundConnectionManager::on_connect_ready` no longer constructs a disconnect_handler lambda; `conn.on_connected(std::move(socket))` takes only the socket. The existing `on_data_ready` and `on_write_ready` paths in the outbound manager already inspected return values and called `teardown_connection`, so they did not need to change.

**Test updates:**
- `PduFramerParserTest.cpp` — seven `PduParser` constructor calls dropped their fourth (disconnect_handler) argument. Two tests had a `disconnected` bool wired through a capturing lambda; flag and lambda removed. `ParseDetectsPeerDisconnect` now relies on the existing `EXPECT_FALSE(ok) && EXPECT_TRUE(error.empty())` assertion alone, which is exactly the new contract.
- `PduProtocolHandlerTest.cpp` — `disconnect_called_` member, lambda, and the disconnect_handler/logger arguments to the `PduProtocolHandler` constructor were removed. All six `send_prebuilt` and `continue_send` call sites now consume the `[[nodiscard]]` tuple return and assert `ASSERT_TRUE(ok) << error`.
- `OutboundConnectionTest.cpp` — three `on_connected` call sites at lines 420, 431, 437 dropped their trailing empty-lambda argument (`OnConnectedRejectsNullSocket` and `OnConnectedRejectsWhenNotConnecting`).

**Files changed (sixteen, in the disconnect_handler removal):**
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ProtocolHandlerInterface.hpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduParser.hpp`
- `libraries/pubsub_itc_fw/src/PduParser.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduProtocolHandler.hpp`
- `libraries/pubsub_itc_fw/src/PduProtocolHandler.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/RawBytesProtocolHandler.hpp`
- `libraries/pubsub_itc_fw/src/RawBytesProtocolHandler.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/InboundConnection.hpp`
- `libraries/pubsub_itc_fw/src/InboundConnection.cpp`
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/OutboundConnection.hpp`
- `libraries/pubsub_itc_fw/src/OutboundConnection.cpp`
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp`
- `libraries/pubsub_itc_fw/tests/PduFramerParserTest.cpp`
- `libraries/pubsub_itc_fw/tests/PduProtocolHandlerTest.cpp`
- `libraries/pubsub_itc_fw/tests/OutboundConnectionTest.cpp`

**Runtime verification of the disconnect_handler fix.** After the library landed clean and all unit tests passed, the system was started and exercised with fix8. The original session-13 SIGSEGV repro (idle-timeout of all four connections simultaneously) ran without crash; the new manager-side teardown paths fired correctly with `Info`-level logs for graceful peer close and no use-after-free. The session-13 SIGSEGV is resolved end-to-end.

**Two cascading runtime regressions discovered and fixed during verification.** End-to-end testing revealed the order-PDU pipeline from gateway to sequencer was failing in a way that turned out to be two long-latent bugs neither caused by the disconnect_handler removal. They are recorded together because they were diagnosed and fixed in the same debugging session.

**Regression 1: empty inbound `service_name`.** `SequencerThread::on_framework_pdu_message` was logging `PDU on unexpected connection 3 () -- dropping`. The connection-established event for connection 3 also showed empty parentheses for service_name. Diagnosis took two trace points: one logging `id.service_name()` at the top of `InboundConnectionManager::on_accept` (showed empty), one logging `msg.connection_id().service_name()` in `SequencerThread::on_framework_pdu_message` (showed empty). The trace confirmed that two different `ConnectionID` objects were in flight for the same connection: the populated one used for the `ConnectionEstablished` event, and the bare one passed to `PduProtocolHandler` and stored as `PduParser::connection_id_`, which then stamps every `FrameworkPdu` event. The fix was to construct a single `populated_id` near the top of `on_accept` and use that everywhere downstream — the two handler constructors, the `InboundConnection` constructor, the `connections_` map key, the `teardown_connection` call, the success log line, and the `ConnectionEstablished` event. Fix landed in `InboundConnectionManager.cpp`.

**Regression 2: `pdu_id` never propagated through `EventMessage`.** With service_name fixed, SequencerThread reached its order-PDU branch but logged `pdu_id=-1`. PduParser was decoding `pdu_id=1000` correctly from the wire (verified by the hex-dump trace added during this session — see below), but the value was never reaching the `EventMessage`. Root cause: `EventMessage::create_framework_pdu_message` had no `pdu_id` parameter. The `EventMessage::pdu_id_` member existed and defaulted to `-1`, the accessor existed and returned it, but the factory never set it. Three-file fix: added `int16_t pdu_id` to the factory's parameter list and assigned it to `msg.pdu_id_` in the implementation; `PduParser::dispatch_pdu` now passes `current_pdu_id_` through. Fix landed in `EventMessage.hpp`, `EventMessage.cpp`, and `PduParser.cpp`.

Both regressions were latent — the project hadn't previously exercised an inbound PDU with subsequent dispatch logic that examined the `service_name` *and* `pdu_id` fields together. Session 13's `inbound:<port>` work documented but only partially landed the service_name plumbing; nobody had noticed `EventMessage::pdu_id` was always `-1` because no consumer had needed it until SequencerThread.

**Trace logging in PduParser.** A `QuillLogger&` parameter and member were re-added to `PduParser` (and re-plumbed through `PduProtocolHandler` and `OutboundConnection` which both construct PduParsers). Two `Info`-level trace lines fire on every header decode in `PduParser::receive`: one logging the decoded fields (canary, byte_count, pdu_id, version), one dumping the raw 16 header bytes in hex. The traces are left in for now; they are valuable for diagnosing future framing issues. Move to `Debug` or wrap in a compile-time switch when the system runs in earnest. Files touched for this: `PduParser.hpp`/`.cpp` (logger member), `PduProtocolHandler.hpp`/`.cpp` (logger parameter forwarded to PduParser), `OutboundConnection.hpp`/`.cpp` (logger member), `InboundConnectionManager.cpp` and `OutboundConnectionManager.cpp` (pass `logger_` at construction), `PduFramerParserTest.cpp` (seven constructor calls updated), `PduProtocolHandlerTest.cpp` (one constructor call), `OutboundConnectionTest.cpp` (two constructor calls).

**Two short trace lines in `SequencerThread::on_framework_pdu_message` and `InboundConnectionManager::on_accept`** were added during diagnosis and remain in the code. Cosmetic to remove later; harmless for now.

**End-to-end pipeline verified.** Final run shows the complete order flow:
- fix8 → gateway: raw FIX bytes via `RawBytesProtocolHandler` on port 9879
- gateway → sequencer (primary and secondary): `NewOrderSingle` PDU encoded, sent on ports 7001/7002
- sequencer recognises connection by `service_name = "inbound:7001"`, reads `pdu_id=1000`, decodes the `NewOrderSingle` view, re-encodes into the owning struct, forwards to ME via `send_pdu`
- ME receives the sequenced PDU on its inbound listener (port 7020) and logs `MatchingEngineThread: sequenced PDU received on connection 1 -- stub`

The ME stub does not yet act on the order; that is the next piece of application work.

**Files changed in the verification work (additional five):**
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/EventMessage.hpp` (factory signature + namespace-comment typo fix)
- `libraries/pubsub_itc_fw/src/EventMessage.cpp` (factory implementation)
- `libraries/pubsub_itc_fw/src/PduParser.cpp` (pass `current_pdu_id_` to factory; trace logs)
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduParser.hpp` (logger member re-added)
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduProtocolHandler.hpp` (logger parameter re-added)
- `libraries/pubsub_itc_fw/src/PduProtocolHandler.cpp` (logger forwarded to PduParser)
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/OutboundConnection.hpp` (logger member added)
- `libraries/pubsub_itc_fw/src/OutboundConnection.cpp` (logger forwarded to PduParser)
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp` (populated_id, logger argument)
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp` (logger argument)
- `applications/sequencer/SequencerThread.cpp` (trace log)

**Build and test status at session end:**
- Library builds clean.
- All `pubsub_itc_fw` unit tests pass. The `QuillLoggerTest.LogsAlertMessage` failure noted earlier in the session was an unrelated regression introduced by separate logger work; the user fixed it during this session.
- All four applications (gateway, sequencer primary, sequencer secondary, matching engine) start cleanly, accept connections cleanly, and pass an order PDU end to end.
- The session-13 SIGSEGV repro runs without crash.

### Session 13

**Sequencer-primary retry-loop diagnosed and resolved** — at session start the sequencer primary appeared to be looping in startup. Diagnosis from the logs: the framework was operating correctly; the symptom was the perpetual 2.5-second retry of `sequencer_peer` against `127.0.0.1:7003`, a port no application binds. Root cause was asymmetric peer addressing in the two sequencer toml files: primary pointed at `:7003` (unbound) and secondary pointed at primary's order listener `:7001`, which the secondary mistakenly accepted as its peer link. Both addresses were design-incorrect for the leader-follower protocol described in section 12.

**Scope-A decision: defer the peer connection entirely until the leader-follower protocol is implemented.** The peer connection is part of the StatusQuery / StatusResponse / Heartbeat protocol, which is DSL-defined but not yet implemented. Issuing a `Connect` for the peer therefore produces only retry-loop noise. This was chosen over fixing the port assignments (which would have been speculative) and over removing the secondary sequencer entirely (scope B/C, larger blast radius). The HA topology in the project — gateway dual-publishes to both sequencers — is unchanged. Both sequencers still run; both still receive every order PDU. The sequencer stub still "behaves as unconditional leader" per `SequencerThread::on_framework_pdu_message`, which means both sequencers currently forward to the ME; this is a known stub limitation, separate from the peer-link work.

Files changed for scope A (eight files):
- `applications/sequencer/Sequencer.cpp` — removed `service_registry_.add("sequencer_peer", ...)`; startup log line no longer prints peer host/port and now mentions the deferral explicitly
- `applications/sequencer/SequencerThread.cpp` — removed `connect_to_service("sequencer_peer")` from `on_app_ready_event`; removed `peer_conn_id_` from ctor init list, from the `on_connection_established` branches, and from `on_connection_lost`; added a comment block at the connect site explaining when to re-enable
- `applications/sequencer/SequencerThread.hpp` — removed `peer_conn_id_` member; comment on `arbiter_conn_id_` updated
- `applications/sequencer/SequencerConfiguration.hpp` — removed `peer_host` and `peer_port` member fields
- `applications/sequencer/SequencerConfigurationLoader.cpp` — removed the two `get_required_except` calls plus local int, validation, and assignment
- `applications/sequencer/SequencerConfigurationLoader.hpp` — removed `peer_host` and `peer_port` from the example TOML in the doc comment
- `etc/sequencer/sequencer.toml` — removed `peer_host` and `peer_port` keys
- `etc/sequencer/sequencer_secondary.toml` — same

**Inbound connections now carry `"inbound:<port>"` in service_name** — the convention described at session 11 was documented but had never landed. `InboundConnectionManager::on_accept` was constructing the `ConnectionEstablished` event with the bare ConnectionID, leaving service_name empty. The patch now constructs a fresh ConnectionID at the event-enqueue site with `fmt::format("inbound:{}", listener.configuration.address.port)` for the service_name, matching the outbound convention at OutboundConnectionManager.cpp:252-253. `ConnectionID.hpp` doc comments (both the class-level Service-name section and the `service_name()` method comment) were updated to describe the new convention; the previous comment claimed inbound service_name was always empty, which was both wrong and would have misled future readers. Audit of all uploaded files using `service_name()` confirmed no caller relied on empty-detection: `FixGatewaySeqThread`, `MatchingEngineThread`, `ArbiterThread`, and the four `BurstListenerThread`/test thread classes all use either explicit `inbound:<port>` checks or fall-through `else` branches. Two files changed:
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ConnectionID.hpp`

**Patch application accidents.** Twice during this session, supplied patched files were misrouted on application: `InboundConnectionManager.cpp` content was placed under `InboundConnectionManager.hpp` (clobbering the real header), causing a recursive include because the .cpp self-includes its own header at line 19; and on a separate occasion `SequencerThread.hpp` content was placed under `Sequencer.hpp`, causing `Sequencer.cpp` to fail to compile with "Sequencer does not name a type". Both were recovered by restoring from version control. Lesson recorded for future sessions: when a header file is observed to lack `#pragma once` or to contain unexpected `class` declarations, the file has likely been overwritten by a same-basename .cpp or sibling .hpp. The .hpp/.cpp distinction must not be elided when copying.

**Reactor SIGSEGV diagnosed: use-after-free in disconnect-handler invocation pattern.** After scope-A and the service_name patches were applied and the system started up cleanly, the gateway crashed (SIGSEGV, exit -11) approximately two minutes after fix8 connected, when both inbound FIX connections and both outbound sequencer connections idle-timed-out simultaneously. Stack trace:

```
#0 PduParser::receive() at PduParser.cpp:44
#1 OutboundConnectionManager::on_data_ready (unique_ptr<>::operator->)
#2 Reactor::dispatch_events at Reactor.cpp:957
```

Diagnosis: `PduParser::receive()` invokes its stored `disconnect_handler_` from inside the method when `recv` returns 0 (graceful peer close). The disconnect_handler is a lambda that calls `OutboundConnectionManager::teardown_connection`, which destroys the `OutboundConnection`. The `OutboundConnection` owns `socket_`, `framer_`, and `parser_` as `unique_ptr` members, so destroying the connection destroys the parser whose method is currently executing. When `disconnect_handler_()` returns, control returns to `receive()` running on a destroyed `*this`. The same bug exists at the payload-phase recv-zero path inside `receive()`, and the same pattern is repeated three times in `PduProtocolHandler` (`on_data_ready`, `send_prebuilt`, `continue_send` — each calls `disconnect_handler_()` directly and returns, with `*this` having been destroyed mid-call).

The crash was *not* introduced by any session-13 patch; the bug has been latent since `PduParser` and `PduProtocolHandler` were written. It surfaced now because session-13 testing was the first run with simultaneous bidirectional idle-timeout fire (both inbound FIX and both outbound sequencer fds dropped in the same epoll batch). In normal operation with traffic flowing the recv-zero path is rarely taken before any other path returns successfully.

### Session 12

**`PduParser` now carries `ConnectionID`** — `PduParser` gains a `ConnectionID connection_id_` member set at construction. `create_framework_pdu_message` gains a `ConnectionID` parameter. `dispatch_pdu` passes the connection ID into the event message. Every `FrameworkPdu` event now correctly identifies its source connection. `PduProtocolHandler` gains the same `ConnectionID` parameter and passes it to `PduParser`. `InboundConnectionManager` passes `id` to `PduProtocolHandler`. `OutboundConnection::on_connected` passes `id_` to `PduParser`. Test files `PduFramerParserTest.cpp` and `PduProtocolHandlerTest.cpp` updated to pass `ConnectionID{}` at all construction sites.

**`ApplicationThread::release_pdu_payload` implemented** — declared in `ApplicationThread.hpp`, defined in `ApplicationThread.cpp` (to avoid incomplete type error from forward-declared `Reactor`). Calls `get_reactor().inbound_slab_allocator().deallocate(message.slab_id(), const_cast<uint8_t*>(message.payload()))`. Satisfies the contract referenced in `PduParser.hpp` and `EventMessage.hpp` comments.

**DSL generator: `enum` → `enum class`** — `_emit_enum` in `generator_cpp.py` changed from `enum {name}` to `enum class {name}`. Fixes name collision where `New = 48` appeared in both `OrdStatus` and other enums within the same namespace. Test files updated: `test_generator_cpp.py`, `test_char_enum.py`, `test_topics.py`.

**Gateway PDU encoding implemented** — `FixGatewaySeqThread::handle_new_order_single` and `handle_order_cancel_request` now build `pubsub_itc_fw_app::NewOrderSingle` / `OrderCancelRequest` structs and call `forward_pdu_to_sequencers<MsgT>()` template. `FixMessage.hpp` gets `Tag::TimeInForce = 59`. `Tag::OrdType`, `Tag::OrderQty`, `Tag::Price` already present.

**Connection retry implemented** — `OutboundConnectionManager` now stashes failed connects in `pending_retries_` map with a retry-due timestamp. `retry_failed_connections()` called from `Reactor::on_housekeeping_tick()` with a lambda for ID allocation. Fixed use-after-free where `conn.service_name()` was accessed after `teardown_connection` destroyed conn — now saves service name and thread ID before teardown. `ReactorConfiguration::connect_retry_interval_` added (default 2s). Documented as temporary TCP rendezvous workaround pending WAL-based brokerless pub/sub.

**`SequencerThread::on_framework_pdu_message` implemented** — decodes order PDUs (pdu_id 1000/1001) arriving on `inbound:7001`/`inbound:7002`, increments `next_sequence_number_`, re-encodes and forwards to ME via `send_pdu`. Decodes ER PDUs (pdu_id 1002) arriving on `inbound:7021`/`inbound:7022`, re-encodes and forwards to gateway via `send_pdu`. All paths call `release_pdu_payload`. Uses `decode_arena_buffer()` for `BumpAllocator`. `matching_engine_conn_id_` member added — stores inbound ME connection when ME connects on port 7021/7022.

**Verified end-to-end:** fix8 sends 50 NewOrderSingle → gateway encodes as PDUs → sequencer primary receives PDUs on correct connection ID → forwards to ME (stub). Connection ID correctly shown in logs (`PDU received on connection 5`).

**Known issue at session end:** Startup not working correctly after latest changes — sequencer primary log missing from final run. Investigation interrupted by usage limit. Resume by sharing `sequencer_primary.log`.

### Session 11

**Inbound connection identification via listener port** — `InboundConnectionManager::on_accept` now delivers `ConnectionEstablished` events with `ConnectionID{value, "inbound:<port>"}` so `on_connection_established()` can identify which inbound listener accepted a connection. Example: gateway sees `"inbound:9879"` for FIX clients and `"inbound:7010"` for sequencer ER connections. *(Note: session 13 discovered this had been described in the design but the code change had never actually landed in `on_accept` — service_name was being left empty. The implementation was completed in session 13.)*

**`FixGatewaySeqThread` connection identification completed** — `on_connection_established` now has four branches: `sequencer_primary`, `sequencer_secondary`, `inbound:7010` (sequencer ER), and FIX client (else). `on_connection_lost` updated to match.

**`SequencerThread` `on_connection_lost` fixed** — `peer_conn_id_` and `arbiter_conn_id_` added as members. Stored in `on_connection_established`. `on_connection_lost` compares against all three stored IDs. *(Note: `peer_conn_id_` was removed in session 13 along with the entire peer Connect — see session 13 entry.)*

**Startup order fixed in `start_fix_seq_system.py`** — gateway starts second (after arbiter, before sequencers). Docstring explains the counterintuitive ordering.

**FIX session verified end-to-end with fix8** — gateway accepts connection on port 9879, Logon established, 50 NOS correctly parsed.

### Session 10

**`ConnectionID` extended to carry service name** — now its own class rather than a `WrappedInteger` typedef. Adds `service_name_` string member and `service_name()` accessor. `OutboundConnectionManager::on_connect_ready` delivers `ConnectionEstablished` with `ConnectionID{value, service_name}`. `constexpr` removed from all methods since `std::string` is not a literal type in C++17.

**Connection identification fixed in all three thread classes** — `FixGatewaySeqThread`, `SequencerThread`, `MatchingEngineThread` all use `service_name()` in `on_connection_established`.

### Session 9

**Logging infrastructure overhaul** — proper startup sequence across all four applications. New framework additions: `FileSystemUtils` (`make_directories` via POSIX `mkdir`), `FwLogLevel::from_string`, `QuillLogger::ensure_log_file_writable`, `QuillLogger::set_syslog_level`. All four configs gain required `[logging]` section. Applications now take `<logfile> <config.toml>` as arguments.

**FIX parsing in `order_gateway`** — `FixParser`, `FixSerialiser`, `FixMessage`, `FixSession` added. Logon handling, heartbeats, preamble checking all working.

### Session 8

**`InboundConnectionManager`** — multi-connection support added (one-connection restriction removed). `on_accept` delivers `ConnectionEstablished` events. `check_for_inactive_connections` implemented.

**`OutboundConnectionManager`** — `check_for_timed_out_connections` implemented. `process_send_pdu_command`, `process_send_raw_command`, `process_disconnect_command` implemented.

**`ReactorConfiguration`** — `connect_retry_interval_` added (later used for retry). `connect_timeout` present.

**Application stubs** — `order_gateway`, `sequencer`, `matching_engine`, `arbiter` — all compiling with correct Aeron topology and startup pattern.

### Session 7

**DSL `char` field type** — added throughout: lexer, parser, validator, generator_cpp, generator_pybind11. Four pybind11 test failures fixed. `fix_equity_orders.dsl` created. Application architecture designed (Aeron sequencer pattern). Four application stubs written and compiling.

### Session 6

**`RawBytesProtocolHandler` bugs fixed** — intermittent `BurstDelivery` test failure resolved. `EventMessage::create_raw_socket_message` carries `tail_position`. Design documentation written. `order_gateway` tested with fix8. All 411 tests passing.

### Session 5

**Logging subsystem rewrite** — `QuillLogger` redesigned. `FwLogLevel` values flipped. `PUBSUB_LOG` and `PUBSUB_LOG_STR` are the only two call-site macros. All 12 test files migrated.

### Session 4

**`MirroredBuffer`**, **`ProtocolType`**, **`PduProtocolHandler`**, **`InboundConnectionManager`**, **`OutboundConnectionManager`**, **`ThreadLookupInterface`** — all implemented. `ExpandableSlabAllocator` use-after-free fixed.

### Session 3

TcpSocket EAGAIN/EOF fix, use-after-free fix, InboundConnection infrastructure, DSL generator fixes, integration test infrastructure, `ApplicationThread::get_reactor()` added.

---


---

## Recent Named Sessions

## Session 2026-06-02 — FIX Logon fix, WAL replication verification, fix-test-client

### Gateway: DefaultApplVerID fix

The order gateway's Logon reply was missing `DefaultApplVerID` (tag 1137), causing fix8 clients to
reject the session with `Missing Mandatory Field`. Fix applied in two parts:

- `applications/order_gateway/FixMessage.hpp`: added `static constexpr int DefaultApplVerID = 1137`
  to the `Tag` namespace.
- `applications/order_gateway/OrderGatewayThread.cpp`: added `reply.set(Tag::DefaultApplVerID, "9")`
  to the Logon reply block (value "9" = FIX.5.0SP2).
- `applications/order_gateway/FixSerialiser.cpp`: added `Tag::DefaultApplVerID` to the `app_tags[]`
  array so the serialiser actually emits the field on the wire.

After rebuilding, fix8 connected successfully. Session log confirms: 1,002 messages processed,
heartbeat active at 10-second interval.

### WAL replication verification

Confirmed WAL sync between leader and follower sequencers is working:

- Primary WAL: `/var/tmp/pubsub/sequencer_wal/wal_001964.log` — 4,194,304 bytes,
  `last_seq_no = 79,479,215`, modified 19:26:46.325 BST.
- Secondary WAL: `/var/tmp/pubsub/sequencer_secondary_wal/wal_002051.log` — 4,194,304 bytes,
  `last_seq_no = 79,479,215`, modified 19:26:46.337 BST.

Both files at identical sequence number, 12 ms apart.

### New component: java/fix-test-client

A FIX 5.0 SP2 gateway test client replacing fix8 for interactive and scripted testing.
Single-user, single-session web application.

**Technology stack:** QuickFIX/J 2.3.1, Javalin 6.3.0, Groovy 4.0.21, toml4j, Logback 1.5.x.
Styling was Pico.css at the time; Pico was removed on 2026-07-29 and `web/style.css` is now the
whole stylesheet. Fat JAR via maven-shade. Java 17. No Spring.

**Architecture:**
- `FixEngine` — wraps `SocketInitiator`; owns session lifecycle; exposes `SessionStatus` record.
- `FixApplication` — `quickfix.Application` implementation; routes inbound messages to blotter and
  capture queue via registered listener.
- `BlotterStore` — thread-safe; accumulates all outbound NOS and inbound ER messages for the session;
  parses ER fields (ClOrdID, OrderID, ExecID, ExecType, OrdStatus, Symbol, Side, OrdQty, Price,
  OrdType, CumQty, LeavesQty) to `BlotterRow` records.
- `MessageCapture` — writer thread drains a `LinkedBlockingQueue<Message>` to a timestamped log
  file in `output/`. Active while a script is running.
- `LogBuffer` — Logback `AppenderBase`; copies every `ILoggingEvent` into a 1000-line ring buffer;
  pushes new entries to SSE subscriber queues.
- `ScriptRunner` — executes Groovy scripts in a dedicated thread via `GroovyShell`; binds `session`
  (`FixSessionBinding`), `fix` (`FixHelper`), and `sleep` (a `groovy.lang.Closure`).
- `WebServer` (Main) — Javalin with five page sets of routes; manual DI; centralized exception
  handling; static files from classpath `/web/`.

**UI (five pages, always-visible nav + session status strip on every page):**
- Session — logon form with optional seq-num override; live post-logon detail (ticking duration,
  live seq counters); last-session summary shown after logout.
- Script — Groovy editor with Load/Save/New; state badge (IDLE / RUNNING / COMPLETED / FAILED);
  live output; capture status.
- Messages — New Order Single send form; blotter table with row colouring by OrdStatus (filled=green,
  partial=amber, rejected/cancelled=red); blotter persists for the session.
- Config — read-only display of `app.toml` and `session.cfg`.
- Logs — SSE log stream with Pause/Resume; last 1000 lines shown on load.

**Build:** `mvn package` in `java/fix-test-client/`. Fat JAR at `target/fix-test-client-*.jar`.
**Run:** `java -jar target/fix-test-client-*.jar` — opens on `http://localhost:8081`.

Session config fix required: `StartTime=00:00:00` and `EndTime=00:00:00` added to `config/session.cfg`
so QuickFIX/J does not reject startup with `ConfigError: StartTime not defined`.

**Status:** builds cleanly; startup confirmed. End-to-end testing (logon, NOS send, ER receipt,
blotter, scripting) deferred to next session.

---

## Session 2026-06-03 — OrderCancelRequest stub in matching engine

### Change

Added `OrderCancelRequest` (PDU 1001) handling to the matching engine stub.

**Root cause of gap:** The gateway (`OrderGatewayThread.cpp:775`) and sequencer
(`SequencerThread.cpp:317`) already decoded and forwarded OCR PDUs end-to-end.
The matching engine's `on_framework_pdu_message()` only checked for NOS (1000)
and dropped everything else as "unsupported".

**Files changed:**
- `applications/matching_engine/MatchingEngineThread.hpp`: added
  `handle_order_cancel_request(const OrderCancelRequestView&, int64_t seq_no)` declaration.
- `applications/matching_engine/MatchingEngineThread.cpp`:
  - Added OCR decode + dispatch branch in `on_framework_pdu_message()` (mirrors NOS branch).
  - Added `handle_order_cancel_request()`: fabricates a cancel-confirmed ExecutionReport
    (`ExecType::Canceled` / `OrdStatus::Canceled`, `LeavesQty=0`, `CumQty=0`) and sends it
    to both sequencer ER connections with the incoming `seq_no` as the transport sequence
    number — same mechanism used by `handle_new_order_single()`.

**Stub behaviour:** every cancel is unconditionally confirmed (no real order book). The ER
echoes `cl_ord_id` and `orig_cl_ord_id` from the OCR, and carries a fabricated `order_id` /
`exec_id` ("ME-ORD-N" / "ME-EXEC-N" from the existing monotonic counters).

**Build:** `cmake --build build --target matching_engine` — clean.

### fix-test-client: OrderCancelRequest support

Added full cancel support to the Java web client.

**Files changed:**
- `BlotterRow.java`: added `origClOrdId` field (after `clOrdId`).
- `BlotterStore.java`: extracts `OrigClOrdID` (tag 41) in `buildRow`.
- `MessagesHandler.java`: added `cancel(Context)` — reads `origClOrdId`, `symbol`, `side`, `qty`
  from form params; auto-generates cancel `clOrdId` as `"CXL-{origClOrdId}-{millis}"`; builds
  and sends `quickfix.fix50sp2.OrderCancelRequest`; adds row to blotter. Updated `getBlotter` to
  expose `origClOrdId`.
- `Main.java`: wired `POST /api/messages/cancel` → `messagesHandler::cancel`.
- `FixHelper.java`: added `orderCancelRequest()` factory for Groovy scripts (pre-sets `TransactTime`).
- `messages.html`:
  - Added "Order Cancel Request" form (OrigClOrdID, Symbol, Side, Qty, Send Cancel button).
  - Added `OrigClOrdID` column to blotter table.
  - Added `Action` column: Cancel button on each outbound NOS row (rows without `origClOrdId`);
    clicking pre-fills the cancel form.
  - Added `doCancel()` and `prefillCancel()` JS functions.

**Build:** `mvn package` in `java/fix-test-client/` — clean.

---

## Session 2026-06-03 (continued) — Matching engine order book, devenv/build pipeline, fix-test-client hardening

### Matching engine: primitive order book + meaningful cancel

Replaced the unconditional-fill stub with a primitive order book keyed by ClOrdID.

**Order lifecycle:**

| Inbound PDU | Condition | Response ER |
|---|---|---|
| NOS, new ClOrdID | — | ExecType=New / OrdStatus=New; order inserted into book |
| NOS, duplicate ClOrdID | — | ExecType=Rejected / OrdStatus=Rejected / OrdRejReason=DuplicateOrder (6) |
| OCR, OrigClOrdID found | — | ExecType=Canceled / OrdStatus=Canceled; order removed from book; original OrderID echoed |
| OCR, OrigClOrdID unknown | — | ExecType=Rejected / OrdStatus=Rejected / OrdRejReason=UnknownOrder (5) |

**No DSL changes required.** ExecutionReport already carries `optional OrdRejReason ord_rej_reason`.
`CxlRejReason` (tag 102) was initially used for cancel rejections but removed after discovery that
QFJ validates ER against FIX50SP2.xml and rejects messages containing tag 102 in MsgType=8.
Both rejection cases now use `OrdRejReason` (tag 103), which IS valid in ExecutionReport.

**Key files:**
- `MatchingEngineThread.hpp`: added `OrderEntry` struct (order_id, symbol, side, order_qty,
  price, ord_type); `order_book_` (`unordered_map<string, OrderEntry>`); `send_er_to_sequencer()`
  helper; `order_book_initial_capacity` config field.
- `MatchingEngineThread.cpp`: complete rewrite of `handle_new_order_single` and
  `handle_order_cancel_request`; `send_er_to_sequencer()` eliminates repeated primary/secondary
  send pattern; `order_book_.reserve(config_.order_book_initial_capacity)` in constructor.
- `MatchingEngineConfiguration.hpp`: added `int32_t order_book_initial_capacity{1024}`.
- `MatchingEngineConfigurationLoader.cpp`: loads and validates `order_book.initial_capacity`.
- `matching_engine.toml`: added `[order_book]` section, `initial_capacity = 1024`.

**Sizing guidance for `order_book.initial_capacity`:** set to peak concurrent live (non-terminal)
orders for the target environment. Default 1024 covers manual/scripted testing without rehashing.
Load-test deployments should increase to 65536 or higher.

### FixErEncoder: OrigClOrdID, OrdRejReason encoding

The FIX wire encoder was missing three optional ER fields. Added to both the body-length
pre-calculation and the writer in `applications/order_gateway/FixErEncoder.cpp`:

- Tag 41 `OrigClOrdID` — written immediately after ClOrdID when `has_orig_cl_ord_id`.
- Tag 103 `OrdRejReason` — written after OrdStatus when `has_ord_rej_reason`.

Tag constants `CxlRejReason = 102` and `OrdRejReason = 103` added to `FixMessage.hpp`.

### build-release-deploy.sh rewrite

Replaced the broken four-line script with a proper pipeline wrapper:

- Cleans `installed/` before build.
- Runs `./build.sh --no-tests` (skips C++, Java, and Python tests).
- Runs `release.py`; finds the produced tarball in `build/release/` dynamically.
- Runs `deploy.py --artefact <tarball>`.
- `set -euo pipefail` aborts on any stage failure.
- `--skip-db` flag passes through to `deploy.py` for environments where the DB already exists.

### devenv.py: docstrings, fix_test_client and admin_service support

Full rewrite of `devenv.py` (pylint score 10.00/10):

- Docstrings added to all public and private functions.
- `build_command()` updated: JAR components with a `config` key have the resolved config path
  appended as a positional argument. This allows the fix-test-client to receive its `app.toml`
  path. admin_service (Spring Boot) has no `config` key and launches without extra args.
- Standard library imports moved above the `tomllib` try/except (fixes pylint ungrouped-imports).
- `subprocess.run` uses explicit `check=False`; `Popen` suppress is documented with inline disable.

### dev.toml, build.py, release.py: fix_test_client integration

**`environments/dev.toml`:**
- Added `fix_test_client` to `startup_order` (last, after `admin_service`).
- Added `[components.fix_test_client]`: `jar = "lib/fix-test-client.jar"`,
  `config = "etc/fix_test_client/config/app.toml"`, `workdir = "etc/fix_test_client"`.
- Removed unused `config` field from `admin_service` (was always ignored for JARs).

**`build.py`:** added `build_fix_test_client()`:
- Runs `mvn package` in `java/fix-test-client/`.
- Copies fat JAR to `build/installed/lib/fix-test-client.jar`.
- Mirrors `java/fix-test-client/config/` → `build/installed/etc/fix_test_client/config/`
  (both `app.toml` and `session.cfg`).
- Called from `main()` alongside `build_java_service()`.

**`release.py`:** added `stage_java_configs()`:
- Scans `java/*/config/` directories; stages into `etc/<component_name>/config/`
  (hyphens in directory name replaced with underscores).
- Called from `main()` after `stage_etc()`.

### fix-test-client: blotter improvements

**Blotter container (sticky header, scrolling):**
- `#blotter-wrap`: `overflow-y: auto; max-height: 50vh` — blotter scrolls independently.
- `#blotter th`: `position: sticky; top: 0` — header sticks within the scroll container,
  no longer collides with page-level scroll.
- `#blotter`: removed `width: 100%` — table is as wide as content requires; `overflow-x: auto`
  on the wrapper provides horizontal scrolling.
- Action column (`th:last-child`, `td:last-child`): `position: sticky; right: 0; background: inherit`
  — Cancel buttons remain visible at the right edge regardless of horizontal scroll position.

**Clear button:** `POST /api/messages/clear` → `blotterStore.clear()`; wired in `Main.java`;
`doClear()` in JS resets `lastBlotterSize` so blotter re-renders immediately.

**Human-readable labels:** `EXEC_TYPE_LABEL` and `ORD_STATUS_LABEL` JS maps translate raw FIX
characters to words (e.g. `'0'→'New'`, `'4'→'Canceled'`). Raw value shown as fallback.

**Rejection reason column:** `Reason` column added between OrdStatus and Symbol.
- `BlotterRow`: added `ordRejReason` and `cxlRejReason` fields.
- `BlotterStore`: extracts tags 103 and 102 via `getString`.
- `MessagesHandler`: exposes both in JSON.
- `rejectionReason(row)` JS function: checks `ordRejReason` first, then `cxlRejReason`;
  maps numeric codes to labels via `ORD_REJ_REASON_LABEL` and `CXL_REJ_REASON_LABEL`.

**Stale Cancel button fix:** `isOrderTerminal(rows, clOrdId)` scans all IN rows for a terminal
OrdStatus (Filled, Canceled, Rejected, DoneForDay, Expired) matching either `r.clOrdId` or
`r.origClOrdId`. The Cancel button is suppressed for terminal orders. Originally only checked
`clOrdId`; the `origClOrdId` check was required because a cancel ER carries the cancel request's
ClOrdID in tag 11, not the original order's ClOrdID.

**Order control button disabling (messages.html):** Send and Send Cancel buttons start `disabled`.
`setOrderControlsEnabled(loggedOn)` toggles them. `pageInit` hooks into `window.updateStatusStrip`
(same pattern as `session.html`) to keep buttons in sync with every 2-second status poll. Initial
state applied immediately from `window._lastSessionStatus` if already populated.

### fix-test-client: session page UX

**`session.cfg`:**
- `ResetOnLogon=N` → `ResetOnLogon=Y`: sequence numbers reset to 1 on every logon. Required
  because the gateway always restarts fresh; without this, the stored client seq nums mismatch
  and the gateway rejects the logon.
- Added `ReconnectInterval=5`: QFJ reconnects within 5 s after disconnect instead of the
  default 30 s. Clicking Log On is now near-instant rather than appearing to do nothing.

**`session.html`:**
- `doLogon()`: disables button and shows "Connecting…" immediately on click.
- `doLogout()`: disables button and shows "Logging out…" immediately on click.
- `renderNotLoggedOn()`: resets the Log On button (text + enabled state) so it is not left
  stuck in "Connecting…" if the logon fails.
- `renderLoggedOn()`: resets the Log Out button (text + enabled state) so it is not left
  stuck in "Logging out…" after a reconnect.

---

## Session 2026-06-03 (continued) — Fix-test-client polish, Doxygen hardening, DSL bytes type

### fix-test-client: local time on session page

`formatLogonTime()` added to `session.html`. The server sends `logonTime` as a UTC string
(`yyyy-MM-dd HH:mm:ss`). The helper appends `Z` to make it unambiguously UTC for the `Date`
constructor, then formats both forms for display:
```
2026-06-03 09:18:54 UTC  (03/06/2026, 10:18:54 local)
```
`toLocaleString()` uses the browser's locale and timezone automatically.

### fix-test-client: blotter sticky column alignment

**Root cause:** Row background colours were applied via `#blotter tr.X td { background: Y }`
selectors. The sticky last column uses `background: inherit`, which inherits from the parent
`<tr>`, not from other `<td>` rules. `<tr>` had no explicit background, so `inherit` resolved
to `transparent` (page background `#f4f4f4`), making the Cancel buttons appear against the
wrong colour.

**Fix:** All row colours moved to `<tr>`-level rules. Added `#blotter tbody tr { background: white }`
as the default and `#blotter thead tr { background: #ccc }` for the header. The sticky column
now correctly matches every row's colour, including status-filled (green), status-cancelled
(pink), and dir-out (light gray).

### Doxygen: warnings-as-errors, documentation fixes

**`Doxyfile`:**
- `WARN_AS_ERROR = FAIL_ON_WARNINGS` — Doxygen exits non-zero on any warning; `run_command()`
  propagates the failure to abort the build.
- `WARN_NO_PARAMDOC = YES` — warns when a documented function has undocumented parameters.
- `EXCLUDE_PATTERNS = *.cpp` — coding rules state all docs live in headers; parsing `.cpp`
  files was generating spurious include-path warnings.
- `TIMESTAMP = DATETIME` — every generated page now shows date and time of generation.
- `LatencyRecorder.hpp` added to INPUT (specific file, not the whole tests_common directory).

**Source fixes (all pre-existing issues caught by the stricter settings):**
- `ConnectionID.hpp`: escaped `<port>` → `\<port\>` in two doc comments (Doxygen was treating
  the angle brackets as HTML tags).
- `InboundConnection.hpp`, `InboundConnectionManager.hpp`, `Reactor.hpp`: added `@param[in]`
  for `idle_timeout_exempt` on `InboundConnection` constructor and both `register_inbound_listener`
  overloads. The parameter exempts a connection/listener from the inactivity timeout.
- `SimpleSpan.hpp`: reworded `std::span/boost::span` as `` `std::span` `` (backtick code span)
  to suppress Doxygen auto-link resolution of the unresolvable qualified name.
- `MatchingEngineThread.hpp` class doc: `CxlRejReason=UnknownOrder` corrected to
  `OrdRejReason=UnknownOrder` (reflects the fix made after QFJ rejected tag 102 in ER).
- `mainpage.dox` development environment: updated from "Linux RHEL8 only" to document both
  supported distributions (Linux Mint 22 primary, RHEL8/Rocky8 also supported).

**Instrumentation subsystem page populated:**
- `HighResolutionClock.hpp`: converted C block comment to Doxygen format; added `@brief`,
  `@par` rationale sections, `@see MillisecondClock`, and `@ingroup instrumentation_subsystem`.
- `MillisecondClock.hpp`: added `@ingroup instrumentation_subsystem` (doc was already thorough).
- `LatencyRecorder.hpp`: added `@ingroup instrumentation_subsystem`; expanded class brief;
  added missing `@param[in] label` to `dump_results()`.

### DSL documentation: bytes type

`docs/dsl_design.dox` updated in three places:

1. **Primitives table** — added `bytes | BytesView | 4-byte length prefix + raw bytes`.
2. **Compound types section** — added entry for `bytes`: variable-length raw byte sequence,
   4-byte length prefix + raw bytes, decode-side C++ type is `BytesView` (non-owning
   `{data, size}` pair pointing into the wire buffer, zero-copy). Distinguished from `string`
   (which implies UTF-8 text).
3. **Wire format table** — added `bytes | 4-byte byte-count + raw bytes` row.

### Build status

Full build (`./build.sh --no-tests --doxygen`) passes cleanly: pylint, C++ (32-core make),
CMake install, Doxygen (zero warnings with `WARN_AS_ERROR=FAIL_ON_WARNINGS`), Java admin
service, fix-test-client. All changes from both 2026-06-03 sessions committed.

---

## Session 2026-06-04 — Injectable WallClock for replay

### Motivation

For replay to produce correct results, any component that generates a business
timestamp (particularly `transact_time` on ExecutionReports) must draw from an
injectable clock rather than calling `system_clock::now()` directly. This allows
a `ReplayClock` to be driven from the original WAL timestamps, ensuring that
replayed ERs carry the same `transact_time` as the originals.

### New framework type: `WallClock`

`libraries/pubsub_itc_fw/include/pubsub_itc_fw/WallClock.hpp`

Three classes, all tagged `@ingroup instrumentation_subsystem`:

- **`WallClock`** — abstract base; single pure-virtual `int64_t now_ns() const`.
- **`SystemWallClock`** — wraps `std::chrono::system_clock::now()`. Used in all
  production configurations (the default everywhere).
- **`ReplayClock`** — holds an `std::atomic<int64_t>`. `set_time_ns(t)` advances
  the clock; `now_ns()` returns it. Thread-safe. Intended to be driven by the
  sequencer replay loop (call `set_time_ns(wal_record.timestamp)` before
  dispatching each WAL record to the ME).

### DSL: `sequenced_at` field on NOS and OCR

`optional datetime_ns sequenced_at` added to both `NewOrderSingle` and
`OrderCancelRequest` in `fix_equity_orders.dsl`. This field is the carrier for
the sequencer's timestamp: it is set when the sequencer sequences the PDU and
read by the ME when generating the ER's `transact_time`. The gateway never sets
it; the field is absent on the inbound PDU from the FIX client.

### Sequencer: stamp `sequenced_at`

`SequencerConfiguration` gains `std::shared_ptr<WallClock> wall_clock` (default:
`SystemWallClock`). `wall_time_ns` is computed once per order PDU and reused for
the WAL record, the NOS/OCR `sequenced_at` field, and the `WalRecord` PDU sent
to the follower (see WAL timestamp section below for detail).

### Matching engine: `sequenced_at`-first transact_time

`MatchingEngineConfiguration` gains `std::shared_ptr<WallClock> wall_clock`
(default: `SystemWallClock`). Both `handle_new_order_single` and
`handle_order_cancel_request` replace the old `system_clock::now()` call with:
```cpp
const int64_t now_ns = view.has_sequenced_at
                           ? view.sequenced_at
                           : config_.wall_clock->now_ns();
```
Live path: `has_sequenced_at` is always true (sequencer stamps it), so the clock
fallback is a safety net only.

### Gateway: clock injected into FixSerialiser and FixErEncoder

`OrderGatewayConfiguration` gains `std::shared_ptr<WallClock> wall_clock`
(default: `SystemWallClock`).

- **`FixSerialiser`**: constructor now takes `const WallClock&`; stored as a
  member reference. `current_utc_timestamp()` made non-static; derives time from
  `wall_clock_.now_ns()` rather than `system_clock::now()`.
- **`FixErEncoder::encode_execution_report()`**: signature extended with
  `const WallClock& wall_clock`; `fill_utc_timestamp()` updated accordingly.

`SendingTime` (FIX tag 52) is a transport field (when the message was put on the
wire). `SystemWallClock` is correct here even during replay; the injection point
exists for unit-test determinism.

---

## Session 2026-06-04 (continued) — WAL timestamp + sequencer --replay flag

### WAL on-disk format: `wall_time_ns` added

**On-disk layout changed** (backward-incompatible; delete the WAL directory
before running updated binaries):

```
Old: pdu_id (int16_t, 2 bytes) | PDU payload bytes
New: wall_time_ns (int64_t, 8 bytes) | pdu_id (int16_t, 2 bytes) | PDU payload bytes
```

**Files changed:**

- `leader_follower.dsl` (`WalRecord` message): added `datetime_ns wall_time_ns`
  so the leader sends the original timestamp to the follower along with the PDU.
- `SequencerWal.hpp`:
  - Class doc updated to reflect new payload format.
  - `append()` gains `int64_t wall_time_ns` parameter.
  - `ReplayCallback` gains `int64_t wall_time_ns` parameter (after `payload_size`).
- `SequencerWal.cpp`:
  - `append()`: writes `wall_time_ns` (8 bytes) before `pdu_id` in the combined
    buffer; total buffer size increases by 8 bytes per record.
  - `open()` callback: extracts `wall_time_ns` from bytes 0–7, `pdu_id` from
    bytes 8–9, then calls the `ReplayCallback` with all five parameters.
- `SequencerThread.hpp`: `send_wal_record()` declaration updated.
- `SequencerThread.cpp`:
  - `wall_time_ns` computed **once** per order PDU at the top of the
    `is_order_pdu` block (`const int64_t wall_time_ns = config_.wall_clock->now_ns()`),
    then used for: local WAL `append()`, NOS/OCR `sequenced_at` field, and the
    `WalRecord` sent to the follower. The three values are guaranteed identical.
  - `send_wal_record()`: populates `wal_record.wall_time_ns`.
  - `handle_wal_record()`: passes `view.wall_time_ns` to `wal_.append()`.

### Sequencer `--replay` flag

Usage: `sequencer <logfile> <config.toml> [--replay]`

**Behaviour in replay mode:**
1. `main()` sets `config.replay_mode = true`.
2. `on_initial_event()`: opens the WAL with a buffering `ReplayCallback` that
   copies each record (seq_no, pdu_id, wall_time_ns, payload bytes) into
   `replay_buffer_`. Skips the WAL snapshot timer. Sets role = leader immediately
   (no HA election).
3. `on_app_ready_event()`: connects to the matching engine only — no gateway,
   arbiter, or peer connections.
4. `on_connection_established()` when ME connects: calls
   `dispatch_replay_records()`.
5. `dispatch_replay_records()`: for each buffered record, decodes the payload into
   an owning NOS/OCR struct, stamps `sequenced_at = record.wall_time_ns`, then
   calls `send_pdu(me_outbound_order_conn_id_, record.pdu_id, record.seq_no, ...)`.
   The ME receives each PDU with its original timestamp, processes it as normal,
   and sends back ERs. Logs completion with dispatched/total counts.

**Key design properties:**
- The ME and gateway binaries are unchanged — replay is entirely a sequencer
  concern.
- `sequenced_at` in each replayed PDU equals the `wall_time_ns` stored in the
  WAL record, which equals what was stamped during the original live run. The ME's
  `sequenced_at`-first logic produces identical `transact_time` values on ERs.
- No `ReplayClock` injection is needed for dispatch: `sequenced_at` carries the
  original timestamp directly. The `wall_clock` in `SequencerConfiguration`
  remains `SystemWallClock` in replay mode (it is not called during dispatch).

**New types in `SequencerThread.hpp`:**
```cpp
struct ReplayRecord {
    int64_t seq_no{};
    int16_t pdu_id{};
    int64_t wall_time_ns{};
    std::vector<uint8_t> payload;
};
std::vector<ReplayRecord> replay_buffer_;
void dispatch_replay_records();
```

**Build:** `cmake --build build --target sequencer` — clean.

---

## Session 2026-06-04 (continued) — Replay bug fixes and end-to-end verification

Two bugs found during live testing and fixed before a successful end-to-end replay.

### Bug 1: dispatch race — ME drops NOS because ER connection not yet ready

**Symptom:** Matching engine log showed:
```
MatchingEngineThread: no sequencer ER connections established -- dropping NOS
```

**Root cause:** `dispatch_replay_records()` was triggered as soon as the outbound
sequencer→ME order connection was established. The ME received the NOS immediately
but its outbound connection back to the sequencer's ER inbound port (7021) was not
yet up, so `sequencer_er_conn_id_` was invalid and the ME dropped the order.

**Fix:** Two readiness flags in `SequencerThread`:
- `replay_me_order_ready_` — set when outbound ME order connection establishes
- `replay_me_er_ready_` — set when the inbound ME→sequencer ER connection arrives
  (detected in `on_connection_established()` `else` branch when
  `svc == er_inbound_svc_`)

New helper `try_dispatch_replay()` gates dispatch on both flags being true.
Dispatch fires only once the ME can both receive orders AND send ERs back.

### Bug 2: snapshot bypasses all WAL records during full replay

**Symptom:** Sequencer replay log showed `0 record(s)` even though `last_seq_no=8`.

**Root cause:** `SequencerWal::open()` loads the snapshot first. The snapshot's WAL
anchor points to the position AFTER all committed records (written at snapshot time),
so `WalReader::replay()` finds nothing to replay from that anchor. This is correct
for crash recovery (avoids re-processing already-applied records) but wrong for full
replay where every record must be revisited.

**Fix:** Added `bool full_replay = false` parameter to `SequencerWal::open()`. When
`true`, `load_snapshot()` is skipped entirely and the WAL anchor stays at `{0, 0}`,
causing `WalReader::replay()` to visit every record from segment 0. The sequencer
passes `full_replay = true` in replay mode; normal crash-recovery open is unchanged.

### End-to-end test result

```
# Sequencer replay log
WAL read complete: 3 record(s), last seq_no=3
Both ME connections ready, starting dispatch
Replay complete -- 3/3 record(s) dispatched to matching engine

# Matching engine log
accepted NOS OrderID=ME-ORD-1 ExecID=ME-EXEC-1 ClOrdID=ORD-001 book_size=1
accepted NOS OrderID=ME-ORD-2 ExecID=ME-EXEC-2 ClOrdID=ORD-002 book_size=2
accepted NOS OrderID=ME-ORD-3 ExecID=ME-EXEC-3 ClOrdID=ORD-003 book_size=3
```

All three orders replayed in sequence with their original `sequenced_at` timestamps.
The ME's order book rebuilds to the same state as the original live run.

**Complete replay flow (as verified):**
1. Run live session: send orders via fix-test-client → WAL records written with
   `wall_time_ns` per record.
2. Stop all components.
3. Start matching engine only (`devenv.py restart matching_engine`).
4. Run replay: `sequencer <logfile> <config.toml> --replay`
5. Sequencer opens WAL from segment 0 (ignoring snapshot), buffers all records,
   connects to ME, waits for both ME connections, dispatches all records with
   original timestamps.
6. ME rebuilds order book identically to the live run.

---

## Session 2026-06-04 (continued) — Outbound retry log noise reduction

### Problem

When the primary sequencer is killed during HA testing, the gateway log filled with
noise: "connection refused" logged at Info/Warning level on every retry attempt
(every 2 seconds by default). This made it impossible to see anything else in the
log during a failover.

### Design

Log behaviour for a failing outbound connection is now:

| Event | Log |
|---|---|
| First connect failure | Warning: "service X failed to connect; retrying every Yms (next reminder in 15min if still down)" |
| Retry attempts (silent period) | Nothing |
| Every 15 minutes still disconnected | Warning: "service X still not connected (N seconds disconnected); still retrying every Yms" — then timer resets |
| Successful reconnect | Warning: "service X reconnected after Ns" |
| First-time connect (no prior failure) | Info: existing "connection established" message unchanged |

The 15-minute reminder interval is configurable via `reactor.connect_retry_warning_interval`
in each application's TOML file.

### Implementation

**`ReactorConfiguration`** — new field:
```cpp
std::chrono::milliseconds connect_retry_warning_interval_{std::chrono::minutes{15}};
```

**`OutboundConnectionManager`** — new inner struct and map:
```cpp
struct RetryContext {
    std::chrono::steady_clock::time_point first_fail_time;
    std::chrono::steady_clock::time_point last_warning_time;
};
std::unordered_map<std::string, RetryContext> retry_contexts_;
```

`retry_contexts_` is created on first failure in `schedule_retry` and erased when
the connection is successfully established (in both the non-TLS `on_connect_ready`
path and the TLS `on_data_ready` handshake-complete path).

Log suppression is applied in three places:
- `schedule_retry` — silent after first call for a service
- `on_connect_ready` `finish_connect` failure log — guarded by `retry_contexts_` check
- `teardown_connection` Info log — guarded by `retry_contexts_` check

Periodic reminders are emitted in `retry_failed_connections` by comparing
`now - ctx.last_warning_time` against `connect_retry_warning_interval_`.

**Wired through all six application config stacks** (order_gateway, matching_engine,
authentication_service, sequencer, witness, arbiter) — each gains a
`connect_retry_warning_interval` field in its `*Configuration.hpp`, loaded via
`get_required_except` in its `*ConfigurationLoader.cpp`, and assigned to
`reactor_configuration_` in its main `.cpp`. Nine TOML files updated with
`connect_retry_warning_interval = "15m"`.

### Coding rules update

The no-alignment rule was generalised from C++ variable declarations to all file
types: "Do not add spaces to make adjacent statements or declarations line up,
regardless of file type (C++, TOML, YAML, or any other)."

### FIX test client blotter

Outbound order rows (`dir-out`) now use a light green background (`#e4f5e4`) instead
of near-white (`#f8f8f8`), making sent orders visually distinguishable from the
default row colour.

---

## Session 2026-06-05 — WAL replication jitter: Option C (inline WAL handler)

### Problem

The WAL round-trip (primary → secondary → primary) involves three sequential `epoll_wait` wakeups:

1. Secondary `SequencerThread` wakes to receive the `WalRecord` via ITC
2. Secondary reactor wakes to execute the resulting `SendPdu(WalAck)` command
3. Primary `SequencerThread` wakes to receive the `WalAck` via ITC

At ~200µs p50 per wakeup on a dev machine, these compound to a ~600µs floor. Option A (decouple ER emission from WalAck) was rejected as unsafe. Option B (timer-event priority) only reduced outliers. The root cause — three sequential thread wakeups — was not addressed by either.

### Solution (Option C)

Handle the `WalRecord` PDU entirely on the secondary reactor thread, with no ITC hop. When a complete `WalRecord` frame arrives at the secondary's `PduParser`:

1. Decode the `WalRecord` inline (stack-allocated arena, no heap allocation)
2. Call `wal_.append()` directly
3. Encode `WalAck` (8 bytes) and call `framer->send()` directly on the same TCP connection

This eliminates wakeups 1 and 2. Only wakeup 3 remains. Expected improvement: 3× reduction in WAL floor latency on any machine, independent of OS tuning.

### Framework changes

**`PduParser`** gains an `InlinePduHandler`:
```cpp
using InlinePduHandler = std::function<bool(int16_t pdu_id, int64_t seq_no, const uint8_t* payload, size_t size)>;
void set_inline_handler(InlinePduHandler handler);
```
In `dispatch_pdu()`, if an inline handler is installed and returns `true`, the slab chunk is freed and no `EventMessage` is enqueued. This is the sole mechanism — no other paths are changed.

**`PduProtocolHandler`** exposes `parser()` and `framer()` accessors (both `[[nodiscard]]`). These are needed by the reactor when installing handlers on inbound connections.

**`InboundConnectionManager`** gains `find_by_id(ConnectionID)`. Previously only `find_by_fd` existed.

**`ReactorControlCommand`** gains an `InstallInlinePduHandler` tag and an `inline_handler_installer_` field of type `std::function<void(PduParser*, PduFramer*)>`. The installer pattern gives application code access to both parser and framer for handler setup without holding a raw `PduFramer*` pointer.

**`Reactor`** handles `InstallInlinePduHandler` in `process_control_commands()`: looks up the connection by `ConnectionID` (trying outbound first, then inbound via `dynamic_cast<PduProtocolHandler*>`), retrieves parser and framer, calls the installer. `enqueue_control_command` changed from `const&` to by-value to allow the `std::function` to be moved into the queue rather than copied.

**`ApplicationThread`** gains `install_inline_pdu_handler(ConnectionID, installer)`, which posts the `InstallInlinePduHandler` command.

### Sequencer changes

`SequencerThread::install_peer_wal_inline_handler(ConnectionID)` (new private method) is called from `on_connection_established()` for both the outbound `"peer"` connection and the inbound peer listener connection. It posts an `InstallInlinePduHandler` command whose installer lambda captures `this` (for `wal_` and logger) and the reactor-supplied `framer*`. The inner `InlinePduHandler` lambda:

1. Returns `false` immediately for any PDU other than `pdu_wal_record` (103).
2. Returns `false` if `framer->has_pending_data()` — backpressure fallback; the PDU goes through the normal ITC path.
3. Otherwise: decodes `WalRecord` with a 4 KiB stack arena, calls `wal_.append()`, encodes an 8-byte `WalAck` with `encode_fast()`, calls `framer->send()`, returns `true`.

The existing `handle_wal_record()` in `SequencerThread` remains as a fallback for the brief startup window before the inline handler command is processed by the reactor.

### Threading invariant

`SequencerWal::append()` is single-writer at any given time:
- On the leader: all writes happen on `SequencerThread` (role guard in `on_framework_pdu_message` prevents follower from writing).
- On the follower: after the inline handler is installed, all writes happen on the reactor thread. `SequencerThread::handle_wal_record()` is bypassed (the inline handler returns `true`, consuming the PDU before ITC dispatch). No concurrent access.

The handler is re-installed on each peer reconnect (`on_connection_established` fires again), so a stale `framer*` is never held.

### Build result

All 33 sequencer build targets clean. Unit tests (30), integration tests (all), and gateway tests (6) all pass.

### Latency measurement — 12 manual orders

First run with inline handler active. NOS→ER latencies from gateway log (GW-NOS-RECV to GW-ER-SENT):

| Order | µs | | Order | µs |
|---|---|---|---|---|
| ORD-006 | **160** | | ORD-009 | 769 |
| ORD-003 | 297 | | ORD-010 | 809 |
| ORD-001 | 329 | | ORD-004 | 918 |
| ORD-012 | 650 | | ORD-002 | 1101 |
| ORD-007 | 665 | | ORD-008 | 1156 |
| ORD-011 | 741 | | ORD-005 | 1420 |

Min **160µs** · Median 769µs · Max 1420µs · Mean 751µs

Previous baseline (session 25, before Option C): min 389µs, typical 490–690µs, max 1769µs.

**Interpretation.** The 160µs result — and the 297µs and 329µs results — are new territory, all below the previous minimum of 389µs and below even a single `epoll_wait` wakeup (p50 ~199µs on this machine). These represent cases where the inline WAL path completed *before* the ME returned the ER. The primary's `wal_acked_seq_nos_` already held the ack when the ER arrived, so the ER was forwarded with no WAL wait at all. The measured latency was purely the ME round-trip plus gateway wakeup.

The 650µs–1420µs tail is unchanged in character. In those cases the primary `SequencerThread` wakeup (wakeup 3 — the one remaining `epoll_wait` hop) suffered the full OS scheduler jitter before processing the WalAck. The secondary log confirms inline handler installation on both peer connections (connections 5 and 8). The reactor debug confirmation is not visible at Info log level.

**Conclusion.** Option C is working as designed. Two of the three sequential wakeups are eliminated. When the inline path outpaces the ME, the WAL is invisible to latency. When wakeup 3 is slow, the tail remains. SCHED_FIFO + isolcpus on production hardware would reduce wakeup 3 from ~200µs p50 to ~5µs, collapsing the tail and making sub-100µs median achievable.

## Session 2026-06-06 — Perf-test reliability, SEGV fix, ResendRequest handler

### Overview

This session investigated a SIGSEGV in the unit-test suite and the root cause of the
"random missing orders" in the perf test. One bug was fully fixed; two issues remain
outstanding and are documented below.

---

### Bug 1 — SIGSEGV in timer unit tests (fixed)

**Symptom.** Running `TimerTest.*` caused a SIGSEGV on the second test case.

**Root cause.** `Reactor::finalize_threads_after_shutdown()` waited for application
threads to stop (step 2) and tried to join them (step 3), but never signalled them to
stop. An `ApplicationThread` that had drained its queue sat blocked in
`epoll_wait(..., 1000 ms)`. With a 200 ms shutdown timeout the wait expired, join failed,
and the test's `Reactor` was destroyed while the zombie thread was still running. When the
second test's `Reactor` was constructed, the zombie thread woke from `epoll_wait`, called
`reactor_.is_running()` on the already-destroyed first `Reactor`, and crashed.

**Fix.** Added a `thread->shutdown(shutdown_reason_)` call for every registered thread
inside `finalize_threads_after_shutdown()`, immediately after cancelling timer fds and
before the step-2 wait loop. `ApplicationThread::shutdown()` sets lifecycle state to
`ShuttingDown` (making `is_running()` return false) and writes to `notify_fd_` to wake the
thread from `epoll_wait` immediately.

File changed: `libraries/pubsub_itc_fw/src/Reactor.cpp`

**Status.** SIGSEGV eliminated; all `TimerTest.*` cases pass. However "did not stop
within shutdown_timeout" and "failed to join within shutdown_timeout" errors still appear
in the test logs. Despite `shutdown()` setting the lifecycle state atomically,
`is_running()` still returns true for the full 200 ms timeout before the thread exits.
Root cause of the remaining timeout not yet identified — separate follow-up item.

---

### Bug 2 — Perf-test "missing orders" root cause investigation

#### What the perf script reports

Running with 20 fix8 clients x 50 T-bursts (1 M orders total) the script reported:

- `ME-ORD stopped at 999,830 / 1,000,000 (170 missing)` — bail-out after 279 s idle
- `GW-ER-SENT stopped at 988,762 / 1,000,000 (11,238 missing)` — bail-out after 8 s idle
- All 20 fix8 clients had to be killed explicitly

The number of "missing" orders varied between runs (166, 170, ...).

#### Finding 1 — ME-ORD "170 missing" is a Quill flushing artefact (no orders lost)

`grep -c "ME-ORD" matching_engine.log` after process exit: **1,000,000** exactly.
`ME-ORD-1000000` has timestamp `20:30:17.128` — the matching engine processed every order
within **16 seconds** of the test starting. The OGT also processed all 1 M orders by
`20:30:14`.

The perf script polls `matching_engine.log` as a real-time proxy. Quill's async backend
writes entries from multiple threads in timestamp order. The reactor thread's last log
entry was `20:30:16.773`; the ME thread's last entries are at `20:30:17.xxx`. Because the
reactor thread went silent at `20:30:16.773`, Quill's backend held the ME thread's
trailing 170 entries (timestamps after the reactor's last entry) until SIGTERM forced a
final flush at `20:36:47`. The dynamic idle bail-out (279 s of no new log lines, already
increased by Gemini) fired before that flush.

**Conclusion.** No orders were lost. The 170 "missing" lines are a Quill multi-thread
timestamp-ordering artefact. Monitoring/tooling issue, not an application bug.

#### Finding 2 — GW-ER-SENT 11,238 gap: genuine ER loss, root cause found and fixed

Python analysis of all 988,762 GW-ER-SENT entries found exactly two contiguous gaps, all
for `gateway_session_conn_id=9`:

| Gap | Orders missing |
|-----|----------------|
| 686,108 - 696,988 | 10,881 |
| 893,205 - 893,561 | 357 |

The gap is visible directly in `er-records.txt`: because line numbers equal ME-ORD
numbers up to the first gap, `grep -n ORD-686107` returns line 686107 and
`grep -n ORD-696989` returns line 686108 — the two consecutive lines, 10,881 ME-ORD
numbers apart.

**Root cause chain:**

1. During the burst at ~20:30:04-20:30:11, connection 9 experienced TCP read backpressure
   (EPOLLIN deregistered for up to 2.5 s). During these windows the TCP send buffer to
   that client also filled up.
2. The OGT calls `session.outbound_seq_num++` *before* `send_raw`. If `send_raw` fails
   (EAGAIN), the sequence number is burned. Only one pending-send slot exists
   (`pending_send_` is `std::optional`); further failed sends are dropped.
3. This created a gap in the OGT's outbound FIX sequence numbers as seen by fix8.
4. Fix8 detected the gap and sent a ResendRequest (`MsgType=2`) at ~20:30:11.
5. The OGT was ignoring all unrecognised message types, including ResendRequest — logged
   as `"ignoring MsgType='2'"`.
6. Fix8 waited ~3 s for a response, got none, and closed the TCP connection. OGT detected
   Broken Pipe at `20:30:14`.
7. All 11,238 ERs that subsequently arrived at the OGT for `gateway_session_conn_id=9`
   were dropped: `"no matching FIX session — dropping"`.

**Fix applied** (`applications/order_gateway/`):

`FixMessage.hpp` — added four tag constants: `Tag::BeginSeqNo` (7), `Tag::EndSeqNo` (16),
`Tag::NewSeqNo` (36), `Tag::GapFillFlag` (123).

`OrderGatewayThread.hpp` — declared `handle_resend_request`.

`OrderGatewayThread.cpp` — implemented `handle_resend_request` and wired it into the FIX
dispatch. On receiving `MsgType=2`:
1. Parses `BeginSeqNo` from the ResendRequest.
2. Temporarily sets `session.outbound_seq_num = BeginSeqNo`.
3. Sends SequenceReset-GapFill (`MsgType=4`, `GapFillFlag=Y`,
   `NewSeqNo=<saved outbound position>`). `send_fix_to_session` stamps
   `MsgSeqNum=BeginSeqNo` and increments `outbound_seq_num` to `BeginSeqNo+1`.
4. Restores `session.outbound_seq_num` to the saved value so subsequent ERs are numbered
   correctly.

**Expected outcome.** Fix8 receives the SequenceReset-GapFill with
`MsgSeqNum=BeginSeqNo` (matching its expected sequence) and advances its expected inbound
sequence to `NewSeqNo`. The connection stays open; all subsequent ERs are delivered,
eliminating the 11,238-ER cascade.

**NOT TESTED YET.** Compiles cleanly. Verification is the next step before check-in.

**Deeper outstanding issue.** The root cause (burning `outbound_seq_num` before
confirming `send_raw` succeeds) is not fixed. If TCP send buffers fill up again a new
small gap will occur, triggering another ResendRequest — now handled gracefully rather
than causing a connection close. Full fix would require not incrementing `outbound_seq_num`
until the send is confirmed, or a proper outbound message queue — a larger refactor.

---

### Outstanding issue — OGT "callback not finished, checking if stuck"

The gateway log contained `"Thread OrderGatewayThread callback not finished, checking if
stuck"` at 20:30:01 and 20:30:04. These fire when the backstop timer catches the OGT
mid-callback during burst processing. The default threshold is
`itc_maximum_inactivity_interval_ = 60 s`; neither instance exceeded it so no shutdown
was triggered and no orders were lost.

Gemini raised a concern: if the OGT becomes idle after a burst but `time_event_started_`
remains greater than `time_event_finished_` (callback appears "in progress"), the reactor
would falsely detect the idle thread as stuck after 60 s and call `shutdown()`.

Current assessment: in the normal completed-callback case, `time_event_finished_` is
always updated at the bottom of `process_message` (ApplicationThread.cpp:488), so a
completed callback satisfies `time_event_started_ <= time_event_finished_`. Gemini's
scenario would only occur if some exit path from `process_message` skips that update.

**Action needed.** Audit every exit path from `process_message` (including exception
paths and any early returns) to confirm `time_event_finished_` is always set before
returning. Not yet investigated — **outstanding risk**.

---

### TCP backpressure (confirmed working)

During the burst all 20 FIX connections had EPOLLIN deregistered (three rounds, 78
engagements total, all with matching releases). Releases came one connection at a time
~125 ms apart, driven by slab-commit rate. Confirmed intentional and working correctly.

---

### Summary of outstanding items from this session

| Item | Status |
|------|--------|
| Shutdown timeout errors after SEGV fix | Root cause not yet identified |
| ResendRequest / SequenceReset-GapFill fix | Compiled, NOT TESTED |
| OGT idle-thread false-stuck risk (Gemini concern) | Needs process_message exit-path audit |
| ME-ORD perf monitoring unreliable (Quill flush ordering) | Known; low-priority tooling issue |
