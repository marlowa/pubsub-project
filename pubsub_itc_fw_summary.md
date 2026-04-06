# pubsub_itc_fw — Project Summary for AI Session Handover

## Quick Facts

| Item | Detail |
|---|---|
| Language | C++17 |
| Namespace | `pubsub_itc_fw` |
| Dev compiler | gcc-13 / Linux Mint 22.2 |
| Target compiler | gcc-8.5 / RHEL 8 |
| Build system | CMake + build.py |
| Logging | Quill (v11.x) |
| Test framework | GoogleTest (C++), pytest (Python DSL tests) |
| License | Apache-2.0 |

---

## What It Is

A low-latency, multi-threaded, event-driven application framework using the **reactor pattern**. It provides:

- Inter-thread communication (ITC) via lock-free MPSC queues
- Inter-process communication (IPC) via unicast TCP
- Simulated pub/sub via unicast fanout
- Timers (timerfd, via epoll)
- High availability via primary/secondary instance pairs with DR arbitration
- A DSL-based binary serialisation layer replacing protobuf/SBE

Target environment is **low-latency** (sub-100ns encode/decode). Heap allocation is avoided on all hot paths.

---

## Architectural Goals

- Threads pinned to specific CPUs
- Lock-free fast paths throughout
- Predictable memory allocation (pool allocators, bump allocators, slab allocators)
- Deterministic shutdown
- Message ordering preserved
- Minimal jitter; suitable for large volumes of inter-thread messages

---

## Major Subsystems

### 1. Allocator Subsystem

| Class | One-liner |
|---|---|
| `FixedSizeMemoryPool<T>` | Single fixed-capacity pool backed by `mmap`; Treiber stack free-list with 128-bit tagged CAS (`CMPXCHG16B`); canary + double-free detection; Valgrind/TSan mutex fallback |
| `ExpandablePoolAllocator<T>` | Chains `FixedSizeMemoryPool<T>` instances; lock-free fast path on current pool, mutex on expansion; O(N) deallocation; pools never removed; handles double-free, canary corruption, invalid free |
| `BumpAllocator` | Non-owning bump allocator over caller-supplied buffer; follows snprintf contract — `allocate<T>()` always advances `bytes_used()` even on failure; `nullptr`+0 construction enters measuring mode to discover bytes needed before real allocation; used as `decode_arena`/`encode_arena` in DSL layer; not thread-safe |
| `SlabAllocator` | Single `mmap`-backed slab; bump allocation (reactor thread only); atomic `outstanding_allocations_count`; on last-chunk free enqueues slab ID into `EmptySlabQueue` for reactor reclamation; `reset()` by reactor only |
| `ExpandableSlabAllocator` | Chains `SlabAllocator` instances; allocates from current slab; drains `EmptySlabQueue` before each allocation; reclaims or resets empty slabs demand-driven (no GC thread); returns `std::tuple<int, void*>` (slab_id + ptr) for structured bindings; thread-safe: allocate reactor-only, deallocate any thread |
| `EmptySlabQueue` | Bespoke intrusive Vyukov-style MPSC queue of slab IDs; one node embedded per slab; no dynamic allocation; used only for empty-slab notifications from app threads to reactor |
| `AllocatorConfig` | Configuration struct for `ExpandablePoolAllocator`: pool name, objects per pool, initial pools, expansion threshold, callbacks |
| `PoolStatistics` | Snapshot struct for pool metrics; `log_statistics(QuillLogger&)` |
| `AllocatorBehaviourStatistics` | Counters for total/fast-path/slow-path allocations, expansion events, per-pool allocation counts |

**Key design rules:**
- `FixedSizeMemoryPool`/`ExpandablePoolAllocator`: fixed-size blocks, pools never removed, no cross-thread reclamation
- `SlabAllocator`/`ExpandableSlabAllocator`: variable-size chunks, reactor allocates, any thread deallocates, reactor reclaims demand-driven
- `BumpAllocator`: per-thread scratch only, never crosses thread boundaries
- These three allocator families must never mix

---

### 2. Lock-Free Queue Subsystem

| Class | One-liner |
|---|---|
| `LockFreeMessageQueue<T>` | Vyukov MPSC queue; nodes from `ExpandablePoolAllocator<Node>`; watermark hysteresis callbacks; shutdown semantics (enqueue becomes no-op); Valgrind/TSan mutex fallback |
| `QueueConfig` | Watermark thresholds, low/high watermark callbacks, `for_client_use` context pointer |

**Key design rules:**
- Single consumer (owning `ApplicationThread`), multiple producers
- Queue never full — backed by expandable allocator
- Watermark callbacks are the backpressure mechanism
- Messages lost during shutdown are acceptable

---

### 3. Threading Subsystem

| Class | One-liner |
|---|---|
| `ApplicationThread` | Abstract base for user threads; owns `LockFreeMessageQueue<EventMessage>` and `ThreadWithJoinTimeout`; lifecycle via atomic `ThreadLifecycleState`; timer APIs enforced from owning thread only; `connect_to_service()` for outbound TCP connections; pure virtual `on_itc_message()` |
| `ThreadWithJoinTimeout` | Wraps `std::thread`; `join_with_timeout()` for hung-thread detection |
| `ThreadID` | Strongly-typed thread identifier (application-assigned, unique, enforced by Reactor) |
| `ThreadLifecycleState` | Enum: NotCreated, Started, InitialProcessed, Operational, ShuttingDown, Terminated |

**Virtual callbacks on `ApplicationThread`:**
- `on_initial_event()` — reactor has started
- `on_app_ready_event()` — all threads operational
- `on_termination_event(reason)` — shutdown in progress
- `on_itc_message(msg)` — ITC message received (pure virtual)
- `on_timer_event(name)` — named timer fired
- `on_pubsub_message(msg)` — pub/sub message received
- `on_raw_socket_message(msg)` — raw socket data received
- `on_framework_pdu_message(msg)` — fully framed DSL PDU received; **caller must call `reactor.inbound_slab_allocator().deallocate(msg.slab_id(), msg.payload())` after processing**
- `on_connection_established(id)` — outbound TCP connection ready; carries `ConnectionID`
- `on_connection_failed(reason)` — outbound TCP connection attempt failed
- `on_connection_lost(id, reason)` — established connection dropped

---

### 4. Reactor Subsystem

| Class | One-liner |
|---|---|
| `Reactor` | epoll-based event loop; owns all `ApplicationThread`s; manages `EventHandler`s; timerfd lifecycle; control command queue; `ServiceRegistry` reference for service name resolution; lifecycle via atomic `ReactorLifecycleState` |
| `ReactorConfiguration` | Config: event loop tuning, inactivity/init/shutdown timeouts, command queue config, full HA network topology, heartbeat/arbitration/status-query timeouts, instance IDs |
| `ReactorLifecycleState` | Enum: NotStarted, Running, ShuttingDown, Finished |
| `ReactorControlCommand` | Commands enqueued to the reactor's internal command queue; tags: `AddTimer`, `CancelTimer`, `Connect`, `Disconnect`, `SendPdu` |
| `EventHandler` | Abstract base for epoll-registered handlers (fd + callback) |
| `ServiceRegistry` | Static name→`ServiceEndpoints` registry; populated by application before threads start; no file I/O; fully testable with inline construction; passed to `Reactor` constructor |
| `ServiceEndpoints` | Value type: primary + secondary `NetworkEndpointConfig`; secondary port==0 means not configured |
| `ConnectionID` | Strongly-typed connection identifier (`WrappedInteger<ConnectionIDTag, int>`); 0 = invalid |

**`ReactorControlCommand` payload fields by tag:**
- `AddTimer`: `owner_thread_id_`, `timer_id_`, `timer_name_`, `interval_`, `timer_type_`
- `CancelTimer`: `owner_thread_id_`, `timer_id_`
- `Connect`: `requesting_thread_id_`, `service_name_` (resolved via `ServiceRegistry`)
- `Disconnect`: `connection_id_`
- `SendPdu`: `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_`

**Key design rules:**
- `fast_path_threads_` (raw pointers) written only during init/shutdown, read-only during running
- Thread registration only before `run()`
- Timer ownership: reactor owns timerfd; `ApplicationThread` only schedules/cancels by name
- All socket I/O performed exclusively on the reactor thread
- `Connect`/`Disconnect`/`SendPdu` implementation stubs present in `Reactor.cpp` — not yet implemented

---

### 5. Messaging Subsystem

| Class | One-liner |
|---|---|
| `EventMessage` | Move-only message envelope; `EventType` tag, raw non-owning payload pointer, `slab_id` (for `FrameworkPdu`), `TimerID`, reason string, originating `ThreadID`, `ConnectionID`; created only via static factory methods |
| `EventType` | Enum wrapper: None, Initial, AppReady, Termination, InterthreadCommunication, Timer, PubSubCommunication, RawSocketCommunication, FrameworkPdu, ConnectionEstablished, ConnectionFailed, ConnectionLost |
| `TimerID` | Strongly-typed timer identifier |
| `TimerType` | Enum: OneOff, Recurring |

**Factory methods on `EventMessage`:**
- `create_reactor_event(type)`
- `create_timer_event(timer_id)`
- `create_termination_event(reason)`
- `create_pubsub_message(data, size)`
- `create_itc_message(originating_thread_id, data, size)`
- `create_raw_socket_message(data, size)`
- `create_framework_pdu_message(data, size, slab_id)` — slab_id required; receiver must deallocate
- `create_connection_established_event(connection_id)`
- `create_connection_failed_event(reason)`
- `create_connection_lost_event(connection_id, reason)`

---

### 6. Socket / IPC Subsystem

| Class | One-liner |
|---|---|
| `IpAddressInterface` | Abstract base for IP addresses; pure virtual: string, port, IPv4/IPv6 test, `sockaddr*`, comparison |
| `InetAddress` | Concrete `IpAddressInterface`; wraps `sockaddr_storage`; factory from host+port string or existing `sockaddr*` |
| `TcpSocket` | Pimpl, non-blocking TCP socket; `TCP_NODELAY` set on all sockets; `create(ip_version)` / `adopt(fd)` factories; implements `ByteStreamInterface` |
| `TcpAcceptor` | Pimpl; non-blocking listening socket; `accept_connection()` returns socket + address + error |
| `TcpConnector` | Pimpl; stateless non-blocking outbound connector; two-phase: `connect()` then `finish_connect()` when writable |
| `ByteStreamInterface` | Abstract base: `send()`, `receive()`, `close()`, `get_peer_address()` |
| `NetworkEndpointConfig` | Simple value type: `host` string + `port` uint16_t |
| `SocketHandler` | epoll handler for connected sockets (early stage, header only) |

---

### 7. PDU Framing Subsystem

| Class | One-liner |
|---|---|
| `PduHeader` | 16-byte wire frame header: `byte_count` (u32), `pdu_id` (i16), `version` (i8), `filler_a` (u8), `canary` (u32 = 0xC0FFEE00), `filler_b` (u32); all multi-byte fields in **network byte order**; DSL payload that follows is little-endian |
| `PduFramer` | Send-side framing; writes `PduHeader` in network byte order + payload into fixed `frame_buffer_[sizeof(PduHeader)+max_payload_size]`; handles partial writes via `continue_send()`; `has_pending_data()` signals reactor to register `EPOLLOUT` |
| `PduParser` | Zero-copy receive-side framing; phase 1 accumulates 16-byte header into `header_buffer_`; phase 2 allocates slab chunk and reads payload **directly from socket into slab memory** (no copy); dispatches `FrameworkPdu` `EventMessage` with `slab_id` to target thread; receiver must deallocate |

**Wire format:**
- `PduHeader` fields: network byte order (`htonl`/`htons` on send, `ntohl`/`ntohs` on receive)
- DSL payload: little-endian (DSL wire format), zero-copy on little-endian hosts
- Canary `0xC0FFEE00` distinguishes PDU frame corruption from allocator slot corruption (`0xDEADC0DEFEEDFACE`)

**Inbound PDU memory ownership:**
- Reactor thread allocates slab chunk, reads payload into it
- `EventMessage` carries `slab_id` and payload pointer
- Application thread owns the chunk from the moment it dequeues the message
- Application thread **must** call `allocator.deallocate(msg.slab_id(), msg.payload())` after processing

---

### 8. DSL Subsystem

A Python-based code generator producing self-contained C++17 headers for zero-copy, zero-heap-allocation encode/decode of structured binary messages.

**DSL types supported:** `i8`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>` (unlimited nesting), `optional T`, `enum : base`, named message references.

**Generated API per message:**
- `encoded_size(msg)` — wire size
- `encode(msg, buf, encode_arena)` — encode to buffer; arena is scratch only, not the wire buffer
- `encode_fast(msg, buf)` — fixed-size messages only, no arena needed
- `decode(buf, arena, arena_bytes_needed)` — snprintf contract: always sets true bytes required
- `skip(buf)` — skip over message in buffer
- `max_decode_arena_bytes<N>()` — conservative upper bound for arena sizing
- `max_encode_arena_bytes<N>()` — conservative upper bound for arena sizing

**`BumpAllocator` two-pass pattern for variable-length encode:**
```cpp
// Pass 1: measure
BumpAllocator measuring_arena(nullptr, 0);
encode(msg, wire_buf, measuring_arena);   // returns false but...
size_t needed = measuring_arena.bytes_used(); // ...this is accurate

// Pass 2: allocate real storage and retry
auto [slab_id, ptr] = allocator.allocate(needed);
BumpAllocator real_arena(static_cast<uint8_t*>(ptr), needed);
encode(msg, wire_buf, real_arena);        // succeeds
```

**Wire format:** Little-endian binary; `u32` length prefixes for strings and lists.

**Benchmark results (x86-64 little-endian, perf-measured):**

| Message | Encode | Decode |
|---|---|---|
| SmallMessage | 17ns | 15ns |
| MediumMessage | 40ns | 56ns |
| LargeMessage | 51ns | 44ns |

**Test status:** 61 Python roundtrip tests passing. Coverage 90%. Pylint 10/10.

---

### 9. Leader-Follower Protocol (DSL defined, not yet implemented)

Five fixed-size messages (all `i64`/`i32`/`enum` fields — `encode_fast` used, no arena needed on decode):

| Message | ID | Purpose |
|---|---|---|
| `StatusQuery` | 100 | Identity + epoch announcement on TCP connect (A↔B) |
| `StatusResponse` | 101 | Identity confirmation + peer echo + epoch reply |
| `Heartbeat` | 102 | Liveness + epoch propagation (A↔B only, never to DR) |
| `ArbitrationReport` | 200 | Sent to DR when arbitration needed |
| `ArbitrationDecision` | 201 | DR's authoritative tie-break, sent to both nodes |

**Role enum:** `unknown=0`, `leader=1`, `follower=2`, `arbiter=3`. Leadership determined by lowest `instance_id`.

---

### 10. Logging Subsystem

| Class | One-liner |
|---|---|
| `QuillLogger` | Wrapper around `quill::Logger*`; three independent sinks (file, console, syslog) each with their own `FwLogLevel` filter; test-sink constructor for unit tests |
| `FwLogLevel` | Framework log level enum: Trace, Debug, Info, Notice, Warning, Error, Critical, Alert |
| `LoggerUtils` | Static utilities: `leafname()`, `function_name()`, bidirectional `FwLogLevel` ↔ `quill::LogLevel` conversion |
| `LoggingMacros.hpp` | `PUBSUB_LOG(logger, level, fmt, ...)` — dispatches to Quill macros by level; no-op under `CLANG_TIDY` |

---

### 11. Miscellaneous / Support

| Class/File | One-liner |
|---|---|
| `CacheLine<T>` | Aligns `T` to cache line boundary to prevent false sharing |
| `PreconditionAssertion` | Exception thrown on precondition violations (not `assert`) |
| `PubSubItcException` | Framework-level exception base |
| `WrappedInteger<Tag, T>` | Type-safe integer wrapper; base for `ThreadID`, `TimerID`, `ConnectionID`; `is_valid()` returns `value != 0` |
| `Backoff` | Spin-wait backoff helper |
| `HighResolutionClock` | Clock alias used for event timing in `ApplicationThread` |
| `MillisecondClock` | Millisecond-precision clock used for inactivity checks |
| `StringUtils` | String utility functions |
| `FileLock` | File-based lock |
| `MemoryMappedFile` | `mmap`-backed file wrapper |
| `UseHugePagesFlag` | Enum: `DoUseHugePages` / `DoNotUseHugePages` |
| `SimpleSpan<T>` | Minimal non-owning span (pre-C++20 compatibility) |
| `CoverageDummy` | Compilation unit to satisfy coverage tooling |

**Test infrastructure:**

| Class | One-liner |
|---|---|
| `TestRunner` | GoogleTest runner entry point |
| `UnitTestLogger` | Logger configured for unit tests |
| `MockLogger` | Mock logger for test assertions |
| `LoggerWithSink` | Logger wired to a `TestSink`; in `pubsub_itc_fw` namespace |
| `TestSink` | In-memory log sink for test assertions |
| `LatencyRecorder` | Nanosecond-bucket histogram recorder; thread-safe; dump to file |
| `MisbehavingThreads` | Test helpers that simulate stuck/crashed threads |

---

## HA Architecture

- Two main nodes: **primary** and **secondary** (per site)
- Two DR nodes: **DR primary** and **DR secondary** (fallback arbiters)
- Leadership determined by lowest `instance_id` — deterministic, no votes
- On partition or startup, nodes send `ArbitrationReport` to DR; DR responds with `ArbitrationDecision`
- Heartbeat A↔B only; DR is pure arbiter, never becomes leader/follower
- All endpoints configured via `ReactorConfiguration` and `ServiceRegistry`

---

## Memory Model Summary

| Allocator | Used for | Thread-safe | Reclamation |
|---|---|---|---|
| `FixedSizeMemoryPool<T>` | Fixed-size objects, single pool | Yes (Treiber stack) | Never (pool is permanent) |
| `ExpandablePoolAllocator<T>` | Queue nodes, reactor commands | Yes | Never (pools never removed) |
| `BumpAllocator` | DSL encode/decode scratch arenas | No (per-thread) | `reset()` only |
| `SlabAllocator` / `ExpandableSlabAllocator` | Inbound PDU payloads; outbound PDU send buffers | Allocate: reactor only; Deallocate: any thread | Yes — demand-driven, reactor-only, no GC thread |

---

## Outbound PDU Path (designed, partially implemented)

Fred (ApplicationThread) sends a PDU to joe over TCP:

1. Fred computes payload size via DSL `encoded_size()`
2. Fred allocates slab chunk: `auto [slab_id, ptr] = reactor.outbound_slab_allocator().allocate(sizeof(PduHeader) + payload_size)`
3. Fred writes `PduHeader` in network byte order at `ptr`
4. Fred encodes payload immediately after header using DSL `encode()` / `encode_fast()`
5. Fred enqueues `ReactorControlCommand{SendPdu}` with `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_`
6. Reactor wakes, dequeues command, sends frame on the target connection's socket
7. On partial write: reactor registers `EPOLLOUT`, completes on next writable event
8. When fully sent: reactor calls `outbound_slab_allocator_.deallocate(slab_id, ptr)`

**Status:** Command queue infrastructure complete. Reactor `SendPdu` dispatch is a stub — per-connection send queue and `EPOLLOUT` handling not yet implemented.

---

## Inbound PDU Path (implemented, tested)

Joe's reactor receives a PDU from fred:

1. epoll signals `EPOLLIN` on joe's connected socket
2. `PduParser::receive()` reads 16-byte `PduHeader` into `header_buffer_`; validates canary
3. `PduParser` allocates slab chunk: `auto [slab_id, chunk] = inbound_slab_allocator_.allocate(byte_count)`
4. `PduParser` reads payload **directly from socket into slab chunk** — zero copy
5. `PduParser` dispatches `EventMessage::create_framework_pdu_message(payload, size, slab_id)` to joe's thread queue
6. Joe's thread calls `on_framework_pdu_message(msg)`, processes payload
7. Joe's thread calls `inbound_slab_allocator_.deallocate(msg.slab_id(), msg.payload())` when done

---

## What Is Done

- Allocator subsystem (FixedSizeMemoryPool, ExpandablePoolAllocator, BumpAllocator, SlabAllocator, ExpandableSlabAllocator, EmptySlabQueue) — complete, tested, adversarial tests passing
- Lock-free MPSC queue — complete, tested
- Reactor event loop (epoll, timers, thread lifecycle, shutdown) — complete, tested
- ApplicationThread base class — complete, tested; includes `connect_to_service()` and connection lifecycle callbacks
- Socket layer (TcpSocket with TCP_NODELAY, TcpAcceptor, TcpConnector, InetAddress) — complete, tested
- PDU framing layer (PduHeader, PduFramer, PduParser zero-copy) — complete, tested
- ServiceRegistry / ServiceEndpoints — complete, tested
- ConnectionID — complete
- EventType / EventMessage — complete with connection lifecycle events and slab_id
- ReactorControlCommand — complete with Connect/Disconnect/SendPdu tags
- DSL code generator and all supported types — complete, 61 tests passing
- Leader-follower DSL messages — defined and generated
- ReactorConfiguration — includes full HA topology and protocol timeouts
- Logging subsystem — complete
- Known intermittent failure: `ExpandablePoolAllocatorTest.CrossPoolAbaInterleaving` — 1-in-100 failure rate under stress; suspected pre-existing race in Treiber stack; needs investigation

## What Is Not Yet Done (in dependency order)

1. **Reactor `Connect` implementation** — resolve service name via `ServiceRegistry`, call `TcpConnector`, register with epoll, assign `ConnectionID`, deliver `ConnectionEstablished` event
2. **Per-connection state** — `ConnectionID` → (`TcpSocket`, `PduFramer`, `PduParser`, outbound queue, send offset)
3. **Reactor `SendPdu` implementation** — per-socket outbound queue, partial write handling, `EPOLLOUT` registration
4. **Reactor `Disconnect` implementation** — graceful teardown
5. **Leader-follower protocol** — state machine, heartbeat timers, arbitration
6. **Pub/sub fanout** — unicast fanout to simulate topic-based pub/sub
7. **Fix `ExpandablePoolAllocatorTest.CrossPoolAbaInterleaving`** — investigate Treiber stack ABA race

## Immediate Next Task

Implement reactor connection management:
- `Reactor::process_connect_command()` — resolve service, `TcpConnector::connect()`, register EPOLLOUT for connect completion, on `finish_connect()` success: assign `ConnectionID`, create per-connection state, register `PduParser`, deliver `ConnectionEstablished`
- Per-connection state struct holding `TcpSocket`, `PduFramer`, `PduParser`, outbound PDU queue, current send offset
- `Reactor::process_send_pdu_command()` — enqueue to per-connection outbound queue, attempt send, register `EPOLLOUT` if partial
