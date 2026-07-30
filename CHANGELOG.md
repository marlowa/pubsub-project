# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the public API is not yet considered stable and may
change in any release.

## [Unreleased]

## [0.2.0] - 2026-07-30

Second tagged release. The headline is a **second order-entry gateway**, which makes the
venue multi-gateway for the first time, and a **declared CPU core layout** that replaces
greedy, start-order-dependent core claiming. FIX message construction is now derived
entirely from a FIX data dictionary rather than hand-written, and both web UIs have lost
their CSS framework.

### Added

- **Binary order gateway** (`applications/binary_gateway`) — a second front door onto the
  same venue, speaking internal DSL-generated PDUs with no FIX layer, authenticated with
  SCRAM-SHA-256. Ships with a load generator, and the fix-test-client gained a gateway
  selector so a session can be pointed at either. Multi-gateway execution-report routing
  works via `origin_gateway_id` on the `WalRecord` envelope, covering WAL replay, the
  matching engine book, and `BookUpdate`.
- **Declared CPU core layout** — a per-machine manifest in the environment TOML declares
  which components run on each host, and a machine-invariant `hot_path_rank` declares the
  order in which entitlement to a good core is surrendered, with whole rank groups admitted
  atomically. `deploy.py` runs on the target host, reads the real topology and resolves rank
  to core ids, so one declaration works on a 32-core workstation, a 20-core machine or a
  small VM. Every process now starts masked to a background pool and the reactor explicitly
  *promotes* the threads that earn hot-path cores, so forgetting a thread is harmless
  instead of silently costly. `CpuRegistry` is reduced from an allocator to a record and
  cross-installation collision detector.
- **`cpu_audit.py`** — checks every running thread's real affinity mask from `/proc`
  against the layout, and classifies what else can run on the hot-path cores by what can be
  done about it (per-CPU kernel threads, steerable IRQs, ordinary userspace needing
  `isolcpus`). `--strict` exits non-zero, so a measurement run can be gated on a quiet
  machine.
- **FIX PDU generation from the data dictionary** — `dd_to_dsl` generates full-message DSL
  from `applications/fix_orders.dd.xml` at build time, and the pipeline now runs on
  100%-DD-derived PDUs. FIX repeating groups (`NoUnderlyings`, `NoPartyIDs`) are carried
  end to end, echoed onto the execution report by the matching engine.
- **`WalRecord` as the pipeline envelope** — routing metadata (`gateway_session_conn_id`,
  `origin_gateway_id`) travels on one envelope on the wire and on disk, rather than being
  re-derived per hop.
- **Repeating-group-aware FIX validation** in `fix_codec`, and a reusable repeating-group
  walker that de-duplicates group descent.
- **`callgrind_run.py`** performance tooling; `topic_probe` now subscribes to a list of
  topics including all.
- **fix-test-client order entry** — a full New Order Single form with repeating groups, a
  blotter showing session and admin messages with a heartbeat toggle, and a message-type
  column.

### Changed

- **Both web UIs dropped Pico.css.** The fix-test-client needed no replacement — its own
  `style.css` already was a dense native-desktop look, and Pico had been layered on top of
  it with a third stylesheet to undo the damage. The admin service, which genuinely
  depended on Pico's classless styling, gained a hand-written `static/desktop.css` styling
  the same markup; no templates changed beyond the stylesheet link. Operator branding via
  `brand.css-file` continues to work through documented custom properties that replace the
  `--pico-*` variables it used to target.
- **Gateway logging made symmetric** across the FIX and binary gateways, so a comparison
  between them measures protocol rather than log volume.
- **Timers identified by integer `TimerID`**, dropping timer names from the hot path.
- **Service names interned to a `ServiceID`** off the reactor command hot path.
- **Generated code moved out of the source tree** — FIX PDU codegen renamed off "equity"
  and emitted into the build directory, so a clean build fully regenerates.
- **Doxygen runs without `dot` on the RHEL8 family**, for much faster documentation builds.
- **TLS status corrected** in `docs/design/secure_comms.md`: TLS is implemented with
  OpenSSL and in use — the order gateway exposes an encrypted FIX listener via `[fix_tls]`
  and the authentication service listener is TLS-secured with optional mutual TLS —
  superseding the earlier "ready to wire up" note. The README gained a Security (TLS and
  SCRAM) section, version/licence/C++ badges and a current-version line.

### Fixed

- **Order gateway dropped echoed repeating groups** from the client-facing execution
  report, and used a fixed-size encode buffer for it. Now growable.
- **A `FixField` was used after the underlying cursor advanced** during group extraction.
- **Deployed paths could escape the install tree**, and CPU pinning could fail silently.
- **Two unresolvable anchor links and three further gaps** were making the Doxygen build
  red; `doxygen` now runs clean in the Rocky image under `WARN_AS_ERROR`.
- **Coverage reporting overstated function coverage.**
- **Admin service: New User and Edit role returned HTTP 500.** `users/form.ftl` used a
  `?:` ternary, which Freemarker does not have, so the template never parsed. Neither page
  had ever rendered.
- **fix-test-client blotter row colours** had been painted over for a month by an opaque
  table-cell background.

### Removed

- **Fixed-size encode buffers**, everywhere they remained, replaced by measure-then-fit or
  grow-and-retry against a reusable buffer. A `check_standards` guard now fails the build
  on new ones.

### Documentation

- **[Gateway High Availability](docs/design/gateway_ha.md)** — a new design document
  recording the agreed direction for gateway redundancy: sessions **pinned to a primary and
  backup instance** rather than pooled any-of-N, and in-flight execution reports surviving
  a reconnect. **This is direction, not code — it is planned for 0.3.0 and none of it is
  built in this release.** The gateway remains a single point of failure.
- The **"N-way pooled redundancy"** claim in `wal_and_ha.md` is **withdrawn**. It was never
  implemented, and it stopped matching the code when execution-report routing moved to the
  gateway-local `gateway_session_conn_id`.
- New design documents for CPU pinning and its anti-affinity problem; the roadmap's
  near-term list pruned of items already done.

### Known limitations

- **The order gateway is a single point of failure.** One instance is run, and the code
  cannot yet express a second: `origin_gateway_id` names a protocol rather than an
  instance, and the sequencer's gateway endpoints are scalars. See the design document
  above.
- **In-flight execution reports do not survive a client reconnect**, even to the same
  gateway. There is no outbound message store; every `ResendRequest` is answered with a
  blanket `SequenceReset-GapFill`. Cancel-on-disconnect *is* implemented, so orders do not
  linger on the book behind a dead session.
- **Hot-path cores are allocated but not quiet.** An affinity mask reserves a core *for* a
  thread; it does not reserve it *from* anything else. On a workstation without `isolcpus`,
  interrupt handlers and ordinary userspace threads still share those cores. Run
  `cpu_audit.py --strict` before trusting any latency measurement.
- **`ResendRequest` / `SequenceReset-GapFill`** remain untested under load.

## [0.1.0] - 2026-07-23

First tagged release. `pubsub_itc_fw` is a low-latency, multi-threaded, event-driven
C++17 application framework built around the reactor pattern.

### Added

- **Reactor core** — an epoll-driven event loop with CPU-pinned application threads,
  inter-thread communication over lock-free MPSC queues, and `timerfd`-based timers.
  No heap allocation on the hot path: pool, bump, and slab allocators throughout, with
  zero-copy inbound and outbound PDU paths.
- **Write-ahead log (WAL)** — a segmented, mmap-backed, single-writer append-only log
  with a scan-from-zero / stop-on-damage replay model and a cursor abstraction. Shared
  as a primitive by the sequencer and the topic publisher.
- **Topic-based publish/subscribe** — fan-out with replay, backed by the WAL and paced
  by TCP (no per-record acks; the WAL is the backlog). Page batching, explicit and
  recoverable slow-consumer handling, a separate control channel, and leader-only
  publishing. Topic identity is generated from the DSL. The Matching Engine Publisher
  (MEP) is the reference publisher; `topic_probe` and the `TopicSubscriberThread` base
  are the reference subscribers.
- **High availability** — component-specific composition of shared primitives:
  arbiter-elected leader/follower with lease + epoch fencing, point-to-point WAL
  replication with two-tier commit, a PSA + witness arbiter pool, cancel-on-failover for
  the matching engine, an N-way gateway pool, and an active/active authentication service.
- **Binary serialisation DSL** — a Python code generator emitting self-contained C++17
  (and Java) encode/decode headers with zero-copy decode on little-endian hardware,
  deterministic wire sizes, and allocator-friendly decoding; sub-100 ns round-trip on
  typical messages. Includes generated structured `to_string` / `toString` dumps.
- **`fix_codec` library** — an hffix-style zero-copy FIX reader, writer, and
  dictionary-driven validator (SessionRejectReason rules, per-message permitted-tag
  bitset, perfect-hash tag lookup), generated from FIX XML data dictionaries.
- **Secure communications** — TLS via OpenSSL memory BIOs and SCRAM-SHA-256 authentication.
- **Sample applications** forming a minimal exchange-system skeleton: order gateway
  (FIX 5.0 SP2 connectivity), sequencer, matching engine, matching engine publisher,
  authentication service, admin service, arbiter, witness, `topic_probe`, and a Java
  fix-test-client.
- **Documentation** — a design/reference documentation set under `docs/`, including the
  pub/sub and WAL designs, and a Doxygen API reference.
- **RHEL8 build support** — verified to build and test on the RHEL8 target (Rocky 8,
  gcc-8.5, JDK17, Maven 3.5.4) via a Docker image: 731 C++ tests, 246 DSL tests, and
  both Java artifacts build cleanly.
