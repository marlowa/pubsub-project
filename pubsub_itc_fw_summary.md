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
   - Success → `on_connected(socket)` → create `PduFramer` + `PduParser` → re-register for `EPOLLIN` → deliver `ConnectionEstablished`
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
| `ProtocolHandlerInterface` | Pure abstract interface: `on_data_ready()`, `send_prebuilt()`, `continue_send()` all return `[[nodiscard]] tuple<bool, std::string>`; plus `has_pending_send()`, `deallocate_pending_send()`, `commit_bytes()` |
| `PduProtocolHandler` | Strategy A: owns `PduParser` + `PduFramer` + pending-send slab state; handles framework-native PDU streams |
| `RawBytesProtocolHandler` | Strategy B: owns `MirroredBuffer`; delivers raw byte streams to the application thread; see Section 7 for full design |

**`PduProtocolHandler` responsibilities:**
- Inbound: `PduParser::receive()` reads and dispatches complete PDU frames; on graceful peer close it returns `(false, "")` to the caller; on protocol error it returns `(false, error_string)`. The owning `InboundConnectionManager`/`OutboundConnectionManager` is responsible for tearing the connection down on `!ok`.
- Outbound: owns `current_allocator_`, `current_slab_id_`, `current_chunk_ptr_`, `current_total_bytes_`; `release_pending_send()` deallocates on completion or teardown. `send_prebuilt`/`continue_send` return `(false, error_string)` on unrecoverable failure (chunk released before return).
- All slab bookkeeping is internal to the handler; the Reactor and `InboundConnectionManager` never touch slab state directly for inbound connections.

**`InboundConnectionManager` maps:**
- `connections_` — `ConnectionID → unique_ptr<InboundConnection>` (owns)
- `connections_by_fd_` — `int fd → InboundConnection*` (non-owning, for epoll dispatch)
- `inbound_listeners_` — `int fd → InboundListener` (owns, keyed by listening socket fd)

**Idle timeout:** `InboundConnectionManager::check_for_inactive_connections()` uses the two-phase identify-then-process pattern. Uses `socket_maximum_inactivity_interval_` from `ReactorConfiguration`.

---

### 7. Raw Socket Communication Design

This section documents how raw byte streams (alien protocols such as ASCII FIX, NMEA, or any custom binary protocol) are handled end-to-end. This is the most complex path in the framework because unlike PDU connections, the application thread is responsible for its own message framing.

**Overview**

The reactor is the only component that performs socket I/O. When bytes arrive on a raw-bytes connection, the reactor reads them into a `MirroredBuffer` and notifies the application thread via the Vyukov queue. The application thread decodes what it can, then tells the reactor how many bytes it has consumed via a `CommitRawBytes` reactor control command. The reactor then advances the buffer tail, releasing those bytes.

**`MirroredBuffer`**

A stream-oriented ring buffer using virtual memory mirroring.

| Detail | Value |
|---|---|
| Backing | `memfd_create` + double `mmap` into adjacent virtual address ranges |
| Purpose | Provides a contiguous view of unprocessed bytes even when data wraps the ring buffer end, eliminating split-packet edge cases |
| Head | Advanced by the reactor thread only, on each `recv()` |
| Tail | Advanced by the reactor thread only, in response to `CommitRawBytes` |
| Exposed to app | `read_ptr()` — pointer to first unprocessed byte; `bytes_available()` — count of unprocessed bytes; `tail()` — current tail position |
| Backpressure | If `space_remaining() == 0` when `on_data_ready()` fires, the connection is disconnected. A rogue or slow peer that fills the buffer is disconnected; all other connections are unaffected. |

Unit tests: `MirroredBufferTest` — including `VerifiesVirtualMemoryMirroringContinuity` and `HandlesExhaustiveWrapAroundStress` (1000 iterations).

**`RawBytesProtocolHandler`**

Implements `ProtocolHandlerInterface` (Strategy B). Owns the `MirroredBuffer` and a `PduFramer` for the outbound path.

Inbound path:
1. `on_data_ready()` is called by the reactor when `EPOLLIN` fires.
2. `recv()` reads available bytes into the buffer, advancing the head.
3. An `EventMessage` of type `RawSocketCommunication` is enqueued to the target `ApplicationThread`. The message carries:
   - `connection_id` — so the app can demultiplex multiple raw connections
   - `payload()` — `read_ptr()` into the `MirroredBuffer` at enqueue time
   - `payload_size()` — `bytes_available()` at enqueue time (ALL unprocessed bytes, not just newly arrived ones)
   - `tail_position()` — the buffer's `tail_` value at enqueue time (used by the app to detect tail advances unambiguously)

Outbound path: identical to `PduProtocolHandler` — `PduFramer` handles partial sends and slab chunk lifetime.

**`EventMessage` for raw socket delivery**

`EventMessage::create_raw_socket_message(connection_id, data, size, tail_position)` — the `tail_position` parameter was added specifically to give the application thread an unambiguous way to detect when the reactor has advanced the tail between two deliveries. Without it, the app cannot reliably distinguish "more data arrived" from "tail advanced and the window shifted", because both can cause `payload_size()` to change in the same direction.

**Reactor control commands for raw bytes**

| Command | Direction | Meaning |
|---|---|---|
| `CommitRawBytes` | App thread → Reactor | "I have finished processing `bytes_consumed` bytes; advance the tail" |
| `SendRaw` | App thread → Reactor | "Send these pre-built raw bytes on connection `connection_id`" |

`CommitRawBytes` is processed by `InboundConnectionManager::process_commit_raw_bytes()`, which calls `RawBytesProtocolHandler::commit_bytes(n)`, which calls `buffer_.advance_tail(n)`.

**Application thread responsibilities**

The application thread subclass must implement `on_raw_socket_message()`. Each call receives ALL currently unprocessed bytes from the tail — not just the newly arrived bytes. The tail only advances when the reactor processes a `CommitRawBytes` command. Between two calls, if the tail has not yet advanced, `payload()` points to the same start address and `payload_size()` may be larger.

Correct application thread pattern (as used in `BurstListenerThread`):
- Track `bytes_decoded_` (bytes decoded since the last tail advance) and `last_tail_` (tail position from the last delivery).
- On each call, compare `message.tail_position()` against `last_tail_`. If different, the tail advanced — reset `bytes_decoded_` to 0.
- Decode from `data + bytes_decoded_` for `available - bytes_decoded_` bytes.
- Only call `commit_raw_bytes()` when `bytes_decoded_ == available` (entire window consumed). This ensures no partial message bytes remain after the commit — the next `EPOLLIN` will deliver them together with any new bytes. If a partial message remains uncommitted, it stays in the buffer and is delivered combined with subsequent data.
- If a rogue client sends a partial message and goes silent, the buffer fills and the framework disconnects them.

**Why `tail_position()` is needed**

Without it, the app uses `available < last_available_` to detect a tail advance. This fails when new data arrives simultaneously: the tail advances (shrinking the window) but new bytes also arrive (growing it), so `available` may increase rather than decrease. The `tail_position()` field makes the detection exact and unambiguous.

**Failure handling in `InboundConnectionManager`**

`on_data_ready()`, `on_write_ready()`, `process_send_pdu_command()`, and `process_send_raw_command()` each inspect the `tuple<bool, std::string>` returned by the handler call and call `teardown_connection(id, reason, true)` directly on `!ok`. The handler does not destroy the connection synchronously, so no re-lookup of the connection in the map is required after the call returns. (Session 14 removed the previous synchronous-disconnect-handler pattern that had been the source of a use-after-free SIGSEGV at the end of session 13.)

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

**Test status:** 133 Python roundtrip tests passing. Coverage 90%. Pylint 10/10.

**DSL types:** `i8`, `char`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>`, `optional T`, `enum : base`, named message references.

**`char` field type** — single-byte wire format, C++ type `char`. Distinct from `i8` (maps to `int8_t`). For FIX protocol char fields. Enum underlying type `char` generates C++ `char`. Character literals (e.g. `'A'`, `'1'`) accepted in enum entry values.

**`fix_equity_orders.dsl`** — FIX 5.0 SP2 equity order topic registry at `applications/fix_equity_orders.dsl`. Topics: `NewOrderSingle` (1000), `OrderCancelRequest` (1001), `ExecutionReport` (1002). Prices/quantities are `string`; `TransactTime` is `datetime_ns`; conditionally required fields are `optional`. Topic IDs start at 1000.

**generate_cpp_from_dsl.py** — takes input DSL path and output **file path** (not directory) as positional arguments, plus `--namespace` and `--topics` flags.

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

### Session 15 (current)

**End-to-end ME ER fabrication.** The matching engine no longer logs `-- stub` and stops; it now decodes inbound `NewOrderSingle` PDUs and emits a fully-filled `ExecutionReport` PDU back to the sequencer. The ER populates every field that `SequencerThread` decodes during ER-forwarding (`order_id`, `exec_id`, `exec_type=Trade`, `ord_status=Filled`, `symbol`, `side`, `leaves_qty=0`, `cum_qty=order_qty`, `avg_px=price`, `transact_time` in nanoseconds, plus optional `cl_ord_id`, `order_qty`, `last_qty`, `last_px`, `price`, `ord_type`). No real order book or matching is performed — every order is fabricated as fully filled at its requested limit price. The ME has its own `order_id_counter_` and `exec_id_counter_` producing `ME-ORD-N` and `ME-EXEC-N` strings.

The ME's `handle_new_order_single` keeps fabricated `std::string` locals on the stack and assigns their `string_view`s into the ER struct, so the views are valid for the full duration of the immediately-following `send_pdu` call. This is a safer pattern than the existing `er.foo = std::string(view.foo);` style used in `SequencerThread`'s ER decoder (which assigns a temporary `std::string` to a `string_view`, leaving the view dangling at the end of the assignment statement; works in practice today only because the calling stack frame is not yet overwritten by the time `send_pdu` reads the bytes). The SequencerThread instances of that pattern have not been changed in this session — they should be cleaned up when next convenient.

**Sequencer-to-ME topology corrected.** Until this session the sequencer was using a single TCP connection bidirectionally: the ME's outbound to the sequencer's `inbound:7021` (the ER channel) was being repurposed by the sequencer to push order PDUs back the wrong way down the same socket. The user confirmed this was unintended, and that the proper topology is two unicast pipes — one each way — until pub/sub fanout replaces direct TCP. The fix:
- `SequencerConfiguration.hpp` gained `matching_engine_host`/`matching_engine_port` members defaulting to `127.0.0.1:7020`.
- `SequencerConfigurationLoader.cpp` now parses a `[matching_engine] host=... port=...` section.
- `Sequencer.cpp` registers the `matching_engine` service in the `ServiceRegistry`.
- `SequencerThread::on_app_ready_event` calls `connect_to_service("matching_engine")` so the sequencer opens its own outbound to the ME's order listener.
- `SequencerThread`'s previously-misnamed `matching_engine_conn_id_` was renamed to `me_outbound_order_conn_id_` to make the direction explicit. The `on_connection_established` branch that captured `inbound:7021` as "matching engine ER connection" was removed; ER PDUs are still routed correctly by `service_name == "inbound:7021"` matching in `on_framework_pdu_message` without needing the ID cached.
- `sequencer.toml` gained a `[matching_engine]` section.

**Secondary sequencer expunged from the gateway.** The gateway-side dual-publish was incomplete and was breaking the single-sequencer test setup: the gateway required `[sequencer.secondary_host]`/`[sequencer.secondary_port]` in its toml and would attempt to connect to a secondary that wasn't running, retrying forever. The user chose to remove the secondary references entirely rather than carry broken half-configuration. The dual-publish concept is preserved in code (the function name `forward_pdu_to_sequencers` retained, with a comment explaining the plural will be reasserted when leader-follower lands) but the secondary endpoint, member, connect call, and toml entries are all gone:
- `sample_fix_gateway_seq.toml` — `secondary_host`/`secondary_port` lines removed from `[sequencer]`.
- `FixGatewaySeqConfiguration.hpp` — `sequencer_secondary_host`/`sequencer_secondary_port` members removed; class doxygen rephrased.
- `FixGatewaySeqConfigurationLoader.cpp` — secondary parsing/validation removed.
- `FixGatewaySeqConfigurationLoader.hpp` — doxygen example updated.
- `SampleFixGatewaySeq.cpp` — `service_registry_.add("sequencer_secondary", ...)` removed; startup log updated.
- `SampleFixGatewaySeq.hpp` — class doxygen "two outbound PDU connections" → "one outbound PDU connection".
- `FixGatewaySeqThread.hpp` — `sequencer_secondary_conn_id_` member removed; `forward_pdu_to_sequencers` template body's secondary branch removed; class doxygen updated.
- `FixGatewaySeqThread.cpp` — secondary init list entry, `connect_to_service("sequencer_secondary")`, and the corresponding branches in `on_connection_established` and `on_connection_lost` all removed; bottom-of-file comment block rewritten.
- `start_fix_seq_system.py` — the `sequencer_secondary` launch step removed; docstring updated.

**Fix `EventMessage::create_framework_pdu_message` to plumb `pdu_id` through.** Diagnosed during the runtime debugging that drove much of this session: `EventMessage::pdu_id_` defaulted to `-1` and was never set by the factory, so SequencerThread saw `pdu_id=-1` on every PDU and dropped them with "unknown order PDU id -1". Three-file fix:
- `EventMessage.hpp` — factory signature gained `int16_t pdu_id` parameter; doxygen updated.
- `EventMessage.cpp` — implementation now assigns `msg.pdu_id_ = pdu_id`.
- `PduParser.cpp::dispatch_pdu` — passes `current_pdu_id_` through to the factory.

**`InboundConnectionManager::on_accept` populates ConnectionID once.** Diagnosed in the same debugging pass: two distinct `ConnectionID` objects were in flight per inbound connection — one populated with `inbound:<port>` for the `ConnectionEstablished` event, and a bare one with empty service_name passed to `PduProtocolHandler` and stamped on every `FrameworkPdu` event by `PduParser::connection_id_`. The fix builds a single `populated_id` near the top of `on_accept` and uses it everywhere downstream (handler ctor, `InboundConnection` ctor, `connections_` map key, `teardown_connection` path, success log line, `ConnectionEstablished` event).

**Trace logging in PduParser.** Two `Info`-level traces fire per header decode: one logging the decoded fields (canary, byte_count, pdu_id, version), one dumping the raw 16 header bytes in hex. Required re-adding a `QuillLogger&` member to `PduParser` and re-plumbing it through `PduProtocolHandler` (logger parameter forwarded to PduParser construction) and `OutboundConnection` (logger member, forwarded similarly). Plus updates to `InboundConnectionManager::on_accept` and `OutboundConnectionManager` construction sites, and three test files (`PduFramerParserTest.cpp` — seven constructor calls; `PduProtocolHandlerTest.cpp` — one; `OutboundConnectionTest.cpp` — two).

**Files changed (twenty):**
- `applications/sequencer/SequencerConfiguration.hpp`
- `applications/sequencer/SequencerConfigurationLoader.cpp`
- `applications/sequencer/Sequencer.cpp`
- `applications/sequencer/SequencerThread.hpp`
- `applications/sequencer/SequencerThread.cpp`
- `applications/sequencer/sequencer.toml`
- `applications/matching_engine/MatchingEngineThread.hpp`
- `applications/matching_engine/MatchingEngineThread.cpp`
- `applications/sample_fix_gateway_seq/sample_fix_gateway_seq.toml`
- `applications/sample_fix_gateway_seq/FixGatewaySeqConfiguration.hpp`
- `applications/sample_fix_gateway_seq/FixGatewaySeqConfigurationLoader.cpp`
- `applications/sample_fix_gateway_seq/FixGatewaySeqConfigurationLoader.hpp`
- `applications/sample_fix_gateway_seq/SampleFixGatewaySeq.cpp`
- `applications/sample_fix_gateway_seq/SampleFixGatewaySeq.hpp`
- `applications/sample_fix_gateway_seq/FixGatewaySeqThread.hpp`
- `applications/sample_fix_gateway_seq/FixGatewaySeqThread.cpp`
- `scripts/start_fix_seq_system.py`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/EventMessage.hpp`
- `libraries/pubsub_itc_fw/src/EventMessage.cpp`
- `libraries/pubsub_itc_fw/src/PduParser.cpp` and the broader plumbing chain noted above

(plus the test files updated for the PduParser logger constructor change).

**Build and runtime status at session end:**
- All four applications build clean. Library tests pass (the `QuillLoggerTest.LogsAlertMessage` failure noted at session-13 end was fixed by the user during session 14).
- End-to-end pipeline verified: fix8 → gateway → sequencer (order on `inbound:7001`, `pdu_id=1000`) → ME (order received on connection 2, NewOrderSingle decoded, `ME-ORD-1`/`ME-EXEC-1` fabricated) → sequencer (ER on `inbound:7021`, `pdu_id=1002`, `byte_count=143`) → gateway → fix8. The fix8 client receives the ExecutionReport reply as expected. **Three sessions converged on this milestone; the comms framework is verified end-to-end with a real FIX client.**

**Session 15 continuation -- payload hex dump trace, dangling-string-view diagnosis and fix.**

A binary-garbage symptom appeared at first verification: the ME log showed `ClOrdID=\x0F\x00\x00\x00`, `Symbol=\x0F\x00\x00`, `OrderQty=\x0F\x00\x00\x00\x00\x00`. Lengths in the decoder were correct (4, 3, 6) but the field contents were SSO-bookkeeping bytes (`0x0F` is the libstdc++ short-string capacity marker). The wire-level forwarding was faithful; the corruption was happening *inside* one of the apps before it sent.

Diagnosis path:
1. Read the gateway log -- `ClOrdID=ord1` was correctly parsed from the FIX message at the gateway side.
2. Read the DSL `encode(NewOrderSingle, ...)` source -- length-prefixed-string format, encoder symmetric with decoder. So if both runs of the DSL on the same struct produce identical bytes, encode/decode is sound.
3. Added a payload hex dump trace to `PduParser::dispatch_pdu` (96 bytes, `Info` level, mirroring the existing header trace). This dumps the wire payload of every PDU at every hop.
4. Re-ran. The hex dump showed:
   - gateway → sequencer NOS: clean (`04 00 00 00 6f 72 64 31 ...` -- "ord1" followed by "BHP", "8517.0", "61.17677")
   - sequencer → ME NOS: corrupted (`04 00 00 00 0f 00 00 00 ...` -- length 4, then four bytes of SSO leakage)
5. Conclusion: the SequencerThread re-encode block was the bug. It used the `er.cl_ord_id = std::string(view.cl_ord_id);` pattern -- assigning a temporary `std::string` to a `string_view` field. The temporary dies at the semicolon, leaving the view pointing at stack memory that the next round of temporaries overwrites with their own SSO buffers (whose first byte is `0x0F`, the libstdc++ inline-capacity sentinel). Same UB the project summary had flagged as item 9 in "What Is Not Yet Done" -- but it was not "works in practice today"; it was *visible* corruption of every forwarded order PDU.

**Fix:** `view.X` fields point into the slab payload owned by the inbound `EventMessage`, which is alive until `release_pdu_payload(message)` is called *after* `send_pdu` returns. So the right pattern is to assign `view.X` directly to the outbound struct's `string_view` field -- no temporary, no copy, no UB. Three blocks fixed in `SequencerThread.cpp`:

1. NOS re-encode (forwarding gateway → ME order PDUs): direct view-assignment; also added propagation of all NOS optional fields (`has_stop_px`, `has_account`, `has_ex_destination`, `has_exec_inst`, `has_min_qty`, `has_max_floor`, `has_expire_time`, `has_text`) which the previous code dropped.
2. OCR re-encode: direct view-assignment; added `has_account`/`has_text` propagation.
3. ER re-encode (forwarding ME → gateway): direct view-assignment; **added `er.order_id = view.order_id;`** which the previous code was missing entirely (a separate latent bug that would have caused fix8 to receive an ER with empty OrderID); added every optional `has_*` flag and value alongside the required fields. The complete-propagation policy is now the rule for re-encode/forward blocks.

Re-run after the fix showed clean wire bytes at every hop. Item 9 in "What Is Not Yet Done" is now resolved; item 3 (the binary-garbage finding from the original Session 15 verification) went away with it.

**Session 15 continuation -- gateway ER routing implementation.**

With the ER bytes now arriving at the gateway intact, `FixGatewaySeqThread::on_framework_pdu_message` -- previously a TODO no-op that silently dropped slabs -- was implemented. Body:
- Validates `pdu_id == ExecutionReport`; drops with warning + slab release otherwise.
- Decodes ER via `BumpAllocator` over the inherited `decode_arena_buffer()`.
- Drops if `view.has_cl_ord_id` is false or empty (no routing key).
- Looks up `cl_ord_id` (named `std::string` local for safe map lookup) in `cl_ord_id_to_session_`. Drops with warning if absent (originating session may have disconnected).
- Looks up the `FixSession` by `ConnectionID`. If gone, erases the stale map entry and drops.
- Builds a FIX `ExecutionReport` populating `OrderID`, `ExecID`, `ExecType`, `OrdStatus`, `Symbol`, `Side`, `CumQty`, `LeavesQty`, `ClOrdID`, plus optionals `OrderQty`, `Price`, `OrdType` when their `has_*` flags are set. Single-character DSL enum fields convert via `static_cast<char>(view.X)` (the enums use FIX char values as their underlying representation).
- Sends via the existing `send_fix_to_session` helper.
- On terminal `OrdStatus` (Filled, Canceled, Rejected, Expired, DoneForDay, Replaced), erases the map entry. Non-terminal statuses (PartiallyFilled, PendingCancel, etc.) leave the entry alive for follow-up fills.
- `release_pdu_payload(message)` on every exit path.

A separate `cl_ord_id_to_session_` cleanup was added to `on_connection_lost`: when a FIX session disconnects, all map entries pointing at its `ConnectionID` are swept. Without this the map would accumulate stale entries indefinitely.

The gateway's `FixSerialiser` already supported every tag the ER routing populates (no serialiser changes needed).

**Files changed in the continuation work (eight, beyond the twenty earlier):**
- `libraries/pubsub_itc_fw/src/PduParser.cpp` (payload hex dump trace)
- `applications/sequencer/SequencerThread.cpp` (three re-encode blocks fixed; complete optional-field propagation)
- `applications/sample_fix_gateway_seq/FixGatewaySeqThread.cpp` (`on_framework_pdu_message` implemented; `on_connection_lost` map sweep added; BumpAllocator include added)

(The summary's earlier "Files changed (twenty)" list at the top of the Session 15 entry covers the topology and ME-ER-fabrication work; the three above are the ones that closed the loop.)

### Session 14

**Use-after-free in `PduParser` and `PduProtocolHandler` fixed (option 2).** The disconnect_handler hook diagnosed at session 13 was removed entirely. `PduParser`, `PduProtocolHandler`, and `RawBytesProtocolHandler` no longer hold or invoke any disconnect-handler callback; failure now propagates up to the owning manager via the return value of the failure-capable methods. The owning manager is then responsible for tearing down the connection. This matches the framework's broader principle that connection lifecycle is owned by the Reactor and its managers, not by the parsers and handlers.

**Contract change on `ProtocolHandlerInterface`.** The three failure-capable virtuals now return `[[nodiscard]] std::tuple<bool, std::string>` instead of `void`:
- `on_data_ready()` — `{true, ""}` on a clean read (including no bytes available); `{false, ""}` on graceful peer disconnect; `{false, error_string}` on protocol failure
- `send_prebuilt(...)` — `{true, ""}` on progress or completion; `{false, error_string}` on unrecoverable send failure (slab chunk released before return)
- `continue_send()` — same convention as `send_prebuilt`

`has_pending_send`, `deallocate_pending_send`, and `commit_bytes` are unchanged.

**Logger removed from `PduProtocolHandler` and `RawBytesProtocolHandler`.** With the disconnect-handler gone, the handlers no longer log anything; all error strings flow up to the manager which logs them. The `QuillLogger&` constructor parameter and member were dropped from both classes. Manager-side logging covers all failure paths now: graceful peer disconnect logs at `Info`, protocol/send errors log at `Error` or `Warning` per existing conventions.

**`InboundConnection::handle_read()`** now returns `std::tuple<bool, std::string>` rather than void, propagating the handler's return up to the manager.

**`OutboundConnection::on_connected()`** no longer takes a `disconnect_handler` parameter. The `PduParser` it constructs internally is built without one too.

**Manager updates:**
- `InboundConnectionManager::on_accept` no longer constructs a disconnect_handler lambda; both handler constructors lose the disconnect_handler and logger arguments.
- `InboundConnectionManager::on_data_ready` inspects the tuple from `handle_read()` and calls `teardown_connection` on `!ok`. Reason string is `"peer 'X' closed connection"` for graceful (logged at `Info`) or `"protocol error on connection from 'X': <error>"` for failure (logged at `Error`).
- `InboundConnectionManager::on_write_ready`, `process_send_pdu_command`, and `process_send_raw_command` all inspect the tuple from `continue_send`/`send_prebuilt` and tear down on failure. The previous "may invoke disconnect handler synchronously, re-look up by cid" guard pattern is gone — the call no longer mutates the connections map, so the post-call lookup is unnecessary.
- `OutboundConnectionManager::on_connect_ready` no longer constructs a disconnect_handler lambda; `conn.on_connected(std::move(socket))` takes only the socket. The existing `on_data_ready` and `on_write_ready` paths in the outbound manager already inspected return values and called `teardown_connection`, so they did not need to change.

**Test updates:**
- `PduFramerParserTest.cpp` — seven `PduParser` constructor calls dropped their fourth (disconnect_handler) argument. Two tests had a `disconnected` bool wired through a capturing lambda; flag and lambda removed. `ParseDetectsPeerDisconnect` now relies on the existing `EXPECT_FALSE(ok) && EXPECT_TRUE(error.empty())` assertion alone, which is exactly the new contract.
- `PduProtocolHandlerTest.cpp` — `disconnect_called_` member, lambda, and the disconnect_handler/logger arguments to the `PduProtocolHandler` constructor were removed. All six `send_prebuilt` and `continue_send` call sites now consume the `[[nodiscard]]` tuple return and assert `ASSERT_TRUE(ok) << error`.
- `OutboundConnectionTest.cpp` — three `on_connected` call sites at lines 420, 431, 437 dropped their trailing empty-lambda argument (`OnConnectedRejectsNullSocket` and `OnConnectedRejectsWhenNotConnecting`).

**Files changed (sixteen, in the disconnect_handler removal):**
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ProtocolHandlerInterface.hpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduParser.hpp`
- `libraries/pubsub_itc_fw/src/PduParser.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduProtocolHandler.hpp`
- `libraries/pubsub_itc_fw/src/PduProtocolHandler.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/RawBytesProtocolHandler.hpp`
- `libraries/pubsub_itc_fw/src/RawBytesProtocolHandler.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/InboundConnection.hpp`
- `libraries/pubsub_itc_fw/src/InboundConnection.cpp`
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/OutboundConnection.hpp`
- `libraries/pubsub_itc_fw/src/OutboundConnection.cpp`
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp`
- `libraries/pubsub_itc_fw/tests/PduFramerParserTest.cpp`
- `libraries/pubsub_itc_fw/tests/PduProtocolHandlerTest.cpp`
- `libraries/pubsub_itc_fw/tests/OutboundConnectionTest.cpp`

**Runtime verification of the disconnect_handler fix.** After the library landed clean and all unit tests passed, the system was started and exercised with fix8. The original session-13 SIGSEGV repro (idle-timeout of all four connections simultaneously) ran without crash; the new manager-side teardown paths fired correctly with `Info`-level logs for graceful peer close and no use-after-free. The session-13 SIGSEGV is resolved end-to-end.

**Two cascading runtime regressions discovered and fixed during verification.** End-to-end testing revealed the order-PDU pipeline from gateway to sequencer was failing in a way that turned out to be two long-latent bugs neither caused by the disconnect_handler removal. They are recorded together because they were diagnosed and fixed in the same debugging session.

**Regression 1: empty inbound `service_name`.** `SequencerThread::on_framework_pdu_message` was logging `PDU on unexpected connection 3 () -- dropping`. The connection-established event for connection 3 also showed empty parentheses for service_name. Diagnosis took two trace points: one logging `id.service_name()` at the top of `InboundConnectionManager::on_accept` (showed empty), one logging `msg.connection_id().service_name()` in `SequencerThread::on_framework_pdu_message` (showed empty). The trace confirmed that two different `ConnectionID` objects were in flight for the same connection: the populated one used for the `ConnectionEstablished` event, and the bare one passed to `PduProtocolHandler` and stored as `PduParser::connection_id_`, which then stamps every `FrameworkPdu` event. The fix was to construct a single `populated_id` near the top of `on_accept` and use that everywhere downstream — the two handler constructors, the `InboundConnection` constructor, the `connections_` map key, the `teardown_connection` call, the success log line, and the `ConnectionEstablished` event. Fix landed in `InboundConnectionManager.cpp`.

**Regression 2: `pdu_id` never propagated through `EventMessage`.** With service_name fixed, SequencerThread reached its order-PDU branch but logged `pdu_id=-1`. PduParser was decoding `pdu_id=1000` correctly from the wire (verified by the hex-dump trace added during this session — see below), but the value was never reaching the `EventMessage`. Root cause: `EventMessage::create_framework_pdu_message` had no `pdu_id` parameter. The `EventMessage::pdu_id_` member existed and defaulted to `-1`, the accessor existed and returned it, but the factory never set it. Three-file fix: added `int16_t pdu_id` to the factory's parameter list and assigned it to `msg.pdu_id_` in the implementation; `PduParser::dispatch_pdu` now passes `current_pdu_id_` through. Fix landed in `EventMessage.hpp`, `EventMessage.cpp`, and `PduParser.cpp`.

Both regressions were latent — the project hadn't previously exercised an inbound PDU with subsequent dispatch logic that examined the `service_name` *and* `pdu_id` fields together. Session 13's `inbound:<port>` work documented but only partially landed the service_name plumbing; nobody had noticed `EventMessage::pdu_id` was always `-1` because no consumer had needed it until SequencerThread.

**Trace logging in PduParser.** A `QuillLogger&` parameter and member were re-added to `PduParser` (and re-plumbed through `PduProtocolHandler` and `OutboundConnection` which both construct PduParsers). Two `Info`-level trace lines fire on every header decode in `PduParser::receive`: one logging the decoded fields (canary, byte_count, pdu_id, version), one dumping the raw 16 header bytes in hex. The traces are left in for now; they are valuable for diagnosing future framing issues. Move to `Debug` or wrap in a compile-time switch when the system runs in earnest. Files touched for this: `PduParser.hpp`/`.cpp` (logger member), `PduProtocolHandler.hpp`/`.cpp` (logger parameter forwarded to PduParser), `OutboundConnection.hpp`/`.cpp` (logger member), `InboundConnectionManager.cpp` and `OutboundConnectionManager.cpp` (pass `logger_` at construction), `PduFramerParserTest.cpp` (seven constructor calls updated), `PduProtocolHandlerTest.cpp` (one constructor call), `OutboundConnectionTest.cpp` (two constructor calls).

**Two short trace lines in `SequencerThread::on_framework_pdu_message` and `InboundConnectionManager::on_accept`** were added during diagnosis and remain in the code. Cosmetic to remove later; harmless for now.

**End-to-end pipeline verified.** Final run shows the complete order flow:
- fix8 → gateway: raw FIX bytes via `RawBytesProtocolHandler` on port 9879
- gateway → sequencer (primary and secondary): `NewOrderSingle` PDU encoded, sent on ports 7001/7002
- sequencer recognises connection by `service_name = "inbound:7001"`, reads `pdu_id=1000`, decodes the `NewOrderSingle` view, re-encodes into the owning struct, forwards to ME via `send_pdu`
- ME receives the sequenced PDU on its inbound listener (port 7020) and logs `MatchingEngineThread: sequenced PDU received on connection 1 -- stub`

The ME stub does not yet act on the order; that is the next piece of application work.

**Files changed in the verification work (additional five):**
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/EventMessage.hpp` (factory signature + namespace-comment typo fix)
- `libraries/pubsub_itc_fw/src/EventMessage.cpp` (factory implementation)
- `libraries/pubsub_itc_fw/src/PduParser.cpp` (pass `current_pdu_id_` to factory; trace logs)
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduParser.hpp` (logger member re-added)
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduProtocolHandler.hpp` (logger parameter re-added)
- `libraries/pubsub_itc_fw/src/PduProtocolHandler.cpp` (logger forwarded to PduParser)
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/OutboundConnection.hpp` (logger member added)
- `libraries/pubsub_itc_fw/src/OutboundConnection.cpp` (logger forwarded to PduParser)
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp` (populated_id, logger argument)
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp` (logger argument)
- `applications/sequencer/SequencerThread.cpp` (trace log)

**Build and test status at session end:**
- Library builds clean.
- All `pubsub_itc_fw` unit tests pass. The `QuillLoggerTest.LogsAlertMessage` failure noted earlier in the session was an unrelated regression introduced by separate logger work; the user fixed it during this session.
- All four applications (gateway, sequencer primary, sequencer secondary, matching engine) start cleanly, accept connections cleanly, and pass an order PDU end to end.
- The session-13 SIGSEGV repro runs without crash.

### Session 13

**Sequencer-primary retry-loop diagnosed and resolved** — at session start the sequencer primary appeared to be looping in startup. Diagnosis from the logs: the framework was operating correctly; the symptom was the perpetual 2.5-second retry of `sequencer_peer` against `127.0.0.1:7003`, a port no application binds. Root cause was asymmetric peer addressing in the two sequencer toml files: primary pointed at `:7003` (unbound) and secondary pointed at primary's order listener `:7001`, which the secondary mistakenly accepted as its peer link. Both addresses were design-incorrect for the leader-follower protocol described in section 12.

**Scope-A decision: defer the peer connection entirely until the leader-follower protocol is implemented.** The peer connection is part of the StatusQuery / StatusResponse / Heartbeat protocol, which is DSL-defined but not yet implemented. Issuing a `Connect` for the peer therefore produces only retry-loop noise. This was chosen over fixing the port assignments (which would have been speculative) and over removing the secondary sequencer entirely (scope B/C, larger blast radius). The HA topology in the project — gateway dual-publishes to both sequencers — is unchanged. Both sequencers still run; both still receive every order PDU. The sequencer stub still "behaves as unconditional leader" per `SequencerThread::on_framework_pdu_message`, which means both sequencers currently forward to the ME; this is a known stub limitation, separate from the peer-link work.

Files changed for scope A (eight files):
- `applications/sequencer/Sequencer.cpp` — removed `service_registry_.add("sequencer_peer", ...)`; startup log line no longer prints peer host/port and now mentions the deferral explicitly
- `applications/sequencer/SequencerThread.cpp` — removed `connect_to_service("sequencer_peer")` from `on_app_ready_event`; removed `peer_conn_id_` from ctor init list, from the `on_connection_established` branches, and from `on_connection_lost`; added a comment block at the connect site explaining when to re-enable
- `applications/sequencer/SequencerThread.hpp` — removed `peer_conn_id_` member; comment on `arbiter_conn_id_` updated
- `applications/sequencer/SequencerConfiguration.hpp` — removed `peer_host` and `peer_port` member fields
- `applications/sequencer/SequencerConfigurationLoader.cpp` — removed the two `get_required_except` calls plus local int, validation, and assignment
- `applications/sequencer/SequencerConfigurationLoader.hpp` — removed `peer_host` and `peer_port` from the example TOML in the doc comment
- `etc/sequencer/sequencer.toml` — removed `peer_host` and `peer_port` keys
- `etc/sequencer/sequencer_secondary.toml` — same

**Inbound connections now carry `"inbound:<port>"` in service_name** — the convention described at session 11 was documented but had never landed. `InboundConnectionManager::on_accept` was constructing the `ConnectionEstablished` event with the bare ConnectionID, leaving service_name empty. The patch now constructs a fresh ConnectionID at the event-enqueue site with `fmt::format("inbound:{}", listener.configuration.address.port)` for the service_name, matching the outbound convention at OutboundConnectionManager.cpp:252-253. `ConnectionID.hpp` doc comments (both the class-level Service-name section and the `service_name()` method comment) were updated to describe the new convention; the previous comment claimed inbound service_name was always empty, which was both wrong and would have misled future readers. Audit of all uploaded files using `service_name()` confirmed no caller relied on empty-detection: `FixGatewaySeqThread`, `MatchingEngineThread`, `ArbiterThread`, and the four `BurstListenerThread`/test thread classes all use either explicit `inbound:<port>` checks or fall-through `else` branches. Two files changed:
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp`
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ConnectionID.hpp`

**Patch application accidents.** Twice during this session, supplied patched files were misrouted on application: `InboundConnectionManager.cpp` content was placed under `InboundConnectionManager.hpp` (clobbering the real header), causing a recursive include because the .cpp self-includes its own header at line 19; and on a separate occasion `SequencerThread.hpp` content was placed under `Sequencer.hpp`, causing `Sequencer.cpp` to fail to compile with "Sequencer does not name a type". Both were recovered by restoring from version control. Lesson recorded for future sessions: when a header file is observed to lack `#pragma once` or to contain unexpected `class` declarations, the file has likely been overwritten by a same-basename .cpp or sibling .hpp. The .hpp/.cpp distinction must not be elided when copying.

**Reactor SIGSEGV diagnosed: use-after-free in disconnect-handler invocation pattern.** After scope-A and the service_name patches were applied and the system started up cleanly, the gateway crashed (SIGSEGV, exit -11) approximately two minutes after fix8 connected, when both inbound FIX connections and both outbound sequencer connections idle-timed-out simultaneously. Stack trace:

```
#0 PduParser::receive() at PduParser.cpp:44
#1 OutboundConnectionManager::on_data_ready (unique_ptr<>::operator->)
#2 Reactor::dispatch_events at Reactor.cpp:957
```

Diagnosis: `PduParser::receive()` invokes its stored `disconnect_handler_` from inside the method when `recv` returns 0 (graceful peer close). The disconnect_handler is a lambda that calls `OutboundConnectionManager::teardown_connection`, which destroys the `OutboundConnection`. The `OutboundConnection` owns `socket_`, `framer_`, and `parser_` as `unique_ptr` members, so destroying the connection destroys the parser whose method is currently executing. When `disconnect_handler_()` returns, control returns to `receive()` running on a destroyed `*this`. The same bug exists at the payload-phase recv-zero path inside `receive()`, and the same pattern is repeated three times in `PduProtocolHandler` (`on_data_ready`, `send_prebuilt`, `continue_send` — each calls `disconnect_handler_()` directly and returns, with `*this` having been destroyed mid-call).

The crash was *not* introduced by any session-13 patch; the bug has been latent since `PduParser` and `PduProtocolHandler` were written. It surfaced now because session-13 testing was the first run with simultaneous bidirectional idle-timeout fire (both inbound FIX and both outbound sequencer fds dropped in the same epoll batch). In normal operation with traffic flowing the recv-zero path is rarely taken before any other path returns successfully.

### Session 12

**`PduParser` now carries `ConnectionID`** — `PduParser` gains a `ConnectionID connection_id_` member set at construction. `create_framework_pdu_message` gains a `ConnectionID` parameter. `dispatch_pdu` passes the connection ID into the event message. Every `FrameworkPdu` event now correctly identifies its source connection. `PduProtocolHandler` gains the same `ConnectionID` parameter and passes it to `PduParser`. `InboundConnectionManager` passes `id` to `PduProtocolHandler`. `OutboundConnection::on_connected` passes `id_` to `PduParser`. Test files `PduFramerParserTest.cpp` and `PduProtocolHandlerTest.cpp` updated to pass `ConnectionID{}` at all construction sites.

**`ApplicationThread::release_pdu_payload` implemented** — declared in `ApplicationThread.hpp`, defined in `ApplicationThread.cpp` (to avoid incomplete type error from forward-declared `Reactor`). Calls `get_reactor().inbound_slab_allocator().deallocate(message.slab_id(), const_cast<uint8_t*>(message.payload()))`. Satisfies the contract referenced in `PduParser.hpp` and `EventMessage.hpp` comments.

**DSL generator: `enum` → `enum class`** — `_emit_enum` in `generator_cpp.py` changed from `enum {name}` to `enum class {name}`. Fixes name collision where `New = 48` appeared in both `OrdStatus` and other enums within the same namespace. Test files updated: `test_generator_cpp.py`, `test_char_enum.py`, `test_topics.py`.

**Gateway PDU encoding implemented** — `FixGatewaySeqThread::handle_new_order_single` and `handle_order_cancel_request` now build `pubsub_itc_fw_app::NewOrderSingle` / `OrderCancelRequest` structs and call `forward_pdu_to_sequencers<MsgT>()` template. `FixMessage.hpp` gets `Tag::TimeInForce = 59`. `Tag::OrdType`, `Tag::OrderQty`, `Tag::Price` already present.

**Connection retry implemented** — `OutboundConnectionManager` now stashes failed connects in `pending_retries_` map with a retry-due timestamp. `retry_failed_connections()` called from `Reactor::on_housekeeping_tick()` with a lambda for ID allocation. Fixed use-after-free where `conn.service_name()` was accessed after `teardown_connection` destroyed conn — now saves service name and thread ID before teardown. `ReactorConfiguration::connect_retry_interval_` added (default 2s). Documented as temporary TCP rendezvous workaround pending WAL-based brokerless pub/sub.

**`SequencerThread::on_framework_pdu_message` implemented** — decodes order PDUs (pdu_id 1000/1001) arriving on `inbound:7001`/`inbound:7002`, increments `next_sequence_number_`, re-encodes and forwards to ME via `send_pdu`. Decodes ER PDUs (pdu_id 1002) arriving on `inbound:7021`/`inbound:7022`, re-encodes and forwards to gateway via `send_pdu`. All paths call `release_pdu_payload`. Uses `decode_arena_buffer()` for `BumpAllocator`. `matching_engine_conn_id_` member added — stores inbound ME connection when ME connects on port 7021/7022.

**Verified end-to-end:** fix8 sends 50 NewOrderSingle → gateway encodes as PDUs → sequencer primary receives PDUs on correct connection ID → forwards to ME (stub). Connection ID correctly shown in logs (`PDU received on connection 5`).

**Known issue at session end:** Startup not working correctly after latest changes — sequencer primary log missing from final run. Investigation interrupted by usage limit. Resume by sharing `sequencer_primary.log`.

### Session 11

**Inbound connection identification via listener port** — `InboundConnectionManager::on_accept` now delivers `ConnectionEstablished` events with `ConnectionID{value, "inbound:<port>"}` so `on_connection_established()` can identify which inbound listener accepted a connection. Example: gateway sees `"inbound:9879"` for FIX clients and `"inbound:7010"` for sequencer ER connections. *(Note: session 13 discovered this had been described in the design but the code change had never actually landed in `on_accept` — service_name was being left empty. The implementation was completed in session 13.)*

**`FixGatewaySeqThread` connection identification completed** — `on_connection_established` now has four branches: `sequencer_primary`, `sequencer_secondary`, `inbound:7010` (sequencer ER), and FIX client (else). `on_connection_lost` updated to match.

**`SequencerThread` `on_connection_lost` fixed** — `peer_conn_id_` and `arbiter_conn_id_` added as members. Stored in `on_connection_established`. `on_connection_lost` compares against all three stored IDs. *(Note: `peer_conn_id_` was removed in session 13 along with the entire peer Connect — see session 13 entry.)*

**Startup order fixed in `start_fix_seq_system.py`** — gateway starts second (after arbiter, before sequencers). Docstring explains the counterintuitive ordering.

**FIX session verified end-to-end with fix8** — gateway accepts connection on port 9879, Logon established, 50 NOS correctly parsed.

### Session 10

**`ConnectionID` extended to carry service name** — now its own class rather than a `WrappedInteger` typedef. Adds `service_name_` string member and `service_name()` accessor. `OutboundConnectionManager::on_connect_ready` delivers `ConnectionEstablished` with `ConnectionID{value, service_name}`. `constexpr` removed from all methods since `std::string` is not a literal type in C++17.

**Connection identification fixed in all three thread classes** — `FixGatewaySeqThread`, `SequencerThread`, `MatchingEngineThread` all use `service_name()` in `on_connection_established`.

### Session 9

**Logging infrastructure overhaul** — proper startup sequence across all four applications. New framework additions: `FileSystemUtils` (`make_directories` via POSIX `mkdir`), `FwLogLevel::from_string`, `QuillLogger::ensure_log_file_writable`, `QuillLogger::set_syslog_level`. All four configs gain required `[logging]` section. Applications now take `<logfile> <config.toml>` as arguments.

**FIX parsing in `sample_fix_gateway_seq`** — `FixParser`, `FixSerialiser`, `FixMessage`, `FixSession` added. Logon handling, heartbeats, preamble checking all working.

### Session 8

**`InboundConnectionManager`** — multi-connection support added (one-connection restriction removed). `on_accept` delivers `ConnectionEstablished` events. `check_for_inactive_connections` implemented.

**`OutboundConnectionManager`** — `check_for_timed_out_connections` implemented. `process_send_pdu_command`, `process_send_raw_command`, `process_disconnect_command` implemented.

**`ReactorConfiguration`** — `connect_retry_interval_` added (later used for retry). `connect_timeout` present.

**Application stubs** — `sample_fix_gateway_seq`, `sequencer`, `matching_engine`, `arbiter` — all compiling with correct Aeron topology and startup pattern.

### Session 7

**DSL `char` field type** — added throughout: lexer, parser, validator, generator_cpp, generator_pybind11. Four pybind11 test failures fixed. `fix_equity_orders.dsl` created. Application architecture designed (Aeron sequencer pattern). Four application stubs written and compiling.

### Session 6

**`RawBytesProtocolHandler` bugs fixed** — intermittent `BurstDelivery` test failure resolved. `EventMessage::create_raw_socket_message` carries `tail_position`. Design documentation written. `sample_fix_gateway` tested with fix8. All 411 tests passing.

### Session 5

**Logging subsystem rewrite** — `QuillLogger` redesigned. `FwLogLevel` values flipped. `PUBSUB_LOG` and `PUBSUB_LOG_STR` are the only two call-site macros. All 12 test files migrated.

### Session 4

**`MirroredBuffer`**, **`ProtocolType`**, **`PduProtocolHandler`**, **`InboundConnectionManager`**, **`OutboundConnectionManager`**, **`ThreadLookupInterface`** — all implemented. `ExpandableSlabAllocator` use-after-free fixed.

### Session 3

TcpSocket EAGAIN/EOF fix, use-after-free fix, InboundConnection infrastructure, DSL generator fixes, integration test infrastructure, `ApplicationThread::get_reactor()` added.

---

## What Is Done

- Allocator subsystem — complete, tested, all races fixed
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
- DSL code generator — complete; `enum class` fix; `char` type; 133 tests passing
- `fix_equity_orders.dsl` — FIX 5.0 SP2 equity order topic registry
- Logging subsystem — complete
- `RawBytesProtocolHandler` — complete; `on_data_ready`/`send_prebuilt`/`continue_send` return `tuple<bool, std::string>`; no disconnect-handler member; no logger member (session 14)
- `sample_fix_gateway_seq` — FIX session layer complete; PDU encoding to sequencer complete; connection identification complete. Single sequencer outbound (`sequencer_primary`); secondary endpoint and dual-publish removed in session 15 pending leader-follower protocol. `forward_pdu_to_sequencers` template kept as the single-target publisher (the plural in the name will be reasserted when dual-publish returns). **ER routing back to fix8 complete**: `on_framework_pdu_message` decodes inbound ExecutionReport PDUs, looks up `cl_ord_id_to_session_` for the originating FIX session, builds a FIX ER and sends via `send_fix_to_session`. Map entries are erased on terminal `OrdStatus` and on FIX session disconnect (sweep in `on_connection_lost`). End-to-end fix8 → ER → fix8 round trip verified at session-15 end.
- `sequencer` — complete for the order-flow round trip. `on_framework_pdu_message` decodes inbound order PDUs from `inbound:7001` and forwards them to the matching engine via the new outbound `me_outbound_order_conn_id_`; ER PDUs from `inbound:7021` are decoded and forwarded to the gateway via `gateway_conn_id_`. Topology corrected in session 15: `[matching_engine]` section added to config and toml; `Sequencer.cpp` registers the service; `connect_to_service("matching_engine")` opens the dedicated outbound to ME's order listener instead of misusing the inbound ER channel bidirectionally. All re-encode blocks (NOS, OCR, ER) use direct `view.X` assignment to outbound `string_view` fields (the slab payload outlives `send_pdu`); the previous `std::string(view.X)` pattern caused SSO-bookkeeping-byte corruption and was fixed in session 15. All `has_*` optional flags propagated alongside their values for every forwarded PDU type. Peer connection still deferred — `peer_host`/`peer_port` removed in session 13, will return with leader-follower.
- `matching_engine` — complete for the round-trip stub. `on_framework_pdu_message` decodes inbound `NewOrderSingle` PDUs (session 15) and emits a fully-filled `ExecutionReport` over the existing outbound `sequencer_er_conn_id_`. The ER populates every field that `SequencerThread`'s ER decoder reads. No real order book or matching — every order becomes a single fill at its limit price (or a zero sentinel for market orders). `OrderID` and `ExecID` are generated as `ME-ORD-N` / `ME-EXEC-N`. `OrderCancelRequest` is not yet handled (logs and drops at the `else` branch); cancel handling is a small follow-up.
- `arbiter` — stub, compiling
- `start_fix_seq_system.py` — runs primary only (secondary launch removed in session 15 pending leader-follower)

## What Is Not Yet Done (in dependency order)

1. **Re-verify fix8 wrong-port issue is gone** — session 13 logs showed fix8 reaching the gateway's ER inbound listener (port 7010) rather than the FIX listener (port 9879). Sessions 14 and 15 did not reproduce this; fix8 connected cleanly to 9879 throughout. Worth one explicit check (`f8test -d -c myfix_gateway_client.xml -N GW1`) before retiring the item.
2. **Matching engine — `OrderCancelRequest` handling** — the ME currently logs and drops cancels at the `else` branch. Mirror the NewOrderSingle path: decode, fabricate a `Canceled` ER, send. Small follow-up; will exercise the same plumbing again with a different message type.
3. **Arbiter stub → real** — `ArbitrationReport` → `ArbitrationDecision`
4. **Leader-follower protocol** — state machine, heartbeat timers, arbitration. When this lands, restore the secondary sequencer config/code (removed in session 15) and the peer Connect (removed in session 13). Decide on a real port plan first — the previous `:7003` was unbound and the port table needs dedicated peer-protocol listener ports. The simplest scheme is dedicated listeners (e.g. 7003 primary peer, 7004 secondary peer) so peer-protocol bytes are not conflated with order PDUs. Once leader-follower lands, the gateway's `forward_pdu_to_sequencers` plural function name becomes accurate again as the dual-publish branch returns.
5. **Sequencer "behaves as unconditional leader" stub limitation** — `SequencerThread::on_framework_pdu_message` currently always forwards. Only the leader should forward. Will be fixed when leader-follower lands.
6. **`SequencedMessage` wrapper** — currently the sequencer forwards raw PDUs to ME without a sequence-number envelope; add this once stable.
7. **Trace logs in `PduParser` and elsewhere** — the two `Info`-level lines in `PduParser::receive` (header decode + raw 16 header bytes), the `Info`-level payload hex dump in `PduParser::dispatch_pdu` (added during session 15 to diagnose the binary-garbage bug), and the short `TRACE` lines in `InboundConnectionManager::on_accept` and `SequencerThread::on_framework_pdu_message` are all valuable diagnostic infrastructure but noisy at production rates. Drop to `Debug` or wrap behind a compile-time switch when production traffic begins.
8. **Pub/sub WAL** — long-term replacement for direct TCP; eliminates the rendezvous problem and the retry workaround.

## Immediate Next Task

A WAL+HA design has been worked through in detail (see "WAL and HA Design" section below) and a nine-slice implementation plan agreed. The next session takes **Slice 1** of that plan:

**Slice 1 -- Add seqNo to `EventMessage` and to the wire format.** Smallest possible change: add an `int64_t seq_no` field to `EventMessage` (peer of the existing `pdu_id`/`connection_id`), update `EventMessage::create_framework_pdu_message` to take it, route the SequencerThread's already-existing `next_sequence_number_` into the field when re-encoding for forwarding, propagate through to the ME and gateway. After this slice the ME and gateway both see the seqNo on every PDU but do nothing with it. Verifies the plumbing without changing any semantics. Likely accompanied by an addition to the `PduHeader` so seqNo is on the wire (currently the header carries `byte_count`, `pdu_id`, `version`, canary; add `seq_no` between `pdu_id` and `version`, with appropriate htonll/ntohll).

After Slice 1 lands, continue with Slice 2 (in-memory WAL) and onward per the staging plan in the design section. Each slice is a session or two of work and leaves the system in a working state.

**Smaller items deferred but still on the list:**
- Trace log cleanup (item 7 in "What Is Not Yet Done"): demote `Info`-level traces to `Debug`. Mechanical, quick. Worth doing whenever it stops being useful for slice verification.
- OrderCancelRequest round trip (item 2): mirrors NewOrderSingle path on ME side. Small. Fits in a session corner.
- fix8 wrong-port re-verification (item 1): one `f8test -d` run.

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

**Rationale** — this design avoids the mistake made at work where logging is unavailable until after config is read (because the log filename comes from the config). Here the log filename comes from the command line, so logging starts immediately and config errors are recorded in the log rather than only printed to stderr.

**Config changes** — all four application configs gain required `[logging]` section:
```toml
[logging]
applog_level = "info"
syslog_level = "info"
```
Both fields are required. There are no optional config fields — making a field optional hides it from operators and makes it unconfigurable in practice.

**FIX parsing implemented in `sample_fix_gateway_seq`** — `FixParser`, `FixSerialiser`, `FixMessage`, `FixSession` copied from `sample_fix_gateway` with namespace changed to `sample_fix_gateway_seq`. `MsgType::OrderCancelRequest` and `Tag::OrigClOrdID` added to `FixMessage.hpp`. Logger threaded through `FixParser` constructor so bad checksums are logged at Debug rather than silently dropped. Full FIX session layer implemented in `FixGatewaySeqThread` (Logon, Heartbeat, TestRequest, Logout, NewOrderSingle, OrderCancelRequest). PDU encoding and ER routing remain TODO.

---

## WAL and HA Design (planned)

Designed in conversation, not yet implemented. This section captures the architecture so subsequent sessions can refer back to it. The implementation is staged into nine vertical slices, listed at the end.

The design follows the convergent pattern that Aeron Cluster, Kafka, Raft, and database checkpointing all arrive at: **separate the irreversible decision (WAL commit) from its replayable effects (ME state, ERs, FIX out).** The WAL is authoritative; everything downstream is reconstructable from it. Followers observe commits, never infer them. Leadership decides who may append; the WAL decides what already happened. Those two concerns must never leak into each other.

### Authority and roles

- **Sequencer + WAL = authority.** The sequencer assigns seqNo and appends to the WAL. The WAL append (via store-release on the commit offset) is the single irreversible act in the system. Before that store, an order does not exist; after it, the order is permanent and globally visible.
- **Matching Engine = pure consumer.** The ME has no independent state, no persistence of its own, no FIX-side effects. It receives orders in seqNo order from the leader, mutates a book in memory, emits ERs back. The book is reconstructable by replaying the WAL, so an ME crash means "throw it away and replay from the leader's WAL".
- **FIX Gateway = edge translator.** Translates FIX wire to/from PDUs. Holds no FixSession→ClOrdID map (that lives in the sequencer's WAL — see below). Routes ERs to whichever FIX session corresponds to the ConnectionID embedded in the ER PDU by the sequencer.

### State location (changed from current)

In session 15's working system, the gateway holds `cl_ord_id_to_session_` mapping. In the WAL design **this map moves to the sequencer**. Reasoning:

- The sequencer is already the single point that observes every order PDU, so adding "stamp the originating ConnectionID into the WAL entry" is a one-field addition rather than a new mechanism.
- The gateway becomes near-stateless. On a gateway restart, it does not lose ER routing capability, because that information lives in the sequencer's WAL.
- After failover, the new sequencer leader has the routing map by virtue of WAL replay.

**Subtlety -- ConnectionID is not stable across reconnects.** A fix8 client logging out and back in gets a new ConnectionID. Two options for how the sequencer addresses an ER target:

- (a) Route on `(SenderCompID, TargetCompID)` -- natural FIX-level addressing. The gateway maintains a small per-comp-id table mapping to the current ConnectionID, and the sequencer addresses ERs by comp-id.
- (b) Route on ConnectionID. Accept that an ER for a session that has reconnected is undeliverable.

(a) is more robust and is the chosen option. The gateway's job is "translate comp-id ↔ ConnectionID for the current FIX session"; the sequencer's WAL holds comp-id-keyed routing.

### WAL format and segmentation

A WAL entry is `[ magic | length | seqNo | payload | checksum ]`. Replay scans from offset 0; on any checksum or bounds failure it stops. Entries past the failure point are treated as "did not happen". Tail corruption is therefore identical in effect to a clean crash before commit of that entry.

The WAL is **segmented** into fixed-size files (e.g. `wal_000001.log`, `wal_000002.log`). Each segment is independently checksummable and archivable. Segmentation simplifies truncation, localises corruption damage, and makes the disk-full scenario predictable.

The WAL is **single-writer**: only the leader sequencer appends. No locks needed; a single thread does all writes.

### Commit semantics

"Commit" has two distinct meanings, both required:

1. **Locally durable (in-memory):** store-release on the commit offset. CPU coherence guarantees that any other reader sees the entry. This is what gates the leader's send to the ME.
2. **Replicated (cross-machine durable):** the follower has acked the record over its dedicated replication channel. This is what gates the leader's send of an ER to the gateway.

The ER is held back from the gateway until replication has acked, so the gateway never observes an order whose existence is held by only one machine. This gives Raft-style two-machine durability without Raft's full state machine, because there is no quorum (just leader + follower) and no log-replication-via-vote (just point-to-point streaming).

**No `fsync` per commit.** Disk durability is a separate concern from commit. The cost of `fsync` per WAL append is significant (10s of microseconds at minimum, often 100µs+) and is unnecessary because durability comes from replication. `fsync` happens out-of-band: as part of segment rotation, snapshot writes, or a periodic flusher. Disk corruption after an undurable commit is recoverable via the replica.

### Replication channel

The leader streams WAL records to the follower over a dedicated TCP connection -- separate from the order/ER data channels and separate from the arbiter control channel. The follower writes records to its own local WAL (own disk, own machine) and sends per-record acks back. The leader uses the highest acked seqNo to gate ER emission.

Followers do not infer commits from heartbeat or timing; they observe records arriving on the replication channel. The follower's role is strictly passive: it tails, it acks, it does not send to the ME, it does not send to the gateway. Connections from gateway and ME to the follower do exist (so they are pre-warmed for promotion) but carry no data while the follower is passive.

### Gateway↔sequencer reconnection on failover

Both the gateway-to-sequencer connection and the ME-to-sequencer connection drop when the leader fails. The gateway and ME need to reconnect to the new leader. Mechanism:

- Gateway and ME each open TCP connections to **both** sequencer instances at startup, and keep both open. Sends go only to whichever instance is currently leader. The non-leader sequencer rejects send commands at the application layer (returning an error PDU or simply closing the data sub-channel) so the gateway and ME know which is leader without needing an out-of-band discovery mechanism.
- On leader change, the old leader's connection drops or starts rejecting. The new leader's connection becomes the live one.
- The gateway buffers outbound order PDUs locally during the cutover window (bounded buffer; FIX-level back-pressure if it fills). The ME, being stateless, simply waits for the new leader to begin sending.

**Per-leg cursors.** The gateway maintains "last sent order seqNo" and "last received ER seqNo" cursors per sequencer instance. On reconnect, the new leader and the gateway exchange cursor positions and replay any gap. The sequencer's WAL holds matching state: "last delivered ER per gateway ConnectionID" or equivalent. This means **the WAL holds two record kinds** -- order/ER events (the data) and delivery cursors (the per-peer progress). Cursors are written periodically rather than per-record, keeping WAL volume reasonable.

Reconnect-and-replay is conceptually similar to FIX's own sequence-number resend mechanism, one layer down.

### Snapshots

Snapshots capture sequencer state only. The ME is never snapshotted -- its book is rebuilt from the WAL on every restart. Snapshot contents are deliberately minimal:

- `lastCommittedSeqNo`
- FixSession routing tables (comp-id → ConnectionID, ClOrdID → ConnectionID)
- Per-gateway delivery cursors
- Anything else the sequencer needs to assign seqNo `S+1` after restart

**Anything that can be recomputed from the WAL is not in the snapshot.**

Snapshotting is non-blocking. The leader briefly gates new seqNo assignment (~tens of microseconds), drains in-flight WAL appends to establish a clean cut at seqNo `S`, captures the snapshot state in memory, releases the gate, and serialises the snapshot file asynchronously. The ME never sees a pause; FIX ingress sees only the brief micro-gate.

**Dual snapshots (rolling).** Two snapshots are kept at all times: `snapshot_A` (older, trusted, used as the truncation anchor) and `snapshot_B` (newer, candidate, validated before promotion). WAL truncation uses the older trusted snapshot, not the newest one just taken. This deliberately retains a safety window of "WAL extending back beyond the trusted snapshot" so that a newer snapshot turning out to be unreadable does not lose history. After validation, `snapshot_B` is promoted to `snapshot_A` and a new candidate is taken.

The invariant: **never delete WAL history unless at least one older verified snapshot can reproduce the same state.** Enforced mechanically, not by policy.

### WAL truncation

After a snapshot at seqNo `S` is durable and validated:

- Delete WAL segments fully behind `S`.
- Keep any segment containing seqNos `> S`.
- The follower must have replayed at least `S` (or have its own snapshot ≥ `S`) before the leader truncates. Otherwise truncation could delete history the follower still needs.

### Failure-handling boundaries

Each of the following is a designed-for case, not a "this might happen and we'll see":

| Situation | Correct behaviour |
|---|---|
| Leader crash before WAL commit | Order disappears (never existed). Gateway resends or fix8 retries. |
| Leader crash after WAL commit, before ME send | Follower promotes, replays WAL, sends order to ME, emits ER. |
| Leader crash after ME send, before ER | Same -- the new leader replays since the ME is a deterministic consumer; FIX will see an ER eventually. |
| ME crash | ME restarts empty; new leader replays from ME's `lastAppliedSeqNo + 1`. |
| Gateway crash | Gateway reconnects, exchanges cursors, replays any gap. |
| WAL disk full | Sequencer immediately gates ingress. No "best effort" continuation. Operator action: rotate, halt cleanly, or fail to replica. |
| WAL tail corruption | Truncate at last good entry -- treat as crash before commit. |
| WAL mid-segment corruption | Halt; promote replica with clean prefix. Single-replica recovery requires manual intervention. |
| Snapshot corrupt during validation | Snapshot discarded; system continues with the previous trusted snapshot. |
| Snapshot format incompatible after upgrade | Roll back binary; the older snapshot in the dual-snapshot pair still works. |

### Arbiter

The arbiter is **off the critical data path**. It holds a single cell: `(leader_id, epoch, lease_expiry)`. The leader sends heartbeats to the arbiter to renew its lease. The arbiter never participates in order processing.

On leader-failover the follower asks the arbiter "am I the new leader?". The arbiter performs an atomic compare-and-swap: if the old leader's lease has expired and the requester's proposed epoch is greater than the current, it grants and bumps the epoch. On revival, the old leader sees its epoch is stale and steps down.

This is the **fencing token / epoch / leader-epoch** pattern (same family as Raft term, Kafka leader-epoch, Chubby/ZooKeeper epoch). It is dramatically simpler than full Raft because there is no log-replication-by-quorum; the WAL replication is a separate point-to-point channel. The arbiter is just leader-election + fencing, a few hundred lines of code at most.

If the leader is alive and the arbiter is unreachable, the leader continues. If the leader has died and the arbiter is also unreachable, the follower cannot promote -- this is the correct behaviour to avoid split-brain.

The arbiter is built from scratch (not etcd / Consul / ZooKeeper / Raft). Off-the-shelf packages are out of scope.

### Topology diagram

```
                                  ┌─────────────┐
                                  │   fix8      │
                                  │   client    │
                                  └──────┬──────┘
                                         │ FIX wire (TCP, port 9879)
                                         │ orders, ERs, heartbeats
                                  ┌──────▼──────────────┐
                                  │   FIX Gateway       │
                                  │                     │
                                  │ - parses FIX        │
                                  │ - encodes PDU       │
                                  │ - tracks per-leg    │
                                  │   cursors           │
                                  │ - reconnects on     │
                                  │   leader change     │
                                  │ - small comp-id ↔   │
                                  │   ConnectionID      │
                                  │   table only        │
                                  └──┬───────────────┬──┘
                                     │               │
                       order PDUs    │               │  ER PDUs
                       (one TCP      │               │  (from current leader)
                       conn each;    │               │
                       gateway sends │               │
                       only to       │               │
                       leader; both  │               │
                       conns kept    │               │
                       open)         │               │
                                     │               │
                       ┌─────────────┴─────┐ ┌───────┴────────────┐
                       │                   │ │                    │
                       ▼                   ▼ ▼                    ▼
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │  Sequencer PRIMARY       │         │  Sequencer FOLLOWER      │
        │  (current leader)        │         │  (passive)               │
        │                          │         │                          │
        │  - assigns seqNo         │         │  - tails leader's WAL    │
        │  - appends to WAL        │         │  - applies records to    │
        │  - state:                │         │    its own WAL & state   │
        │    · FixSession→         │         │  - never sends to ME     │
        │      ConnID map          │         │  - never sends to GW     │
        │    · ClOrdID→ConnID map  │         │  - heartbeats to arbiter │
        │    · per-GW cursors      │         │  - on promotion: stops   │
        │  - heartbeats to arbiter │         │    tailing, replays its  │
        │  - sends to ME           │         │    own WAL, becomes      │
        │  - sends ERs to GW       │         │    leader                │
        │                          │         │                          │
        │  ┌────────────────────┐  │         │  ┌────────────────────┐  │
        │  │ WAL on local disk  │  │         │  │ WAL on local disk  │  │
        │  │ (mmap, segmented)  │  │         │  │ (mmap, segmented)  │  │
        │  │                    │  │         │  │                    │  │
        │  │ - order records    │  │         │  │ - mirror of leader │  │
        │  │ - cursor records   │  │         │  │   (one record      │  │
        │  │ - snapshot files   │  │         │  │   behind, due to   │  │
        │  │   (dual, rolling)  │  │         │  │   ack RTT)         │  │
        │  └────────────────────┘  │         │  │ - own snapshots    │  │
        │                          │         │  └────────────────────┘  │
        └────┬──────────────────┬──┘         └──────────────────────────┘
             │                  ▲                      ▲
             │ order PDUs       │ ER PDUs              │ WAL replication
             │ (committed,      │                      │ (TCP, dedicated
             │  with seqNo)     │                      │ "replication" conn)
             │                  │                      │ leader pushes,
             │                  │                      │ follower acks;
             │                  │                      │ leader does not
             │                  │                      │ acknowledge to GW
             │                  │                      │ until follower-acked
             ▼                  │                      │
        ┌──────────────────────────┐                   │
        │  Matching Engine         │                   │
        │                          │                   │
        │  - receives orders in    │                   │
        │    seqNo order from      │                   │
        │    LEADER only           │                   │
        │  - mutates book          │◄──────────────────┘
        │  - emits ERs back to     │  (this is the leader→follower
        │    leader sequencer      │   WAL replication channel,
        │  - stateless across      │   not an ME data path)
        │    crashes (book         │
        │    rebuilt by replay)    │
        │                          │
        └──────────────────────────┘

        ╔══════════════════════════════════════════════════════════════════╗
        ║                              ARBITER                              ║
        ║                                                                   ║
        ║   - holds (leader_id, epoch, lease_expiry) -- one cell            ║
        ║   - leader sends heartbeats: "I'm still leader, epoch=N"          ║
        ║   - follower can ask: "current leader? lease state?"              ║
        ║   - on promotion request: atomic compare-and-swap                 ║
        ║       if lease_expired and request.proposed_epoch > current:      ║
        ║         grant; bump epoch; record new leader                      ║
        ║   - NEVER on the order/ER data path                               ║
        ║                                                                   ║
        ╚══════════════════════════════════════════════════════════════════╝
              ▲                              ▲
              │ heartbeats / lease renewal   │ heartbeats (less frequent)
              │ from current leader          │ from passive follower
              │                              │
              └──────┬───────────────────────┘
                     │
              both sequencers know about the arbiter;
              only one is leader at any moment

        Channels summary:
        ─────────────────────────────────────────────────────────────────────
        FIX              fix8 ↔ Gateway          (TCP, FIX wire)
        Orders           Gateway → Sequencer     (TCP, PDU; only to leader)
        ERs              Sequencer → Gateway     (TCP, PDU; only from leader)
        ME orders        Sequencer → ME          (TCP, PDU; only from leader)
        ME ERs           ME → Sequencer          (TCP, PDU; only to leader)
        WAL replication  Leader → Follower       (TCP, dedicated; bidirectional acks)
        Arbiter control  Sequencers ↔ Arbiter    (TCP; not on data path)
        ─────────────────────────────────────────────────────────────────────
```

### Implementation staging (nine slices)

Each slice is shippable -- the system works at the end of each. Each slice is a session or two. The whole programme is roughly a year of part-time work but never leaves the system in a half-built state.

1. **Add seqNo to `EventMessage` and to wire format.** PDU header gains `seq_no` field; `EventMessage` gains a peer to `pdu_id`. ME and gateway see seqNos but do nothing with them yet. Plumbing only. *(Slice 1; Immediate Next Task.)*
2. **In-memory WAL.** Sequencer maintains an in-memory log of every committed order. Used for nothing yet but the data structure is exercised. Verifies ring-buffer or segmented-vector design works.
3. **mmap'd WAL on disk, single-host, no fsync.** Crash recovery: replay WAL on startup, rebuild state. No replication yet. Validates the durable-log mechanism.
4. **Snapshot (single, no rolling).** Snapshot at intervals, truncate WAL behind the snapshot point. Fast restart. Validates the snapshot mechanism in isolation.
5. **Move FixSession ↔ ClOrdID mapping into sequencer's state.** Gateway becomes the translator described above. Comp-id-keyed routing replaces the gateway's current `cl_ord_id_to_session_` map.
6. **Single-host failover infrastructure.** Two sequencer processes on the same host. File-based fencing for leader detection. Gateway connects to leader-side. Validates the protocol mechanics (handshakes, cursor exchange, replay) without network complexity.
7. **Network replication.** Follower on another host. Leader streams WAL records over a dedicated TCP connection. Follower acks; commit = follower-acked. The classic Aeron model, end-to-end.
8. **Arbiter.** Lease + epoch. Replaces file-based fencing. The leader-election + fencing layer.
9. **Dual snapshots, snapshot validation, additional polish.** The safety-net layer that turns the system from "works" into "operationally bullet-proof".

---

## HA Architecture (legacy stub -- predates the WAL+HA design above)

The legacy stub described two sequencer instances with the gateway dual-publishing every order PDU to both, so a follower stayed in sync and failover would be gap-free. That stub never fully landed: session 15 removed the secondary sequencer and the dual-publish mechanism because their semantics under the "behaves as unconditional leader" stub were broken (both sequencers would forward to the ME, producing duplicate fills). The full WAL+HA design above replaces this stub. When the design lands, the secondary returns as a passive follower (not a parallel publisher), order PDUs go only to the leader, and the WAL replication channel keeps the follower in sync.

For the framework's *generic* leader-follower DSL protocol (separate from the sequencer-specific design above), the four-node DR topology described in subsystem 12 still applies. The sequencer-specific design uses a simpler topology (two sequencers + one arbiter, single site) because matching-engine workloads have different durability constraints than the framework's generic streaming use case.

---

## Application Architecture — Sequencer-Based Order Flow

Inspired by the Aeron sequencer pattern. The sequencer is the **sole writer** to the matching engine's input stream, imposing total order on all messages.

**Current state (session 15 end -- single sequencer, no HA):**

```
FIX client
    | raw FIX bytes (RawBytesProtocolHandler)
    v
sample_fix_gateway_seq          (single instance)
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
sample_fix_gateway_seq --> FIX ER --> FIX client (via cl_ord_id_to_session_)
```

**Future state (after WAL+HA slices land):** the second sequencer returns as a passive follower, the gateway connects to both but sends only to the leader, and the WAL replication channel runs alongside the data channels. See "WAL and HA Design" above for the full topology diagram.

**Startup order** (counterintuitive but necessary): gateway must start before the sequencer because the sequencer connects outbound to the gateway's ER inbound listener on port 7010. If the sequencer starts first the connect retries (2-second interval, framework-level retry implemented since session 12). Long-term fix is the WAL+HA design's pattern of always-open dual connections from gateway to sequencer pair.

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
| 7021 | ME → sequencer ER listener |
| 7022 | (reserved) ME → sequencer-follower ER listener; not in use post session 15 |
| 7100 | sequencer → arbiter |

The reserved ports are kept in the table so they are not accidentally repurposed before the WAL+HA slices land. When slice 6 (single-host failover) adds the second sequencer, 7002, 7022, and one of 7003/7004 will become live; when slice 7 (network replication) runs, the WAL replication channel will bind a chosen port from the 7003/7004 pair.

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
