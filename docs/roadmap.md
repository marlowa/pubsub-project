# Roadmap

## Slices

Each slice leaves the system in a working state. Slices 1–8 and 10 are complete; slice 11
(TAP) is the next planned application.

| # | Description | Status |
|---|-------------|--------|
| 1 | Add seqNo to `EventMessage` and wire format | ✓ Done |
| 2 | In-memory WAL | ✓ Done |
| 3 | mmap'd WAL on disk, single-host, no fsync | ✓ Done |
| 4 | Snapshot (single, no rolling) | ✓ Done |
| 5 | Move FixSession ↔ ClOrdID mapping into sequencer state | ✓ Done |
| 6 | Single-host failover infrastructure (leader-follower state machine) | ✓ Done |
| 7 | Network WAL replication (leader streams records; follower acks; ER gated on ack) | ✓ Done |
| 8 | Arbiter implementation — replaces file-based fencing with real lease+epoch arbiter; PSA+witness topology | ✓ Done |
| 9 | Dual snapshots, snapshot validation, operational polish | Not started |
| 10 | WAL multi-subscriber generalisation + MEP (MatchingEnginePublisher); topic pub/sub over the WAL | ✓ Done |
| 11 | TAP (Trade Activity Publisher) — topic subscriber to MEP; Kafka/Pulsar publisher | Not started |
| 12+ | Gateway pool; market data; seamless ME failover; DR site; multi-instrument scaling | Forward-looking, not yet planned |

The ME primary-secondary pair was listed under slice 12+ as forward-looking; it landed on
2026-07-05 (role config, book replication via `BookUpdate`, arbiter-mediated promotion, WAL
reconciliation with cancel-on-failover). `ha_test.py` scenario 16 covers it. A second gateway
(`applications/binary_order_gateway`, same venue over internal PDUs with no FIX layer) also exists
now, which makes the venue multi-gateway for the first time — but that is one more gateway, not
the pool of slice 12+, and it raises the availability and fairness questions listed below.

See [WAL and High Availability](design/wal_and_ha.md) for the full design behind slices 1–11.

---

## Outstanding Items

Near-term tasks not tied to a specific slice.

### Active / Next

- **CPU core layout: declared allocation and background by default** — **DONE.** Design agreed
  2026-07-27, ranks and reserve settled and the whole design implemented and live-verified
  2026-07-28.  
  Raised 2026-07-26 while planning item 16. The Prometheus endpoint's civetweb thread must not run
  on any pinned core, and the CPU registry cannot answer that reliably: it records what has claimed
  so far, and no process knows when machine-wide claiming has finished, so a component that starts
  early computes a mask that silently permits cores pinned moments later. The same root cause —
  greedy, start-order-dependent claiming — is why both gateways landed on E-cores under full HA and
  on P-cores under `--no-ha`, which would make the FIX-versus-binary comparison measure core type
  rather than protocol.  
  The agreed design is in
  [CPU Core Layout](design/cpu_pinning_anti_affinity.md). In outline: a per-machine manifest in the
  environment TOML declares which components run on each host; a machine-invariant `hot_path_rank`
  per component declares the order in which entitlement to a good core is surrendered, with whole
  rank groups admitted atomically so the two gateways can never be split; `deploy.py` runs on the
  target host, reads the real topology and resolves rank to core ids, so the same declaration works
  on a 32-core workstation, a 20-core work machine and a small VM. At runtime every process starts
  masked to the background pool — applied by a `deploy.py`-generated wrapper script, which also
  covers the JVM components and doubles as a `perf` / `valgrind` interposition point — and the
  Reactor explicitly promotes only the threads that earn hot-path cores. Forgetting a thread becomes
  harmless, and the anti-affinity requirement dissolves rather than being solved.  
  All three points originally flagged as proposals were ratified and built: the binary reports its
  hot-path thread count through `--hot-path-thread-count`, `CpuRegistry` is now a record and
  cross-installation collision detector rather than an allocator, and one machine-wide layout file
  (`run/cpu_layout.toml`) carries both tiers.  
  Verified on the 32-core workstation: every ranked component landed on the cores it was allocated,
  `matching_engine_secondary` was demoted at rank 5 with its reason logged, and both JVMs —
  `fix_test_client` included — start masked to the background tier. `cpu_audit.py` checks every
  running thread's real mask from `/proc` against the layout and exits non-zero on a mismatch, so a
  performance run can be gated on it.  
  **This unblocks item 16.** The civetweb thread now needs no anti-affinity calculation of its own:
  it is background by default like every other unpinned thread, which is what the requirement was
  asking for all along.

- **Prometheus metrics** (item 16).  
  Continuous observability, so latency analysis stops being log archaeology. This is now the
  head of the queue for a second reason: it is the prerequisite for a meaningful FIX-versus-binary
  gateway comparison, and gateway performance work is paused until it lands. A day spent comparing
  the two gateways by profiling and log timestamps produced one solid number and three things that
  could not be concluded; the metrics that would settle each of them are specified in the summary's
  item 16 note dated 2026-07-26.  
  Priority metrics: `order_latency_ns` histogram by phase (`gw_nos_received`, `seq_wal_roundtrip`, `me_roundtrip`, `gw_er_sent`); `seq_pending_er_count` gauge; `seq_wal_replication_lag_records` gauge; `seq_sequence_number` counter; queue depth gauges per `ApplicationThread`.  
  For the gateway comparison specifically: `gw_ingress_ns` and `gw_egress_ns` histograms on both
  gateways with a `gateway` label and **identical bucket boundaries**, `gw_decode_ns`/`gw_encode_ns`
  on the FIX gateway only, `gw_sessions_active` gauge (latency is meaningless without the session
  count that produced it), and `gw_orders_received_total`/`gw_reports_sent_total` counters. Both
  gateways must be instrumented at the same points and to the same depth — the first comparison was
  dominated by one gateway logging more than the other, and metrics can reproduce that mistake
  exactly.  
  Hot-path instrumentation: `std::atomic` increments only — no locks, no allocation. Dedicated metrics-serving thread on a non-hot CPU, excluded from the hot-path CPU registry.

- **Gateway availability, fairness and identity** — **design agreed 2026-07-30, targeted at
  0.3.0.** Written up in [Gateway High Availability](design/gateway_ha.md); nothing is built yet,
  and the gateway remains a single point of failure until it is.
  Two decisions were taken. Sessions are **pinned to a primary and a backup gateway**, not pooled
  any-of-N — that is how venues actually provision order entry, and it turns a distributed-state
  problem into a replication problem between two known endpoints. The claim of "N-way pooled
  redundancy" in `wal_and_ha.md` is withdrawn: it was never implemented, and it stopped matching
  the code when ER routing moved to the gateway-local `gateway_session_conn_id`. And **in-flight
  execution reports must survive the reconnect**, which is the expensive half.
  Verifying the code turned up more than the summary had assumed. `origin_gateway_id` names a
  *protocol*, not an instance, so two FIX gateways would be indistinguishable to the sequencer;
  the sequencer's gateway endpoints are scalars with no way to express a second instance; an ER
  for a disconnected gateway is dropped outright; and there is no outbound message store at all —
  `handle_resend_request` answers every ResendRequest with a blanket `SequenceReset-GapFill`, so
  in-flight reports do not survive a reconnect today even to the *same* gateway.
  Cancel-on-disconnect, by contrast, is already implemented.
  Implementation order is in the design doc; steps 1-3 (instance identity, endpoint collection,
  two instances actually running) are the SPOF work and are worth landing on their own. Open:
  whether cancel-on-disconnect stays, becomes configurable, or goes.
  Still decided but not built: one comp id may hold a session only once venue-wide, which needs
  the sequencer as the shared authority and so is a cross-component protocol change. Pinning makes
  it smaller — two instances to check rather than N — but does not solve it.

- **Transport encryption on the binary order gateway and the internal PDU paths** — undecided, awaiting
  a security specialist. The binary order gateway authenticates with SCRAM but has no TLS listener, so
  it is not equivalent to the FIX gateway on this point. The wider question is whether order-flow
  PDUs need encrypting on the internal hops too (gateway to sequencer, sequencer to ME, WAL
  replication, topic streams — all plain TCP today), which pulls against the latency work above.
  Nothing to be built until the question is settled.

### Deferred

- **Adopt Conan for C++ dependency management** (build tooling).  
  Replace the current `THIRDPARTY_DIR` + `*_VERSION` env-var scheme with a Conan `conanfile.py` pinning the C++ third-party deps (fmt, quill, argparse, tsl-robin-map, googletest — all in ConanCenter) plus a gcc / `cppstd=17` profile. Low-churn fit: the build already uses config-mode `find_package(<pkg> CONFIG)`, so Conan's `CMakeDeps` / `CMakeToolchain` generators slot in with minimal `CMakeLists` change. Conan is pip-installable, so no root needed on RHEL8. **Must be designed first:** the Docker build is deliberately offline/air-gapped (deps prebuilt, liquibase copied from host), whereas Conan defaults to fetching from ConanCenter — so it needs a local Conan remote or a pre-seeded cache baked into the image. Approach when picked up: spike on a branch, validate the offline path in the Rocky 8 container. Deferred for now — the current env-var scheme works; not urgent.

### Completed since this list was last revised

Kept as a record of what the "Active / Next" and "Deferred" lists used to hold. The full
account of each is in `pubsub_itc_fw_summary.md` under the same item number.

- **Pub/sub WAL** (item 7) — done as slice 10. Topic-based fan-out over the WAL, streamed and
  socket-paced, with replay from a cursor. See [Pub/Sub](design/pubsub.md). The MEP is the
  reference publisher, `topic_probe` the reference subscriber, and TAP (slice 11) is the next
  consumer to build on it. This also removes the rendezvous problem that the connection-retry
  workaround exists to paper over.

- **WAL replication jitter — Option B fix** (item 11) — done 2026-07-03.
  `ApplicationThread::prioritise_data_over_timers()` (virtual, default `false`) makes the drain
  loop buffer `Timer` events until all non-timer events in the cycle are exhausted;
  `SequencerThread` overrides it to `true`, so a heartbeat or snapshot timer can no longer add
  latency ahead of a `WalAck` that arrived in the same `epoll_wait` wakeup. FIFO ordering is
  unchanged for every other subclass. The unit test proves the ordering guarantee; the magnitude
  of the tail improvement needs `seq_wal_roundtrip` from item 16 to quantify.

- **cpu_registry_shm_path configurable from TOML** (item 12) — done 2026-07-03. Present in every
  application configuration struct and loader, and in every application and environment TOML.

- **FixCapture: replace mutex/vector with SPSC lock-free queue** (item 13) — done. `FixCapture`
  packs records straight into a pre-allocated ring (`ring_bytes`, default 64 MB) with
  cache-line-aligned `write_offset_`/`read_offset_` atomics and a sentinel record for wrap-around.
  No mutex and no per-record allocation. A contiguous ring beat `LockFreeMessageQueue` plus a pool
  allocator here because it removes per-record node allocation entirely.

- **fix-test-client: idempotent ClOrdID (`fix.uniqueId()`)** (item 14) — done 2026-07-03.
  `FixHelper.uniqueId()` returns `currentTimeMillis() + "-" + counter.getAndIncrement()` over a
  never-reset `AtomicLong`. `example.groovy` and `buys_sells_and_cancels.groovy` both use it, so
  both are safely re-runnable.

- **fix-test-client smoke test** (item 15) — done 2026-07-05. `fix_client_smoke_test.py`
  (stdlib only) drives the REST/Groovy API: NOS burst, cancels, then a heavy idempotent loop,
  and validates sent-versus-acked from the script output because the blotter records inbound ERs
  only. Verified live at 128 orders plus 3 cancels.

- **Burst test with WAL replication active** (item 17) — done 2026-07-05.
  `fix_client_burst_test.py` submits N orders with `ha_enabled = true`, waits for every New ER,
  and scans the sequencer log delta for WAL-path distress. Verified at 50,000 orders,
  ~34,500 orders/s, zero drops, no slab or pool exhaustion — `pending_er_` and the WAL TCP
  channel absorbed the burst. A 1M-order run via `perf_run.py`'s multi-client driver also passed.

- **Doxygen navigation layer — clickable architecture maps** (item 18) — done 2026-07-05.
  `docs/architecture.dot` carries `URL="\ref <page-label>"` on each component node and is embedded
  via `\dotfile` in `docs/architecture_map.dox`, giving a clickable image map that links straight
  to the existing markdown docs — no per-component stub pages needed. `doxygen Doxyfile` builds
  clean under `WARN_AS_ERROR = FAIL_ON_WARNINGS`.

- **Matching Engine HA wiring** (item 19) — done 2026-07-05. Role config and a second instance,
  `BookUpdate` replication, arbiter-mediated promotion keyed by `(component-group, instance_id)`,
  and WAL reconciliation before cancel-on-failover. `ha_test.py` scenario 16 passes, as does a run
  with 15,000 orders WAL-committed during the failover gap and all replayed to the promoted ME.

### Known Issues

- **Shutdown timeout errors** — after the timer SEGV fix, "did not stop within shutdown_timeout" and "failed to join within shutdown_timeout" errors still appear in timer test logs. Root cause not yet identified.

- **OGT `process_message` exit-path audit** — potential false-stuck detection if any exit path from `process_message` skips updating `time_event_finished_`. Not yet audited.

- **ResendRequest / SequenceReset-GapFill** — implemented, compiled, not yet tested under load.

---

## Decision Log

Key architectural decisions and the reasoning behind them.

### Decided

- **Per-component HA, no central broker.** Each component pair (sequencer, ME, etc.) has its own primary-secondary instances, its own WAL replication, and its own arbitrated failover. Components share framework-level HA *primitives* (WAL data structure, replication-channel pattern, arbiter-client API, fencing discipline) but compose them independently.

- **Lease + epoch arbitration.** The arbiter holds leadership state; leaders renew via heartbeat; failover requires arbiter consultation, not unilateral promotion.

- **Arbiter is itself HA — PSA+witness topology.** Two full arbiter instances plus one witness in a failure-independent location. Three votes; majority is two. Three machines is the structural minimum and stays at three.

- **WAL format.** Segmented, mmap'd, single-writer. Entry: `magic | length | seqNo | payload | CRC32`. Replay scans from offset 0; stops at first failure. Tail corruption is equivalent to a clean crash before commit.

- **No `fsync` per WAL append.** Disk durability is out-of-band (segment rotation, periodic flusher). Cross-machine durability comes from WAL replication, not disk.

- **Two-tier commit.** Locally durable (CPU store-release on commit offset) gates send to ME. Replicated (follower has acked) gates ER emission to gateway.

- **Epoch on every PDU.** Every cross-component PDU carries the sender's view of the current leader-epoch. Receivers check: same/expected = accept; lower = sender is stale (discard); higher = receiver may be stale (re-validate with arbiter). This detects split-brain at every cross-component interaction.

- **Cancel-on-failover as ME HA baseline.** ME-secondary maintains a replicated book; on promotion it reconciles against the sequencer WAL before issuing cancel ERs for outstanding orders. Halt-on-failure is preserved as a fallback for unrecoverable failure modes. Seamless lockstep failover is a future aspiration only.

- **Integer-only prices and quantities.** All price/qty values multiplied by a constant (e.g. 1,000,000). Avoids floating-point determinism hazards in replay and cross-instance comparison.

- **Dual rolling snapshots.** Truncation gated by the older trusted snapshot, never the newest one just taken. Validation required before promotion.

- **Halt is the correct response** to WAL mid-segment corruption, both arbiter halves unreachable during failover, and snapshot validation failure on the only available snapshot.

- **PTP (IEEE 1588), not NTP** for cross-machine clock synchronisation. Required for sub-microsecond accuracy in lease checks and ordering.

- **`CLOCK_MONOTONIC` for local interval measurement.** `CLOCK_MONOTONIC_RAW` was considered and rejected: unaffected by NTP/PTP slewing is a disadvantage for interval timers on long-running processes.

- **Clock injection.** Components that read time take a `MonotonicClock&` or `WallClock&` constructor parameter. Concrete motivator: GTD order support in the ME requires replay-deterministic clock reads.

- **Two distinct timer mechanisms.** OS `timerfd` for infrastructure timers (idle timeouts, connect retries, lease heartbeats, FIX logon timeout) — not observable to matching logic. Sequencer-mediated timers for ME-domain events (GTD expiry, auction expiry) — replay-critical, travel through the WAL.

- **Prometheus for statistics.** Hot-path instrumentation via shared-memory atomic counter/gauge/histogram updates. Dedicated gatherer process per machine reads shared memory and exposes scrape endpoints.

- **WAL-follower pattern for a single downstream consumer** (e.g. a Kafka publisher). The consumer opens a connection to the sequencer leader with a position cursor and receives WAL records from cursor onward, reusing the existing replication primitive. For multi-subscriber fan-out with replay, the framework now provides the [topic pub/sub primitive](design/pubsub.md) (slice 10) instead; the two coexist and are chosen per consumer.

### Leaning

- **Per-component HA primitives provided by the framework** as reusable building blocks (`Wal`, replication-channel pattern, arbiter-client API, fencing-discipline helper). Avoids each component implementing HA differently.

- **Quill backtrace logging** — when an `Error` or `Critical` record fires, a ring of recent diagnostic context is also flushed. Quill v11 supports this directly. Planned when convenient.

### Open

- **Arbiter internal HA mechanism.** Intent is hand-rolled lease+epoch with witness voting, not a consensus library. Not yet designed in detail.

- **Sub-second sequencer failover target.** How aggressively to tune lease and heartbeat intervals. Should be configurable via `ReactorConfiguration`, not baked in.

- **Sequencer-to-gateway connection direction.** Currently the sequencer initiates outbound connections to gateways (unusual direction). The reverse (gateway connects to sequencer) is more conventional and easier to scale horizontally. Open until a multi-gateway deployment scenario forces the choice.

- **Market data integration mechanism.** Depends on requirements from the downstream market data consumer. Possibilities: another WAL follower, topic-based pubsub, or bespoke mechanism. Under investigation.

- **DR site topology.** Second site, cross-site replication, separate arbiter pair. Out of scope until main-site design is implemented.

- **Multi-instrument scaling.** Single sequencer vs sharded vs per-instrument. Out of scope until single-instrument is operationally proven.
