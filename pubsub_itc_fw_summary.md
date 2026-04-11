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

**Bug fixed in `ExpandableSlabAllocator`:** `drain_empty_slab_queue()` was destroying a `SlabAllocator` (via `unique_ptr::reset()`) while still traversing the Vyukov queue whose nodes are embedded inside that slab. Fixed by collecting all slab IDs into a `std::vector` first, then processing them after the queue traversal completes. The vector allocation is on a cold path and not a hot-path concern. Detected by ASan on `ExpandableSlabAllocatorTest.OldSlabIsDestroyedAfterChaining`.

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
| `Reactor` | epoll event loop; owns all threads, timers; inherits `ThreadLookupInterface`; delegates inbound and outbound connection management to dedicated managers |
| `ThreadLookupInterface` | Pure abstract interface with single method `get_fast_path_thread(ThreadID)`; implemented by `Reactor`; allows connection managers to deliver events to threads without depending on `Reactor` |
| `InboundConnectionManager` | Owns all inbound connection state: listener registry, accepted connection maps, accept/read/write/teardown/idle-timeout logic |
| `OutboundConnectionManager` | Owns all outbound connection state: connection maps, connect/read/write/teardown/timeout logic |
| `ReactorConfiguration` | All config: timeouts, slab sizes, HA topology, command queue config, `connect_timeout` (default 5s), `socket_maximum_inactivity_interval_` (default 60s) |
| `ReactorControlCommand` | Commands: `AddTimer`, `CancelTimer`, `Connect`, `Disconnect`, `SendPdu` |
| `ServiceRegistry` | Static name→`ServiceEndpoints` map; populated before threads start; no file I/O |
| `ServiceEndpoints` | Primary + secondary `NetworkEndpointConfig`; secondary port==0 means not configured |
| `ConnectionID` | Strongly-typed connection identifier; 0 = invalid; monotonically increasing from 1; allocated by `Reactor::allocate_connection_id()` which is shared between both managers |
| `OutboundConnection` | Per-connection state for reactor-managed outbound TCP connections (see below) |
| `InboundConnection` | Per-connection state for reactor-managed inbound TCP connections (see below) |

**Key reactor design rules:**
- All socket I/O on reactor thread only
- `fast_path_threads_` written only during init/shutdown, read-only during running
- Connect timeout checked by `on_housekeeping_tick()` via backstop timer — now delegated to `OutboundConnectionManager::check_for_timed_out_connections()`
- Idle socket timeout checked by `on_housekeeping_tick()` — delegated to `InboundConnectionManager::check_for_inactive_connections()`
- `pending_send_` — each manager owns its own `std::optional<ReactorControlCommand>` for blocked `SendPdu` commands
- ConnectionID space is shared between inbound and outbound: the Reactor allocates the ID and passes it into both managers as a parameter, avoiding coupling

**Reactor decomposition (in progress — see Immediate Next Task):**
The Reactor has been partially refactored. `InboundConnectionManager` and `OutboundConnectionManager` have been written and are ready. The Reactor itself still needs to be rewritten to inherit `ThreadLookupInterface`, remove the extracted members, and construct/delegate to the two managers. This is the immediate next task.

---

### 5. OutboundConnection

Represents one reactor-managed outbound TCP connection. Lives in `OutboundConnectionManager::connections_` map.

**Two lifecycle phases:**

| Phase | Indicator | Active members |
|---|---|---|
| Connecting | `is_connecting()` true | `connector_`, `connect_started_at_`, `trying_secondary_` |
| Established | `is_established()` true | `socket_`, `framer_`, `parser_` |

**Connection flow:**
1. `Connect` command → `OutboundConnectionManager::process_connect_command()` → `TcpConnector::connect(primary)` → register fd for `EPOLLOUT`
2. `EPOLLOUT` fires → `on_connect_ready()` → `finish_connect()`:
   - Success → `on_connected(socket, disconnect_handler)` → create `PduFramer` + `PduParser` → re-register for `EPOLLIN` → deliver `ConnectionEstablished`
   - Failure + secondary configured → `retry_with_secondary()` → repeat from step 1 with secondary endpoint
   - Both fail → `teardown_connection()` → deliver `ConnectionFailed`
3. Connect timeout → `check_for_timed_out_connections()` → `teardown_connection()` → deliver `ConnectionFailed`
4. `EPOLLIN` fires → `on_data_ready()` → `PduParser::receive()` → zero-copy into slab → dispatch `FrameworkPdu` to thread queue
5. `SendPdu` command → `process_send_pdu_command()` → `PduFramer::send_prebuilt()` (zero-copy)
6. Partial send → store in `current_*` fields + register `EPOLLOUT` → `on_write_ready()` → `continue_send()` → deallocate slab when complete
7. `Disconnect` or peer close → `teardown_connection()` → deliver `ConnectionLost`

**OutboundConnectionManager maps:**
- `connections_` — `ConnectionID → unique_ptr<OutboundConnection>` (owns)
- `connections_by_fd_` — `int fd → OutboundConnection*` (non-owning, for epoll dispatch)

**`pending_send_` pattern:** `OutboundConnectionManager::drain_pending_send()` is called by the Reactor at the start of `process_control_commands()`. If a `SendPdu` cannot proceed (partial write in flight or connection not yet established), it is stashed in the manager's `pending_send_`. Cleared when `on_write_ready()` completes the send.

---

### 6. InboundConnection and Protocol Handler Strategy

**`InboundConnection`** is a thin transport shell representing one accepted TCP connection. It owns:
- `TcpSocket` — the accepted socket
- `unique_ptr<ProtocolHandlerInterface>` — the protocol handler (Strategy pattern)
- `last_activity_time_` — for idle timeout enforcement
- `target_thread_id_` — for `ConnectionLost` delivery

`InboundConnection` no longer owns `PduParser`, `PduFramer`, or any slab bookkeeping — all of that lives in the handler.

**Protocol handler strategy:**

| Class | One-liner |
|---|---|
| `ProtocolHandlerInterface` | Pure abstract interface: `on_data_ready()`, `send_prebuilt()`, `has_pending_send()`, `continue_send()`, `deallocate_pending_send()` |
| `PduProtocolHandler` | Strategy A: owns `PduParser` + `PduFramer` + pending-send slab state; handles framework-native PDU streams |
| `RawBytesProtocolHandler` | Strategy B: not yet implemented; will own a `MirroredBuffer` for alien (e.g. FIX) streams |

**`PduProtocolHandler` responsibilities:**
- Inbound: `PduParser::receive()` reads and dispatches complete PDU frames; disconnect handler invoked on EOF or protocol error
- Outbound: owns `current_allocator_`, `current_slab_id_`, `current_chunk_ptr_`, `current_total_bytes_`; `release_pending_send()` deallocates on completion or teardown
- All slab bookkeeping is internal to the handler; the Reactor and `InboundConnectionManager` never touch slab state directly for inbound connections

**`InboundConnectionManager` maps:**
- `connections_` — `ConnectionID → unique_ptr<InboundConnection>` (owns)
- `connections_by_fd_` — `int fd → InboundConnection*` (non-owning, for epoll dispatch)
- `inbound_listeners_` — `int fd → InboundListener` (owns, keyed by listening socket fd)

**Idle timeout:** `InboundConnectionManager::check_for_inactive_connections()` uses the two-phase identify-then-process pattern. Uses `socket_maximum_inactivity_interval_` from `ReactorConfiguration`.

---

### 7. MirroredBuffer (implemented, tested)

Stream-oriented ring buffer using virtual memory mirroring for alien protocol support.

| Detail | Value |
|---|---|
| Backing | `memfd_create` + double `mmap` |
| Purpose | Provides contiguous view of a byte stream even when data wraps the ring buffer end |
| Head/tail arithmetic | Modular wrap; one-byte sentinel to distinguish full from empty |
| Owner | Will be owned by `RawBytesProtocolHandler` (not yet implemented) |

Unit tests: `MirroredBufferTest` — including `VerifiesVirtualMemoryMirroringContinuity` (adversarial straddle test) and `HandlesExhaustiveWrapAroundStress` (1000 iterations).

---

### 8. Messaging Subsystem

| Class | One-liner |
|---|---|
| `EventMessage` | Move-only envelope; `EventType` tag, payload pointer, `slab_id`, `TimerID`, reason string, originating `ThreadID`, `ConnectionID` |
| `EventType` | None, Initial, AppReady, Termination, InterthreadCommunication, Timer, PubSubCommunication, RawSocketCommunication, FrameworkPdu, ConnectionEstablished, ConnectionFailed, ConnectionLost |

**Key factory methods:**
- `create_framework_pdu_message(data, size, slab_id)` — receiver must deallocate
- `create_raw_socket_message(data, size)` — for alien protocol byte streams
- `create_connection_established_event(connection_id)`
- `create_connection_failed_event(reason)`
- `create_connection_lost_event(connection_id, reason)`

---

### 9. Socket / IPC Subsystem

| Class | One-liner |
|---|---|
| `TcpSocket` | Non-blocking TCP socket; `TCP_NODELAY` on all sockets; `get_file_descriptor()` for epoll |
| `TcpAcceptor` | Non-blocking listening socket |
| `TcpConnector` | Stateless non-blocking connector; `connect()` + `finish_connect()` + `get_fd()` + `get_connected_socket()` |
| `ByteStreamInterface` | Abstract base: `send()`, `receive()`, `close()`, `get_peer_address()` |
| `InetAddress` | Concrete IP address; factory from host+port string via `getaddrinfo` |

---

### 10. PDU Framing Subsystem

| Class | One-liner |
|---|---|
| `PduHeader` | 16-byte wire header: `byte_count` (u32), `pdu_id` (i16), `version` (i8), `filler_a` (u8), `canary` (u32=0xC0FFEE00), `filler_b` (u32); all multi-byte fields network byte order |
| `PduFramer` | Two-mode send: `send()` builds frame internally (small fixed PDUs, max 256 bytes payload); `send_prebuilt()` zero-copy from slab chunk (large PDUs); both share `continue_send()` / `has_pending_data()` |
| `PduParser` | Zero-copy receive: phase 1 reads 16-byte header; phase 2 allocates slab chunk and reads payload directly from socket into it; dispatches `FrameworkPdu` EventMessage with slab_id |

**Inbound PDU ownership:** reactor allocates slab → PduParser reads into it → EventMessage carries ptr+slab_id → app thread must call `inbound_slab_allocator().deallocate(msg.slab_id(), msg.payload())` after processing.

**Outbound PDU ownership:** app thread allocates slab from `outbound_slab_allocator()` → writes PduHeader + encoded payload → enqueues `SendPdu` → reactor sends via `send_prebuilt()` → reactor deallocates slab when send complete.

---

### 11. DSL Subsystem

Python code generator producing C++17 headers for zero-copy binary encode/decode.

**Benchmark results:** SmallMessage 17ns/15ns, MediumMessage 40ns/56ns, LargeMessage 51ns/44ns.

**Test status:** 61 Python roundtrip tests passing. Coverage 90%. Pylint 10/10.

---

### 12. Leader-Follower Protocol (DSL defined, not yet implemented)

#### Overview

This is a bespoke, intentionally simple protocol. There is no need for a full consensus algorithm such as Raft or Paxos. The deployment topology is fixed: exactly two active nodes per site, with a third node (DR arbiter) available to break ties at startup. Leader election is deterministic — the node with the lowest `instance_id` wins. The arbiter never becomes a leader or follower; it only resolves startup ambiguity when both nodes are undecided.

#### Topology

Four instances in total, each with a unique integer `instance_id` configured in `ReactorConfiguration`:

| Instance | Site | Role in election |
|---|---|---|
| Node A (primary) | Main | Participant |
| Node B (secondary) | Main | Participant |
| Node C (primary-DR) | DR | Arbiter |
| Node D (secondary-DR) | DR | Arbiter |

Main-site nodes use DR-site nodes as arbiters. DR-site nodes use main-site nodes as arbiters. Primary arbiter is tried first; secondary arbiter is the fallback if the primary is unreachable.

#### PDU Summary

| Message | ID | Purpose |
|---|---|---|
| `StatusQuery` | 100 | Identity + epoch announced on TCP connect |
| `StatusResponse` | 101 | Identity confirmation + peer echo + current role |
| `Heartbeat` | 102 | Liveness detection + epoch propagation |
| `ArbitrationReport` | 200 | Sent to DR when arbitration needed |
| `ArbitrationDecision` | 201 | DR's authoritative tie-break + epoch assignment |

#### Epoch Semantics

The epoch is a generation counter that exists to detect stale nodes from a previous leadership cycle.

Rules:
1. A node that has never participated in an election starts with epoch 0.
2. At startup, when DR arbitration is used, the DR arbiter assigns the epoch in `ArbitrationDecision`. Both nodes adopt this value.
3. When a follower detects leader death and promotes itself to leader (no DR contact), it increments its own epoch by 1. This is the sole mechanism for local epoch advancement.
4. When a restarting node connects and receives a `StatusResponse`, it compares epochs. If the peer's epoch is higher, the restarting node is stale and adopts the follower role immediately without contacting DR.
5. A heartbeat carrying an epoch lower than the receiver's own epoch indicates a stale sender; the receiver logs a warning and ignores the heartbeat.

#### Startup Election Flow

1. On startup, each node attempts TCP connection to its peer (A→B, B→A).
2. On connection, both sides immediately send `StatusQuery` (identity + epoch).
3. On receiving `StatusQuery`, each side replies with `StatusResponse` including its `current_role`.
4. **If the peer's `StatusResponse` carries `Role::leader`:** the connecting node adopts `Role::follower` immediately. No DR contact needed.
5. **If the peer's `StatusResponse` carries `Role::unknown`:** both sides are undecided. Both send `ArbitrationReport` to DR (primary-DR first, secondary-DR as fallback).
6. DR receives both reports and issues `ArbitrationDecision` assigning leader and follower deterministically by lowest `instance_id`, and sets the epoch for this generation.
7. Both nodes adopt their assigned roles and the DR connection is closed.

#### Post-Election Steady State

- The peer-to-peer TCP connection remains open with `Heartbeat` messages sent at regular intervals in both directions.
- Heartbeats carry `instance_id` and `epoch` for liveness detection and stale-node detection.
- If the **follower** dies: the leader logs a warning. No other action is taken.
- If the **leader** dies: the follower promotes itself (see Leader Death below).

#### Restart Flow

When a node restarts it connects to the peer and exchanges `StatusQuery`/`StatusResponse`. If the peer's `StatusResponse` carries `Role::leader` and a higher epoch, the restarting node adopts `Role::follower` without contacting DR.

#### Leader Death and Follower Promotion

On heartbeat loss:
1. The surviving node first attempts to reconnect to the peer.
2. If reconnection succeeds: exchange `StatusQuery`/`StatusResponse`; the epoch resolves roles as normal.
3. If reconnection fails, the peer is presumed dead:
   - **Lowest `instance_id` node:** promotes itself to leader and increments its epoch by 1.
   - **Highest `instance_id` node:** enters a degraded waiting state. Does NOT promote itself.

#### Split-Brain Protection

**Normal startup with DR reachable:** DR is the sole authority and assigns exactly one leader. Split-brain is impossible.

**One node already established:** The epoch difference immediately resolves this — the restarting node unconditionally adopts follower role.

**Network partition (both nodes alive, link down):** The lowest `instance_id` node promotes and increments its epoch; the highest `instance_id` node enters degraded waiting state and does not promote.

#### Open Design Questions

- **Epoch at DR site:** The exact interaction between DR-site epoch management and main-site epoch management has not yet been fully specified.
- **Heartbeat interval and loss threshold:** Not yet specified. These will be `ReactorConfiguration` parameters.

---

### 13. Logging Subsystem

`QuillLogger` wrapping `quill::Logger*`. `PUBSUB_LOG(logger, level, fmt, ...)` for format args; `PUBSUB_LOG_STR(logger, level, str)` for single string (required by `-Werror=variadic-macros`).

Log levels: `FwLogLevel::Alert`, `Critical`, `Error`, `Warning`, `Notice`, `Info`, `Debug`, `Trace`. Currently everything is logged at `Info`; level differentiation is a future task.

Any class that needs to log receives a `QuillLogger&` in its constructor and stores it as a member. The Reactor does not own all logging — each class logs for itself.

---

## Memory Model Summary

| Allocator | Used for | Thread-safe | Reclamation |
|---|---|---|---|
| `FixedSizeMemoryPool<T>` | Fixed-size objects | Yes (Treiber stack) | Never |
| `ExpandablePoolAllocator<T>` | Queue nodes, reactor commands | Yes | Never |
| `BumpAllocator` | DSL encode/decode scratch | No | `reset()` only |
| `ExpandableSlabAllocator` | PDU payloads (in/out) | Alloc: reactor; Dealloc: any | Demand-driven, reactor only |

---

## Outbound PDU Path (implemented, tested)

The sending node allocates a slab chunk, writes the `PduHeader` in network byte order, encodes the payload using the DSL, then enqueues a `SendPdu` reactor control command:
1. Call `reactor.outbound_slab_allocator().allocate(sizeof(PduHeader) + payload_size)`
2. Write `PduHeader` in network byte order at chunk start
3. Encode payload after header using DSL `encode()` / `encode_fast()`
4. Enqueue `ReactorControlCommand{SendPdu}` with `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_`
5. Reactor delegates to `OutboundConnectionManager::process_send_pdu_command()`
6. On partial write: handler records `current_*` state, registers `EPOLLOUT`
7. `EPOLLOUT` fires: `continue_send()` resumes; when complete `release_pending_send()` deallocates

## Inbound PDU Path (implemented, tested, zero-copy)

The receiving node's reactor accepts data via epoll and delivers it zero-copy to the application thread:
1. epoll signals `EPOLLIN` on connected socket
2. Reactor delegates to `InboundConnectionManager::on_data_ready()` → `InboundConnection::handle_read()` → `PduProtocolHandler::on_data_ready()` → `PduParser::receive()`
3. `PduParser` reads 16-byte `PduHeader` into `header_buffer_`; validates canary
4. `PduParser` allocates slab chunk: `auto [slab_id, chunk] = inbound_slab_allocator_.allocate(byte_count)`
5. `PduParser` reads payload **directly from socket into slab chunk** — zero copy
6. Dispatches `EventMessage::create_framework_pdu_message(payload, size, slab_id)` to thread queue
7. Application thread calls `on_framework_pdu_message(msg)`, processes payload
8. Application thread calls `inbound_slab_allocator_.deallocate(msg.slab_id(), msg.payload())`

---

## Session Accomplishments

### Session 3
- TcpSocket EAGAIN/EOF contract fix
- Use-after-free fix in on_data_ready / on_inbound_data_ready
- InboundConnection infrastructure completed
- DSL generator fixes (3 bugs)
- Integration test infrastructure
- StatusQueryResponseRoundTrip integration test passing
- ApplicationThread `get_reactor()` accessor added

### Session 4 (current)
- **`MirroredBuffer`** — implemented and fully tested (virtual memory double-mapping for alien protocol support)
- **`ProtocolType`** — value class discriminating `FrameworkPdu` vs `RawBytes` connections
- **Protocol handler Strategy pattern** — `ProtocolHandlerInterface` with `on_data_ready()`, `send_prebuilt()`, `has_pending_send()`, `continue_send()`, `deallocate_pending_send()`
- **`PduProtocolHandler`** — Strategy A; owns `PduParser`, `PduFramer`, and all outbound slab bookkeeping; `release_pending_send()` handles both normal completion and teardown
- **`InboundConnection` refactored** — now a thin transport shell owning `unique_ptr<ProtocolHandlerInterface>` and `last_activity_time_`; all protocol logic moved into handler
- **`Reactor` updated** — `on_accept` builds `PduProtocolHandler`, `dispatch_events` uses `handler()`, `teardown_inbound_connection` uses `deallocate_pending_send()`, `check_for_inactive_sockets` implemented using two-phase pattern
- **`InboundConnectionManager`** — extracted from Reactor; owns listener registry, accepted connection maps, all inbound logic; receives `ThreadLookupInterface&` for event delivery
- **`OutboundConnectionManager`** — extracted from Reactor; owns outbound connection maps and all outbound logic; receives `ThreadLookupInterface&` for event delivery
- **`ThreadLookupInterface`** — pure abstract interface implemented by Reactor; allows managers to deliver events without coupling to Reactor
- **`ExpandableSlabAllocator` bug fix** — use-after-free in `drain_empty_slab_queue()` fixed by two-phase collect-then-process pattern; detected by ASan
- All unit tests and integration tests passing; Valgrind clean; ASan clean

---

## What Is Done

- Allocator subsystem — complete, tested, all races fixed
- Lock-free MPSC queue — complete, tested
- Reactor event loop — complete, tested
- ApplicationThread — complete, tested
- Socket layer — complete, tested
- PDU framing (`PduFramer` two-mode, `PduParser` zero-copy) — complete, tested
- `OutboundConnection` — complete
- `InboundConnection` — complete (refactored to thin shell + strategy handler)
- `ProtocolHandlerInterface` / `PduProtocolHandler` — complete
- `MirroredBuffer` — complete, tested
- `InboundConnectionManager` — complete (ready for integration into Reactor)
- `OutboundConnectionManager` — complete (ready for integration into Reactor)
- `ThreadLookupInterface` — complete
- Reactor connection management — complete (pending final Reactor refactor)
- `ServiceRegistry` / `ServiceEndpoints` / `ConnectionID` — complete
- `EventType` / `EventMessage` — complete (includes `create_raw_socket_message`)
- `ReactorControlCommand` — complete
- `ReactorConfiguration` — complete
- DSL code generator — complete, 61 tests passing
- Leader-follower DSL messages — defined and generated
- Logging subsystem — complete

## What Is Not Yet Done (in dependency order)

1. **Reactor refactor (immediate)** — Reactor must be rewritten to inherit `ThreadLookupInterface`, remove extracted connection members, construct `InboundConnectionManager` and `OutboundConnectionManager`, and delegate all inbound/outbound operations to them. `InboundConnectionManager` and `OutboundConnectionManager` are fully written and ready.
2. **`RawBytesProtocolHandler`** — Strategy B; will own `MirroredBuffer`; requires `CommitRawBytes` tag added to `ReactorControlCommand`
3. **Leader-follower protocol** — state machine, heartbeat timers, arbitration
4. **Pub/sub fanout**
5. **`ExpandablePoolAllocatorTest.CrossPoolAbaInterleaving`** — intermittent 1-in-500 failure under TSan; the `free_next` atomic fix resolved the main race; residual failure still present
6. **Logging levels** — everything currently at Info; verbose paths should move to Debug once framework applications are running

## Immediate Next Task

Rewrite `Reactor.hpp` and `Reactor.cpp` from scratch to complete the manager extraction:
- `Reactor` inherits `ThreadLookupInterface` and provides `get_fast_path_thread()` as the override
- Constructor initialises `InboundConnectionManager` and `OutboundConnectionManager`, passing `epoll_fd_`, `config_`, `inbound_slab_allocator_`, `service_registry_` (outbound only), `*this` (as `ThreadLookupInterface&`), and `logger_`
- `next_connection_id_` stays on Reactor; `allocate_connection_id()` is called by Reactor before delegating to `on_accept(listener, id)` and `process_connect_command(command, id)`
- `dispatch_events` uses `inbound_manager_.find_by_fd()`, `outbound_manager_.find_by_fd()`, `inbound_manager_.find_listener_by_fd()`
- `process_control_commands` drains both `inbound_manager_.drain_pending_send()` and `outbound_manager_.drain_pending_send()` before queue; delegates `Connect`/`SendPdu`/`Disconnect` to appropriate manager
- `on_housekeeping_tick` calls `inbound_manager_.check_for_inactive_connections()` and `outbound_manager_.check_for_timed_out_connections()`
- `on_accept(listener)` allocates ID and calls `inbound_manager_.on_accept(listener, id)`
- `initialize_listeners()` delegates to `inbound_manager_.initialize_listeners()`
- `get_first_inbound_listener_port()` delegates to `inbound_manager_.get_first_listener_port()`
- All extracted functions removed from Reactor: `on_inbound_data_ready`, `on_inbound_write_ready`, `teardown_inbound_connection`, `check_for_inactive_sockets`, `on_data_ready(OutboundConnection&)`, `on_write_ready(OutboundConnection&)`, `teardown_connection`, `process_connect_command`, `process_send_pdu_command`, `process_disconnect_command`, `check_for_timed_out_connections`

---

## HA Architecture

- Four instances total: two at main site (primary, secondary), two at DR site (primary-DR, secondary-DR)
- Each instance has a unique integer `instance_id` configured in `ReactorConfiguration`
- Leadership is deterministic — lowest `instance_id` wins; no voting required
- DR is a pure arbiter and never becomes leader or follower
- Main-site nodes arbitrate via DR-site nodes; DR-site nodes arbitrate via main-site nodes
- Full election, epoch, split-brain protection, and failure handling documented in Section 12 above
- All endpoints configured via `ReactorConfiguration` and `ServiceRegistry`

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
