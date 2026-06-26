# Roadmap

## Slices

Each slice leaves the system in a working state. Slices 1–7 are complete.

| # | Description | Status |
|---|-------------|--------|
| 1 | Add seqNo to `EventMessage` and wire format | ✓ Done |
| 2 | In-memory WAL | ✓ Done |
| 3 | mmap'd WAL on disk, single-host, no fsync | ✓ Done |
| 4 | Snapshot (single, no rolling) | ✓ Done |
| 5 | Move FixSession ↔ ClOrdID mapping into sequencer state | ✓ Done |
| 6 | Single-host failover infrastructure (leader-follower state machine) | ✓ Done |
| 7 | Network WAL replication (leader streams records; follower acks; ER gated on ack) | ✓ Done |
| 8 | Arbiter implementation — replaces file-based fencing with real lease+epoch arbiter; PSA+witness topology | Not started |
| 9 | Dual snapshots, snapshot validation, operational polish | Not started |
| 10 | WAL multi-subscriber generalisation + MEP (MatchingEnginePublisher) | Not started |
| 11 | TAP (Trade Activity Publisher) — topic subscriber to MEP; Kafka/Pulsar publisher | Not started |
| 12+ | ME primary-secondary pair; gateway pool; market data; seamless ME failover; DR site; multi-instrument scaling | Forward-looking, not yet planned |

See [WAL and High Availability](design/wal_and_ha.md) for the full design behind slices 1–11.

---

## Outstanding Items

Near-term tasks not tied to a specific slice.

### Active / Next

- **WAL replication jitter — Option B fix** (item 11).  
  Root cause: three `epoll_wait` hops in the sequencer WAL round-trip; timer events can queue ahead of WalAck events.  
  Fix: change `SequencerThread`'s event drain loop to process `FrameworkPdu` and connection events to exhaustion before processing any timer event.

- **Burst test with WAL replication active** (item 17).  
  Re-run a high-volume burst (≥ 1,000 orders) with `ha_enabled = true` and WAL replication live. Validates `pending_er_` accumulation and WAL channel backpressure behaviour under peak load. Natural companion to the smoke test (item 15).

- **fix-test-client smoke test** (item 15).  
  Python script driving the fix-test-client REST/Groovy API end-to-end: NOS burst, cancel round-trip, ER validation. Depends on `fix.uniqueId()` (item 14).

- **fix-test-client: idempotent ClOrdID (`fix.uniqueId()`)** (item 14).  
  Add `fix.uniqueId()` to `FixHelper` so scripts can be re-run without duplicate ClOrdID errors. Add a canonical example script to the scripts directory.

### Deferred

- **FixCapture: replace mutex/vector with SPSC lock-free queue** (item 13).  
  `capture()` currently acquires a mutex and heap-allocates on the gateway hot path when capture is enabled. Replace with a single-producer single-consumer lock-free queue backed by pre-allocated slots (framework slab/pool infrastructure is the natural backing store). Zero overhead when disabled is already correct.

- **cpu_registry_shm_path configurable from TOML** (item 12).  
  The shm file path is still hardcoded in `ReactorConfiguration` (`/dev/shm/pubsub_cpu_registry`). Making it configurable requires touching six application config structs, six loaders, and nine TOML templates. Deferred.

- **Prometheus metrics** (item 16).  
  Priority metrics: `order_latency_ns` histogram by phase (`gw_nos_received`, `seq_wal_roundtrip`, `me_roundtrip`, `gw_er_sent`); `seq_pending_er_count` gauge; `seq_wal_replication_lag_records` gauge; `seq_sequence_number` counter; queue depth gauges per `ApplicationThread`.  
  Hot-path instrumentation: `std::atomic` increments only — no locks, no allocation. Dedicated metrics-serving thread on a non-hot CPU.

- **Pub/sub WAL** (item 7).  
  Long-term replacement for direct TCP between components. Eliminates the rendezvous problem and the retry workaround. Covered by slice 10.

- **Doxygen navigation layer — clickable architecture maps** (item 18).  
  Hierarchy of SVG architecture maps embedded in Doxygen HTML. Each component is a clickable region linking to a curated `.dox` landing page. Tool: Graphviz/DOT (`URL` attribute → native SVG `<a>` elements, no JavaScript). See the full discussion in `pubsub_itc_fw_summary.md` under item 18.

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

- **WAL-follower pattern for downstream consumers** (Kafka publisher, market data). Each consumer opens a connection to the sequencer leader with a position cursor and receives WAL records from cursor onward. Reuses the existing replication primitive. Topic-based pubsub may be added later if multi-subscriber fanout with replay semantics is genuinely needed.

### Leaning

- **Per-component HA primitives provided by the framework** as reusable building blocks (`Wal`, replication-channel pattern, arbiter-client API, fencing-discipline helper). Avoids each component implementing HA differently.

- **Quill backtrace logging** — when an `Error` or `Critical` record fires, a ring of recent diagnostic context is also flushed. Quill v11 supports this directly. Planned when convenient.

### Open

- **Arbiter internal HA mechanism.** Intent is hand-rolled lease+epoch with witness voting, not a consensus library. Not yet designed in detail.

- **Sub-second sequencer failover target.** How aggressively to tune lease and heartbeat intervals. Should be configurable via `ReactorConfiguration`, not baked in.

- **Sequencer-to-gateway connection direction.** Currently the sequencer initiates outbound connections to gateways (unusual direction). The reverse (gateway connects to sequencer) is more conventional and easier to scale horizontally. Open until a multi-gateway deployment scenario forces the choice.

- **Market data integration mechanism.** Depends on requirements from the market data system. Possibilities: another WAL follower, topic-based pubsub, or bespoke mechanism. Under investigation.

- **DR site topology.** Second site, cross-site replication, separate arbiter pair. Out of scope until main-site design is implemented.

- **Multi-instrument scaling.** Single sequencer vs sharded vs per-instrument. Out of scope until single-instrument is operationally proven.
