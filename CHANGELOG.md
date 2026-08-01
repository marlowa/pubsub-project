# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the public API is not yet considered stable and may
change in any release.

## [Unreleased]

### Added

- **Cancel-on-disconnect now has a grace period** (`[cancel_on_disconnect] enabled` and
  `grace_period`, defaulting to on and 30 seconds, in both gateways). A dropped session's
  resting orders are held rather than cancelled, and if the same comp id reconnects inside
  the window nothing is cancelled at all. Previously the whole book went the instant a
  socket closed, so a member whose connection blipped came back flat -- and a gateway
  failure flattened every book on the instance, the high-availability mechanism producing
  exactly the outcome high availability exists to prevent.
- GoodTillCancel and GoodTillDate orders are never cancelled on disconnect; they were
  placed to outlive the session. A clean FIX Logout still cancels immediately, since a
  member that logs out has said what it wants.
- **The matching engine echoes `TimeInForce` on execution reports.** It previously did not,
  which is what made the exemption above impossible to implement.

- **The binary gateway now runs as two instances, `binary_order_gateway_a` and
  `binary_order_gateway_b`**, for the same reason the FIX gateway does: losing one
  instance should not take every session on that protocol with it. The sequencer
  carries four `[[gateway]]` entries in dev — FIX 1 and 2, binary 1 and 2 — and
  routes each execution report back to the instance its order arrived on. Only the
  `_a` instance of each protocol is deployed outside dev; the `_b` entries are
  configured but disabled.
- **`perf_run.py --gateway-instance` now applies to the binary gateway too**, not
  only to FIX. The load generator's target port is read from the chosen instance's
  deployed configuration rather than from a constant in the script, so it cannot
  drift from the deployment.

### Changed

- **Both gateways renamed so the name says the protocol and the job.**
  `order_gateway` is now `fix_order_gateway` and `binary_gateway` is now
  `binary_order_gateway`. The old names said which wire format a gateway spoke, or
  didn't, but neither said what kind of gateway it was — and order entry is not the
  only kind a venue needs. A gateway carrying nothing but risk-parameter changes
  should not have to compete with members placing orders, and naming the existing
  pair for what they actually are leaves room for that without a second rename
  later. The change runs all the way through: directories, binaries, CMake targets,
  namespaces, class names, config files, component names, deployed paths and docs.

## [0.2.1] - 2026-07-31

A patch release fixing three separate failures on the RHEL8 target. **v0.2.0 does
not build on RHEL8**; this release does. There are no functional changes.

All three shared a cause: each was a property of the *target toolchain* rather than
of the source, so a development machine could not see any of them, and no unit test
could have caught them.

### Fixed

- **`fix_codec` did not compile under gcc 8.5.** `FixReject::describe` used
  `snprintf`, and gcc 8.5 rejects it under `-Werror=format-truncation` at a call
  site with a small constant capacity — which the test suite deliberately
  provides, because truncation there is the documented behaviour. Replaced with
  `fmt::format_to_n`, which has the same bounded, non-allocating contract but is
  not a printf format string. Newer gcc does not emit the diagnostic at all, which
  is why the development build stayed green.
- **The Python lint gate disagreed between machines.** pylint 3.3 split
  `too-many-arguments` into a total count (`R0913`) and a positional count
  (`R0917`); a `disable` comment written before the split no longer covered the
  new check. `_emit_dump_value` now takes keyword-only arguments, which satisfies
  the check honestly rather than widening the suppression — `expr`, `out` and
  `indent` are all strings, so a transposition at a call site would silently emit
  wrong C++.
- **The pybind11 round-trip tests failed to load their extension.** The harness
  compiled a shared object into the system temp directory and `dlopen`ed it from
  there; `/tmp` is mounted `noexec` on the target, so the mapping was refused and
  every test failed with `failed to map segment from shared object` after building
  perfectly. The extension is now built under the project's own build tree, which
  also satisfies the rule that a build produces nothing outside the project
  directory. The load failure now explains itself, naming `noexec` and SELinux,
  rather than repeating dlopen's cryptic wording.

### Changed

- **The printf family is banned project-wide in favour of fmt**, with the rule in
  `coding-rules-for-ai-chatbots.txt` and enforcement as `check_standards.py`
  check 26. All 84 remaining call sites converted. A printf format string is
  unchecked against its arguments, so a mismatch is undefined behaviour rather
  than a diagnostic. The rule names which member of the family to use —
  `format_to_n` for a caller's buffer, `format_to` to append, `print` for a
  stream, and `format` only where a `std::string` was wanted anyway, since that
  one allocates — and carves out signal handlers, where neither fmt nor printf is
  legal and `write(2)` is the only correct answer.
- **`pylint` and `pybind11` are now pinned in the Python dev extras.** Neither was.
  pybind11 was not even declared, despite three test modules being unable to run
  without it, which is why it was simply absent on the RHEL8 host and why two
  machines ended up four major versions apart.

### Added

- **`release_check.py`** — a pre-release gate, deliberately not part of a normal
  build. It checks the working tree is committed, that the version agrees across
  `CMakeLists.txt`, both README references and the CHANGELOG, runs the coding
  standards and a full build with nothing skipped, and **builds under RHEL8's
  gcc 8.5 in the Rocky container**. It states plainly what it cannot cover: the
  container shares RHEL8's gcc but not its Python, so it would have caught the
  first of the three failures above and neither of the others.

### Known limitations

Unchanged from 0.2.0: the order gateway remains a single point of failure,
in-flight execution reports do not survive a client reconnect, hot-path cores are
allocated but not quiet, and `ResendRequest` / `SequenceReset-GapFill` remain
untested under load. See the 0.2.0 entry for detail.

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
