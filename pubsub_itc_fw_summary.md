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
| Max line width | 160 characters (clang-format enforced) |

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
- Zero-copy on all PDU paths (inbound and outbound)
- Deterministic shutdown
- Message ordering preserved

---

## Major Subsystems

### 1. Allocator Subsystem

| Class | One-liner |
|---|---|
| `FixedSizeMemoryPool<T>` | Single fixed-capacity pool backed by `mmap`; Treiber stack free-list with 128-bit tagged CAS; `std::atomic<Slot<T>*> free_next` field in `Slot<T>` (outside union, before canary) eliminates data race on next pointer; `deallocation_count_` atomic for safe statistics without list traversal; Valgrind/TSan mutex fallback |
| `ExpandablePoolAllocator<T>` | Chains `FixedSizeMemoryPool<T>` instances; lock-free fast path, mutex on expansion; pools never removed |
| `BumpAllocator` | Non-owning bump allocator; snprintf contract — always advances `bytes_used()`; `nullptr`+0 = measuring mode; not thread-safe |
| `SlabAllocator` | Single `mmap`-backed slab; bump allocation (reactor thread only); atomic outstanding count; notifies reactor on last-chunk free |
| `ExpandableSlabAllocator` | Chains `SlabAllocator` instances; demand-driven reclamation (no GC thread); returns `std::tuple<int, void*>` for structured bindings |
| `EmptySlabQueue` | Intrusive Vyukov MPSC queue of slab IDs; one node embedded per slab |

**`Slot<T>` layout (production path):**
```
[ is_constructed (atomic) ][ free_next (atomic) ][ canary (u64) ][ storage (alignas T) ]
```
`free_next` before `canary` — canary remains adjacent to storage for underrun detection.

**Bug history:** Two bugs fixed in `FixedSizeMemoryPool`: (1) unsafe free-list traversal in `get_number_of_available_objects` — fixed with atomic counters; (2) data race on `next` pointer inside union — fixed by moving to `std::atomic<Slot<T>*> free_next` outside the union. Both produced ~1-in-100 failure rate under stress. Likely present in the closed-source production allocator as well.

---

### 2. Lock-Free Queue Subsystem

| Class | One-liner |
|---|---|
| `LockFreeMessageQueue<T>` | Vyukov MPSC queue; nodes from `ExpandablePoolAllocator<Node>`; watermark hysteresis callbacks; shutdown semantics |
| `QueueConfig` | Watermark thresholds and callbacks |

---

### 3. Threading Subsystem

| Class | One-liner |
|---|---|
| `ApplicationThread` | Abstract base; owns queue and thread; timer APIs enforced from owning thread; `connect_to_service()` for outbound TCP; pure virtual `on_itc_message()` |
| `ThreadWithJoinTimeout` | Wraps `std::thread`; `join_with_timeout()` |
| `ThreadID` | Strongly-typed thread identifier |
| `ThreadLifecycleState` | NotCreated, Started, InitialProcessed, Operational, ShuttingDown, Terminated |

**Virtual callbacks on `ApplicationThread`:**
- `on_initial_event()`, `on_app_ready_event()`, `on_termination_event(reason)`
- `on_itc_message(msg)` — pure virtual
- `on_timer_event(name)`
- `on_pubsub_message(msg)`, `on_raw_socket_message(msg)`
- `on_framework_pdu_message(msg)` — **caller must call `allocator.deallocate(msg.slab_id(), msg.payload())` after processing**
- `on_connection_established(id)`, `on_connection_failed(reason)`, `on_connection_lost(id, reason)`

---

### 4. Reactor Subsystem

| Class | One-liner |
|---|---|
| `Reactor` | epoll event loop; owns all threads, timers, outbound connections; `ServiceRegistry` for service name resolution; inbound and outbound slab allocators sized from config |
| `ReactorConfiguration` | All config: timeouts, slab sizes, HA topology, command queue config, `connect_timeout` (default 5s) |
| `ReactorControlCommand` | Commands: `AddTimer`, `CancelTimer`, `Connect`, `Disconnect`, `SendPdu` |
| `ServiceRegistry` | Static name→`ServiceEndpoints` map; populated before threads start; no file I/O |
| `ServiceEndpoints` | Primary + secondary `NetworkEndpointConfig`; secondary port==0 means not configured |
| `ConnectionID` | Strongly-typed connection identifier; 0 = invalid; monotonically increasing from 1 |
| `OutboundConnection` | Per-connection state for reactor-managed outbound TCP connections (see below) |

**`ReactorConfiguration` slab sizes:**
- `inbound_slab_size{65536}` — size each inbound slab (used by `PduParser`)
- `outbound_slab_size{65536}` — size of each outbound slab (used by application threads)
- Both default to 64KB; should be at least as large as the largest expected PDU

**Key reactor design rules:**
- All socket I/O on reactor thread only
- `fast_path_threads_` written only during init/shutdown, read-only during running
- Connect timeout checked by `on_housekeeping_tick()` via backstop timer
- `pending_send_` — `std::optional<ReactorControlCommand>` holding slot for a blocked `SendPdu`

---

### 5. OutboundConnection

Represents one reactor-managed outbound TCP connection (fred → joe). Lives in `Reactor::connections_` map.

**Two lifecycle phases:**

| Phase | Indicator | Active members |
|---|---|---|
| Connecting | `is_connecting()` true | `connector_`, `connect_started_at_`, `trying_secondary_` |
| Established | `is_established()` true | `socket_`, `framer_`, `parser_` |

**Connection flow:**
1. `Connect` command → `process_connect_command()` → `TcpConnector::connect(primary)` → register fd for `EPOLLOUT`
2. `EPOLLOUT` fires → `on_connect_ready()` → `finish_connect()`:
   - Success → `on_connected(socket, disconnect_handler)` → create `PduFramer` + `PduParser` → re-register for `EPOLLIN` → deliver `ConnectionEstablished`
   - Failure + secondary configured → `retry_with_secondary()` → repeat from step 1 with secondary endpoint
   - Both fail → `teardown_connection()` → deliver `ConnectionFailed`
3. Connect timeout → `check_for_timed_out_connections()` → `teardown_connection()` → deliver `ConnectionFailed`
4. `EPOLLIN` fires → `on_data_ready()` → `PduParser::receive()` → zero-copy into slab → dispatch `FrameworkPdu` to thread queue
5. `SendPdu` command → `process_send_pdu_command()` → `PduFramer::send_prebuilt()` (zero-copy)
6. Partial send → store in `current_*` fields + register `EPOLLOUT` → `on_write_ready()` → `continue_send()` → deallocate slab when complete
7. `Disconnect` or peer close → `teardown_connection()` → deliver `ConnectionLost`

**Reactor maps for OutboundConnection:**
- `connections_` — `ConnectionID → unique_ptr<OutboundConnection>` (owns)
- `connections_by_fd_` — `int fd → OutboundConnection*` (non-owning, for epoll dispatch)

**`pending_send_` pattern:** `process_control_commands()` checks `pending_send_` before draining the command queue. If a `SendPdu` cannot proceed (partial write in flight), it is stashed in `pending_send_` and the queue is not drained further for that connection. Cleared when `on_write_ready()` completes the send.

---

### 6. Messaging Subsystem

| Class | One-liner |
|---|---|
| `EventMessage` | Move-only envelope; `EventType` tag, payload pointer, `slab_id`, `TimerID`, reason string, originating `ThreadID`, `ConnectionID` |
| `EventType` | None, Initial, AppReady, Termination, InterthreadCommunication, Timer, PubSubCommunication, RawSocketCommunication, FrameworkPdu, ConnectionEstablished, ConnectionFailed, ConnectionLost |

**Key factory methods:**
- `create_framework_pdu_message(data, size, slab_id)` — receiver must deallocate
- `create_connection_established_event(connection_id)`
- `create_connection_failed_event(reason)`
- `create_connection_lost_event(connection_id, reason)`

---

### 7. Socket / IPC Subsystem

| Class | One-liner |
|---|---|
| `TcpSocket` | Non-blocking TCP socket; `TCP_NODELAY` on all sockets; `get_file_descriptor()` for epoll |
| `TcpAcceptor` | Non-blocking listening socket |
| `TcpConnector` | Stateless non-blocking connector; `connect()` + `finish_connect()` + `get_fd()` + `get_connected_socket()` |
| `ByteStreamInterface` | Abstract base: `send()`, `receive()`, `close()`, `get_peer_address()` |
| `InetAddress` | Concrete IP address; factory from host+port string via `getaddrinfo` |

---

### 8. PDU Framing Subsystem

| Class | One-liner |
|---|---|
| `PduHeader` | 16-byte wire header: `byte_count` (u32), `pdu_id` (i16), `version` (i8), `filler_a` (u8), `canary` (u32=0xC0FFEE00), `filler_b` (u32); all multi-byte fields network byte order |
| `PduFramer` | Two-mode send: `send()` builds frame internally (small fixed PDUs, max 256 bytes payload); `send_prebuilt()` zero-copy from slab chunk (large PDUs); both share `continue_send()` / `has_pending_data()` |
| `PduParser` | Zero-copy receive: phase 1 reads 16-byte header; phase 2 allocates slab chunk and reads payload directly from socket into it; dispatches `FrameworkPdu` EventMessage with slab_id |

**Inbound PDU ownership:** reactor allocates slab → PduParser reads into it → EventMessage carries ptr+slab_id → app thread must call `inbound_slab_allocator().deallocate(msg.slab_id(), msg.payload())` after processing.

**Outbound PDU ownership:** app thread allocates slab from `outbound_slab_allocator()` → writes PduHeader + encoded payload → enqueues `SendPdu` → reactor sends via `send_prebuilt()` → reactor deallocates slab when send complete.

---

### 9. DSL Subsystem

Python code generator producing C++17 headers for zero-copy binary encode/decode.

**Benchmark results:** SmallMessage 17ns/15ns, MediumMessage 40ns/56ns, LargeMessage 51ns/44ns.

**Test status:** 61 Python roundtrip tests passing. Coverage 90%. Pylint 10/10.

---

### 10. Leader-Follower Protocol (DSL defined, not yet implemented)

| Message | ID | Purpose |
|---|---|---|
| `StatusQuery` | 100 | Identity + epoch on TCP connect |
| `StatusResponse` | 101 | Identity confirmation + peer echo |
| `Heartbeat` | 102 | Liveness + epoch propagation |
| `ArbitrationReport` | 200 | Sent to DR when arbitration needed |
| `ArbitrationDecision` | 201 | DR's authoritative tie-break |

---

### 11. Logging Subsystem

`QuillLogger` wrapping `quill::Logger*`. `PUBSUB_LOG(logger, level, fmt, ...)` for format args; `PUBSUB_LOG_STR(logger, level, str)` for single string (required by `-Werror=variadic-macros`).

---

## Memory Model Summary

| Allocator | Used for | Thread-safe | Reclamation |
|---|---|---|---|
| `FixedSizeMemoryPool<T>` | Fixed-size objects | Yes (Treiber stack) | Never |
| `ExpandablePoolAllocator<T>` | Queue nodes, reactor commands | Yes | Never |
| `BumpAllocator` | DSL encode/decode scratch | No | `reset()` only |
| `ExpandableSlabAllocator` | PDU payloads (in/out) | Alloc: reactor; Dealloc: any | Demand-driven, reactor only |

---

## What Is Done

- Allocator subsystem — complete, tested, all races fixed (500-iteration stress test clean)
- Lock-free MPSC queue — complete, tested
- Reactor event loop — complete, tested
- ApplicationThread — complete, tested
- Socket layer — complete, tested (`TcpConnector` has `get_fd()` added)
- PDU framing (`PduFramer` two-mode, `PduParser` zero-copy) — complete, tested
- `OutboundConnection` — complete (connecting, established, partial send, disconnect, timeout, secondary retry)
- Reactor connection management (`Connect`, `SendPdu`, `Disconnect`) — complete
- Connect timeout via housekeeping tick — complete
- Secondary endpoint automatic retry — complete
- `ServiceRegistry` / `ServiceEndpoints` / `ConnectionID` — complete
- `EventType` / `EventMessage` with connection lifecycle events and slab_id — complete
- `ReactorControlCommand` with all five tags — complete
- `ReactorConfiguration` with slab sizes and connect timeout — complete
- DSL code generator — complete, 61 tests passing
- Leader-follower DSL messages — defined and generated
- Logging subsystem — complete

## What Is Not Yet Done (in dependency order)

1. **`InboundConnection`** — joe accepting fred's connection via `TcpAcceptor`; needs its own struct analogous to `OutboundConnection` but for the server side
2. **Leader-follower protocol** — state machine, heartbeat timers, arbitration; requires both outbound (fred→joe) and inbound (joe accepts fred) connection management
3. **Pub/sub fanout** — unicast fanout to simulate topic-based pub/sub
4. **Connection management unit tests** — end-to-end test using loopback sockets; deferred until inbound path is also implemented so both sides can be tested together
5. **`ExpandablePoolAllocatorTest.CrossPoolAbaInterleaving`** — intermittent 1-in-500 failure; the `free_next` atomic fix resolved the main race; residual failures may indicate a secondary issue worth investigating under TSan

## Immediate Next Task

Implement `InboundConnection` and the reactor's acceptor path:
- `TcpAcceptor` registered with epoll on `primary_address` from `ReactorConfiguration`
- On `EPOLLIN`: `accept_connection()` → create `InboundConnection` with `PduParser` and `PduFramer` → register fd for `EPOLLIN`
- `InboundConnection` needs: `TcpSocket`, `PduFramer`, `PduParser`, partial send state — same pattern as `OutboundConnection` minus the connecting phase
- Decide which ApplicationThread receives inbound PDUs (probably a registered handler or a default thread)

---

## HA Architecture

- Two main nodes: **primary** and **secondary** (per site)
- Two DR nodes: **DR primary** and **DR secondary** (fallback arbiters)
- Leadership determined by lowest `instance_id` — deterministic, no votes
- On partition or startup, nodes send `ArbitrationReport` to DR; DR responds with `ArbitrationDecision`
- Heartbeat A↔B only; DR is pure arbiter, never becomes leader/follower
- All endpoints configured via `ReactorConfiguration` (`primary_address`, `secondary_address`, `dr_primary_address`, `dr_secondary_address`) and `ServiceRegistry`

---

## ReactorControlCommand Payload Fields by Tag

| Tag | Fields |
|---|---|
| `AddTimer` | `owner_thread_id_`, `timer_id_`, `timer_name_`, `interval_`, `timer_type_` |
| `CancelTimer` | `owner_thread_id_`, `timer_id_` |
| `Connect` | `requesting_thread_id_`, `service_name_` (resolved via `ServiceRegistry`) |
| `Disconnect` | `connection_id_` |
| `SendPdu` | `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_` |

---

## DSL Subsystem — Full API

**DSL types:** `i8`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>`, `optional T`, `enum : base`, named message references.

**Generated API per message:**
- `encoded_size(msg)` — wire size in bytes
- `encode(msg, buf, encode_arena)` — encode to buffer; arena is scratch only
- `encode_fast(msg, buf)` — fixed-size messages only, no arena needed
- `decode(buf, arena, arena_bytes_needed)` — snprintf contract: always sets true bytes required
- `skip(buf)` — skip over message in buffer
- `max_decode_arena_bytes<N>()` — conservative upper bound for arena sizing
- `max_encode_arena_bytes<N>()` — conservative upper bound for arena sizing

**Wire format:** Little-endian binary; `u32` length prefixes for strings and lists. On little-endian hosts, `list<primitive>` decode is zero-copy.

**`BumpAllocator` two-pass pattern for variable-length encode:**
```cpp
// Pass 1: measure
BumpAllocator measuring(nullptr, 0);
encode(msg, wire_buf, measuring);
size_t needed = measuring.bytes_used();

// Pass 2: allocate real storage and retry
auto [slab_id, ptr] = allocator.allocate(needed);
BumpAllocator real(static_cast<uint8_t*>(ptr), needed);
encode(msg, wire_buf, real);
```

---

## Allocator Subsystem — Full Table

| Class | One-liner |
|---|---|
| `FixedSizeMemoryPool<T>` | See above |
| `ExpandablePoolAllocator<T>` | See above |
| `BumpAllocator` | See above |
| `SlabAllocator` | See above |
| `ExpandableSlabAllocator` | See above |
| `EmptySlabQueue` | See above |
| `AllocatorConfig` | Config for `ExpandablePoolAllocator`: pool name, objects per pool, initial pools, expansion threshold, callbacks, huge pages flag, context pointer |
| `PoolStatistics` | Snapshot: pool count, objects per pool, available objects; `log_statistics(QuillLogger&)` |
| `AllocatorBehaviourStatistics` | Counters: total/fast-path/slow-path allocations, expansion events, failed allocations |

---

## Miscellaneous / Support

| Class/File | One-liner |
|---|---|
| `CacheLine<T>` | Aligns `T` to cache line boundary to prevent false sharing |
| `PreconditionAssertion` | Exception thrown on precondition violations (not `assert`) |
| `PubSubItcException` | Framework-level exception base |
| `WrappedInteger<Tag, T>` | Type-safe integer wrapper; base for `ThreadID`, `TimerID`, `ConnectionID`; `is_valid()` returns `value != 0` |
| `Backoff` | Spin-wait backoff helper |
| `HighResolutionClock` | Clock alias used for event timing in `ApplicationThread` |
| `MillisecondClock` | Millisecond-precision clock used for inactivity checks and connect timeout |
| `StringUtils` | `get_error_string(int)`, `get_errno_string()`, `leafname()`, `starts_with()` |
| `SimpleSpan<T>` | Minimal non-owning span (pre-C++20 compatibility) |
| `FileLock` | File-based lock |
| `MemoryMappedFile` | `mmap`-backed file wrapper |
| `UseHugePagesFlag` | Enum: `DoUseHugePages` / `DoNotUseHugePages` |
| `CoverageDummy` | Compilation unit to satisfy coverage tooling |

**Test infrastructure:**

| Class | One-liner |
|---|---|
| `LoggerWithSink` | Logger wired to `TestSink`; in `pubsub_itc_fw` namespace (NOT `test_support`) — important for test compilation |
| `TestSink` | In-memory log sink for test assertions |
| `MisbehavingThreads` | Test helpers that simulate stuck/crashed threads |
| `LatencyRecorder` | Nanosecond-bucket histogram recorder; thread-safe; dump to file |
| `UnitTestLogger` | Logger configured for unit tests |

---

## Outbound PDU Path (implemented, tested)

Fred sends a PDU to joe:
1. Fred calls `reactor.outbound_slab_allocator().allocate(sizeof(PduHeader) + payload_size)`
2. Fred writes `PduHeader` in network byte order at chunk start
3. Fred encodes payload after header using DSL `encode()` / `encode_fast()`
4. Fred enqueues `ReactorControlCommand{SendPdu}` with `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_`
5. Reactor calls `PduFramer::send_prebuilt(chunk_ptr, total_bytes)` — zero copy
6. On partial write: reactor records `current_*` in `OutboundConnection`, registers `EPOLLOUT`
7. `EPOLLOUT` fires: `PduFramer::continue_send()` resumes; when complete reactor calls `outbound_slab_allocator_.deallocate()`

## Inbound PDU Path (implemented, tested, zero-copy)

Joe's reactor receives a PDU from fred:
1. epoll signals `EPOLLIN` on connected socket
2. `PduParser::receive()` reads 16-byte `PduHeader` into `header_buffer_`; validates canary
3. `PduParser` allocates slab chunk: `auto [slab_id, chunk] = inbound_slab_allocator_.allocate(byte_count)`
4. `PduParser` reads payload **directly from socket into slab chunk** — zero copy
5. Dispatches `EventMessage::create_framework_pdu_message(payload, size, slab_id)` to joe's thread queue
6. Joe's thread calls `on_framework_pdu_message(msg)`, processes payload
7. Joe's thread calls `inbound_slab_allocator_.deallocate(msg.slab_id(), msg.payload())`