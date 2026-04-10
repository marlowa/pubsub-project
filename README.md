# pubsub_itc_fw

A low-latency, multi-threaded, event-driven application framework for C++17, built around the **reactor pattern**. It provides inter-thread communication, inter-process communication, pub/sub messaging, timers, high availability, and a binary serialisation DSL — all designed for environments where heap allocation on the hot path is not acceptable.

## Features

- **Inter-thread communication (ITC)** via lock-free MPSC queues
- **Inter-process communication (IPC)** via unicast TCP with zero-copy PDU paths
- **Pub/sub messaging** via unicast fanout
- **Timers** via `timerfd` and `epoll`
- **High availability** via primary/secondary instance pairs with DR arbitration and automatic leader election
- **Binary serialisation DSL** — a Python code generator producing C++17 encode/decode headers; sub-100ns round-trip on typical messages

## Design Principles

- CPU-pinned threads with lock-free fast paths throughout
- No heap allocation on any hot path — pool allocators, bump allocators, and slab allocators used exclusively
- Zero-copy on all inbound and outbound PDU paths
- Deterministic shutdown
- Message ordering preserved

## High Availability

The framework provides a built-in leader-follower protocol for deploying resilient application pairs. Four instances are deployed in total: two at the main site (primary and secondary) and two at a DR site. Each site elects a leader and follower deterministically — the node with the lowest configured `instance_id` wins.

A DR node acts as arbiter at startup to prevent split-brain when both nodes are undecided. Once elected, the peer-to-peer connection is maintained with heartbeats. If the leader fails, the follower promotes itself and increments the epoch, ensuring that any restarting node can immediately recognise it is stale and rejoin as follower without requiring further arbitration.

The protocol is intentionally simple — there is no need for a full consensus algorithm such as Raft or Paxos given the fixed two-node-plus-arbiter topology.

## Serialisation DSL

Messages are defined in a lightweight DSL and compiled to C++17 headers by a Python code generator:

```
message StatusQuery (id=100, version=1)
    i64 instance_id
    i32 epoch
end
```

Supported field types include `i8`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>`, `optional T`, and named enum and message references. The wire format is little-endian binary. On little-endian hosts, `list<primitive>` decode is zero-copy.

## Requirements

| Item | Detail |
|---|---|
| Language | C++17 |
| Target compiler | gcc-8.5 / RHEL 8 |
| Build system | CMake + `build.py` |
| Logging | Quill v11.x |
| Test framework | GoogleTest (C++), pytest (DSL tests) |

## Building

```bash
python3 build.py
```

Unit tests and integration tests are run automatically as part of the build. The build script reports signal-based failures (SIGABRT, SIGSEGV, etc.) by name.

## Namespace

All framework classes live in the `pubsub_itc_fw` namespace.

## License

Apache-2.0