# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, the public API is not yet considered stable and may
change in any release.

## [Unreleased]

### Changed

- Documentation: corrected the TLS status in `docs/design/secure_comms.md` — TLS is
  implemented with OpenSSL and in use (the order gateway exposes an encrypted FIX
  listener via `[fix_tls]`, and the authentication service listener is TLS-secured with
  optional mutual TLS), superseding the earlier "ready to wire up" note. Added a Security
  (TLS and SCRAM) section, version/license/C++ badges, and a current-version line to the
  README.

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
