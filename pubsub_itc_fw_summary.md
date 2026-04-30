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

### Session 13 (current)

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
- PDU framing (`PduFramer` two-mode, `PduParser` zero-copy with `ConnectionID`) — complete, tested. **KNOWN BUG (session 13): use-after-free in disconnect-handler invocation. See "Immediate Next Task".**
- `OutboundConnection` — complete; passes `id_` to `PduParser`
- `InboundConnection` — complete
- `ProtocolHandlerInterface` / `PduProtocolHandler` — complete; accepts `ConnectionID`. **KNOWN BUG (session 13): same use-after-free pattern as `PduParser` in three methods. See "Immediate Next Task".**
- `MirroredBuffer` — complete, tested
- `InboundConnectionManager` — complete; passes `id` to `PduProtocolHandler`; inbound connections carry `"inbound:<port>"` in service name
- `OutboundConnectionManager` — complete; connection retry implemented; use-after-free on service name fixed
- `ThreadLookupInterface` — complete
- Reactor connection management — complete; `retry_failed_connections` called from housekeeping tick
- `ServiceRegistry` / `ServiceEndpoints` — complete
- `ConnectionID` — own class with `service_name()` for both inbound and outbound connections
- `EventType` / `EventMessage` — complete; `create_framework_pdu_message` carries `ConnectionID`
- `ReactorControlCommand` — complete
- `ReactorConfiguration` — complete; `connect_retry_interval_` (2s default, WAL-pending workaround)
- `FileSystemUtils` — complete
- DSL code generator — complete; `enum class` fix; `char` type; 133 tests passing
- `fix_equity_orders.dsl` — FIX 5.0 SP2 equity order topic registry
- Logging subsystem — complete
- `RawBytesProtocolHandler` — complete
- `sample_fix_gateway_seq` — FIX session layer complete; PDU encoding to sequencers complete; connection identification complete
- `sequencer` — `on_framework_pdu_message` implemented: order PDUs decoded and forwarded to ME; ER PDUs decoded and forwarded to gateway. Peer connection deferred (session 13) — `peer_host`/`peer_port` removed from config, `connect_to_service("sequencer_peer")` removed, `peer_conn_id_` removed. Will return when leader-follower protocol is implemented.
- `matching_engine`, `arbiter` — stubs, compiling
- `start_fix_seq_system.py` — working with correct startup order

## What Is Not Yet Done (in dependency order)

1. **Fix the use-after-free in `PduParser` and `PduProtocolHandler`** (session 13 SIGSEGV) — see "Immediate Next Task"
2. **Investigate fix8 connecting to wrong port** — session 13 logs show fix8 reaching the gateway's ER inbound listener (port 7010) rather than the FIX listener (port 9879), even though `myfix_gateway_client.xml` declares `port="9879"`. Run `f8test -d -c myfix_gateway_client.xml -N GW1` to dump the parsed config and confirm what port fix8 actually has resolved. Deferred until SIGSEGV is fixed because the gateway needs to stay up to test against.
3. **ER routing in gateway** — `on_framework_pdu_message` decodes `ExecutionReport` PDU and routes back to FIX client via `cl_ord_id` map
4. **Matching engine stub → real** — decode incoming order PDUs, match, send ER back to sequencer
5. **Arbiter stub → real** — `ArbitrationReport` → `ArbitrationDecision`
6. **Leader-follower protocol** — state machine, heartbeat timers, arbitration. When this lands, restore the peer Connect: re-add `peer_host`/`peer_port` to `SequencerConfiguration`, the toml files, and the loader; restore the `service_registry_.add("sequencer_peer", ...)` block in `Sequencer.cpp`; restore the `connect_to_service("sequencer_peer")` call and `peer_conn_id_` in `SequencerThread`. Decide on a real port plan first — the previous `:7003` was unbound and the port table at the bottom of this document needs to specify dedicated peer-protocol listener ports for both sequencers. The simplest scheme is dedicated listeners (e.g. 7003 primary peer, 7004 secondary peer) so the peer-protocol bytes are not conflated with order PDUs.
7. **Sequencer "behaves as unconditional leader" stub limitation** — `SequencerThread::on_framework_pdu_message` currently has both sequencer instances forwarding to the matching engine. Only the leader should forward. Will be fixed when leader-follower lands.
8. **`SequencedMessage` wrapper** — currently the sequencer forwards raw PDUs to ME without a sequence number envelope; add this once ME decode path exists
9. **Pub/sub WAL** — long-term replacement for direct TCP; eliminates the rendezvous problem and the retry workaround

## Immediate Next Task

Fix the use-after-free in `PduParser` and `PduProtocolHandler` that caused the gateway SIGSEGV at session-13 end.

**The bug:** `PduParser::receive()` invokes its stored `disconnect_handler_` from inside the method when the peer closes gracefully. The handler tears down the connection, which destroys the `OutboundConnection` (or `InboundConnection`), which destroys the `unique_ptr<PduParser>` member, which destroys the parser whose method is currently executing. When `disconnect_handler_()` returns, control returns to a method running on a destroyed `*this` — undefined behaviour, observed as SIGSEGV in production. The bug is also present (three times) in `PduProtocolHandler::on_data_ready`, `send_prebuilt`, and `continue_send`. Stack trace from session 13:

```
#0 PduParser::receive() at PduParser.cpp:44
#1 OutboundConnectionManager::on_data_ready
#2 Reactor::dispatch_events at Reactor.cpp:957
```

**Plan: option 2 — remove `disconnect_handler_` from `PduParser` and `PduProtocolHandler` entirely.** Make `receive()` and the various send methods just return their (success, error) status. The caller (the outbound or inbound manager's `on_data_ready` etc.) is responsible for tearing down the connection on failure. This matches the broader design philosophy that the Reactor and its managers own connection lifecycle; having parsers and handlers reach in and destroy connections is an inversion that creates exactly this class of bug. Two other options were considered and rejected:

- *Option 1 (defer teardown via a control command):* less code change but introduces a delay between detection and teardown that complicates state reasoning.
- *Option 3 (set a flag instead of calling the handler):* the parser/handler still has to communicate the close-needed state up the stack, which is what option 2 already does via the `(false, "")` return value. Strictly less clean than option 2.

**Files that will change for option 2** (estimate; verify before committing):
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduParser.hpp` — remove `disconnect_handler_` member and the corresponding constructor parameter
- `libraries/pubsub_itc_fw/src/PduParser.cpp` — remove the two `disconnect_handler_()` call sites and adjust the recv-zero path to return `(false, "")` directly
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/PduProtocolHandler.hpp` — remove `disconnect_handler_` member and the corresponding constructor parameter
- `libraries/pubsub_itc_fw/src/PduProtocolHandler.cpp` — remove the three `disconnect_handler_()` call sites; the methods become straightforward delegates returning success/error
- `libraries/pubsub_itc_fw/src/OutboundConnection.cpp` — remove `disconnect_handler` parameter from `on_connected` and from the `PduParser` construction call
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/OutboundConnection.hpp` — match
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp` — `on_data_ready` already calls `teardown_connection` on `(false, ...)` return at line 271 — that becomes the single teardown path. The `on_connect_ready` site that builds the disconnect_handler lambda (lines 226-228) goes away.
- `libraries/pubsub_itc_fw/src/InboundConnectionManager.cpp` — `on_data_ready` currently delegates to `conn.handle_read()`; needs to be changed to inspect the parser's return value and call `teardown_connection` on failure. The disconnect_handler lambda at lines 138-140 goes away.
- `libraries/pubsub_itc_fw/src/InboundConnection.cpp` (or .hpp) — `handle_read` needs to return success/error or otherwise communicate the close-needed state up to the inbound manager's `on_data_ready`.
- Tests — `PduParserTest`, `PduProtocolHandlerTest` and any test using the disconnect_handler hook need updating to check return values rather than handler invocation. Estimate 5-15 lines of test changes per affected file.

**Verification once option 2 lands:** the original repro is to start the system, connect fix8, then leave it idle for 120 seconds. Both inbound FIX connections and both outbound sequencer connections will idle-timeout in the same epoll batch, exercising the previously-crashing path. With the fix in place the gateway should log the teardowns and stay alive.

**After SIGSEGV is fixed:** investigate the fix8 wrong-port issue (item 2 above), then proceed to ER routing (item 3).

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
    | SequencedMessage PDU -- leader only forwards to ME (port 7020)
    v
matching_engine                 (single instance)
    | ExecutionReport PDU -- sent back to sequencer ER listener (ports 7021/7022)
    v
sequencer (receives ER, forwards to gateway on port 7010)
    v
sample_fix_gateway_seq --> FIX ER --> FIX client (via cl_ord_id map)
```

**Startup order** (counterintuitive but necessary): gateway must start before sequencers because the sequencer connects outbound to the gateway's ER inbound listener on port 7010. If sequencer starts first it cannot connect and there is currently no retry. Long-term fix is connection retry in the framework.

**Port allocation (local testing):**

| Port | Usage |
|---|---|
| 9879 | FIX client → gateway (RawBytes inbound) |
| 7001 | gateway → sequencer primary (order PDUs) |
| 7002 | gateway → sequencer secondary (order PDUs) |
| 7003 | (deferred) sequencer peer-to-peer; not in use until leader-follower protocol is implemented |
| 7010 | sequencer → gateway (ER forwarding inbound) |
| 7020 | sequencer → ME (sequenced order PDUs inbound) |
| 7021 | ME → sequencer primary ER listener |
| 7022 | ME → sequencer secondary ER listener |
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
