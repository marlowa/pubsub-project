# Framework notebook {#framework_notebook}

This is the project's working notebook: designs that were discussed, plans, measurements, and
session notes, written at the time and kept for reference. It is not maintained, and much of it no
longer describes the code. The current description of the framework is
[summary.md](../framework/summary.md), and the current plan is the [roadmap](../roadmap.md).

---

## Development Sessions

The full session-by-session narrative is in **[SESSIONS.md](../history/sessions.md)**. That file
records what was built, fixed, or investigated in each session and is the primary source
for "how did we get here" questions. Sessions are referenced by number throughout this
summary (e.g. "session 16", "session 25") to indicate when work was completed.

---

## What Is Done

- Allocator subsystem — complete, tested, all races fixed. Session 16 fixed the `EmptySlabQueue::reset_to_empty` Vyukov-sentinel race (Vyukov deferred-reclaim with `deferred_reclaim_slab_id_`; test files reorganised one-fixture-per-file; ~65 tests). Session 17 fixed a second, independent race in `ExpandableSlabAllocator`: `std::vector::push_back()` reallocation freed the backing array while workers read raw pointers from it; replaced with a two-level segmented atomic array (`pages_[1024]` directory of heap-allocated `Page` structs, each with 256 `atomic<SlabAllocator*>` slots; pages never move; workers load with `acquire`, reactor stores with `release`).
- Lock-free MPSC queue — complete, tested
- Reactor event loop — complete, tested
- ApplicationThread — complete, tested; `release_pdu_payload()` added
- Socket layer — complete, tested
- PDU framing (`PduFramer` two-mode, `PduParser` zero-copy with `ConnectionID`) — complete, tested. `PduParser::receive()` returns `tuple<bool, std::string>` directly to caller; no disconnect-handler callback. Holds a `QuillLogger&` and emits two-line `Info` trace per header decode (decoded fields + raw 16 header bytes); see Session 14 for details. `dispatch_pdu` passes `current_pdu_id_` through to the EventMessage factory so receivers see the correct PDU id (session 14).
- `OutboundConnection` — complete; passes `id_` to `PduParser`; `on_connected` takes only the socket. Holds a `QuillLogger&` member, forwarded to PduParser at construction (session 14).
- `InboundConnection` — complete; `handle_read()` returns `tuple<bool, std::string>` (session 14)
- `ProtocolHandlerInterface` / `PduProtocolHandler` — complete; accepts `ConnectionID`. `on_data_ready`, `send_prebuilt`, `continue_send` all return `[[nodiscard]] tuple<bool, std::string>`; no disconnect-handler member. `PduProtocolHandler` accepts a `QuillLogger&` constructor parameter and forwards it to the `PduParser` it constructs; no logger member of its own (session 14).
- `MirroredBuffer` — complete, tested
- `InboundConnectionManager` — complete; constructs a populated `ConnectionID{value, "inbound:<port>"}` once at the top of `on_accept` and propagates it to handler/connection/map/event consistently. `on_data_ready`/`on_write_ready`/`process_send_pdu_command`/`process_send_raw_command` inspect handler return values and tear down on failure (session 14)
- `OutboundConnectionManager` — complete; connection retry implemented; use-after-free on service name fixed
- `ThreadLookupInterface` — complete
- Reactor connection management — complete; `retry_failed_connections` called from housekeeping tick
- `ServiceRegistry` / `ServiceEndpoints` — complete
- `ConnectionID` — own class with `service_name()` for both inbound and outbound connections
- `EventType` / `EventMessage` — complete; `create_framework_pdu_message` carries `ConnectionID` and `pdu_id` (session 14 added the `pdu_id` parameter; the `pdu_id_` member existed but was never being set, leaving every PDU event with the default `-1`)
- `ReactorControlCommand` — complete
- `ReactorConfiguration` — complete; `connect_retry_interval_` (2s default, WAL-pending workaround)
- `FileSystemUtils` — complete
- DSL code generator — complete; C++ and Java backends; `enum class` fix; `char` type; 203 tests passing. Java backend: `JavaGenerator(class_name, package_name)`, `generate_java_from_dsl.py` wrapper with `--package` option; 47 Java-specific tests.
- `fix_equity_orders.dsl` — FIX 5.0 SP2 equity order topic registry
- Logging subsystem — complete
- `RawBytesProtocolHandler` — complete; `on_data_ready`/`send_prebuilt`/`continue_send` return `tuple<bool, std::string>`; no disconnect-handler member; no logger member (session 14)
- TLS subsystem — complete, tested (session 20). `TlsContext` (wraps SSL_CTX; `create_server`/`create_client`; TLS 1.2 minimum, TLS 1.3 preferred; AEAD-only ciphers), `TlsState` (per-connection; memory BIOs; pending ciphertext buffer), `TlsRawBytesProtocolHandler` (implements `ProtocolHandlerInterface`; non-blocking handshake; same `RawSocketCommunication` delivery), `TlsListenerConfiguration`, `TlsClientConfiguration`. `ServiceEndpoints` carries `optional<TlsClientConfiguration>`. `ProtocolHandlerInterface` gains `start_outbound_handshake`, `is_handshake_complete`, `is_reads_paused`. `ProtocolType::TlsRawBytes` (value 2). 9 integration tests (5 inbound, 4 outbound). Not yet wired to any application.
- `fix_order_gateway` — FIX session layer complete; PDU encoding to sequencer complete; ER routing back to fix8 complete. Session 17 adds `ha_enabled` flag (default false): when false, secondary sequencer connect is skipped, `forward_pdu_to_sequencers` sends only to primary, and secondary host/port are not required in the toml. When `ha_enabled=true`, dual-publish to primary and secondary is restored. `forward_pdu_to_sequencers` name kept plural — the dual-publish branch returns when leader-follower is fully live.
- `sequencer` — Slices 1–7 complete. PDU forwarding (NOS, OCR, ER), topology, re-encode fixes all from session 15. WAL (`SequencerWal`: mmap'd segments, snapshot, CRC32, replay on restart), seqNo on wire, `routing_comp_id` stamping, and leader-follower state machine (`Role::unknown/leader/follower`, `adopt_role`, `peer_heartbeat_timeout`, epoch, fence file) from sessions covered by the session-17 entry. Slice 7 (session 18): network WAL replication — leader streams `WalRecord` (id=103) PDUs to follower over peer TCP; follower appends to its WAL and replies `WalAck` (id=104); leader buffers ERs in `pending_er_` keyed by seq_no and gates gateway ER emission on WalAck; follower WAL written exclusively from WalRecord (not from direct gateway PDU); `flush_pending_er()` releases all buffered ERs on peer disconnect (degraded mode). `ha_enabled=false` (default): sequencer immediately adopts `Role::leader` in `on_initial_event` and skips arbiter/peer connects. Both TOMLs now have `ha_enabled=true`.
- `matching_engine` — complete for the round-trip stub. `on_framework_pdu_message` decodes inbound `NewOrderSingle` PDUs (session 15) and emits a fully-filled `ExecutionReport` over the existing outbound `sequencer_er_conn_id_`. The ER populates every field that `SequencerThread`'s ER decoder reads. No real order book or matching — every order becomes a single fill at its limit price (or a zero sentinel for market orders). `OrderID` and `ExecID` are generated as `ME-ORD-N` / `ME-EXEC-N`. `OrderCancelRequest` is not yet handled (logs and drops at the `else` branch); cancel handling is a small follow-up.
- `arbiter` — complete. The third voter in each component pair's majority lease, and one of the three voters deciding which arbiter is active. `ha_test.py` scenario 15 checks each step of a follower being granted the lead.
- PostgreSQL schema and migration tooling — complete (session 22). `db/create_db.py` idempotent setup script; Liquibase 5.x changelog; three tables: `pubsub_firm`, `pubsub_comp_id` (SCRAM fields, account status, audit timestamps), `pubsub_comp_id_gateway_permission`. Table prefix configurable (default `pubsub_`).
- Java admin service (`java/admin-service/`) — complete (sessions 22–23). Javalin 6 + Freemarker 2.3 + plain JDBC, no CSS framework. Full CRUD for firms, comp_ids, and gateway permissions. Password set path: derives SCRAM-SHA-256 → writes to DB → pushes plaintext password to auth service via `SetCredentialRequest` (PDU 510) over TLS. Credential revocation: `RemoveCredentialRequest` (PDU 512) sent when a firm or comp_id is disabled, locked, or deleted. Maven build with Checkstyle, SpotBugs (exclude filter for DI false positives), JaCoCo (80% threshold), and OWASP Dependency Check. Logging: SLF4J API + Logback 1.2.13 (not Log4j2 — Logback is the native SLF4J implementation and needs only one dependency; Log4j2 requires an additional `log4j-slf4j-impl` bridge adapter with no benefit in this context; Javalin 6.3.0 depends on SLF4J 1.x so Logback 1.5.x is incompatible — 1.2.13 is the correct version). `logback.xml` suppresses Javalin/Jetty/HikariCP noise to WARN. `FreemarkerRenderer` registered via `config.fileRenderer()` (Javalin 6 requires explicit registration). Fat JAR built with maven-shade-plugin including signature-file exclusion and ServicesResourceTransformer. Service starts cleanly and responds on port 8080. Admin UI authentication: Jenkins-style login system backed by a TOML file (`admin_users.toml`) — no database dependency. BCrypt-hashed passwords (jbcrypt 0.4, cost 12). Two roles: ADMIN (full CRUD) and VIEWER (read-only; POST routes blocked with 403 by `AuthFilter`). First-run setup wizard creates the initial ADMIN account. Force-password-change flag set on admin-created accounts; user is redirected to `/change-password` on next login. Session auth via Jetty `SessionHandler`; `AuthFilter` runs as Javalin `before()` handler. Styling is a single hand-written stylesheet, `static/desktop.css`, bundled in the JAR — no CDN dependency; works in air-gapped corporate environments. Pico.css was removed 2026-07-29. Three branding properties in `application.properties`: `brand.name` (product name shown in titles and nav), `brand.logo-url` (logo image in nav and login page), `brand.css-file` (path to a CSS file inlined into every page for colour overrides). See `java/admin-service/README.md` for deployment and branding instructions. Credential lifecycle gap: re-enabling a firm or comp_id, or unlocking a comp_id, does NOT automatically restore the auth service credential (PDU 510 requires the plaintext password, which is never stored); the operator must reset the password afterwards. The Edit forms display a warning when this applies; the full procedure is documented in the README "Credential Lifecycle" section.
- `db/export_credentials.py` — complete (session 23). Exports SCRAM credentials from `pubsub_comp_id` (enabled comp_ids from enabled firms, not locked) to `credentials.toml` in auth service `[[credential]]` TOML format. Uses `psql --csv --tuples-only` with `PGPASSWORD` env var. Atomic write via temp file + rename.

## What Is Not Yet Done (in dependency order)

1. ~~**Re-verify fix8 wrong-port issue is gone**~~ — DONE (session 2026-06-03). `f8test` connected to port 9879, SCRAM auth succeeded, FIX session established. Closed.
2. ~~**Matching engine — `OrderCancelRequest` handling**~~ — DONE (session 2026-06-03). ME now decodes `OrderCancelRequest`, fabricates a `Canceled` ER, and sends it back via the sequencer to the gateway. Verified end-to-end via fix-test-client (place order, cancel order, Execution Report received with `OrdStatus=Canceled`).
3. ~~**Arbiter — end-to-end failover verification**~~ — DONE (session 2026-06-03). Added `VerifyStep` NamedTuple to `ha_test.py` and scenario 15 (`arbiter_mediated_election`). Scenario 15 explicitly verifies each step of the ArbitrationReport/Decision PDU exchange after killing `sequencer_primary`: (a) `sequencer_secondary` sends `ArbitrationReport` to the arbiter pool (confirmed in log in 5.8 s — well within the 15 s `peer_heartbeat_timeout`); (b) `arbiter_primary` sends `ArbitrationDecision` back (confirmed in 0.0 s); (c) `sequencer_secondary` receives the decision (0.0 s); (d) `sequencer_secondary` transitions to leader (0.0 s). Recovery orders flow. Scenario 15 PASS confirmed.
4. ~~**Leader-follower — Slice 7 (network WAL replication)**~~ — DONE. Slice 7 complete (session 18): leader streams `WalRecord` PDUs to follower; follower acks with `WalAck`; leader gates ER emission on ack. WAL sequence number continuity across failover verified by scenario 14 (`wal_recovery`) added in session 2026-05-30: primary kills → secondary becomes leader → 1000 interim orders (seq 1001–2000) → primary restarts, reads WAL, syncs from peer, rejoins as follower → secondary kills → primary re-elected leader → recovery orders continue from seq 2001 with no reset or gap.
5. ~~**`SequencedMessage` wrapper**~~ — the sequencer already forwards to the ME with `send_pdu(me_conn, pdu_id, seq, nos)` where `seq` is the WAL sequence number encoded into `PduHeader.seq_no`; the ME reads `message.seq_no()` to retrieve it. The envelope is already explicit. **Open question for WAL replay:** when a downstream consumer (Kafka publisher, future broadcast) connects with a position cursor, the seq_no in each replayed `WalRecord` already serves as the cursor position identifier. Verify at implementation time that no additional wrapper is needed for the replay/Aeron-style consumer path.
6. ~~**Trace logs in `PduParser` and elsewhere**~~ — DONE. All five `PduParser.cpp` log lines (header fields, raw 16 header bytes, slab alloc, alloc result, payload hex dump) are at `Debug`. `InboundConnectionManager::on_accept` TRACE is `Debug`. `SequencerThread::on_framework_pdu_message` TRACE is `Debug`. Per-PDU `Info` hot-path logs in `SequencerThread` (WAL append, ER forwarding) and `MatchingEngineThread` (sequenced PDU received) also demoted to `Debug` in the same pass.
7. ~~**Pub/sub WAL**~~ — DONE as slice 10. Topic-based fan-out over the WAL, streamed and socket-paced, with replay from a cursor; the MEP is the reference publisher and `topic_probe` the reference subscriber. See [Pub/Sub](../pubsub/pubsub.md). This is the long-term replacement for direct TCP and eliminates the rendezvous problem and the retry workaround; the retry logic can be removed as consumers migrate onto topics.
8. ~~**Credential export script**~~ — done (session 23). `db/export_credentials.py` exports DB credentials to auth service `credentials.toml`. Live CRUD updates go via PDU 510/512.
9. ~~**`RestoreCredentialRequest` PDU (514/515)**~~ — DONE (session 2026-06-03). PDU 514/515 added to `authentication.dsl`; `handle_restore_credential_request()` implemented in `AuthenticationThread` (decodes pre-derived SCRAM binary fields, validates sizes, installs into `credentials_` map, persists). `AuthServiceClient.restoreCredential()` added in Java (hex→binary conversion; sends PDU 514, validates PDU 515). `CompIdHandler.update()` now calls `restoreCredential` when transitioning from disabled/locked → enabled+unlocked. `FirmHandler.update()` now calls `restoreCredential` for all enabled+unlocked comp_ids when a firm is re-enabled. Warning notices removed from both Edit form templates. README credential lifecycle table updated to include the new restore actions.
10. ~~**FIX message capture**~~ — DONE (session 2026-06-04). `FixCapture` class (`applications/fix_order_gateway/FixCapture.hpp/.cpp`): gateway thread calls `capture(Direction, data, size, timestamp_ns)` which enqueues a record onto a `std::vector<Record>` queue (protected by mutex; short critical section, no file I/O). A background `std::thread` drains the queue via `condition_variable` and writes binary records to disk. Record format (little-endian): `uint32_t payload_size | int64_t timestamp_ns | uint8_t direction(0=in,1=out) | bytes`. Three capture points in `FixOrderGatewayThread`: (1) inbound — after `parser.feed()`, captures the consumed bytes of all complete FIX messages; (2) outbound session messages — in `send_fix_to_session`, after serialise; (3) outbound ERs — after `encode_execution_report`. Config: mandatory `[fix_capture] enabled` + `file` fields in `fix_order_gateway.toml`; `enabled=false` in `dev.toml` by default. `capture_` member is `nullptr` when disabled; all three capture calls are guarded by `if (capture_ != nullptr)` so there is zero overhead when capture is off.
11. ~~**WAL replication jitter — Option B fix**~~ — DONE (2026-07-03). Added `prioritise_data_over_timers()` virtual hook to `ApplicationThread` (default `false`). When `true`, the drain loop buffers any `Timer` events encountered and processes them only after all non-timer events in the same drain cycle are exhausted. `SequencerThread` overrides to return `true`. This prevents a heartbeat or snapshot timer from adding latency before a `WalAck` that arrived in the same `epoll_wait` wakeup. All other `ApplicationThread` subclasses are unaffected (FIFO ordering preserved). 583 unit + 33 integration tests pass.
12. ~~**cpu_registry_shm_path configurable from TOML**~~ — DONE (2026-07-03). Added `cpu_registry_shm_path` to all seven application `*Configuration.hpp`, `*ConfigurationLoader.cpp`, and `*.cpp` wiring files. Added `cpu_registry_shm_path = "${shared_reactor_cpu_registry_shm_path}"` to all eleven application TOML templates and `reactor_cpu_registry_shm_path` to all four environment TOMLs. `ReactorConfiguration::cpu_registry_shm_path` is now populated from the TOML rather than falling back to the hardcoded `/dev/shm/pubsub_cpu_registry` default. `deploy.py` already injected the correct install-dir-relative path; the C++ side now reads it. 583 unit + 33 integration tests pass.
13. ~~**FixCapture: replace mutex with SPSC lock-free queue**~~ — DONE (implemented before session log coverage; discovered 2026-07-04). `FixCapture` uses a pre-allocated SPSC ring buffer (`ring_bytes` allocated once at construction, default 64 MB), not a mutex or `std::vector<Record>`. `capture()` packs records directly into the ring via `memcpy` with no malloc. `write_offset_` / `read_offset_` are cache-line-aligned atomics for lock-free SPSC coordination. A sentinel record handles ring wrap-around so records are never split. The writer thread drains via `fread` from the ring. No per-record heap allocation; no mutex. The chosen approach (contiguous ring buffer) is more efficient than `LockFreeMessageQueue` + pool allocator for this use case because it eliminates per-record node allocation/deallocation entirely.
15. ~~**fix-test-client smoke test (Python script driving the Groovy scripting API).**~~ — DONE (2026-07-05). `fix_client_smoke_test.py` (stdlib only) drives the running fix-test-client's REST API. It clears the blotter, then submits two Groovy scripts — phases 1–3 (rapid NOS + a few cancels + ~3 s settle) and phase 4 (a heavy loop of 100+ orders using `fix.uniqueId()` for idempotent, re-runnable ClOrdIDs) — polling `GET /api/script` to `COMPLETED`/`FAILED`. Each script echoes `SENT_NOS`/`SENT_CANCEL` ClOrdIDs in its output. The blotter (`GET /api/messages`) records inbound ERs only, so the validator matches sent-vs-acked from the script output: (a) every sent NOS has a matching New ER (none dropped), (b) every cancel produced a `Canceled` ER (matched via `origClOrdId`), (c) no ER carries an unexpected OrdStatus (only New/Canceled — the ME stub never matches, so no fills). Reports PASS/FAIL with counts; exit 0/1/2. Verified live end-to-end: 128 orders + 3 cancels → PASS, and re-runnable with different counts. pylint 10/10. Depended on item 14 (`fix.uniqueId()`), which is done.
16. **Prometheus metrics.** Add continuous observability so latency analysis does not require post-hoc log archaeology. Currently, measuring phases such as WAL replication lag vs ME processing time requires grepping log timestamps and computing differences by hand. Prometheus histograms would give live p50/p90/p99 breakdowns per phase, updating every scrape interval.

*C++ instrumentation (`prometheus-cpp` or a minimal bespoke atomic-based implementation):*
Hot-path metrics must use only `std::atomic` increments or thread-local accumulators — no locks, no allocations on the measurement path. A dedicated metrics-serving thread (pinned to a non-hot CPU outside the hot-path CPU range) formats and serves the Prometheus text endpoint on HTTP GET `/metrics`. The scrape thread reads the atomics off the hot path entirely. Cost per observation on a cache-warm atomic: 3–5ns, negligible even under heavy load.

Priority metrics:
- `order_latency_ns` histogram labelled by phase: `gw_nos_received`, `seq_wal_roundtrip`, `me_roundtrip`, `gw_er_sent`. This is the single most valuable metric — right now we can only see the total (GW-NOS-RECV to GW-ER-SENT); breaking it into phases would immediately show whether the bottleneck is ME processing or WAL replication without any log mining.
- `app_thread_wakeup_ns` histogram per thread — the ITC latency measurement we currently derive manually by pairing heartbeat timer log lines, made automatic and continuous.
- `seq_pending_er_count` gauge — number of ERs buffered in `pending_er_` on the sequencer primary waiting for a WalAck. Should normally be 0 or 1; a rising value under load indicates replication is falling behind.
- `seq_wal_replication_lag_records` gauge — difference between the leader's current `next_sequence_number` and the last seq_no the follower has acked. Currently invisible.
- `seq_sequence_number` counter — gives throughput directly in Grafana without log parsing.
- Queue depth gauges per `ApplicationThread` — early warning for backpressure situations.

**Note (added 2026-07-26): metrics needed to compare NOS handling between the FIX and binary
gateways.** A day spent comparing the two by profiling and log timestamps produced one solid
number and three things that could not be concluded, and every one of the failures is a case
this item exists to fix. What follows is what that exercise says the instrumentation must
provide.

*What the comparison actually needs.* The two gateways differ only in a bounded segment: what
happens between the client's bytes arriving and the order PDU leaving for the sequencer. The
FIX gateway parses, validates, extracts repeating groups and builds the PDU; the binary order gateway
forwards the client's bytes as they arrived. Everything downstream -- sequencer, WAL, matching
engine -- is common. So the metric that answers the question is not end-to-end latency but the
gateway-internal segment, timed identically on both:

- `gw_ingress_ns` histogram, from the reactor delivering client bytes to the envelope being
  handed to the sequencer. This is the segment in which the protocols differ, and the only one
  where a difference can legitimately be attributed to FIX.
- `gw_egress_ns` histogram, from an execution report arriving from the sequencer to the bytes
  being written to the client socket. The FIX gateway decodes the PDU and encodes FIX text; the
  binary order gateway relays the payload untouched.
- `gw_decode_ns` and `gw_encode_ns` histograms on the FIX gateway only, as sub-phases of the
  above. On the binary order gateway these do not exist rather than reading zero -- a metric that is
  structurally absent is clearer than one that is always empty.

*Every one of these must carry a `gateway` label* (`fix_order_gateway`, `binary_order_gateway`) and use
**identical bucket boundaries**, so a single Grafana panel can overlay them. Different buckets
would make the two incomparable in exactly the way that is hardest to notice.

*Metrics that make a latency figure interpretable.* Two measurements from 2026-07-26 mean a
gateway latency number is close to meaningless on its own:

- `gw_sessions_active` gauge, per gateway. Median latency was measured at 119us with 4 sessions,
  356us with 20 and 528us with 40, at an identical offered rate -- roughly `sessions^0.7`. Any
  latency figure quoted without the session count that produced it is not reproducible.
- `gw_orders_received_total` and `gw_reports_sent_total` counters, per gateway. Their *rates*
  give throughput directly, which is the number the current harness cannot produce for the FIX
  gateway at all: `perf_run.py` infers FIX completion by polling log lines, and in a fast run
  the poller observes the finish long after it happened. A counter removes the inference. The
  divergence between the two rates is also the live backlog signal -- when orders are offered
  faster than reports come back, latency is queueing and any percentile is measuring queue
  depth, not service time.

*The trap this exercise fell into, which the instrumentation must not repeat.* The first
comparison was dominated not by protocol but by **logging**: the FIX gateway logged a line per
order at Info and the binary order gateway logged none, which put 32% of the FIX gateway's samples in
Quill against 11% for the binary one -- more than three times the cost of FIX parsing itself.
Demoting both to Debug and adding a shared `GW-PROGRESS` total halved it. The same asymmetry is
easy to reproduce with metrics: **if one gateway is instrumented more heavily than the other,
the comparison measures the instrumentation.** Both gateways must observe the same histograms at
the same points, and the per-observation cost must stay in the few-nanosecond range the design
above requires.

*What Prometheus will not settle.* Around 70% of both gateways' samples are in the kernel --
TCP, epoll, syscalls -- and that is a deliberate property of the measurement environment, which
must stay comparable with a development VM that has no kernel bypass (see the note on item 6).
Wall-clock histograms of the segments above do include the syscall time inside them, so they
measure the real cost; but no amount of instrumentation will make the userspace difference
between the two gateways larger than the roughly 11% of samples that FIX parsing occupies. The
value of the metrics is to measure that 11% continuously and reliably, not to find something
larger.

**Note (added 2026-07-03 re item 11):** `seq_wal_roundtrip` latency in the histogram above is also the metric needed to measure the practical benefit of the Option B (`prioritise_data_over_timers`) fix. The unit test (`ApplicationThreadTest.PrioritisesDataOverTimers`) proves the ordering guarantee mechanistically, but the magnitude of improvement in the tail distribution is only visible under load. When Prometheus is wired up, compare `seq_wal_roundtrip` p99 before and after to quantify how often a heartbeat or snapshot timer was previously competing with a WalAck in the same drain cycle.

*Java instrumentation (admin service, fix-test-client):* Micrometer with the Prometheus registry. Gauges for FIX session state, counters for messages sent/received. A few lines of Javalin integration per service.

*Deployment:* Prometheus server and Grafana added to the environment configuration. The metrics HTTP endpoint for each C++ process should be on a configurable port, added to the TOML config templates and `dev.toml`. The scrape thread's CPU pinning must be excluded from the hot-path CPU registry so it does not collide with `FixOrderGatewayThread`, `SequencerThread`, or `MatchingEngineThread`.

**Note (added 2026-07-26): that last sentence is a whole design problem, not a detail.** Keeping the
scrape thread off the pinned cores requires knowing which cores are pinned, and the CPU registry
cannot say reliably: it records what has claimed so far, and no process knows when machine-wide
claiming has finished. A component that starts early computes a mask that silently permits cores
pinned moments later. The same root cause also decides which components get P-cores, so it bears
directly on the gateway comparison above. The design is now agreed -- see the TODO section
"CPU core layout" below and
[cpu_pinning_anti_affinity.md](../framework/cpu_pinning_anti_affinity.md) -- but it is
not built, and it should land before the endpoint does, or the first thing the endpoint measures
will be its own scheduling.

17. ~~**Burst test with WAL replication active.**~~ — DONE (2026-07-05). `fix_client_burst_test.py` (stdlib only) drives the fix-test-client's Groovy API with a tight loop of N NewOrderSingles (default 20,000; `fix.uniqueId()` for idempotent ClOrdIDs), against the current system with `ha_enabled = true` and WAL replication live. It waits for every order's New ER (zero drops), checks for unexpected OrdStatus, and scans the delta of `sequencer_primary.log`/`sequencer_secondary.log` for WAL-path distress (slab/pool exhaustion, errors, drops; backpressure/EPOLLOUT reported as informational since they are handled). Verified live: **50,000 orders submitted at ~34,500 orders/s, all 50,000 acked (zero drops), no slab/pool exhaustion, no distress in the sequencer logs** — the `pending_er_` buffer and WAL TCP channel absorbed the burst. Reports throughput + PASS/FAIL; exit 0/1/2. pylint 10/10. For million-scale peak load, `perf_run.py`'s multi-client fix8 driver is the complement (a 1M-order burst with WAL active also passed this session). Original risk analysis retained below.

The gateway thread is not the bottleneck: it is fully non-blocking (parse NOS → encode PDU → SendPdu command → return to event loop; the ER arrives as a separate later event). Multiple orders are genuinely in-flight simultaneously at different pipeline stages. Under a 1,000-order burst the gateway thread spends nearly all its time parsing and forwarding, not waiting.

The real risks under burst load with WAL active are both in the sequencer:

- **`pending_er_` accumulation.** The sequencer primary buffers each ER in `pending_er_` (a `seq_no → slab-allocated PDU payload` map) until the corresponding WalAck arrives from the follower. Under a burst the ME returns ERs faster than WalAcks arrive, so the map can grow to hundreds of entries simultaneously. If the slab backing those payloads fills up, the sequencer stalls. This is a new failure mode that did not exist before WAL replication. It is exactly what the `seq_pending_er_count` gauge in item 16 (Prometheus) would catch in production; the burst test will expose it first.

- **WAL channel backpressure.** The sequencer reactor streams one `WalRecord` per order over a dedicated TCP connection to the follower. If the follower's `SequencerThread` cannot drain fast enough (measured p90 wakeup latency: 354 µs), the follower's socket receive buffer fills, which fills the sequencer's TCP send buffer, which causes the reactor's `SendPdu` for the next `WalRecord` to block on a partial write. The reactor's `EPOLLOUT`-based partial-send path handles this correctly but adds queueing delay that compounds across the burst.

The burst test is a natural companion to item 15 (fix-test-client smoke test): the smoke test verifies correctness at moderate load; the burst test verifies the WAL replication path does not saturate or exhaust slab memory under peak load. A Groovy script submitted via the fix-test-client scripting API (item 14/15) is the natural driver.

19. ~~**Matching Engine HA wiring.**~~ — DONE (2026-07-05, session HA work). Implemented as four slices: (A) role config + second ME instance; (B) book-replication channel (ME-primary streams `BookUpdate` PDUs to ME-secondary); (C) arbiter-mediated promotion (`ArbitrationReport`/`ArbitrationDecision`, `group=matching_engine`); (D) WAL reconciliation (`MePositionRequest`/`MePositionAck`) + cancel-on-failover, with the leader sequencer promoting its standby connection to re-route sequenced orders to the promoted ME. All three of the original requirements — (a) book-update tailing, (b) arbiter-mediated promotion, (c) WAL reconciliation before cancel ERs (the correctness rule) — are met. Commits: `c7f8b9d` (A+B), `dbbb91b` (C+D), plus `dddb415` (arbiter keyed by `(component-group, instance_id)` so the sequencer/ME/MEP pairs don't alias onto shared `{1,2}` slots) and `b6451d8` (catch-up served by the leader sequencer, not a follower). Verified: `ha_test.py` scenario 16 PASS, a live perf run through a failover, and an orders-in-flight-during-the-gap run (15,000 orders WAL-committed during the ~15 s gap and all replayed to the promoted ME — none dropped). Docs updated: `docs/applications/matching_engine.md`, `docs/design/wal_and_ha.md`. Halt-on-failure remains the fallback for irreconcilable failure modes.

18. ~~**Doxygen navigation layer — clickable architecture maps.**~~ — DONE (2026-07-05).
Implemented with the Graphviz DOT approach chosen below. `docs/architecture.dot` is a
component-topology digraph in which each node carries `URL="\ref <page-label>"`; Doxygen
rewrites the `\ref` into a real link when the graph is embedded via `\dotfile` in
`docs/architecture_map.dox`, producing a clickable client-side image map over the rendered
PNG — click any component box to open that component's documentation. The map links
**directly to the existing markdown docs** via their Doxygen-generated page labels
(e.g. `md_docs_2applications_2matching__engine`), so no per-component stub pages were needed
and the markdown stays pristine for GitHub. Wired in with `DOTFILE_DIRS = docs` in the
`Doxyfile` and a dedicated **System Architecture** section on the mainpage linking both the
map (`\ref architecture_map`) and a maintenance guide (`\ref architecture_map_howto`) that
documents the `URL="\ref …"` mechanism, the label-encoding rule, and an "adding a new
component" recipe. As part of this the docs were made to build cleanly under the existing
`WARN_AS_ERROR = FAIL_ON_WARNINGS`: four pre-existing/introduced warnings were fixed (two
unresolved `#anchor` links in `authentication_service.md`, a `DESIGN.md` link outside the
Doxygen input in `fix_test_client.md`, and an undocumented `logger` constructor parameter in
`TlsRawBytesProtocolHandler.hpp`). Verified: `doxygen Doxyfile` exits 0 with 0 warnings, all
nine component boxes resolve to real doc pages, and both mainpage links render. Sub-maps for
complex components (Reactor, Sequencer) remain an optional future extension. The design
rationale and tool trade-off analysis below are retained for reference.

**Documentation restructure (markdown docs/) — DONE 2026-07-03.** A `docs/` hierarchy was
created and fully populated as a human-navigable alternative to this summary file. Entry
point: `docs/index.md`. Design subsystem docs in `docs/design/` (threading, reactor,
allocators, WAL+HA, serialisation DSL, socket comms, secure comms, CPU pinning, sequencer,
MEP/OAR). Application docs in `docs/applications/` (FIX order gateway, sequencer, matching
engine, admin service, FIX test client). `pubsub_itc_fw_summary.md` is NOT deleted — it
remains the authoritative narrative and session log. The remaining part of item 18 is
specifically the **Graphviz DOT clickable maps in Doxygen**, described below.

### Problem

Doxygen's generated output is comprehensive but navigable only as a tree: classes, files, namespaces. Developers do not naturally think in document trees. When a developer wants to understand `ReferencePriceDataInterface`, they do not want to browse `Architecture → Core → Framework → Reactor → Services`; they want to click on a picture of the system and land in the right place. The documentation tree is fine as a reference index once you know where you are, but it is a poor entry point for orientation.

The root insight (from a design discussion on 2026-06-25): the SVGs are not illustrations — they are part of the navigation layer. A clickable architecture map is a first-class navigation mechanism, not decoration.

### What we are building

A hierarchy of SVG architecture maps embedded directly in the Doxygen HTML output. Each map is a set of labelled rectangles, one per major component. Each rectangle is a hyperlink. Clicking it lands the developer on a curated landing page for that component, from which they can drill into the auto-generated API reference for the relevant classes.

The top-level map covers the whole system. Complex components (Reactor, Sequencer) may have their own sub-maps linking to their internal subsystems.

### Tool choice: Graphviz/DOT

**Why DOT was chosen:**
- Eclipse Public License — genuinely free software with no proprietary hosted component.
- Native Doxygen support: the `\dotfile` command and `@dot`...`@enddot` inline blocks embed DOT diagrams directly into Doxygen HTML. No extra tooling step, no build pipeline change beyond what is already there.
- Clickable links are trivial: adding `URL="doxygen_page_id"` to any node causes `dot -Tsvg` to wrap that node in an SVG `<a href>` element. No JavaScript required; the links work in static HTML.
- Text-based source: the `.dot` files are version-controlled plain text alongside the code they describe. Diffing, reviewing, and updating is the same workflow as editing any other source file.
- Auto-layout: when a component is added or removed, Graphviz recalculates the layout automatically. There is no need to manually reposition boxes.

**Alternatives considered and rejected:**

*draw.io* — Explicitly excluded. Although the desktop application source is Apache-licensed, draw.io is fundamentally a hosted service with a proprietary back end. The project requirement is free software only.

*Mermaid (MIT)* — Free software, Doxygen support since v1.9.3 via `\mermaid` blocks. Syntactically simpler than DOT for some diagram types. Rejected because Mermaid's `click` directive for interactive links is JavaScript-driven: the links only work when the Mermaid JS runtime is present in the browser context. In static Doxygen HTML output (e.g. viewed from a file system or offline) this is not guaranteed. DOT's URL attribute produces native SVG `<a>` elements that work unconditionally.

*PlantUML (GPL)* — Free software, native Doxygen support, SVG with clickable links via `[[URL]]` syntax. Rejected for two reasons: it requires a Java runtime as a separate build dependency, and for simple box-and-arrow architecture maps its syntax is more verbose than DOT with no compensating benefit. DOT is already available on the build machine (Doxygen depends on it).

*Inkscape (GPL)* — Free software, excellent SVG editor, full support for adding hyperlinks to any element via Object Properties. Rejected because it produces a GUI-authored file rather than a text description. The result is harder to maintain in version control, harder to diff and review, and — critically — has no auto-layout: every time a component is added the developer must manually reposition boxes. The maintenance cost over time outweighs the advantage of a visual editor.

*Hand-written SVG XML* — Maximally flexible but completely impractical. As noted in the design discussion: "creating and maintaining a hierarchy of clickable architecture maps by hand in SVG XML would be miserable." Rejected immediately.

### Link target choice: dedicated .dox files

Each rectangle links to a dedicated `.dox` file rather than to an auto-generated class page or a running service URL.

**Why not auto-generated class pages:**
- Each architectural component (Gateway, Reactor, Transport) spans many classes across many files. There is no single class that is the natural landing point for someone trying to understand the component.
- Auto-generated Doxygen page names can shift when Doxygen changes its naming or hashing scheme. URL attributes in `.dot` files would break silently.
- Auto-generated pages show API reference (the *what*). They do not contain architecture rationale, design decisions, invariants, or the *why*.

**Why not service URLs (e.g. `http://localhost:8080`):**
- Only work when the service is running. Documentation should be readable offline and independently of runtime state.

**Why dedicated .dox files:**
- User-defined page IDs are stable: `\page gateway_overview "Order Gateway"` gives the page the ID `gateway_overview`, which never changes unless you rename it deliberately.
- Each `.dox` page is a curated landing page under the author's control: a component overview, design notes, non-obvious invariants, explicit `\ref` links to the key classes within the component.
- The `.dox` files form the translation layer between visual navigation (the SVG maps) and API reference (the auto-generated pages). Each layer serves its purpose without collapsing into another.
- Maintenance of the `.dox` files is forced to be conscious: when the architecture changes, the developer must update both the `.dot` diagram and the relevant `.dox` file. This is a feature — it prevents the navigation layer from silently drifting away from the implementation.

### Intended hierarchy

```
Doxygen mainpage
    └── architecture.dot (top-level SVG, one rectangle per major component)
            │
            ├── docs/gateway.dox          — Order Gateway landing page
            │       overview, design notes, \ref GatewaySession, \ref FixParser, ...
            │
            ├── docs/reactor.dox          — Reactor Framework landing page
            │       overview, \ref Reactor, \ref ApplicationThread, \ref SlabAllocator, ...
            │       (may include a sub-diagram of reactor subsystem internals)
            │
            ├── docs/sequencer.dox        — Sequencer landing page
            │       overview, WAL design, HA state machine, \ref SequencerThread, ...
            │
            ├── docs/matching_engine.dox  — Matching Engine landing page
            │       overview, order book design, \ref MatchingEngineThread, ...
            │
            ├── docs/admin_service.dox    — Admin Service landing page
            │       overview, auth flow, DB schema summary, link to Javadoc
            │
            └── docs/fix_test_client.dox  — FIX Test Client landing page
                    overview, scripting API, link to web UI
```

### Implementation plan

1. Create `docs/architecture.dot` with component nodes, directed edges showing data flow, and `URL` attributes linking to `.dox` page IDs.
2. Write `docs/<component>.dox` stubs with `\page` declarations, one-paragraph overviews, and `\ref` links to key classes. Stubs can be expanded over time.
3. Embed `architecture.dot` in a Doxygen `.dox` page (e.g. `docs/overview.dox` with `\mainpage` or `\page overview "System Overview"`) using `\dotfile docs/architecture.dot`.
4. Build Doxygen and verify: open the generated HTML, click each rectangle, confirm navigation lands on the correct component page and that all `\ref` links resolve.
5. Add sub-diagrams to complex component pages as needed.

14. ~~**fix-test-client scripting: idempotent ClOrdID and example script**~~ — DONE (2026-07-03). Added `uniqueId()` to `FixHelper`: returns `System.currentTimeMillis() + "-" + counter.getAndIncrement()` where `counter` is an `AtomicLong` that never resets, guaranteeing uniqueness across all script runs for the lifetime of the process. Updated `example.groovy` and `buys_sells_and_cancels.groovy` to use `fix.uniqueId()` for all ClOrdIDs. Both scripts are now safely re-runnable without generating duplicate IDs.

## TODO — quieten the hot-path cores before the next latency measurement (raised 2026-07-28)

The CPU core layout decides *which* component owns *which* core, and it does that correctly. It does
not make those cores quiet, and the two are easy to conflate. **An affinity mask reserves a core for
a thread; it does not reserve it from anything else.** Found by auditing all 2075 threads on the
development workstation on 2026-07-28, with the deployment running and its own layout perfectly
consistent:

- **Fifteen interrupt handlers were pinned to cores 1-14.** Concretely: `iwlwifi:default_queue` on
  CPU 14 (`mep_secondary`), `iwlwifi:queue_1` on CPU 7 (`sequencer_secondary`), `iwlwifi:queue_4` on
  CPU 10 (`matching_engine_primary`), `iwlwifi:queue_5` on CPU 6 (`sequencer_primary`). Wireless
  interrupts on the sequencer cores matter more than they sound: under the two-tier commit the
  follower's wake-up latency is added to every order's round trip.
- **About 1500 unrelated userspace threads** were free to run on those cores -- a JVM (214 threads),
  Firefox (150 + 125 + 78), Rider (87), apache2 (55), tor (33). Nothing prevents it, because the
  workstation does not use `isolcpus`.
- **`irqbalance` is running**, so any hand-steering of interrupts is undone on its own schedule.

None of this is a fault in the layout, and none of it was visible in the component logs -- they
report the pinning each component performed, which is not the same as what else can run there.

**Why it matters now:** development is the environment where every latency measurement and every
protocol comparison is taken, and the FIX-versus-binary order gateway comparison is the reason item 16
exists. Interrupt noise on one gateway's core and not the other's would be indistinguishable from a
protocol difference.

**What to do**, cheapest first: steer the movable IRQs to the background tier and stop or restrict
`irqbalance` (no reboot; see "Interrupt affinity" in
[cpu_pinning.md](../framework/cpu_pinning.md)); then consider `isolcpus` with `nohz_full`
and `rcu_nocbs` for the hot-path range, which is the only thing that stops ordinary userspace being
scheduled there and which does need a reboot. `python3 cpu_audit.py --strict` fails while the
machine is still noisy, so it can gate a measurement run.

## DONE — Pico.css removed from both web UIs (raised 2026-07-28, completed 2026-07-29)

**Pico is gone from the tree. Nothing here is outstanding.** Kept as a record: the reasoning is
worth having, and the table-cell trap below is a standing constraint on anything that styles the
FIX blotter.

The fix-test-client did not need a replacement framework, because it already had one. `style.css`
was written before Pico arrived and is a deliberate native-desktop look: monospace throughout, a
`#e0e0e0` fixed chrome bar, bevelled buttons (`box-shadow: 2px 2px 4px #888, -1px -1px 2px #fff`),
and a 12px blotter with a sticky header and sticky first column. The page load order told the whole
story -- `style.css`, then Pico, then `overrides.css` -- a stylesheet we wanted, a framework that
clobbered it, and a third file to claw the look back.

So Pico was deleted rather than swapped. The Pico-specific surface across the six pages turned out
to be two `class="secondary"` buttons and three `<details class="panel">` accordions; there was no
`<article>` and no `role="button"`. `style.css` absorbed what Pico had been quietly providing for
bare elements -- a general rule for text inputs, `select` and `textarea` (previously only
`input[type=text]` and `[type=number]` inside `.form-row`, so the password and datetime fields had
been relying on Pico), `fieldset`/`legend` as a desktop group box, `code`, `summary { cursor }`, a
`button.secondary`, and native checkbox/radio widgets left alone deliberately. `.group-table` grew
its own borders. `overrides.css` and the 71KB `pico.classless.min.css` are gone, along with the
`#blotter td,th { background: inherit }` workaround, which existed only to undo Pico.

**The admin service** was the real dependency, not an accident: 13 Freemarker templates lean on
`<main>`, `<article>` (6), `<hgroup>` (4), `<mark>` (5), `role="button"` (5), `class="grid"`, the
classless `header > nav > ul` navbar, and forms written as `<label>Caption <input></label>` for Pico
to stack. It got `src/main/resources/static/desktop.css`, which styles all of that in the same
desktop idiom as the client, so **no template markup changed** -- only the stylesheet link in the
three templates that carry a `<head>` (`layout.ftl`, `login.ftl`, `setup.ftl`).

Two things worth knowing about that file. Its `:root` block declares the recolourable values as
custom properties, because `brand.css-file` was documented as a place to override Pico's `--pico-*`
variables and that hook would otherwise have silently stopped working; `brand.css` is still inlined
*after* the stylesheet link, so a site override still wins. And row backgrounds are set on `<tr>`
with cells left transparent, deliberately, so the trap described below cannot recur.

The two apps are independent Maven projects with no parent pom. A single shared `desktop.css` would
therefore mean either a duplicated file or a build-time copy step, so each app keeps its own
stylesheet and the shared colours and sizes are held in step by hand -- noted in the header comment
of both files.

The original argument, kept as the rationale:

**Pico was the wrong framework for these UIs.** It is designed for touch
screens, so its controls are sized for fingers: large slab buttons, `0.75rem` vertical form padding
and a `1rem` bottom margin on every element. On a dense desktop tool -- an order-entry form of forty
FIX fields, a blotter of twenty columns -- that turns the page into a column of paving slabs.

The evidence was in the tree. `java/fix-test-client/src/main/resources/web/overrides.css` (since
deleted) existed almost entirely to undo Pico: it reset button and input padding, font size, line
height and margins by overriding Pico's own custom properties on the elements. When the overrides
file exists mainly to cancel the framework, the framework is not earning its place.

Pico also caused a silent regression worth recording, because any future styling must be judged
against it. Pico set an opaque background on every table cell --
`td,th{background-color:var(--pico-background-color)}` -- which painted over the FIX blotter's row
state colours (green for a normal execution report, pink for a cancel, red for an error), set on the
`<tr>`. Only the sticky first column kept its colour, because that one cell already declared
`background: inherit`. The row colours landed on 2026-06-24 (`6f93f17`); site-wide Pico landed a
month later (`b6b4545`, 2026-07-24) and broke them, and it went unnoticed until 2026-07-28. Fixed by
letting every blotter cell inherit, in `overrides.css` -- `style.css` loads *before* Pico and so
cannot win on equal specificity.

The standing constraint that came out of this: **never set a background on `td`/`th`.** Row-level
colouring belongs on the `<tr>`, and both stylesheets now follow that rule. Javalin as the web
framework was never in question and stays.

## TODO — transport encryption on the binary order gateway, and on the PDU paths generally (raised 2026-07-26)

**Undecided. To be discussed with a security specialist before anything is built.**

The binary order gateway authenticates with SCRAM-SHA-256 but has **no TLS listener**, so a
client's password crosses the client-to-gateway hop in clear text. SCRAM limits the damage --
the password is never stored, never forwarded, and never leaves the gateway process -- but
that is a property of the credential handling, not of the transport. The FIX gateway does
offer TLS, so the two gateways are not equivalent on this point, and a venue offering one
encrypted and one unencrypted front door would be hard to defend.

The wider question is the more interesting one. The argument for encrypting the binary
gateway is not only the password: **PDUs carry order flow**, and some venues would treat that
as sensitive in its own right -- who is trading what, in what size, at what price, ahead of
anyone else seeing it. On that reading the case for encryption does not stop at the client
edge. It applies to the internal PDU paths too: gateway to sequencer, sequencer to matching
engine, WAL replication between sequencer instances, and the topic streams external
subscribers read. Those are all currently plain TCP.

Points to settle:
- Whether client-edge TLS on the binary order gateway is required, optional, or configured per
  deployment as the FIX gateway's is.
- Whether the internal PDU hops need it, and if so whether that is a deployment concern
  (trusted network segment, IPsec) or an application one.
- What it costs. The framework's TLS path exists and is used by the FIX listener and the
  admin channel, so the mechanism is not new -- but encryption on the hot path is exactly
  the kind of thing the perf work in this document is measuring, and the two goals pull
  against each other.

## Measured: latency grows with sessions per gateway (2026-07-26)

Five runs against the binary order gateway with `perf_run.py`, all on the Mint dev box, plain
loopback TCP, no kernel bypass. Directly relevant to the fairness question below, because it
shows the problem arising *inside* a single gateway before any question of balancing across
several.

### Throughput ceiling

Offering orders as fast as the socket accepts them (no `--rate`), 20 sessions, 200,000 full
DD-derived orders including both repeating groups:

| Run | Round-trip throughput |
|-----|----------------------|
| First | 73,860 orders/s |
| Second | 57,930 orders/s |

Two runs 16,000/s apart, so **treat the ceiling as "roughly 60-75k/s" rather than a figure**.
Nothing changed between them that should have slowed the order path. Repeats are needed before
quoting a number.

Latency cannot be read from these runs: at 10-15 million orders/s offered into a pipeline
draining at ~60-75k/s, everything queues, and the reported p50 of 2.7-3.4 **seconds** is queue
depth. `binary_load_client` says so in its own output. Use `--rate` for latency.

### Latency versus session count

All three runs offered 10,000 orders/s and achieved ~9,990, all drained in 20ms after the last
send, so throughput is identical and the only variable is the number of client sessions:

| Sessions | min | p50 | p99 | p99.9 | max |
|---------:|----:|----:|----:|------:|----:|
| 4 | 92.8 us | **118.5 us** | 310 us | 1,214 us | 2,368 us |
| 20 | 94.1 us | **356.1 us** | 650 us | 4,651 us | 14,229 us |
| 40 | 96.5 us | **527.9 us** | 1,579 us | 6,799 us | 14,761 us |

Two conclusions, and the first is what makes the second interesting:

- **The pipeline itself is unaffected by session count.** The minimum is flat -- 92.8, 94.1,
  96.5 us across a tenfold change in sessions. One order's round trip through gateway,
  sequencer, matching engine and back costs the same however many sessions exist, which rules
  out per-session state, lookup cost, or anything on the order path.
- **Median latency grows with session count, sub-linearly.** 4 to 20 sessions (5x) costs 3.0x;
  20 to 40 (2x) costs 1.48x. That fits roughly `sessions^0.7`. The tail is worse than the
  median: p99 goes 310 to 650 to 1,579 us, the last step being 2.4x for 2x the sessions, so
  tail latency degrades slightly *super*-linearly even as the median flattens.

The cause is reactor-turn contention: one thread multiplexes every client socket, so an
order's report waits behind other sockets' processing within the same epoll iteration. The
profile agrees -- no hot function, work spread thin, around 75% of samples in the kernel. It is
structural, not a hotspot to optimise away.

**Load and session count compound.** Adding rate to the picture: 10,000/s with 4 sessions gives
p50 119 us; 10,000/s with 20 sessions gives 356 us (3x from sessions); 50,000/s with 20 sessions
-- about 70-86% of the ceiling -- gives 3,962 us, a further 11x from running near saturation.

### Why this belongs next to the fairness TODO

A member's latency depends on how many other members happen to share its gateway: 119 us median
on a quiet gateway, 528 us on one carrying 40 sessions. That is a 4.4x disadvantage arising from
nothing the member did or can control, which is the equitable-access problem in miniature. It
suggests **capacity per gateway is part of the fairness design, not only the number of gateways
or how orders are steered between them**. If sessions per gateway are to be capped to keep
latency uniform, this is the data that says where the cap bites.

### Caveats

One run per data point, on a shared desktop rather than an isolated machine, and the throughput
figures above show run-to-run variance of over 20%. The session-count trend is consistent across
three points and the flat minimum is a strong internal control, but the absolute numbers should
be re-measured before any of them is quoted as a result.

## TODO — gateway availability, fairness and identity (raised 2026-07-26)

Adding the binary order gateway made the venue multi-gateway for the first time, and that has
surfaced three questions the current design does not answer. None is a defect in what is
built; all three need deciding before this could be called an exchange design.

**1. The gateway is a single point of failure.** `docs/design/wal_and_ha.md` records the
model as "N-way pooled redundancy": the gateway elects nothing and holds no WAL, so
redundancy is meant to come from running several and letting clients reconnect. Only one is
ever run. If its process or host dies, every member loses connectivity at once — unable to
enter orders, and unable to manage the risk already on the book. A single reactor also puts
a ceiling on inbound TCP, frame decode and dispatch that will be met under volatility.

**2. Load balancing across gateways must preserve deterministic fairness.** The usual
answers — round-robin, least-connections, random — are not available to an exchange. If one
member's order is served in 50us on gateway 1 while an identical order queues 200us on
gateway 2 through uneven balancing or an asymmetric network path, the venue has failed its
duty of fair and equitable access. Participants detect that delta immediately via drop
copies and market data, and the consequence is regulatory rather than technical. Any pooling
scheme here has to be argued for on determinism, not throughput.

**3. Pooled redundancy is weaker than the document claims, because ER routing changed.**
`wal_and_ha.md` still says routing is on the FIX comp-id pair and "not on ConnectionID".
The code routes on `gateway_session_conn_id`; `FixOrderGatewayThread` marks the comp-id lookup
"legacy -- no longer used for ER routing". That change was right and must be kept: it fixed
ClOrdID collisions between sessions sharing a comp id. But a connection id is gateway-local
and unstable across reconnects, so a client reconnecting to a *different* pool member can no
longer be handed its in-flight reports. Either the document records the trade-off, or
pooled redundancy needs a comp-id-based path back alongside the connection-id one.

**The intended answer: stateless front-end gateways behind the sequencer.** Rather than
load-balancing orders across independent matching gateways, split the tiers: several
redundant gateway processes accept member connections and do only session-layer work
(protocol parse, validation, authentication), and every message they accept is funnelled
into the one strictly-ordered sequencer. The sequencer stamps a monotonic sequence number at
the moment the exchange accepts the message, and that number — not the arrival gateway —
dictates processing order. Fairness then comes from a single ordering authority rather than
from trying to balance load evenly, which is what makes it workable for a venue.

**This system already has that shape**, which is the useful part: the gateways are already
near-stateless and own no book state, the sequencer already assigns the monotonic number, and
`origin_gateway_id` already lets any number of gateways funnel into it while their execution
reports still find their way home. What is missing is only running more than one instance of
a gateway, and the front-end fairness work below.

Note what the sequencer does and does not settle. It makes *processing order* fair and
deterministic however many gateways feed it. It does not equalise the time a message takes to
*reach* it: two members on different gateway instances, or different network paths, can still
see different latencies to the sequencing point. So Pattern A removes the ordering-fairness
problem but leaves the access-latency one, which is what has to be argued about symmetric
paths, pinned cores and per-gateway capacity rather than about sequencing.

**Decided, not yet implemented: one comp id may be logged on only once, venue-wide.**
Today each gateway checks for a duplicate comp id only among its own sessions, so the same
identity can hold a session on the FIX gateway and the binary order gateway simultaneously. This
cannot be fixed inside a gateway: two processes cannot see each other's sessions. It needs a
shared authority, and the sequencer is the natural one — it is arbiter-elected so there is
exactly one leader, and both gateways already connect to it. That means a logon round trip
through the sequencer and a new PDU pair, so it is a cross-component protocol change rather
than a local edit.

## DONE — CPU core layout: declared allocation and background by default (raised 2026-07-26, design agreed 2026-07-27, built 2026-07-28)

**Implemented and live-verified 2026-07-28.** Every ranked component landed on the cores the layout
allocated it (gateways 1-4, sequencers 5-8, matching engine 9-10, MEPs 11-14),
`matching_engine_secondary` was demoted at rank 5 with its reason logged, and both JVMs --
`fix_test_client` included -- start masked to the background tier. `cpu_audit.py` compares every
running thread's real mask from `/proc` against the layout and exits non-zero on a mismatch.
What was built: `cpu_layout.py` and a new `deploy.py` step that writes `run/cpu_layout.toml` and the
`run/background_tier` wrapper; `CpuLayout` and `HotPathThreadCount` in the framework;
`--hot-path-thread-count` on the five ranked binaries; Reactor promotion plus
`verify_hot_path_thread_count()`; `CpuRegistry` reduced to a record and cross-installation collision
detector; and `cpu_audit.py`.
Three things the implementation found that the design had not: the Quill backend never inherits the
process mask (it starts before the config is read, so it must be placed explicitly, and placing it
in the Reactor left *demoted* components' backends unmasked); all thirteen backends must not share
one background core, so `deploy.py` allocates one each; and `cpu_pinning_enabled` had to become
"take part in the machine's CPU layout" and be true for every component, since the five that pinned
nothing were sitting unmasked and free to run on the gateways' cores.
Full treatment, including the rejected approaches and the specific flaw in each, is in
[cpu_pinning_anti_affinity.md](../framework/cpu_pinning_anti_affinity.md). Summary here
so the problem and its answer are visible from this file.

The Prometheus endpoint (item 16) runs its HTTP server on a civetweb background thread that must
not share a core with a hot-path thread. That needs the inverse of the current facility: not "pin
thread T to core C" but "restrict thread T to whatever cores nobody has pinned". Applying such a
mask is easy -- `sched_setaffinity` takes a set, and the civetweb thread id is obtainable.
Determining the set is the problem.

**The registry was a record, not a plan.** `claim_cpus()` reported what had claimed so far. The
anti-affinity mask needs to know what *will* claim, and a process that has not started yet leaves
no trace. So a component that starts early computes a complement covering nearly every core --
including the ones pinned seconds later -- and applies it successfully. The failure is silent.

**The same root cause misallocates P-cores.** Claiming is greedy and follows `devenv.py` start
order, so the gateways, which start last, get what is left. Measured 2026-07-26, same binaries and
configuration: under full HA `FixOrderGatewayThread` landed on CPU 19 and `BinaryOrderGatewayThread` on
CPU 22, both E-cores; under `devenv.py --no-ha` they landed on CPU 10 and CPU 13, both P-cores.
Nothing in the output records which regime produced a measurement. This is the scheduling form of
the trap already noted under item 16 -- and the symmetry that makes the gateway comparison valid at
all is an accident of arithmetic, since the P/E boundary falls wherever the cumulative thread count
reaches 15 and one extra thread anywhere upstream would split the pair.

**HA narrows the options, but less than first thought.** 24 threads want 15 claimable P-cores, so
nine must sit on E-cores regardless. The Quill backend threads (eight, genuinely off the hot path)
are fair game. Among the followers, only the sequencer's must stay on the hot-path tier: it is
synchronously inside the client round trip, because the leader parks each ER in `pending_er_` and
releases it only on the matching WalAck. The matching engine secondary is not -- `send_book_update()`
is fire-and-forget -- so it can be demoted, and the objection that a promoted follower instantly
becomes latency-critical is answered by re-pinning on promotion, since the dead leader's cores are
freed by the registry's dead-pid eviction the moment it goes. That brings hot-path demand to about
ten threads against fifteen P-cores. **"Mandatory for an operational system" and "latency-critical"
are orthogonal, and the sequencer secondary is the counterexample that proves it** -- the venue
trades without it, yet its ack gates every ER.

**Development is the environment that matters.** Production, preprod and test-1 all run one
component per dedicated host, where there is no contention and anti-affinity is trivial. Only
`dev.toml` puts eight claimants on one workstation -- and that is where every latency measurement
and every protocol comparison is taken. A design that is sound in production and indeterminate in
development yields a system that cannot be characterised.

**Not viable, for the record:** a configured cap on how many programs may run. A maximum is an
upper bound, not an expectation; the two coincide only at exactly full capacity, and `--no-ha`
drops three of the eight claimants so a full-HA count is never reached and waiters block forever.
"Programs running" is also the wrong quantity -- five components never claim at all -- and program
count is a lossy proxy for core demand while `register_extra_thread()` exists.

### The agreed design

**Rank and cut point are two quantities, not one.** "Should this instance get a hot-path core" splits
into a **rank** -- machine-invariant, declared, the order in which entitlement is surrendered when a
machine is short -- and a **cut point** -- computed per machine from its population and real
topology. On a dedicated production host the cut point falls below everything, so a secondary gets
exactly what its primary gets with no special case; on the dev workstation the cut point binds and
the rank decides. A *class* ("this component is background") would have been wrong: there is no
reason to withhold a P-core from `matching_engine_secondary` on a host where nothing else wants one.
Demotion is a consequence of contention, not a property of the component.

**Two declarations, both machine-invariant.** A `[machines.*]` section in the environment TOML lists
which components run on each host (`localhost` recognised, used by `dev.toml`) plus an absolute
`minimum_background_cores` floor; and `hot_path_rank` on `[components.*]` alongside `ha_only`.
  The two tiers are two *ways of using a core* — hot-path is dedicated, one thread per core;
  background is ordinary shared multitasking — so background cores are where everything else runs,
  not idle. `minimum_background_cores` is a small floor (2-6 on any machine) whose first job is
  correctness: the background pool must never be empty, since an empty affinity mask is `EINVAL`
  and every process has at least a Quill backend needing somewhere to run. It binds only on
  uniform-core machines; omitted on the hybrid dev workstation, where the 15-P-core ceiling binds
  first, and inert in production, where a host runs one component wanting two cores out of twenty.
Absence of a rank means background, so forgetting is harmless. The machine list names **every**
process on the host, not only those that pin -- under the old scheme the seven dev components that
claimed nothing ran anywhere at all, including on the gateways' cores, which is the hole they were
missing. In the built version `cpu_pinning_enabled` means "take part in the machine's layout" and is
true for all of them; one that pins nothing is unadmitted and stays in the background tier.

**Ties are a constraint, not just an ordering.** A rank group is admitted whole or not at all. Both
gateways are rank 1, so they can never be split across core types -- turning the gateway symmetry
from an accident of arithmetic into a structural guarantee.

**`deploy.py` resolves rank to core ids on the target host**, which it can do because it takes no
host argument and already runs there. Same declaration, different resolution on a 32-core
workstation, a 20-core work machine or a small VM. Admission stops (not skips) at the first group
that does not fit, subject to two constraints: enough P-cores on a hybrid machine, and enough cores
left to meet the background reserve. On the dev box that admits ranks 1-4 (14 of 15 P-cores); on a
20-core uniform machine with reserve 6 it admits ranks 1-3 -- reproducing, unprompted, the tiering
that was chosen by hand when the shortfall was first analysed.

**Runtime: background by default, promotion by exception.** Every process starts masked to the
background pool, every thread it creates inherits that mask (verified -- `pthread_create` copies the
creator's), and the Reactor explicitly promotes only the `ApplicationThread`s and the reactor thread.
Promotion works because an affinity mask is not a ratchet -- `sched_setaffinity` is bounded by the
cgroup cpuset, not the current mask. That is also why the design uses `taskset` and not cpusets.
Library threads -- civetweb, a future Kafka client -- never need to be known about. Quill's backends
are still pinned explicitly via `quill::Backend::get_thread_id()`, but to background cores.

The prompt for this was how the author's workplace does it: a per-thread CPU bitmask config
variable, differing per environment, where threads spawned inside libraries get forgotten and then
interfere with important ones. That is a **default-value bug**, not a diligence failure -- absence
of configuration means "run anywhere", and you cannot enumerate what a library will spawn next
release. A bitmask also declares the answer rather than the intent, so it is machine-specific;
declaring a rank and resolving it at deploy time survives a hardware change.

**The mask is applied by a `deploy.py`-generated wrapper script**, not by a particular launcher --
production launch is still undecided (schedulix is one candidate). Whatever invokes
`bin/run_binary_order_gateway` gets identical behaviour. This covers the JVM components (an affinity mask
is preserved across `execve`, so `taskset` constrains a JVM and every thread it will ever create,
with no Java code), closes the pre-`main()` static-initialiser hole, and doubles as an interposition
point for `perf` / `valgrind` / `gdb`. The wrapper is not the guarantee, though: each C++ component
also self-masks in `main()`, so nothing in the production hot path depends on launcher cooperation.

**`fix_test_client` matters more than it looks.** It pins nothing, is Java, and was written off as
"only a test program". But it drives *both* gateways (`dev.toml:437`), it exists in dev, FT and
**NFT** -- where the numbers are taken -- and it is a load generator, so it saturates cores by
design at exactly the moment of measurement. Pinning restricts the pinned thread and excludes
nobody, and the dev box has no `isolcpus`, so today it can be scheduled straight onto CPUs 19 and 22
while the gateway threads are pinned there. Same shape as the logging asymmetry under item 16.

**`--no-ha` needs no special handling.** The assignment is computed at deploy time over the full
manifest; `--no-ha` is a runtime flag and the skipped components' cores simply sit idle. Identical
assignment across both deployments falls out for free, and nothing in the allocation path needs to
know what an HA component is. (A naming-suffix heuristic for that was considered and rejected: it
misclassifies `witness` and `arbiter_primary`, both `ha_only = true` while being nobody's
secondary.)

**Verification:** a machine-wide affinity audit -- read `Cpus_allowed_list` from
`/proc/*/task/*/status` and report anything overlapping the hot-path pool that is not a declared
hot-path thread. In-process `/proc/self/task` is not enough now that JVMs are in scope.

**Thread counts stay in the code; the TOML never declares them.** Hot-path demand is the reactor
thread plus the registered `ApplicationThread`s — every component registers exactly one, so every
ranked component wants two cores. Threads from `register_extra_thread()` are background by default
and do not count, which removes a real hazard: `FixOrderGatewayThread` registers `FixCaptureWriter`
*conditionally* on `fix_capture_enabled`, so today the same binary has different core demand
depending on a config flag, and any figure in the TOML would be silently wrong for one setting.
`deploy.py` asks the binary (`--hot-path-thread-count`) rather than reading a number, backed by a
fail-loud startup check when fewer cores are assigned than needed.

**Two points remain flagged as proposals, not settled:** `CpuRegistry` becoming a runtime record and
collision detector rather than the allocator; and one machine-wide layout file written into `run/`.
**Deferred:** re-pin-on-promotion, which under a declared layout becomes "adopt the dead leader's
assignment". **Withdrawn:** a `required_hot_path_rank` per machine — a global rank scale does not
compose with a per-machine assertion, and on the deployments in hand it was unreachable.

## Immediate Next Task

**Item 16 — Prometheus metrics.** Every other near-term item on the roadmap's "Active / Next" list
is now done (items 11, 12, 13, 14, 15, 17, 18, 19), and the CPU core layout that stood in front of
Prometheus was built on 2026-07-28, so nothing is left ahead of it. That work has also removed the
requirement that prompted it: the metrics thread needs no anti-affinity calculation of its own,
because every thread not explicitly promoted is already confined to the background tier. Prometheus
is a prerequisite rather than a nicety: gateway performance work is paused
until it lands, because the FIX-versus-binary comparison cannot be settled by profiling and log
timestamps — the 2026-07-26 note under item 16 records exactly which measurements failed and
which metrics would fix each one. The two design constraints that matter most are that both
gateways must be instrumented at the same points with identical histogram buckets (otherwise the
comparison measures the instrumentation), and that no observation may lock or allocate.

Two design questions are open but not blocking: gateway availability/fairness/identity, and
whether the internal PDU hops need transport encryption. Both are recorded as TODO sections
above; the second is waiting on a security specialist and nothing should be built for it yet.

**Item 18 — Doxygen navigation layer (clickable architecture maps) is DONE (2026-07-05).**
The Graphviz DOT clickable maps are implemented: `docs/architecture.dot` (component-topology
digraph with `URL="\ref <page-label>"` nodes) is embedded via `\dotfile` in
`docs/architecture_map.dox`, producing a clickable client-side image map that links directly
to the existing markdown docs. `doxygen Doxyfile` builds clean (0 warnings under
`WARN_AS_ERROR = FAIL_ON_WARNINGS`). See item 18 in "What Is Not Yet Done" for the full record.

**Library unit test coverage is adequate and is NOT a pending task** (confirmed 2026-07-15).
The current LCOV report for `libraries/pubsub_itc_fw/src` shows 80.8% lines (3424/4239) and
72.7% functions (408/561). Some individual files remain low (e.g. `OutboundConnectionManager.cpp`,
`TimerHandler.cpp`, `Reactor.cpp`), but overall coverage is considered sufficient. (An earlier
draft of this section cited 74.4% / 50.3% from 2026-07-09 and named coverage as the next task;
that is superseded.)

## RT scheduling and CPU isolation: machine assessment guide

Option B (item 11) reduces latency outliers caused by timer events competing with WalAck events, but it does not move the median. The median WAL round-trip latency is dominated by three sequential `epoll_wait` wakeups — one per `ApplicationThread` hop — and on a normal Linux desktop or server kernel each wakeup carries 50–200µs of scheduler jitter. To move the median below 100µs consistently, the threads need `SCHED_FIFO` scheduling on CPUs removed from the general scheduler pool.

The feasibility and cost of doing this depends entirely on the target machine. The following assessment procedure determines where a given machine stands and what the improvement path looks like.

### Step 1 — CPU governor

The CPU frequency governor controls whether the CPU boosts to its rated clock. On `powersave` the CPU runs at its minimum frequency most of the time and boosts opportunistically; on `performance` it runs at maximum rated clock at all times. Frequency transitions add jitter to any latency measurement.

```bash
# Show the governor for every CPU
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort -u

# Show the available governors
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
```

If `powersave` is shown, switch to `performance` before drawing any conclusions from latency measurements. This requires no reboot and no code change:

```bash
# Set all CPUs to performance governor (requires root)
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$cpu" > /dev/null
done
```

On cloud instances or VMs the governor may be absent (`no cpufreq`); this is normal and means the hypervisor controls the clock — governor tuning is unavailable.

### Step 2 — CPU topology: P-cores vs E-cores

Intel hybrid CPUs (12th generation / Alder Lake and later) have two core types: Performance cores (P-cores) and Efficiency cores (E-cores). E-cores have lower single-thread performance and higher wakeup latency. If hot-path threads land on E-cores, latency measurements are misleading and CPU pinning must be revised.

```bash
# Show the core type for each logical CPU (Intel hybrid only)
# P-cores report "Intel Core Processor" or similar; E-cores report "Atom"
for d in /sys/devices/system/cpu/cpu*/topology; do
    cpu=$(basename $(dirname $d))
    core_id=$(cat $d/core_id 2>/dev/null)
    echo "$cpu core_id=$core_id $(cat $d/../cpufreq/scaling_driver 2>/dev/null)"
done

# More direct: check /sys/devices/system/cpu/cpuX/acpi_cppc/highest_perf
# P-cores have higher values than E-cores
for cpu in /sys/devices/system/cpu/cpu*/; do
    hp=$(cat ${cpu}acpi_cppc/highest_perf 2>/dev/null)
    [ -n "$hp" ] && echo "$(basename $cpu): highest_perf=$hp"
done | sort -t= -k2 -rn
```

If two distinct `highest_perf` values appear, the higher value is a P-core, the lower is an E-core. On an i9-14900F for example, P-cores are typically CPUs 0–15 and E-cores are 16–31. The CPU registry must restrict hot-path threads to P-cores. This is a configuration change (adjust the `available_cpus` range in the TOML), not a code change.

On a uniform-core machine (AMD, older Intel, server CPUs) this step is a no-op.

### Step 3 — RT priority budget

`SCHED_FIFO` requires the process to have real-time priority capability. Without it, `pthread_setschedparam` will return `EPERM`.

```bash
# Show the current RT priority ceiling for the shell's user
ulimit -r

# Try to actually set SCHED_FIFO at priority 1 (the minimum)
chrt -f 1 echo "SCHED_FIFO works"

# Show what limits.conf grants
grep -r rtprio /etc/security/limits.conf /etc/security/limits.d/ 2>/dev/null
```

If `ulimit -r` returns `0` and `chrt` fails with `Operation not permitted`, the process has no RT priority budget. To grant it without running as root, add a line to `/etc/security/limits.conf` and re-login:

```
# /etc/security/limits.conf
yourusername   -   rtprio   99
```

Alternatively, grant `CAP_SYS_NICE` to the specific binary (survives across logins without a limits.conf change, appropriate for a deployed install):

```bash
sudo setcap cap_sys_nice+ep /path/to/sequencer
sudo setcap cap_sys_nice+ep /path/to/fix_order_gateway
# etc. for each binary that uses ApplicationThread
```

The `sched_rt_runtime_us` throttle (`/proc/sys/kernel/sched_rt_runtime_us`) defaults to 950000 out of a 1000000µs period (95%). Under sustained RT load the kernel enforces this ceiling; threads stall for the remaining 5%. For testing, disable the throttle:

```bash
echo -1 | sudo tee /proc/sys/kernel/sched_rt_runtime_us
```

Disabling RT throttle is safe on a dedicated benchmarking or trading machine but unwise on a shared development machine — a runaway RT thread can make the machine unresponsive. Re-enable it with:

```bash
echo 950000 | sudo tee /proc/sys/kernel/sched_rt_runtime_us
```

### Step 4 — Kernel preemption model

Even with `SCHED_FIFO`, a non-RT kernel can preempt a user-space thread in response to a hardware interrupt. The preemption model determines whether this matters in practice.

```bash
# Show the kernel preemption configuration
grep -E "^CONFIG_PREEMPT" /boot/config-$(uname -r) 2>/dev/null \
    || zcat /proc/config.gz 2>/dev/null | grep -E "^CONFIG_PREEMPT"

# Check for the PREEMPT_RT indicator file
cat /sys/kernel/realtime 2>/dev/null && echo "PREEMPT_RT kernel" || echo "not a PREEMPT_RT kernel"

# Show the running kernel string
uname -r
```

Interpret the results:

| `CONFIG_PREEMPT_*` value | Meaning | Impact on SCHED_FIFO |
|---|---|---|
| `CONFIG_PREEMPT_NONE=y` | Server kernel, no voluntary preemption | Worst; IRQs and long kernel paths can delay RT threads |
| `CONFIG_PREEMPT_VOLUNTARY=y` | Desktop kernel, explicit preemption points | IRQs still preempt; moderate jitter |
| `CONFIG_PREEMPT=y` | Full preemption (non-RT) | IRQs still preempt; better than voluntary |
| `CONFIG_PREEMPT_RT=y` | Full RT preemption | Hardware IRQs handled as threaded IRQs; best determinism |

On Ubuntu, `linux-lowlatency` installs a kernel with `CONFIG_PREEMPT=y`; `linux-rt` (or `linux-realtime` on some releases) installs a `PREEMPT_RT` kernel. Neither requires a hardware change; both require a package install and reboot.

```bash
# Check what lowlatency/RT kernels are available (Ubuntu/Debian)
apt-cache search linux-image | grep -E "lowlatency|realtime|rt-"
```

For the purposes of this system, `linux-lowlatency` is a practical middle ground: eliminates most OS-induced jitter without the operational overhead of a full `PREEMPT_RT` deployment.

### Step 5 — CPU isolation (`isolcpus`)

`isolcpus` is a kernel boot parameter that removes named CPUs from the general scheduler pool. Once isolated, the kernel will not schedule any process or thread on those CPUs unless explicitly assigned. This eliminates the primary source of scheduler interference on the hot-path threads.

```bash
# Check whether isolcpus is already configured
grep isolcpus /proc/cmdline

# Check whether nohz_full is set (stops timer ticks on idle isolated CPUs)
grep nohz_full /proc/cmdline

# Check whether rcu_nocbs is set (moves RCU callbacks off isolated CPUs)
grep rcu_nocbs /proc/cmdline
```

If none of these appear, `isolcpus` is not configured. To add it, edit the kernel boot parameters. On Ubuntu with grub:

```bash
sudo nano /etc/default/grub
# Find the line: GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
# Add to it:    isolcpus=A,B,C nohz_full=A,B,C rcu_nocbs=A,B,C
# where A,B,C are the CPU IDs to reserve for hot-path threads

sudo update-grub
sudo reboot
```

The CPUs listed must match those used by the `ApplicationThread` CPU registry — whichever CPUs the sequencer, gateway, and matching engine are pinned to. `nohz_full` stops the per-CPU timer tick on the isolated CPUs when they have exactly one runnable thread; `rcu_nocbs` moves RCU grace-period processing off those CPUs. Both are recommended alongside `isolcpus` for lowest jitter; without them, isolated CPUs still receive periodic kernel timer interrupts.

After reboot, verify isolation took effect:

```bash
cat /proc/cmdline | grep isolcpus
# The isolated CPUs should no longer appear in the scheduler's runqueue
taskset -c <cpu_id> stress-ng --cpu 1 --timeout 5s &
# Check with htop that only the pinned process runs on that CPU
```

### Summary: what a machine needs for sub-100µs median wakeup

All five steps are independent but cumulative:

| Step | Requires reboot | Expected benefit |
|---|---|---|
| 1. Set governor to `performance` | No | Eliminates clock-scaling jitter; free baseline win |
| 2. Pin hot-path threads to P-cores only (hybrid CPUs) | No | Avoids E-core latency inconsistency |
| 3. Grant RT priority (`rtprio` in limits.conf or `CAP_SYS_NICE`); set `SCHED_FIFO` in `ApplicationThread`; disable RT throttle for benchmarking | No | Prevents userspace preemption; reduces scheduler jitter |
| 4. Install `linux-lowlatency` or `PREEMPT_RT` kernel | Yes | Reduces IRQ preemption; moves from ~50–200µs jitter range to ~5–20µs range |
| 5. Add `isolcpus`+`nohz_full`+`rcu_nocbs` to boot params | Yes | Removes all kernel scheduler interference from hot-path CPUs; the dominant final improvement |

Steps 1–3 can be applied on any machine and verified without disruption. Steps 4–5 require a reboot and are most valuable on dedicated hardware. On a shared development machine, steps 1–3 alone typically bring median wakeup from 150–200µs down to 80–120µs; the full five steps on dedicated hardware with a PREEMPT_RT kernel and isolcpus can reach consistent 5–15µs wakeup latency.

---

A WAL+HA design has been worked through in detail (see "WAL and HA Design" section below) and an eleven-slice implementation plan agreed. **Slices 1–7 are complete** (session-18 entry for Slice 7):

**Slice 7 — Network WAL replication — COMPLETE (session 18).** Leader streams `WalRecord` (pdu_id=103) PDUs to follower over the existing peer TCP connection (7003/7004). Follower appends each record to its own WAL and replies with `WalAck` (pdu_id=104). Leader buffers ERs from the ME in `pending_er_` (keyed by seq_no) and only forwards them to the gateway once the corresponding WalAck arrives. On peer disconnect the buffered ERs are flushed immediately (degraded mode). The follower no longer writes its WAL from the direct gateway PDU path; it writes exclusively from WalRecord, ensuring WAL contents are byte-for-byte identical to the leader's. WAL sequence number continuity across failover verified by scenario 14 (`wal_recovery`) — added and passing.

**Smaller items deferred but still on the list:**
- ~~OrderCancelRequest round trip (item 2)~~ — DONE (session 2026-06-03).
- ~~fix8 wrong-port re-verification~~ — DONE (session 2026-06-03).
- ~~`k`-prefix constants in `ExpandableSlabAllocatorTest`'s fixture~~ — checked clean; no k-prefix violations anywhere in the codebase.
- ~~Quill thread-name population~~ — DONE (session 2026-06-03). `ApplicationThread::run_internal()` calls `pthread_setname_np(pthread_self(), os_name.c_str())` before first log; Quill captures the OS thread name on first log call. Names longer than 15 chars are truncated by the OS limit.
- ~~Quill backend CPU pinning~~ — DONE. Confirmed in startup logs: `CPU pinning: Quill backend thread pinned to CPU N`.
- ~~Sequencer ER inbound idle-timeout killing healthy quiet connections~~ — DONE (session 2026-06-03). Gateway ER inbound listener registered with `idle_timeout_exempt=true`; the 600s timeout no longer applies to this framework-internal connection.
- ~~Hex-dump debug logging on hot path~~ — DONE (session 2026-06-04). All hex dump calls moved to `FwLogLevel::Trace` with level-check guards around string construction; zero evaluation cost at Info/Debug level.

**Design note — the rendezvous problem:**
The connection retry mechanism is a temporary TCP workaround pending WAL-based brokerless pub/sub. In the pub/sub design, publishers write to the WAL regardless of subscriber presence and there is no connection to establish, so the rendezvous problem disappears. The retry logic should be removed when direct TCP is replaced by pub/sub topics.

**Logging infrastructure overhaul** — proper startup sequence implemented across all four applications. This was a significant refactor touching the framework, all four config structs/loaders, all four application classes, all four toml files, and the startup script.

**New framework additions:**

- `FileSystemUtils` — new class in `libraries/pubsub_itc_fw/include/pubsub_itc_fw/utils/FileSystemUtils.hpp` with a single static method `make_directories(path)`. Implemented using POSIX `mkdir(2)`/`stat(2)` rather than `std::filesystem::create_directories` because GCC 8.5 on RHEL 8 requires linking a separate `-lstdc++fs` library for `std::filesystem` and has known bugs in that area. `FileSystemUtils.cpp` must be added to the library `CMakeLists.txt`. Note: follows the same static-methods-on-a-class pattern as `StringUtils`, not free functions.

- `FwLogLevel::from_string(str, level)` — static method added to `FwLogLevel.hpp`. Case-insensitive parse of "trace", "debug", "info", "notice", "warning", "error", "critical", "alert". Returns bool; does not throw.

- `QuillLogger::ensure_log_file_writable(path)` — new static method. Calls `FileSystemUtils::make_directories` on the parent directory, then attempts to open the file for writing. Returns empty string on success, error description on failure. Must be called before constructing `QuillLogger` since there is no console fallback once the logger is live.

- `QuillLogger::set_syslog_level(level)` — new method, separate from `set_log_level`. Updates the syslog sink filter and recomputes the gate as `min(applog, syslog)`. Separate from `set_log_level` because the syslog level is always required in config but is set independently.

**Application startup sequence** (all four applications now follow this):
1. Check `argc == 3`, print usage and exit if wrong: `Usage: <exe> <logfile> <config.toml>`
2. Call `QuillLogger::ensure_log_file_writable(logfile)` — print to stderr and exit on failure
3. Call `QuillLogger::block_signals_before_construction()`
4. Construct `QuillLogger` at `Info`/`Info` — logging is now live
5. Load config via `ConfigurationLoader::load()` — log error and exit on failure
6. Call `logger->set_log_level(config.applog_level)` and `logger->set_syslog_level(config.syslog_level)`
7. Move logger into application class constructor (logger no longer constructed inside the app class)

**Rationale** — this design avoids a common pitfall where logging is unavailable until after config is read (because the log filename comes from the config). Here the log filename comes from the command line, so logging starts immediately and config errors are recorded in the log rather than only printed to stderr.

**Config changes** — all four application configs gain required `[logging]` section:
```toml
[logging]
applog_level = "info"
syslog_level = "info"
```
Both fields are required. There are no optional config fields — making a field optional hides it from operators and makes it unconfigurable in practice.

**FIX parsing implemented in `fix_order_gateway`** — `FixParser`, `FixSerialiser`, `FixMessage`, `FixSession` copied from `fix_order_gateway` with namespace changed to `fix_order_gateway`. `MsgType::OrderCancelRequest` and `Tag::OrigClOrdID` added to `FixMessage.hpp`. Logger threaded through `FixParser` constructor so bad checksums are logged at Debug rather than silently dropped. Full FIX session layer implemented in `FixGatewaySeqThread` (Logon, Heartbeat, TestRequest, Logout, NewOrderSingle, OrderCancelRequest). PDU encoding and ER routing remain TODO.

---

## WAL and HA Design (planned)

> **Topology diagram:** `pubsub_itc_fw_topology.puml` (rendered via PlantUML) is the authoritative single-site, single-instrument deployment diagram for everything described in this section. Its companion explanation is `pubsub_itc_fw_topology.md`.

Designed in conversation, not yet implemented. This section captures the architecture so subsequent sessions can refer back to it. The implementation is staged into vertical slices, listed at the end.

The design follows the convergent pattern that Aeron Cluster, Kafka, Raft, and database checkpointing all arrive at: **separate the irreversible decision (WAL commit) from its replayable effects (ME state, ERs, FIX out).** The WAL is authoritative; everything downstream is reconstructable from it. Followers observe commits, never infer them. Leadership decides who may append; the WAL decides what already happened. Those two concerns must never leak into each other.

### Glossary -- terms that must not be confused

The framework uses two pairs of terms with strict, non-overlapping meanings. Confusing them is a recognised source of bugs in HA systems generally; the discipline matters more than the exact words chosen.

- **primary / secondary**: configured identity, set in the toml at deploy time, never changes for the life of an instance. Primary has the lower `instance_id`. Used only for deterministic tiebreaking on cold start when no instance currently holds a valid lease and the arbiter is being asked to assign initial leadership.
- **leader / follower**: runtime role, determined by the arbiter's lease grant. Either configured primary or configured secondary can be leader at any given moment. Code paths that perform commit / forward / publish actions check the lease state, not the configured identity.
- **active / standby**: NOT USED. These terms ambiguously refer to either configured identity or runtime role and are a permanent source of confusion when discussing HA. Always use one of the two more specific terms above.

In the happy path, primary is leader and secondary is follower. After a primary failure and successful failover, secondary becomes leader (still configured as secondary). After the original primary recovers and rejoins, it becomes follower (still configured as primary). A graceful failback is an operational choice, not automatic.

### Decision log

What is decided, what is leaning, what is open.

**Decided:**

- Per-component HA, no central broker. Each component pair (sequencer pair, ME pair, etc.) has its own primary-secondary instances, its own state replication, its own failover decided by majority lease. Components do not share a runtime broker; they share framework-level HA *primitives* (data structures and protocols) but compose them independently.
- Leadership by majority lease. An instance leads only while two of three voters -- itself, its peer and the active arbiter -- agree; no instance promotes itself. See [Majority leases](../availability/majority_leases.md).
- The arbiter is itself HA, in a Primary+Secondary+Witness (PSA) topology. Two full arbiter instances each hold a copy of the leadership-state map; one third small witness machine holds no state but votes on which of the two arbiters is currently active. The witness machine must be in a failure-independent location relative to the two arbiters: different power supply, different network switch, ideally different network segment. Three votes total means a majority is two; this prevents split-brain in network partitions. Three machines is the structural minimum and stays at three -- adding more witnesses degrades the design rather than improving it (four votes means three needed for majority, so any single failure becomes catastrophic). See "Arbiter PSA topology" section below for the protocol mechanics.
- WAL is segmented, mmap'd, single-writer. Format: `[ magic | length | seqNo | payload | checksum ]`. Replay scans from offset 0 and stops at first failure. Tail corruption equivalent to a clean crash before commit.
- No `fsync` per WAL append. Disk durability is out-of-band (segment rotation, snapshot writes, periodic flusher). Cross-machine durability comes from replication, not from disk.
- Two-tier commit: locally durable (CPU coherence via store-release on commit offset) gates the leader's send to the ME. Replicated (follower has acked over the dedicated replication channel) gates the leader's emission of ERs back to the gateway.
- Every cross-component PDU carries the sender's view of the relevant component pair's leader-epoch. Receivers check the epoch before processing: same/expected = accept, lower = sender is stale (discard with warning), higher = receiver might be stale (re-validate with arbiter, do not silently accept or discard). This is fencing applied to every message rather than only to commits, and is the mechanism that detects split-brain at every cross-component interaction. See "Epoch propagation on every PDU" section below.
- Per-connection isolation in outbound sends. Each TCP connection has its own outbound queue and non-blocking send semantics; a stalled peer cannot block sends to a fast peer. Slow peers that exceed a configured lag threshold are dropped and must reconnect-and-replay. The sequencer-to-follower replication channel is not droppable (it is on the critical path for ER emission); other channels are. See "Per-connection isolation and backpressure" section below.
- Cold-start mmap warm-up via `madvise(MADV_WILLNEED)` on WAL open. Pre-faults the mmap pages so cold-start MTTR is dominated by disk read time done in parallel with snapshot load, not by lazy faulting during replay. Implementation note for slice 3. See "Cold-start MTTR and mmap warm-up" section below.
- Gateway and ME each open TCP connections to **both** sequencer instances at startup, and keep both open. Sends go only to the current leader. Non-leader rejects at the application layer.
- FixSession ↔ ClOrdID mapping moves from gateway to sequencer's WAL. Routing on `(SenderCompID, TargetCompID)` rather than ConnectionID, so that a fix8 client reconnecting (possibly to a different gateway in the gateway pool) is naturally addressable.
- ME failover policy is **cancel-on-failover** as the chosen baseline. ME-secondary maintains a replicated copy of the book; on promotion it reconciles its book against the new sequencer leader's WAL and then issues cancel ERs for all genuinely-outstanding orders. FIX clients receive explicit "Cancelled" messages rather than experiencing a market halt. Halt-on-failure is preserved as a fallback for failure modes that cannot be cleanly reconciled (e.g. WAL corruption, total arbiter unavailability). Seamless lockstep failover (option b) remains a future aspiration. See "ME failover policy" section below for the full rationale and the critical correctness rule about reconciling against the WAL before issuing cancels.
- Integer-only prices and quantities. All price/qty values multiplied by a constant (e.g. 1,000,000) to avoid floating-point determinism hazards. Common practice in matching-engine implementations and a hard rule for this framework.
- Dual rolling snapshots. Truncation gated by the older trusted snapshot, never the newest one just taken. Validation required before promotion.
- Halt as the correct response to several specific failure modes (WAL mid-segment corruption, both arbiter halves unreachable during a failover, snapshot validation failure on the only available snapshot). Halt is conservative and unambiguous; it is preferred over clever recovery in scenarios where correctness cannot be proven.
- Time synchronisation via PTP (IEEE 1588), not NTP. Cross-machine clocks must agree to sub-microsecond accuracy for lease checks, timestamps, and ordering. PTP is operational infrastructure the framework relies on; it is not implemented inside the framework. See "Time synchronisation and clock skew" section below.
- Local interval measurement uses `CLOCK_MONOTONIC` (already in `HighResolutionClock`). `CLOCK_MONOTONIC_RAW` was considered and rejected: it is unaffected by NTP/PTP slewing, but that is a disadvantage rather than an advantage for interval timers, since intervals can drift from real-world expectations on long-running processes if the underlying TSC is inaccurate.
- Clock injection. Components that need to read time will take a `MonotonicClock&` or `WallClock&` constructor parameter rather than calling `HighResolutionClock::now()` directly. Concrete motivator: GTD (Good-Til-Date) order support in the matching engine requires replay-deterministic clock reads, which only injection makes possible. Planned as a dedicated session of work; not blocking any HA slice but to land before the ME grows GTD or any other time-dependent logic. See "Clock injection" section below.
- Two distinct timer mechanisms, kept separate. Local OS `timerfd` for infrastructure timers (idle timeouts, connect retries, lease heartbeats, backstop, FIX logon timeout) -- these are not observable to matching logic, do not need replay determinism, and stay as `timerfd`. Sequencer-mediated timers for ME-domain timer events (GTD expiry, auction expiry, self-trade prevention windows when added) -- these are replay-critical and travel through the WAL alongside orders. See "Timer sourcing" section below.
- Statistics via Prometheus, not via a Kafka publishing chain. Hot-path instrumentation is shared-memory atomic counter/gauge/histogram updates (nanosecond cost). A separate Prometheus gatherer process per machine reads the shared memory and exposes scrape or remote-write endpoints. Cumulative counters in shared memory satisfy the regulatory "no statistic ever gets dropped" requirement: the cumulative count is mathematically complete and durable across gatherer restarts; only fine-grained rate detail within a missed scrape window is lost, which is acceptable. See "Statistics and metrics" section below.
- ME audit log via the existing Quill async logger. The matching engine logs order acceptance, ER emission, and other regulator-relevant events at PTP-disciplined `CLOCK_REALTIME` timestamps. Hot-path cost is sub-100ns per `PUBSUB_LOG` call. The ME audit log is best-effort crash-durable (Quill is async; in-flight records may be lost on a crash), but the WAL is the crash-durable record of order existence -- the ME log is supplementary timing detail. Per-statement synchronous flushing was considered and rejected on latency grounds. See "Statistics and metrics" section below.
- Downstream consumers of order/trade events (Kafka publisher, future broadcast use cases) follow the **WAL-follower pattern**, not topic-based pubsub. Each consumer opens a connection to the sequencer leader, identifies a position cursor, and receives WAL records from cursor onward. The sequencer's WAL replication channel generalises from "one follower (the secondary sequencer)" to "N followers, each with their own cursor". This reuses the framework's existing replication primitive rather than introducing a new pubsub abstraction for a single named consumer. Where multiple downstream consumers with fan-out-and-replay semantics are needed, the framework now provides a **topic-based pub/sub primitive** built on the same WAL (the MEP publishes topics; see [Pub/Sub](../pubsub/pubsub.md)) — the two patterns coexist, chosen per consumer.

**Leaning:**

- Per-component HA primitives provided by the framework: a `WAL` data structure, a replication-channel pattern, an arbiter-client API, a fencing-discipline helper. Each component composes these into its own HA strategy. Avoids "every component implements HA differently with different bugs".
- Quill backtrace logging configured on each component's logger: when an `Error` or `Critical` log record fires, a buffered ring of recent diagnostic context is also flushed to the sink. Useful for incident debugging with any log aggregation tool. Quill v11 supports this directly. To-do for the framework when convenient; not blocking any HA slice. See "Statistics and metrics" section below.

**Open:**

- Mechanism for the arbiter's own internal HA. **DONE.** Implemented in `applications/arbiter/` (active/passive pair) and `applications/witness/` (tiebreaker). The active arbiter manages the leadership-state map and replicates to passive via `ArbiterStateRecord`(400)/`ArbiterStateAck`(401). Arbiter active/passive election uses the same StatusQuery/StatusResponse/Heartbeat protocol as the sequencer. When both arbiters are undecided, each sends `ArbiterVoteRequest`(301) to the witness; the witness grants the vote to the lower `instance_id` via `ArbiterVoteResponse`(302). The witness sends no state; it only breaks ties. This is the PSA+witness lease+epoch design from scratch, avoiding consensus library dependencies.
- Sub-second failover target for the sequencer: how aggressively to tune lease and heartbeat intervals. Tighter intervals trade arbiter availability for failover speed. The framework should make this tunable via `ReactorConfiguration` rather than baking in a number.
- DR site topology. Currently the design is main-site only. DR will require additional design work (a separate site, separate machines, presumably its own arbiter pair, its own sequencer pair, and a cross-site replication strategy). Out of scope until the main-site design is implemented.
- Multi-instrument scaling. A real exchange runs hundreds to thousands of instruments. Single sequencer for everything, sharded sequencer per instrument group, or sequencer per instrument? Each has different failover and replay implications. Not in the immediate slicing plan.
- Sequencer-to-gateway connection direction. The framework currently has the sequencer initiating the outbound connection to the gateway's ER inbound listener (a session-13 finding documented elsewhere in this summary). This is the unusual direction; conventional FIX architectures have the gateway as a client of the core. The trade-off: as designed, the sequencer's configuration must list every gateway address, and adding a gateway requires updating the sequencer configuration. The reverse direction (gateway connects outbound to the sequencer for both order send and ER receive) makes the core "anonymous" and easier to scale horizontally, but requires the sequencer to route ERs by lookup against currently-connected gateway sessions rather than by initiating connections. For the framework's current single-instrument scale this is an acceptable operational cost; for production multi-gateway deployments it likely needs reversing. Open until a deployment scenario forces the choice.
- Market data integration mechanism. The reference system has a downstream market data consumer that consumes data published by the order placement system. The exact mechanism and the exact data are not yet settled for this project; the requirements for that consumer are still pending. Until that information is available, the framework-side mechanism for delivering equivalent data cannot be decided. Possibilities range from "another WAL follower" (analogous to the Kafka publisher) to "a topic-based pubsub primitive" (if multi-subscriber fanout is genuinely needed) to "a bespoke market-data-specific mechanism". Tracked in the "Open Questions and Items to Investigate" section.

The detailed design — architecture, authority and roles, WAL format and segmentation,
commit semantics, epoch propagation, per-connection isolation, replication channel,
gateway reconnection, failover targets, ME failover policy, snapshots, WAL truncation,
cold-start MTTR, failure-handling boundaries, arbiter PSA topology, and the
consensus-libraries discussion — is in **[wal_and_ha.md](../availability/wal_and_ha.md)**.

---

## Open Questions and Items to Investigate

This section tracks specific unresolved questions whose answers will inform future design decisions. It is distinct from the "Open" entries in the WAL+HA Design's Decision Log: those are architectural decisions deferred until a slice forces them. The items here are research items -- things the project author needs to find out about, often by talking to people or reading documentation, before the answer can be committed to code.

Each item names what is unknown, what would change once the answer is known, and roughly when the answer needs to be available.

### Market data integration mechanism

**What is unknown:** Exactly what the downstream market data consumer consumes from the order placement system. Specifically: what data fields, at what frequency, with what delivery semantics (per-event, batched, snapshot-plus-deltas), with what subscriber count and how subscribers identify themselves, with what gap/reconnection handling, with what regulatory constraints on the delivery path.

**What changes once known:** The framework-side mechanism for delivering equivalent data. Three candidate shapes:
- WAL follower: the market data consumer becomes another consumer of the sequencer's WAL, analogous to the Kafka publisher.
- Topic-based pubsub primitive: justified if the market data side has multiple downstream subscribers with fanout-and-replay semantics that don't fit cleanly as WAL followers.
- Bespoke mechanism: if the market data consumer has specific requirements that don't fit either of the above.

**Plan:** Gather the requirements for the downstream market data consumer, from whoever has the deepest understanding of what that consumer does. Communication may proceed through written follow-ups to allow careful confirmation of technical points.

**When needed:** Before slice 12+ designs market data delivery. Slice 11 (Kafka publisher) is unaffected. Earlier slices are unaffected.

### Long-term retention and archival of audit-relevant artefacts

**What is unknown:** Specific regulatory retention requirements for the WAL, the ME audit log, and the Prometheus shared-memory counter files. Periods are typically multi-year (5-7 years for financial trading records is common) but vary by jurisdiction and venue type. The framework's regulatory environment for any deployment scenario isn't yet defined.

**What changes once known:** Operational tooling for log rotation, archival, offsite copies, tamper-detection, and recovery from archives. None of this is core framework code, but the framework's design must support it (e.g. the WAL's segment file format must be archive-friendly; segments must be self-describing enough to restore from archive without the live system being available).

**Plan:** Investigated when a deployment scenario emerges. For the personal-project phase, this is documented but not actively researched.

**When needed:** Before any production deployment. Not for any current slice.

### Operational monitoring of PTP, leases, and arbiter health

**What is unknown:** Specific Nagios (or equivalent) check definitions for the framework's HA and time-sync state. Sketched in the "Time synchronisation and clock skew" and "Arbiter PSA topology" sections but not yet specified at the level of "here is the check_ptp config that this framework requires".

**What changes once known:** A library of Nagios checks shipped alongside the framework, or pointers to standard-issue checks with framework-specific configuration recipes.

**Plan:** Drafted alongside slice 8 (arbiter implementation) when the lease/epoch state actually exists to monitor.

**When needed:** Before any deployment that goes beyond the personal-project test setup.

## HA Architecture (legacy stub -- predates the WAL+HA design above)

The legacy stub described two sequencer instances with the gateway dual-publishing every order PDU to both, so a follower stayed in sync and failover would be gap-free. That stub never fully landed: session 15 removed the secondary sequencer and the dual-publish mechanism because their semantics under the "behaves as unconditional leader" stub were broken (both sequencers would forward to the ME, producing duplicate fills). The full WAL+HA design above replaces this stub. When the design lands, the secondary returns as a passive follower (not a parallel publisher), order PDUs go only to the leader, and the WAL replication channel keeps the follower in sync.

For the framework's *generic* leader-follower DSL protocol (separate from the sequencer-specific design above),
the five-node topology described in subsystem 12 still applies.
The sequencer-specific design uses a simpler topology (two sequencers + one arbiter, single site)
because matching-engine workloads have different durability constraints than the framework's generic streaming use case.

---

## Application Architecture — Sequencer-Based Order Flow

Inspired by the Aeron sequencer pattern. The sequencer is the **sole writer** to the matching engine's input stream, imposing total order on all messages.

**Current state (session 15 end -- single sequencer, no HA):**

```
FIX client
    | raw FIX bytes (RawBytesProtocolHandler)
    v
fix_order_gateway          (single instance)
    | NewOrderSingle / OrderCancelRequest PDUs -- single sequencer (post session 15)
    v
sequencer (single instance, "primary" naming preserved)
    | order PDU forwarded to ME on port 7020 (via me_outbound_order_conn_id_)
    v
matching_engine                 (single instance)
    | ExecutionReport PDU -- sent back to sequencer ER listener (port 7021)
    v
sequencer (receives ER, forwards to gateway on port 7010)
    v
fix_order_gateway --> FIX ER --> FIX client (via cl_ord_id_to_session_)
```

**Future state (after WAL+HA slices land):** the second sequencer returns as a passive follower, the gateway connects to both but sends only to the leader, and the WAL replication channel runs alongside the data channels. See "WAL and HA Design" above for the full topology diagram.

**Startup order** does not matter. The sequencer dials the gateway's ER inbound listener on port 7010 and retries every two seconds until it answers, without limit, so starting the sequencer first costs at most one retry interval before reports can flow and loses nothing --- there are no orders yet to report on. `perf_run.py` starts the sequencers before the gateways. This was written as "counterintuitive but necessary" when the framework-level retry it describes in the same sentence had already made it neither.

**Port allocation (local testing, session-15 state):**

| Port | Usage |
|---|---|
| 9879 | FIX client → gateway (RawBytes inbound) |
| 7001 | gateway → sequencer (order PDUs) |
| 7002 | (reserved) gateway → sequencer follower (order PDUs); not in use post session 15 |
| 7003 | (reserved) sequencer peer-to-peer / WAL replication; final port choice TBD with leader-follower |
| 7004 | (reserved) follower-side equivalent of 7003 if leader and follower listen on different ports |
| 7010 | sequencer → gateway (ER forwarding inbound) |
| 7020 | sequencer → ME (sequenced order PDUs inbound) |
| 7070 | gateway → authentication_service_primary (PDU, ProtocolType::FrameworkPdu) |
| 7071 | gateway → authentication_service_secondary (PDU, ProtocolType::FrameworkPdu) |
| 7021 | ME → sequencer ER listener |
| 7022 | (reserved) ME → sequencer-follower ER listener; not in use post session 15 |
| 7100 | sequencer → arbiter |

The reserved ports are kept in the table so they are not accidentally repurposed before the WAL+HA slices land. When slice 6 (single-host failover) adds the second sequencer, 7002, 7022, and one of 7003/7004 will become live; when slice 7 (network replication) runs, the WAL replication channel will bind a chosen port from the 7003/7004 pair.

---

## Gateway Performance Analysis

Profiling flags: `perf record --call-graph dwarf -F 999`.
Kernel tuning: `/proc/sys/kernel/kptr_restrict = 0`, `/proc/sys/kernel/perf_event_paranoid = -1`.
Binary: `fix_order_gateway` (RelWithDebInfo, full DWARF).
Workload: fix8 sending 100,000 NewOrderSingles + OrderCancelRequests over loopback (127.0.0.1).

> **Why dwarf instead of fp?**
> With `--call-graph fp` the call chain was lost whenever a sample landed inside a syscall or a kernel function that did not preserve the frame pointer register. This caused 53 % of gateway samples to appear as `[unknown] [k] 0xffffffff…` (genuine kernel addresses hidden by the default `kptr_restrict=1`). Switching to `--call-graph dwarf` records the full register state at sample time and unwinds both userspace and kernel stacks offline using DWARF unwind tables. Setting `kptr_restrict=0` then resolved the kernel symbol names. Data file size grew from ~550 KB to ~12 MB reflecting the richer per-sample data.

### Category breakdown — gateway reactor thread (`sample_fix_gate`)

| Category | % of samples | Notes |
|---|---|---|
| Kernel TCP / net stack (`kernel.kallsyms`) | 39.24 % | Normal for TCP I/O — send/recv, SKB management, scheduler |
| **Netfilter** (`nf_tables` / `nf_conntrack` / `nf_nat`) | **13.04 %** | **Surprise: loopback traffic goes through the full nftables chain** |
| Framework (`libpubsub_itc_fw`) | 8.80 % | Dominated by `ReactorControlCommand` slab operations |
| Application binary (`fix_order_gateway`) | 8.14 % | FIX parsing, serialisation, hashtable, PDU send |
| libc | 7.82 % | Heap allocation (3.34 %), timestamp (0.86 %), memchr/memmove |
| libstdc++ | 1.84 % | |
| vdso | 0.68 % | `gettimeofday` fast-path |

### Netfilter — the most important finding

13 % of all gateway CPU is consumed by nftables/conntrack/NAT processing **loopback packets** (source and destination 127.0.0.1). This is not obvious: nftables hooks fire on every packet regardless of interface, including `lo`. The fix8 test client connects over the loopback interface, so every NOS and ER traverses the full netfilter chain.

Top netfilter symbols:

| Symbol | % |
|---|---|
| `nft_do_chain` | 4.56 % |
| `nft_counter_eval` | 2.82 % |
| `nft_immediate_eval` | 1.39 % |
| `expr_call_ops_eval` | 0.94 % |
| `nf_nat_*` (combined) | 0.60 % |
| `nft_meta_get_eval` | 0.38 % |
| `__nf_conntrack_find_get` | 0.33 % |
| `nf_conntrack_tcp_packet` | 0.39 % |

**Remediation**: flush nftables rules (`nft flush ruleset`) or disable conntrack for loopback before benchmark runs. This recovers the full 13 % at zero code cost.

### Application binary symbols

| Symbol | % | Interpretation |
|---|---|---|
| `parse_fields` | 1.04 % | FIX tag/value scanning (string_view, no copies) |
| `from_chars<int>` | 1.03 % | Integer tag parsing inside `parse_fields` |
| `FixSerialiser::append_field` | 0.77 % | Outbound ER field serialisation |
| `validate_checksum` | 0.75 % | Checksum verification on inbound messages |
| `on_framework_pdu_message` | 0.69 % | ER dispatch from sequencer |
| `handle_new_order_single` | 0.67 % | NOS handler including ER routing setup |
| `unordered_map::operator[]` | 0.55 % | ClOrdID → session routing hashtable |
| `try_extract_message` | 0.48 % | Message boundary detection in parser |
| `send_pdu<NewOrderSingle>` | 0.33 % | PDU encoding to sequencer |
| `_Hashtable::find` | 0.28 % | Hashtable probe (ER routing) |

The inbound path (parse_fields + from_chars + validate_checksum + try_extract_message = **3.30 %**) and the outbound ER path (append_field + unordered_map + _Hashtable = **1.38 %**) are the two addressable clusters within application code.

### Framework symbols — ReactorControlCommand queue

| Symbol | % | Notes |
|---|---|---|
| `pop_slot_from_free_list` | 3.34 % | Slab allocator freelist pop per NOS |
| `run_internal` | 0.95 % | Reactor main loop |
| `deallocate` | 0.73 % | Slab return after command processed |
| `dequeue` | 0.66 % | Lock-free queue dequeue |
| `allocate` | 0.32 % | Slab allocation for outbound PDU |
| `enqueue` | 0.29 % | Lock-free queue enqueue |
| **Total** | **~5.34 %** | Structural cost of app-thread → reactor crossing |

Every NOS crossing the app-thread → reactor boundary allocates and frees a `ReactorControlCommand` slot. This is structural: eliminating it would require batching PDUs or merging the app thread with the reactor thread.

### libc symbols

| Category | Symbols | % |
|---|---|---|
| Heap allocation | `cfree` 1.05 % + `_int_malloc` 1.04 % + `_int_free` 0.92 % + `malloc` 0.33 % | **3.34 %** |
| Timestamp formatting | `__strftime_internal` 0.50 % + `__tz_convert` 0.23 % + `__offtime` 0.13 % | **0.86 %** |
| Memory operations | `__memchr_avx2` 0.78 % + `__memmove_avx_unaligned_erms` 0.55 % | **1.33 %** |

The heap cost (3.34 %) is driven by the outbound `FixMessage` — `unordered_map<int, string>` inside `FixSerialiser` allocates on every ER sent. Replacing it with a flat fixed-size structure would eliminate this.
The timestamp cost (0.86 %) comes from `FixSerialiser::current_utc_timestamp()` being called once per ER; caching it at second resolution would reduce this to near zero.

### Quill backend thread (`Quill_Backend`)

The logger backend thread is a separate profiling process. Top symbols:

| Symbol | % |
|---|---|
| `fmtquill::write` | 4.68 % |
| `fmtquill::write` (lambda) | 1.86 % |
| `vformat_to` | 1.29 % |
| `copy_noinline` | 0.97 % |
| `_populate_transit_event` | 0.97 % |
| `sanitize_non_printable_chars` | 0.71 % |

GW-NOS-RECV and GW-ER-SENT are logged at `Info` level, generating approximately 1 M Quill queue writes per 100 K order run. Dropping these to `Debug` level would eliminate almost all Quill backend activity during benchmarks.

### Kernel TCP symbols (selected)

| Symbol | % | Notes |
|---|---|---|
| `native_queued_spin_lock_slowpath` | 1.99 % | Lock contention in network stack |
| `__memcpy` | 1.18 % | SKB data copy |
| `__tcp_transmit_skb` | 1.17 % | TCP transmit path |
| `_copy_to_iter` | 1.00 % | Scatter-gather copy to userspace |
| `entry_SYSRETQ_unsafe_stack` | 0.99 % | syscall return overhead |
| `net_rx_action` | 0.87 % | Receive softirq processing |
| `tcp_rcv_established` | 0.84 % | TCP fast-path receive |
| `tcp_sendmsg_locked` | 0.69 % | TCP send path |

These are normal for a TCP-over-loopback workload and cannot be reduced without switching to a shared-memory transport (e.g. Unix domain sockets or a custom ring buffer between processes).

### Priority list for further optimisation

1. **Flush nftables rules before benchmarking** — recovers 13 % at zero code cost.
2. **Reduce GW-NOS-RECV / GW-ER-SENT to Debug level** — eliminates ~1 M Quill writes and reduces Quill backend load substantially.
3. **Replace `FixMessage` (outbound ER path) with a flat fixed-size structure** — eliminates 3.34 % heap allocation from libc.
4. **Cache `FixSerialiser::current_utc_timestamp()` at second resolution** — eliminates 0.86 % strftime cost.
5. **Batch `ReactorControlCommand` allocations** — reduces 5.34 % framework overhead; requires API change.
6. **Switch to Unix domain sockets for intra-host connections** — bypasses kernel TCP entirely (39 % of samples); largest possible gain but highest effort.

   **Do not pursue this.** (Decided 2026-07-26.) Performance studies here must stay
   comparable with the work development environment, which is a VM with no kernel
   bypass -- no onload, full networking. Removing kernel TCP would make the numbers
   measure a system nobody runs. Both gateways currently spend around 70% of their
   samples in the kernel (binary 68.3%, FIX 71.8%), and that is the floor, not a
   defect: the remaining ~30% of userspace is the whole space in which gateway design
   choices can move the number. Optimise within it, and read any gateway comparison
   with that ceiling in mind.

---

## Session Log (2026-06-02 onwards)

Named session entries are in **[SESSIONS.md](../history/sessions.md)**.
