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
| `ProtocolHandlerInterface` | Pure abstract interface: `on_data_ready()`, `send_prebuilt()`, `has_pending_send()`, `continue_send()`, `deallocate_pending_send()`, `commit_bytes()` |
| `PduProtocolHandler` | Strategy A: owns `PduParser` + `PduFramer` + pending-send slab state; handles framework-native PDU streams |
| `RawBytesProtocolHandler` | Strategy B: owns `MirroredBuffer`; delivers raw byte streams to the application thread; see Section 7 for full design |

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

**Use-after-free protection in `InboundConnectionManager`**

`send_prebuilt()` may invoke the disconnect handler synchronously (e.g. on `EPIPE`), which calls `teardown_connection()` and destroys the `InboundConnection`. Both `process_send_pdu_command()` and `process_send_raw_command()` therefore re-look up the connection by `ConnectionID` after calling `send_prebuilt()`, before attempting to register `EPOLLOUT`. If the connection was destroyed, the re-lookup returns `end()` and the function returns cleanly.

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

### Session 6 (current)
- **Reactor refactor confirmed complete** — `Reactor` correctly delegates all inbound/outbound connection work to `InboundConnectionManager` and `OutboundConnectionManager`; was done in Session 4 but not recorded
- **`RawBytesProtocolHandler` implemented and all bugs fixed** — intermittent `BurstDelivery` test failure resolved; root cause was ambiguity in tail-advance detection when new data arrives simultaneously with a commit being processed; fix: `EventMessage::create_raw_socket_message` now carries `tail_position` (from `MirroredBuffer::tail()`); app thread uses exact tail comparison instead of the fragile `available < last_available_` heuristic; commit policy changed to only commit when entire window is consumed
- **`MirroredBuffer`** — added `tail()` accessor
- **`EventMessage`** — added `tail_position` field to `Header` and `tail_position()` getter; `create_raw_socket_message` takes `int64_t tail_position` as fourth argument
- **`InboundConnectionManager` use-after-free fix** — `process_send_pdu_command` and `process_send_raw_command` both re-look up connection by ID after `send_prebuilt` returns, since `send_prebuilt` may invoke the disconnect handler synchronously and destroy the connection
- **Re-delivery machinery removed** — `deliver_pending_redeliveries`, `pending_redelivery_`, `fresh_delivery_pending_`, `bytes_buffered()`, `buffered_read_ptr()`, `take_pending_redelivery()`, `has_fresh_delivery_pending()` all removed from `RawBytesProtocolHandler`, `ProtocolHandlerInterface`, `InboundConnectionManager`, and `Reactor`
- **Format string mismatch detection documented and tested** — In Quill 11.0.2 format errors are caught in `_populate_formatted_log_message` and emitted as `[Could not format log statement...]` log records; `error_notifier` is NOT called for format errors; `QuillLoggerTest.FormatMismatchIsReportedAsLogRecord` test added; `LoggingMacros.hpp` and `QuillLogger.hpp` updated with full explanation of C++17 limitation and C++20 path
- **Quill flush limitation documented** — no per-severity flush in Quill 11.x; fatal signal handler approach (calling `Backend::stop()`) deliberately not implemented due to stability problems; documented in `QuillLogger.hpp`
- **TODO cleanup** — `TimerType` converted from enum class to proper class with `as_string()`/`as_tag()`; `FileOpenMode` cleaned up and made consistent; `LoggerInterface` and `MockLogger` deleted (superseded by Quill callback sink); `LoggerTest.cpp` deleted; `SocketHandler.hpp` stale include removed
- **CMake install support** — `include(GNUInstallDirs)` added; `install()` rules added for shared library (lib/lib64), headers, test binaries, performance harnesses, and `sample_fix_gateway`; Doxygen runs at install time; Python DSL generator installed via `pip install --prefix`
- **`build.py` updated** — `--install-dir` argument added; `configure_cmake` passes `-DCMAKE_INSTALL_PREFIX`; `install_project()` function added
- **Doxygen fixed** — `PROJECT_NAME` changed to `pubsub_itc_fw`; `OUTPUT_DIRECTORY` changed to `build/docs`; `INPUT` updated to include source and header directories plus `mainpage.dox` explicitly
- **Design documentation added** — three new `.dox` files in `docs/`:
  - `raw_socket_design.dox` -- MirroredBuffer internals, fragmentation handling, commit protocol, why `tail_position()` is needed, backpressure table
  - `memory_management_design.dox` -- no-heap-on-hot-path principle, ExpandableSlabAllocator, ExpandablePoolAllocator, Treiber stack with ABA prevention via 128-bit tagged pointer and CMPXCHG16B, backpressure via watermark callbacks
  - `dsl_design.dox` -- DSL grammar, all types and wire formats, generated C++ API (owning structs, view structs, encode/decode/skip/arena functions), zero-copy decode on little-endian hardware, toolchain structure
- **`mainpage.dox`** updated with links to all three design pages, coding rules section restored, stale content removed
- **Stale docs deleted** -- `mainpage.md`, `design_overview.md`, `dsl_grammer.md` all removed
- **`sample_fix_gateway` tested** with fix8 -- raw bytes path confirmed working end-to-end with real FIX 5.0 SP2 traffic
- **Coverage analysis** -- 86.1% line, 93.2% function; all uncovered lines in `RawBytesProtocolHandler` are legitimate error paths
- All 411 tests passing; 1000-iteration stress test completed without failure

### Session 7 (current)
- **DSL `char` field type implemented** — `char` added as a primitive field type throughout the DSL toolchain:
  - `parser.py` — `char` added to `_parse_type`; produces `PrimitiveType("char")`; was already a keyword and already accepted as an enum underlying type
  - `generator_cpp.py` — `char` added to `_primitive_wire_size` (1 byte), `_cpp_primitive_type` (maps to C++ `char`), `_emit_write_primitive` (single byte, cast to `uint8_t`), `_emit_read_primitive` (cast byte to `char`); enum underlying type `char` changed from `int8_t` to `char` in `_cpp_int_type`; `char` added to `wire_helpers` in `_emit_enum_functions`
  - `generator_pybind11.py` — `char` added to `_cpp_primitive` type mapping
- **Four pybind11 test failures fixed** — `goto` crosses initialisation for optional `string` fields; `Unknown field: sender_comp_id` (all if-branches were independent); `Unknown field: has_price` (`has_` kwargs never matched); `string` cast via `decltype` produced dangling `string_view`
- **`fix_equity_orders.dsl` created** — FIX 5.0 SP2 equity order topic registry at `applications/fix_equity_orders.dsl`; see DSL Subsystem section for design rationale
- **Parser error message improved** — comma used as enum entry separator now gives clear diagnostic; test added to `test_char_enum.py`
- **Application architecture designed** — sequencer-based Aeron-pattern order flow; see Application Architecture section. Key decisions: gateway sends to both sequencer instances; HA on sequencer only with main-site arbiter; `SequencedMessage` DSL wrapper; ME→gateway is TCP PDU stub for future pub/sub; `cl_ord_id` correlation
- **Four application stubs written and compiling** — `sample_fix_gateway_seq`, `sequencer`, `matching_engine`, `arbiter` all compile cleanly
- **CMake DSL generation rule** — `applications/CMakeLists.txt` updated with `add_custom_command` writing to `${CMAKE_CURRENT_BINARY_DIR}/fix_equity_orders.hpp`; all FIX application targets depend on `fix_equity_orders_generated`
- **Framework APIs confirmed this session** (do not guess at these):
  - `ServiceRegistry::add(name, primary, secondary)` — always three arguments; use `NetworkEndpointConfiguration{}` as secondary when only one endpoint is needed
  - `ProtocolType::FrameworkPdu` — not `Pdu`; the only other value is `RawBytes`
  - No `reactor_->connect()` — outbound connections initiated via `connect_to_service(name)` called from `on_app_ready_event()`
  - `generate_cpp_from_dsl.py` second argument is an output **file path**, not a directory

### Session 6
- **Reactor refactor confirmed complete** — `Reactor` correctly delegates all inbound/outbound connection work to `InboundConnectionManager` and `OutboundConnectionManager`
- **`RawBytesProtocolHandler` bugs fixed** — intermittent `BurstDelivery` test failure resolved; root cause was ambiguity in tail-advance detection; fix: `EventMessage::create_raw_socket_message` now carries `tail_position`; commit policy changed to only commit when entire window consumed
- **`MirroredBuffer`** — `tail()` accessor added
- **`EventMessage`** — `tail_position` field added; `create_raw_socket_message` takes `int64_t tail_position` as fourth argument
- **`InboundConnectionManager` use-after-free fix** — `process_send_pdu_command` and `process_send_raw_command` re-look up connection after `send_prebuilt` returns
- **Design documentation written** — `raw_socket_design.dox`, `memory_management_design.dox`, `dsl_design.dox`; `mainpage.dox` updated
- **`sample_fix_gateway` tested** with fix8 — raw bytes path confirmed end-to-end
- All 411 tests passing

### Session 5
- **Logging subsystem rewrite** — `QuillLogger` redesigned with two clean constructors (production: file+syslog; unit test: console+optional callback); `FwLogLevel` enum values flipped so lower=more severe, matching Quill convention; `should_log_to_*` routing functions removed; sink-level filtering used throughout; macros forward args directly to Quill backend thread (no front-end formatting); `PUBSUB_LOG` and `PUBSUB_LOG_STR` are the only two call-site macros
- **`LoggerWithSink` rewritten** — now uses unit-test constructor with callback wired to `std::vector<std::string> records`; two-argument constructor removed; `TestSink` dependency removed from `LoggerWithSink`
- **`TestSink` fixed** — missing `<iomanip>`, `<sstream>`, `<chrono>`, `<stdexcept>` includes added
- **All test files updated** — 12 test files migrated from old `LoggerWithSink("name","sink")` constructor and `.sink->*` accessor pattern to new API; all 410 tests passing

### Session 4
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

### Session 3
- TcpSocket EAGAIN/EOF contract fix
- Use-after-free fix in on_data_ready / on_inbound_data_ready
- InboundConnection infrastructure completed
- DSL generator fixes (3 bugs)
- Integration test infrastructure
- StatusQueryResponseRoundTrip integration test passing
- ApplicationThread `get_reactor()` accessor added

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
- Reactor connection management — complete
- `ServiceRegistry` / `ServiceEndpoints` / `ConnectionID` — complete
- `EventType` / `EventMessage` — complete (includes `create_raw_socket_message` with `tail_position`)
- `ReactorControlCommand` — complete
- `ReactorConfiguration` — complete
- DSL code generator — complete, `char` field type added, 133 tests passing
- Leader-follower DSL messages — defined and generated
- `fix_equity_orders.dsl` — FIX 5.0 SP2 equity order topic registry defined
- Logging subsystem — complete, rewritten in Session 5
- `RawBytesProtocolHandler` — complete (Strategy B; owns `MirroredBuffer`; `CommitRawBytes` command implemented)
- Application stubs — `sample_fix_gateway_seq`, `sequencer`, `matching_engine`, `arbiter` — compiling

## What Is Not Yet Done (in dependency order)

1. **`SequencedMessage` DSL file** — define `sequencer_messages.dsl` with the envelope (sequence_number: i64, payload_type: i16, payload: TBD); decide where it lives and how the payload bytes are carried
2. **Sequencer stub → real implementation** — decode inbound order PDUs, wrap in `SequencedMessage`, forward to ME (leader only); leader-follower state machine
3. **Gateway seq stub → real implementation** — parse inbound FIX, encode as `fix_equity_orders` PDU, send to both sequencers; decode inbound ER PDU, look up `cl_ord_id`, serialise FIX ER back to client
4. **Matching engine stub → real implementation** — unwrap `SequencedMessage`, decode inner PDU, run matching logic, send ER PDU to gateway
5. **Arbiter stub → real implementation** — decode `ArbitrationReport`, reply with `ArbitrationDecision`
6. **Leader-follower protocol** — state machine, heartbeat timers, arbitration
7. **Pub/sub fanout** — replace direct ME→gateway TCP with pub/sub
8. **Logging levels** — move verbose paths from Info to Debug once applications are running

## Immediate Next Task

Define `sequencer_messages.dsl` for the `SequencedMessage` envelope. Before writing:
- Ask the user where it should live (`applications/` alongside `fix_equity_orders.dsl`, or in the library include path like `leader_follower.dsl`)
- Confirm the payload field design — how are the inner PDU bytes carried? Options: fixed-size byte array, length-prefixed field, or a new DSL type

Then implement the sequencer's PDU forwarding logic in `SequencerThread`.

---

## HA Architecture

The original framework HA design (four-node DR topology) still applies to any use of the leader-follower protocol. However for the sequencer-based application architecture, a simpler HA model is used:

- HA applies to the **sequencer only** — gateway and matching engine are single-instance
- Two sequencer instances on the main site: primary (`instance_id` 1) and secondary (`instance_id` 2)
- A **main-site arbiter** (separate lightweight `arbiter` process) resolves startup ties — DR arbiters are NOT used for this topology
- Leadership is deterministic: lowest `instance_id` wins
- The gateway maintains outbound connections to **both** sequencer instances and sends every order PDU to both, so the follower stays in sync and failover is gap-free
- All endpoints configured via `ReactorConfiguration` and `ServiceRegistry`

---

## Application Architecture — Sequencer-Based Order Flow

Inspired by the Aeron sequencer pattern. The sequencer is the **sole writer** to the matching engine's input stream, imposing total order on all messages.

```
FIX client
    | raw FIX bytes (RawBytesProtocolHandler)
    v
sample_fix_gateway_seq          (single instance)
    | NewOrderSingle / OrderCancelRequest PDUs -- sent to BOTH sequencer instances
    v
sequencer primary + secondary   (leader-follower HA, main-site arbiter)
    | SequencedMessage PDU -- leader only forwards to ME
    v
matching_engine                 (single instance)
    | ExecutionReport PDU -- direct TCP (TODO: replace with pub/sub)
    v
sample_fix_gateway_seq --> FIX ER --> FIX client (via cl_ord_id map)
```

**Application directory layout:** all under `applications/`, one dir per executable: `sample_fix_gateway_seq/`, `sequencer/`, `matching_engine/`, `arbiter/`.

**Outbound connection pattern:** `ServiceRegistry::add(name, primary, secondary)` called in the application constructor (always three args; use `NetworkEndpointConfiguration{}` for unused secondary). `connect_to_service(name)` called from `on_app_ready_event()` in the thread class. No `reactor_->connect()` method exists.

**Port allocation (local testing):**

| Port | Usage |
|---|---|
| 9879 | FIX client → gateway |
| 7001 | gateway → sequencer primary |
| 7002 | gateway → sequencer secondary |
| 7003 | sequencer peer-to-peer |
| 7010 | ME → gateway ER |
| 7020 | sequencer → ME |
| 7100 | sequencer → arbiter |

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
