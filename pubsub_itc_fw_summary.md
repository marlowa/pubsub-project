# pubsub_itc_fw — Project Summary

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
- Lock-free thread-safe pool allocators
- Broadcast / fanout via WAL followers;
a topic-based pubsub primitive may be added later if specific use cases require it
(see "Downstream consumers and broadcast streams" in the WAL and HA Design section)
- Timers (timerfd, via epoll)
- High availability via primary/secondary instance pairs with arbitration
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
| `ExpandableSlabAllocator` | Chains `SlabAllocator` instances; demand-driven reclamation (no GC thread); Vyukov sentinel deferred-reclamation (`deferred_reclaim_slab_id_`) so popped slabs are destroyed one drain after they are popped, safe against producers still mid-enqueue; wall-clock drain tripwire; returns `std::tuple<int, void*>` for structured bindings |
| `EmptySlabQueue` | Intrusive Vyukov MPSC queue of slab IDs; one node embedded per slab. Consumer never resets head_/tail_ — Vyukov sentinel pattern relies on the most-recently-popped slab staying alive as the queue's sentinel (deferred-reclaim by one drain cycle, managed by `ExpandableSlabAllocator`). Four `peek_*` const accessors for diagnostics. |

**`Slot<T>` layout (production path, not valgrind):**
```
[ is_constructed (atomic) ][ free_next (atomic) ][ canary (u64) ][ storage (alignas T) ]
```
`free_next` before `canary` — canary remains adjacent to storage for underrun detection.

**Bug history:** Two bugs fixed in `FixedSizeMemoryPool`: (1) unsafe free-list traversal in `get_number_of_available_objects` — fixed with atomic counters; (2) data race on `next` pointer inside union — fixed by moving to `std::atomic<Slot<T>*> free_next` outside the union. Both produced ~1-in-100 failure rate under stress.

**Bug fixed in `ExpandableSlabAllocator`:** `drain_empty_slab_queue()` was destroying a `SlabAllocator` (via `unique_ptr::reset()`) while still traversing the Vyukov queue whose nodes are embedded inside that slab. Fixed by collecting all slab IDs into a `std::vector` first, then processing them after the queue traversal completes. The vector allocation is on a cold path and not a hot-path concern. Detected by ASan on `ExpandableSlabAllocatorTest.OldSlabIsDestroyedAfterChaining`.

**Bug fixed in `EmptySlabQueue` / `ExpandableSlabAllocator` (session 16):** `EmptySlabQueue::reset_to_empty()` did three non-atomic stores (`dummy_.next = nullptr; head_ = &dummy_; tail_.store(&dummy_)`). The `tail_.store(&dummy_)` would occasionally clobber a concurrent producer's `tail_.exchange(node)`, leaving the queue in a wedged state where `head_->next` pointed at a "ghost-enqueued" slab but `tail_` was reset to the dummy. The wedge could persist indefinitely. Reproducer test fired the bug in 1.1 seconds. Fixed by removing `reset_to_empty` entirely and adopting the classical Vyukov sentinel pattern: the most-recently-popped slab stays alive as the queue's sentinel until the next drain confirms head_ has advanced past it (via a `deferred_reclaim_slab_id_` member on `ExpandableSlabAllocator`). Producers never need a reset because they only touch `tail_` and `prev->next`. The fix also switched the drain-loop tripwire from iteration-count (100,000) to wall-clock (one second) — iteration-count was meaningless at nanosecond per-iteration speeds. Diagnosed in production under heavy fix8 T-burst load on the integrated gateway/sequencer; integrated load test now runs without the tripwire firing.

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

This section documents how raw byte streams (alien protocols such as ASCII FIX, or any custom binary protocol) are handled end-to-end. This is the most complex path in the framework because unlike PDU connections, the application thread is responsible for its own message framing.

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

The application thread pattern (as used in `BurstListenerThread`) employs the following:
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

This is a bespoke, intentionally simple protocol. There is no need for a full consensus algorithm such as Raft or Paxos. The deployment topology is fixed: exactly two participating nodes per site (one configured as primary, one as secondary), with a third node (the arbiter, itself HA) to break ties at startup. Leader election is deterministic — the node with the lowest `instance_id` wins. The arbiter never becomes a leader or follower; it only resolves startup ambiguity when both nodes are undecided.

#### Topology

Four instances in total, each with a unique integer `instance_id` configured in `ReactorConfiguration`. "Main" refers to the site; the four instances run on four different machines at the main site:

| Instance | Site | Role in election |
|---|---|---|
| Node A (primary) | Main | Participant |
| Node B (secondary) | Main | Participant |
| Node C (primary) | Main | Arbiter |
| Node D (secondary) | Main | Arbiter |

The arbiter has primary and secondary instances to avoid single-machine SPOF for the arbiter itself.
Note: a previous early design, now rejected, was to use arbiters at the DR site.

For the sequencer-specific HA deployment described in the "WAL and HA Design" section below, a third arbiter pool member is added: a *witness* machine that holds no state but votes in elections of which of the two arbiters is currently active. The witness is deployed in a failure-independent location (different power, different switch, ideally different network segment) so that no single failure can take out an arbiter and the witness simultaneously. The witness is not part of the generic DSL leader-follower protocol described here; it is an addition specific to the arbiter pool's own internal HA. See "Arbiter PSA topology" in the WAL and HA Design section for protocol details.

#### PDU Summary

| Message | ID | Purpose |
|---|---|---|
| `StatusQuery` | 100 | Identity + epoch announced on TCP connect |
| `StatusResponse` | 101 | Identity confirmation + peer echo + current role |
| `Heartbeat` | 102 | Liveness detection + epoch propagation |
| `ArbitrationReport` | 200 | Sent when arbitration needed |
| `ArbitrationDecision` | 201 | authoritative tie-break + epoch assignment |

#### Epoch Semantics

The epoch is a generation counter that exists to detect stale nodes from a previous leadership cycle.

Rules:

1. A node that has never participated in an election starts with epoch 0.
2. At startup, when arbitration is used, the arbiter assigns the epoch in `ArbitrationDecision`. Both nodes adopt this value. Because the arbiter is itself HA (PSA+witness, see "Arbiter PSA topology" in the WAL and HA Design section), the epoch counter is durable across arbiter restarts: the arbiter's primary→secondary replication keeps the most recent epoch state on both full arbiter instances, and on arbiter restart the surviving instance restores from its replicated copy. The arbiter does not lose track of epochs when an arbiter process restarts.
3. When a follower detects leader death, it does NOT promote itself unilaterally. It contacts the arbiter and requests promotion via `ArbitrationReport`. The arbiter, having confirmed the previous leader's lease has expired, issues an `ArbitrationDecision` granting the requesting node the leader role and assigning the next epoch. The follower adopts the leader role only after receiving this decision. This prevents split-brain in network-partition scenarios where the follower can no longer see the leader but the leader is still alive on the other side of the partition. (An earlier design had the follower promote unilaterally and increment its own epoch by 1; that design was rejected because it permits split-brain when the arbiter is reachable from both partition halves.)
4. When a restarting node connects and receives a `StatusResponse`, it compares epochs. If the peer's epoch is higher, the restarting node is stale and adopts the follower role immediately without contacting arbiter.
5. A heartbeat carrying an epoch lower than the receiver's own epoch indicates a stale sender; the receiver logs a warning and ignores the heartbeat.

#### Startup Election Flow

1. On startup, each node attempts TCP connection to its peer (A→B, B→A).
2. On connection, both sides immediately send `StatusQuery` (identity + epoch).
3. On receiving `StatusQuery`, each side replies with `StatusResponse` including its `current_role`.
4. **If the peer's `StatusResponse` carries `Role::leader`:** the connecting node adopts `Role::follower` immediately. No arbiter contact needed.
5. **If the peer's `StatusResponse` carries `Role::unknown`:** both sides are undecided. Both send `ArbitrationReport` to arbiter (primary first, secondary as fallback).
6. arbiter receives both reports and issues `ArbitrationDecision` assigning leader and follower deterministically by lowest `instance_id`, and sets the epoch for this generation.
7. Both nodes adopt their assigned roles and the arbiter connection is closed.

#### Post-Election Steady State

- The peer-to-peer TCP connection remains open with `Heartbeat` messages sent at regular intervals in both directions.
- Heartbeats carry `instance_id` and `epoch` for liveness detection and stale-node detection.
- If the **follower** dies: the leader logs a warning. No other action is taken.
- If the **leader** dies: the follower promotes itself (see Leader Death below).

#### Restart Flow

When a node restarts it connects to the peer and exchanges `StatusQuery`/`StatusResponse`. If the peer's `StatusResponse` carries `Role::leader` and a higher epoch, the restarting node adopts `Role::follower` without contacting arbiter.

#### Leader Death and Follower Promotion

On heartbeat loss:
1. The surviving node first attempts to reconnect to the peer.
2. If reconnection succeeds: exchange `StatusQuery`/`StatusResponse`; the epoch resolves roles as normal.
3. If reconnection fails, the peer is presumed dead. The surviving node sends `ArbitrationReport` to the arbiter requesting promotion. The arbiter checks whether the previous leader's lease has expired; if so, it issues `ArbitrationDecision` granting leadership and assigning the next epoch. The surviving node adopts the leader role only after receiving the decision.
4. If the arbiter is unreachable, the surviving node cannot promote. It enters a degraded waiting state and continues retrying the arbiter. The system is unavailable for new orders during this window. This is the correct behaviour: without arbiter confirmation that the previous leader is gone, promoting unilaterally risks split-brain.

#### Split-Brain Protection

**Normal startup with arbiter reachable:** arbiter is the sole authority and assigns exactly one leader. Split-brain is impossible.

**One node already established:** The epoch difference immediately resolves this — the restarting node unconditionally adopts follower role.

**Network partition (both nodes alive, link down):** Neither node can promote itself unilaterally (per rule 3). Whichever node can still reach the arbiter requests promotion; the arbiter grants if the other node's lease has expired. If both nodes can reach the arbiter, the arbiter grants to one and refuses the other. If neither can reach the arbiter, both enter degraded waiting state and the system is unavailable until arbiter contact is restored. Split-brain is not possible because no node ever assumes leader role without an `ArbitrationDecision` (or, on cold start, a deterministic arbiter-mediated tie-break).

#### Open Design Questions

- **HA has not considered DR yet, the design at the moment is for main site only.**
- **Heartbeat interval and loss threshold:** Not yet specified. These will be `ReactorConfiguration` parameters.

---

### 13. Authentication Service and SCRAM-SHA-256

**Overview.** A standalone application (`applications/authentication_service/`) that authenticates FIX gateway clients using SCRAM-SHA-256 (RFC 5802 variant). The service is stateless: each four-message exchange is self-contained. Two instances run for HA (primary port 7070, secondary port 7071); they share no state and require no synchronisation.

**PDU protocol** (defined in `applications/authentication.dsl`, namespace `pubsub_itc_fw_app`):

| ID | Message | Key fields |
|---|---|---|
| 500 | `AuthenticationRequest` | `request_id` (i64), `comp_id` (string), `client_nonce` (bytes) |
| 501 | `AuthenticationChallenge` | `request_id`, `server_nonce` (bytes), `salt` (bytes), `iterations` (i32) |
| 502 | `AuthenticationProof` | `request_id`, `client_proof` (bytes, 32 bytes) |
| 503 | `AuthenticationResult` | `request_id`, `outcome` (enum), `server_signature` (bytes, 32 bytes), `force_password_change` (bool) |

`request_id` is the gateway's `ConnectionID` for the FIX session, carried unchanged through all four messages so the gateway can correlate the result with the correct pending session.

**SCRAM computation** (performed client-side by the gateway):
```
SaltedPassword = PBKDF2-SHA256(password, salt, iterations)
ClientKey      = HMAC-SHA256(SaltedPassword, "Client Key")
StoredKey      = SHA256(ClientKey)
ServerKey      = HMAC-SHA256(SaltedPassword, "Server Key")
AuthMessage    = uint32le(len(comp_id)) || comp_id
               || uint32le(len(client_nonce)) || client_nonce
               || uint32le(len(server_nonce)) || server_nonce
               || uint32le(len(salt)) || salt
               || uint32le(iterations)
ClientSig      = HMAC-SHA256(StoredKey, AuthMessage)
ClientProof    = ClientKey XOR ClientSig        -- sent in AuthenticationProof
ServerSig      = HMAC-SHA256(ServerKey, AuthMessage)  -- verified by gateway on AuthenticationResult
```

**`ScramCrypto` shared library** (`libraries/scram_crypto/`). Static library linked by both the authentication service and the gateway. Namespace `scram_crypto`. Free functions: `hmac_sha256`, `sha256`, `pbkdf2_sha256`, `make_scram_credential`, `compute_auth_message`. Depends on `OpenSSL::Crypto` (PRIVATE linkage). `find_package(OpenSSL REQUIRED)` in top-level CMakeLists.

**Current credential store.** A single stub `ScramCredential` (`stub_credential_`) hardcoded in `AuthenticationThread`. Credential database integration is the next major work item; see "Database Access Design" below.

**Mutual authentication.** The gateway verifies the `ServerSignature` in `AuthenticationResult` before completing the FIX Logon. This confirms the service is genuine (not an impostor). If verification fails the gateway sends FIX Logout and disconnects.

---

### 14. Logging Subsystem

`QuillLogger` wrapping `quill::Logger*`. `PUBSUB_LOG(logger, level, fmt, ...)` for format args; `PUBSUB_LOG_STR(logger, level, str)` for single string (required by `-Werror=variadic-macros`).

Log levels: `FwLogLevel::Alert`, `Critical`, `Error`, `Warning`, `Notice`, `Info`, `Debug`, `Trace`. Currently everything is logged at `Info`; level differentiation is a future task.

Any class that needs to log receives a `QuillLogger&` in its constructor and stores it as a member. The Reactor does not own all logging — each class logs for itself.

---

### 15. Database Access Design (discussed, not yet implemented)

**Rationale for a database.** `comp_id` identities appear in many places beyond SCRAM credentials: per-comp-id and per-firm-id gateway throttle limits, risk management parameters, position limits, and more. A flat file per concern quickly becomes unmanageable. The authentication service and risk subsystem both need a relational store. The workplace uses Oracle; personal preference is PostgreSQL. To avoid vendor lock-in, **unixODBC** is the chosen abstraction layer — the application talks to `libodbc.so` via the standard ODBC API and the DSN configuration selects the underlying driver.

**The async problem.** Standard ODBC has no async API. Each query blocks the calling thread until the RDBMS replies. Blocking the reactor thread or any `ApplicationThread` would stall the entire event loop. The solution is a **thread pool of `std::thread` workers**, each holding a persistent ODBC connection. The reactor thread never touches ODBC directly.

**`DatabaseThread` design.** A subclass of `ApplicationThread`. It owns:
- A pool of `std::thread` workers (count configurable). Each worker holds one open ODBC connection and blocks on a work queue.
- An ITC interface: other `ApplicationThread` subclasses post request messages to `DatabaseThread`'s ITC queue. `DatabaseThread::on_itc_message()` dispatches the request to a free worker.
- A result-delivery path back into the reactor's epoll loop (two open options — see below).

No `std::thread` idle-keepalive timer is needed. `Reactor::check_for_stuck_threads()` checks only callback duration (time from `time_event_started_` to `time_event_finished_` per thread); a `DatabaseThread` that has no work to do sits idle between ITC deliveries and the reactor never marks it stuck. An idle thread is always safe.

**Two open options for worker → reactor result delivery:**

1. **eventfd registered with epoll.** Each worker writes to an `eventfd` when a result is ready. The reactor sees `EPOLLIN` on the `eventfd` and delivers a `DatabaseResult` event to the requesting `ApplicationThread`. Requires a small extension to the framework to support non-socket fds in epoll.
2. **Workers post directly to the ITC queue.** Workers call `thread.post_to_queue(result_message)` directly. The result lands in the requesting thread's ITC queue without involving the reactor at all. Simpler — no framework changes needed — but the ITC queue must be safe for cross-thread post from a raw `std::thread` (it is: `LockFreeMessageQueue` is MPSC-safe).

Neither option has been chosen yet; this remains an open design question.

**Credential pre-load strategy.** For the authentication service the hot path (SCRAM exchange) must never block on the database. The chosen approach is to **pre-load all credentials at startup** into an `unordered_map<string, ScramCredential>` held in the `AuthenticationThread`. The hot path only reads the in-memory map. On SIGHUP or an admin PDU, `DatabaseThread` reloads the credential table and posts the new map to `AuthenticationThread` via ITC. This also avoids the N idle ODBC connections problem — the worker pool can be shut down after the initial load (or kept alive only for periodic refresh).

---

### 16. TLS Subsystem (framework complete; not yet wired to applications)

TLS support was added to the framework's raw-bytes connection layer in Session 20. It covers both inbound (server-side) and outbound (client-side) connections. The matching-engine-facing and auth-service-facing paths remain plain TCP; TLS is intended for future use on the FIX client-facing gateway listener and any other externally-exposed connection.

**New `ProtocolType` value:** `ProtocolType::TlsRawBytes` (value 2). Selecting this type on an `InboundListenerConfiguration` causes `InboundConnectionManager` to create a `TlsRawBytesProtocolHandler` for each accepted connection instead of a `RawBytesProtocolHandler`. The application thread receives the same `RawSocketCommunication` events as it does for plain `RawBytes`; TLS is transparent above the protocol-handler boundary.

**`TlsContext`** (`TlsContext.hpp` / `.cpp`). Wraps an `SSL_CTX`. Non-copyable. Factory methods:
- `create_server(cert_path, key_path, ca_path, require_client_cert)` — server-side context. `ca_path` empty disables client certificate verification.
- `create_client(ca_path, cert_path, key_path)` — client-side context. `ca_path` empty skips server verification.

Both enforce TLS 1.2 minimum, prefer TLS 1.3. Ciphers: TLS 1.2 AEAD only (`ECDHE-ECDSA-AES256-GCM-SHA384` etc.); TLS 1.3 `TLS_AES_256_GCM_SHA384` and `TLS_CHACHA20_POLY1305_SHA256`. One `TlsContext` per listener or outbound service; certificate loading happens once at construction, not per-connection.

**`TlsState`** (`TlsState.hpp` / `.cpp`). Per-connection. Owns `SSL*`, `BIO* rbio`, `BIO* wbio`. Owns a `pending_outbound` byte vector (ciphertext bytes that could not be sent immediately). `HandshakePhase` enum: `Pending`, `Complete`, `Failed`. Move-constructible (needed when `OutboundConnection` is move-inserted into the connections map).

**`TlsRawBytesProtocolHandler`** (`TlsRawBytesProtocolHandler.hpp` / `.cpp`). Implements `ProtocolHandlerInterface`. Uses **OpenSSL memory BIOs** so the reactor thread never blocks: all socket reads/writes use `MSG_DONTWAIT`; `SSL_read`/`SSL_write` work against in-memory BIOs rather than the socket fd directly. Key details:
- Constructor: `is_server` flag selects `SSL_accept` vs `SSL_connect` path.
- `start_outbound_handshake()`: generates the client's initial `ClientHello` record and flushes the write BIO to the socket. Called once by `OutboundConnectionManager` immediately after TCP connection is established.
- Handshake subsequent steps are driven by `on_data_ready()` arrivals from epoll. `ConnectionEstablished` is NOT delivered until `HandshakePhase::Complete`.
- Once complete, `drain_plaintext()` loops `SSL_read()` into the `MirroredBuffer` and delivers a `RawSocketCommunication` event.
- `send_prebuilt()`: calls `SSL_write()` (which copies the plaintext internally), then releases the slab chunk **immediately**. The resulting ciphertext in the write BIO is flushed; unsent ciphertext goes into `TlsState::pending_outbound`.
- `continue_send()`: drains `pending_outbound` on `EPOLLOUT`.
- Backpressure: same high-water (75%) / low-water (50%) mark scheme as `RawBytesProtocolHandler`.
- Peer close: `SSL_ERROR_ZERO_RETURN` (TLS `close_notify`) → `{false, "", false}` → `ConnectionLost`.

**`TlsListenerConfiguration`** (`TlsListenerConfiguration.hpp`). Fields: `certificate_path`, `private_key_path`, `ca_path`, `require_client_certificate`. Carried by `InboundListenerConfiguration::tls` (`std::optional<TlsListenerConfiguration>`). The `Reactor` reads this during init, calls `TlsContext::create_server`, and stores the context in the `InboundListener`. Each accepted connection creates one `SSL` object from the shared context.

**`TlsClientConfiguration`** (`TlsClientConfiguration.hpp`). Fields: `ca_path`, `certificate_path`, `private_key_path`, `raw_buffer_capacity`. Carried by `ServiceEndpoints::tls` (`std::optional<TlsClientConfiguration>`). When present, `OutboundConnectionManager` creates a `TlsContext` and a `TlsRawBytesProtocolHandler` for the connection instead of a `PduProtocolHandler`.

**`ProtocolHandlerInterface` additions**: `start_outbound_handshake()`, `is_handshake_complete()`, `is_reads_paused()` virtuals. Non-TLS handlers return sensible defaults (`{true, ""}`, `true`, `false` respectively).

**`OutboundConnectionManager` TLS integration**: on TCP connect-ready, if `conn.is_tls()`, calls `start_outbound_handshake()` instead of delivering `ConnectionEstablished` immediately. On subsequent `on_data_ready()` arrivals while handshake is pending, drives `on_data_ready()` which internally calls `drive_handshake()` until `HandshakePhase::Complete`, at which point `ConnectionEstablished` is delivered. `process_send_pdu_command` and `process_send_raw_command` check `is_tls()` and use `send_prebuilt()` accordingly; TLS slab deallocation differs (slab freed inside `send_prebuilt()` rather than on send completion).

**OpenSSL dependency**: `find_package(OpenSSL REQUIRED)` in top-level `CMakeLists.txt`; `target_link_libraries` against `OpenSSL::SSL` and `OpenSSL::Crypto`.

**Integration tests** (`TlsProtocolHandlerIntegrationTest.cpp` — 5 tests; `TlsOutboundIntegrationTest.cpp` — 4 tests):

| Test | Scenario |
|---|---|
| `TlsHandshakeAndRoundTrip` | Inbound: client establishes TLS, sends framed message, receives reply |
| `FragmentedCiphertextDelivery` | Inbound: length prefix in first SSL_write, payload 20 ms later; framework accumulates both records |
| `PeerDisconnect` | Inbound: SSL_shutdown → close_notify → ConnectionLost |
| `MutualTlsHandshake` | Inbound: server requires client certificate; both sides authenticate |
| `HandshakeFailure` | Inbound: client has wrong CA; TLS alert → server tears down → ConnectionLost |
| `OutboundTlsHandshakeAndRoundTrip` | Outbound: reactor as TLS client; send on ConnectionEstablished; server replies; ConnectionLost on server close |
| `OutboundMutualTls` | Outbound: server requires client certificate; TlsClientConfiguration carries cert/key paths |
| `OutboundTlsServerDisconnect` | Outbound: server closes after handshake; ConnectionEstablished delivered before ConnectionLost |
| `OutboundTlsHandshakeFailureNoConnectionEstablished` | Outbound: wrong trust anchor; cert verification fails; ConnectionEstablished never delivered; reactor stays alive |

All certificates generated programmatically in tests via OpenSSL C API (EC prime256v1, SHA-256). No external tooling required.

**Relationship to SCRAM-SHA-256.** SCRAM (see Section 13) provides *authentication* — proof that the client knows the correct password — but does not encrypt the channel. TLS provides *confidentiality and integrity* for the byte stream. In the full production design, the gateway's inbound FIX listener should use `TlsRawBytes` so that both the FIX messages and the SCRAM exchange over that channel are protected in transit. The two mechanisms are complementary: SCRAM authenticates the SCRAM exchange itself (mutual authentication via `ServerSignature`), TLS prevents the exchange from being observed or tampered with by a network eavesdropper. Until TLS is wired to the gateway listener, the SCRAM exchange travels over a plaintext TCP connection; this is acceptable for localhost/internal development but not for production.

**Current status.** Framework complete and tested. No application currently uses `TlsRawBytes` — the gateway FIX listener uses `RawBytes` and the authentication service uses plain `FrameworkPdu`. TLS is available to wire up when the gateway needs to expose an encrypted endpoint to external FIX clients.

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

### Session 24 (current)

**`ApplicationAnnouncer` — startup one-liner in each application.**

Every application now logs a single structured line at `Info` level as soon as its logger is ready:

```
sequencer: version=0.1.0 pid=12345 built=2026-06-02T10:14:07Z branch=main sha=654da89 host=seq-primary
```

**CMake build info generation.** Three new files:

- `cmake/BuildInfo.hpp.in` — template for the generated header; substitution variables are `@PROJECT_VERSION@`, `@GIT_SHA@`, `@GIT_BRANCH@`, `@BUILD_DATETIME@`.
- `cmake/GenerateBuildInfo.cmake` — cmake -P script run at build time. Queries `git rev-parse --short HEAD` and `git rev-parse --abbrev-ref HEAD`; stamps UTC datetime with `string(TIMESTAMP ... UTC)`; writes `${CMAKE_BINARY_DIR}/generated_build_info/pubsub_itc_fw/BuildInfo.hpp` using `copy_if_different` so an unchanged git state does not force recompilation.
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ApplicationAnnouncer.hpp` — header-only static class. `announce(QuillLogger& logger, const std::string& app_name)` calls `gethostname` + `getpid`, then `PUBSUB_LOG` at `Info` level with all fields.

`CMakeLists.txt` gains a `build_info_generated ALL` custom target (runs before `add_subdirectory(libraries/pubsub_itc_fw)`) and a `BUILD_INFO_INCLUDE_DIR` cache variable. `libraries/pubsub_itc_fw/CMakeLists.txt` adds `build_info_generated` to `add_dependencies` and `$<BUILD_INTERFACE:${BUILD_INFO_INCLUDE_DIR}>` to public include paths, so all downstream targets get `BuildInfo.hpp` transitively.

**Call sites in `main()`.** Two patterns:

- *Pattern A* (sequencer, arbiter, matching_engine, witness): announce immediately after `make_unique<QuillLogger>`, before config load. Logger is at its initial `Info` level; the announcement is always visible.
- *Pattern B* (authentication_service, order_gateway): announce after `load_and_init_logging` + `set_log_level` + `set_syslog_level`. Logger is fully configured before the announcement fires.

**Version management.** The semantic version is the `VERSION` field in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`. This is the source of truth; `release.py` already reads it by regex when naming the artefact, and the new `build_info_generated` target passes it to `GenerateBuildInfo.cmake` via `-DPROJECT_VERSION`. Release workflow: bump the version in `CMakeLists.txt`, commit, tag `v{version}`.

*Future enhancement:* derive the version from `git describe --tags --match 'v*'` at build time so the tag is the single source of truth. Preferred approach would be to examine the tag the current commit is reachable from (i.e. the nearest ancestor tag, resolving ambiguity via commit distance). Deferred for now because `git describe` returns nothing before the first tag exists and produces a non-semver suffix (`v1.2.3-5-gabc1234`) between releases; the manual bump is simpler and fully auditable.

---

### Session 23

**HA scenario 10 (`me_death_restart`) fixed — all 14 HA scenarios now pass.**

The scenario kills and restarts the matching engine during a live order run. The previous failure: `_ME_READY_MARKERS` fires at ~0.3 s when the secondary sequencer connects to the restarted ME; the primary sequencer only re-establishes its own ME connection at ~2.1 s. Phase 5 orders arriving in that gap were forwarded to ME over an invalid (unestablished) connection and dropped — approximately 21,000 drops.

Two-part fix in `ha_test.py`:
1. Capture `seq_primary_pos_pre_kill = file_end(seq_primary_log)` before Phase 4 (the kill/restart phase).
2. After Phase 4, when the scenario includes a ME restart, poll `seq_primary_log` from that position for the log message `"matching engine order connection established"` (10 s timeout) before proceeding to Phase 5.
3. In Phase 5, if ME was restarted, stop and restart the FIX session — the existing FIX session was blocked waiting for dropped in-flight order responses and would never drain.

**`devenv.py` — developer sandbox management script.**

New root-level script for starting, stopping, and monitoring all components on a dev machine.

- Subcommands: `start`, `stop`, `status`, `restart [name]`
- `--env PATH` selects the environment TOML (default: `environments/dev.toml`); `--no-ha` skips `ha_only=true` components; `--delay SECONDS` controls inter-start sleep.
- Components are launched in the order listed in `[startup_order]`, shut down in reverse order.
- Binary processes: `[binary_path, log_file, config_path]`, cwd = `install_dir/workdir`.
- JAR processes: `["java", "-jar", jar_path]`, cwd = `install_dir/workdir`. stdout/stderr go to `log_dir/<name>.stdout`.
- PID files: `run_dir/<name>.pid`. Stale PIDs cleaned on stop.
- Exports credentials (runs `db/export_credentials.py`) before starting any component; re-exports when restarting an auth service instance.

**`release.py` — versioned deployment artefact assembly.**

New root-level script that packages the build output into a `pubsub-<version>-<git-short-hash>.tar.gz`.

- Version read from `project(...VERSION x.y.z...)` in `CMakeLists.txt` via regex.
- Git hash from `git rev-parse --short HEAD`; `--no-git-hash` omits it.
- Deployment binaries derived from `[startup_order]` + `[components]` in the env TOML (set deduplication handles the shared `arbiter` binary).
- Staged layout: `bin/` (deployment binaries only; test/bench binaries skipped), `lib/` (`libpubsub_itc_fw.so` + jars from env TOML), `etc/` (config templates from `applications/`, not the installed tree; `credentials.toml` excluded), `db/` (Liquibase changelog + scripts), `environments/` (all `.toml` files), `devenv.py`, `deploy.py`, `release.json` (version, git hash, build timestamp).
- `--install-dir`, `--env`, `--version`, `--output-dir`, `--no-git-hash` options.

**Config template system — all 9 component TOML files converted.**

Component TOML files in `applications/` are now templates with `${placeholder}` syntax (Python `string.Template`). Placeholder names are the full flattened TOML path of the substitution value — for example `[arbiter_primary] peer_host` → `${arbiter_primary_peer_host}`. This avoids cryptic abbreviations.

Files converted: `witness/witness.toml`, `arbiter/arbiter.toml`, `arbiter/arbiter_secondary.toml`, `authentication_service/authentication_service.toml`, `authentication_service/authentication_service_secondary.toml`, `matching_engine/matching_engine.toml`, `order_gateway/order_gateway.toml`, `sequencer/sequencer.toml`, `sequencer/sequencer_secondary.toml`.

`environments/dev.toml` extended with substitution sections at the end: `[shared]`, `[witness]`, `[arbiter_primary]`, `[arbiter_secondary]`, `[auth_service_primary]`, `[auth_service_secondary]`, `[matching_engine]`, `[order_gateway]`, `[sequencer_primary]`, `[sequencer_secondary]`. Values are placed in their logically-appropriate section, not a flat `[vars]` block.

**`cpu_pinning_dev_mode` → `cpu_pinning_reserve_cpu0` rename (C++ + TOML).**

`cpu_pinning_dev_mode` was ambiguous — "dev" could mean "development mode" generally. The flag means "exclude CPU 0 from pinning candidates (leave it for OS/interrupt use)"; `cpu_pinning_reserve_cpu0` says this precisely.

Renamed across 24 files:
- C++ struct field: `ReactorConfiguration::cpu_pinning_reserve_cpu0` and all six app-level `*Configuration` structs.
- C++ function parameter: `is_dev_mode` → `reserve_cpu0` in `get_available_cpu_ids()` (`CpuPinning.hpp`), `claim_cpus()` (`CpuRegistry.hpp`, `CpuRegistry.cpp`).
- TOML key in all 9 config templates: `cpu_pinning_reserve_cpu0 = ${shared_reactor_cpu_pinning_reserve_cpu0}`.
- `dev.toml` substitution key: `reactor_cpu_pinning_reserve_cpu0 = true` (set to `false` in `prod.toml`).
- `ReactorConfiguration.hpp` doc comment updated to remove "development machines" phrasing.

**`deploy.py` — deployment script.**

New root-level script; included in the release artefact.

Steps performed in order:
1. **Unpack** `--artefact pubsub-<ver>.tar.gz` into `install_dir`, stripping the top-level artefact directory (optional; skip if already unpacked).
2. **Expand templates**: `flatten_toml(env)` recursively flattens the env TOML into a `{key: str}` namespace (booleans → `"true"`/`"false"`; lists skipped), then `string.Template.substitute(namespace)` expands every `*.toml` in `install_dir/etc/`. Undefined placeholder → hard exit with file name and key.
3. **Generate TLS certs**: for each `[tls.*]` section, looks up the matching component's `workdir`, resolves cert/key paths, generates a 2048-bit self-signed RSA cert via `openssl req -x509` (3650-day validity, `/CN=localhost`). Deduplicates pairs sharing the same cert file. Skipped if cert+key already exist (use `--force-certs` to regenerate). Pass `--skip-certs` when deploying real CA-signed certs.
4. **Create database**: delegates to `db/create_db.py` with `[db]` section values. `--drop-db`, `--sudo-postgres`, `--liquibase-contexts` forwarded.
5. **Export credentials**: delegates to `db/export_credentials.py`.

Options: `--artefact`, `--env`, `--install-dir`, `--skip-certs`, `--force-certs`, `--skip-db`, `--drop-db`, `--sudo-postgres`, `--liquibase-contexts`.

**`environments/prod.toml` — production environment configuration.**

Mirrors `dev.toml` structure; every field that requires a real value is marked `# REPLACE`. Key differences:

| Setting | dev | prod |
|---|---|---|
| `paths.install_dir` | `installed` | `/opt/pubsub` |
| `paths.log_dir` | `installed/log` | `/var/log/pubsub` |
| `paths.run_dir` | `/var/tmp/pubsub/run` | `/var/run/pubsub` |
| WAL directories | `/var/tmp/pubsub/sequencer*_wal` | `/var/lib/pubsub/sequencer*_wal` |
| Listen hosts | `127.0.0.1` | `0.0.0.0` |
| Connect hosts | `127.0.0.1` | `*.exchange.internal` (REPLACE) |
| `reactor_cpu_pinning_reserve_cpu0` | `true` | `false` (isolated CPUs) |
| TLS `ca` field | `""` | `"ca.crt"` (REPLACE with CA cert) |
| SCRAM password | `"stubpassword"` | `"REPLACE_WITH_GATEWAY_SCRAM_PASSWORD"` |

Production deploy workflow: edit `prod.toml` with real hostnames, place CA-signed certs, set `PUBSUB_APP_DB_PASSWORD`, then:
```
./deploy.py --env environments/prod.toml \
            --artefact pubsub-<ver>.tar.gz \
            --install-dir /opt/pubsub \
            --skip-certs
```

**`environments/preprod.toml` and `environments/test-1.toml` — additional environment configurations.**

`preprod.toml` is structurally identical to `prod.toml` — same paths, CA-signed TLS, `cpu_pinning_reserve_cpu0 = false` — with hostnames in the `*.preprod.exchange.internal` namespace. Intended for final release validation under near-production conditions before promotion.

`test-1.toml` targets a dedicated test cluster with full HA enabled. Differs from preprod/prod in two ways: `cpu_pinning_reserve_cpu0 = true` (test machines are not CPU-isolated) and self-signed TLS (`ca = ""`, deploy.py generates certs — no CA required). Hostnames in the `*.test-1.exchange.internal` namespace. Additional test environments follow the same pattern (`test-2.toml`, etc.).

Environment comparison:

| Setting | `dev` | `test-1` | `preprod` | `prod` |
|---|---|---|---|---|
| `ha.enabled` | `true` (overridable with `--no-ha`) | `true` | `true` | `true` |
| Topology | single machine | multi-machine | multi-machine | multi-machine |
| `cpu_pinning_reserve_cpu0` | `true` | `true` | `false` | `false` |
| TLS CA verification | none (`ca=""`) | none (`ca=""`) | CA cert required | CA cert required |
| TLS certs | self-signed by deploy.py | self-signed by deploy.py | CA-signed, `--skip-certs` | CA-signed, `--skip-certs` |
| Hostnames | `127.0.0.1` | `*.test-1.exchange.internal` | `*.preprod.exchange.internal` | `*.exchange.internal` |

---

### Session 22

**`auth_service_test.py` — `--tls` flag.** Without `--tls`, the service starts with no TLS admin section and scenarios 1–3 (plain-PDU SCRAM) are the only valid choices. With `--tls`, all 5 scenarios are available; requesting scenario 4 or 5 without the flag exits with an error message. `_wait_for_service_ready` now takes an explicit `marker` argument; `_SERVICE_READY_MARKER_PDU` and `_SERVICE_READY_MARKER_TLS` are separate constants. `_write_test_toml` only emits the `[admin]` TLS section when `tls_mode=True`.

**PostgreSQL database setup and schema (`db/`).**

- `db/create_db.py` — idempotent Python script: (1) creates `pubsub_app` role with `DO $$ IF NOT EXISTS … CREATE ROLE … ELSE ALTER ROLE …`; (2) skips `CREATE DATABASE` if `pubsub` already exists (checked via `pg_database`); (3) runs `liquibase update`. Omits `--host` for localhost connections (peer auth; TCP password prompting avoided). Options: `--pg-superuser <name>` (default `postgres`), `--sudo-postgres`, `--drop-existing`. JDBC driver is expected at `/opt/liquibase/lib/postgresql*.jar`; script checks and exits with instructions if absent — no network downloads (appropriate for corporate environments). Liquibase is invoked with `--search-path=<script_dir>` and `--changeLogFile=changelog/db.changelog-root.xml` (Liquibase 5.x requires changeLogFile relative to searchPath).
- `db/liquibase.properties` — datasource config; password from `${env.PUBSUB_APP_DB_PASSWORD}`.
- `db/changelog/db.changelog-root.xml` — root changelog; property `tablePrefix = pubsub_`; includes `v1_initial_schema.xml`.
- `db/changelog/v1_initial_schema.xml` — three changesets, all using `${tablePrefix}`:
  - `v1-001-create-firm`: `firm_id varchar(32) PK`, `name varchar(255) NOT NULL`, `enabled boolean DEFAULT true NOT NULL`, `created_at/updated_at timestamptz DEFAULT now() NOT NULL`.
  - `v1-002-create-comp-id`: `comp_id varchar(64) PK`, `firm_id FK→firm NOT NULL`, SCRAM fields `stored_key/server_key/salt varchar(64) NOT NULL`, `iterations integer NOT NULL`, `enabled DEFAULT true`, `force_password_change DEFAULT true`, `consecutive_failed_logins DEFAULT 0`, `locked DEFAULT false`, `locked_reason varchar(255)` (nullable), `locked_at/last_login_at/password_changed_at timestamptz` (nullable), `created_at/updated_at NOT NULL`.
  - `v1-003-create-comp-id-gateway-permission`: `comp_id FK→comp_id`, `gateway_type varchar(64)`, `enabled DEFAULT true`, `created_at`; composite PK `(comp_id, gateway_type)`. Gateway type is free text (e.g. `order`, `drop_copy`, `risk`).

**Java admin service (`java/admin-service/`).**

A standalone Javalin 6 + Freemarker 2.3 + plain JDBC (HikariCP) web application. Technology choices: Javalin for HTTP (not Spring — never Spring), Pico.css classless CSS from CDN, Freemarker for server-rendered templates, HikariCP + PostgreSQL JDBC for database access, Maven Shade plugin for a fat JAR. Java 17.

*Package layout:*
- `Config` — loads `application.properties` from classpath; overrides `db.url`, `db.username`, `db.password` from env vars `PUBSUB_DB_URL`, `PUBSUB_DB_USERNAME`, `PUBSUB_APP_DB_PASSWORD`. Fields include `tablePrefix`, `authServiceEnabled`, `authServiceHost`, `authServiceAdminPort`, `serverPort`.
- `model/`: `FirmRow`, `CompIdRow` (SCRAM fields present but never rendered in templates), `GatewayPermissionRow`.
- `exception/`: `NotFoundException` (404), `ConflictException` (409).
- `db/`: `Database` (HikariCP factory), `FirmDao`, `CompIdDao`, `GatewayPermissionDao` — plain JDBC, explicit column lists, `PreparedStatement` for all data. `CompIdDao.insert` requires a `ScramCredential`; `updateStatus` sets `enabled/locked/forcePasswordChange/lockedReason` and manages `locked_at` via a SQL CASE expression; `updateCredentials` resets `force_password_change=false`, `consecutive_failed_logins=0`, updates `password_changed_at`.
- `service/`: `ScramCredential` (record: `storedKey`, `serverKey`, `salt`, `iterations` — all hex strings matching the format used in `auth_service_test.py`). `ScramDerivation.derive(password, iterations)` — PBKDF2WithHmacSHA256 → HMAC-SHA256(saltedPassword, "Client Key") → SHA256(clientKey) for storedKey; random 16-byte salt encoded as 32-char hex. `AuthServiceClient.setCredential(compId, password, iterations)` sends `SetCredentialRequest` (PDU 510) over TLS: 24-byte big-endian header (canary `0xC0FFEE00`) + little-endian payload (i64 requestId, string compId, string password, i32 iterations); reads `SetCredentialResult` (PDU 511); validates requestId and outcome. One TLS connection per call; server cert not verified (admin channel, internal network only).
- `web/`: `FirmHandler`, `CompIdHandler`, `GatewayPermissionHandler`. Methods match Javalin's `Handler` interface (`throws Exception`). SCRAM iterations fixed at 4096. On password set: derive SCRAM → write DB → call auth service (when `authServiceEnabled=true`).
- `Main` — Javalin routes and global exception handlers.

*Routes:* `GET/POST /firms`, `/firms/new`, `/firms/{id}`, `/firms/{id}/delete`; `GET /comp-ids` (all or `?firmId=X`); `GET/POST /firms/{firmId}/comp-ids/new`, `/firms/{firmId}/comp-ids`; `GET/POST /comp-ids/{id}`, `/comp-ids/{id}/delete`, `/comp-ids/{id}/password`; `GET/POST /comp-ids/{id}/gateways`, `/comp-ids/{id}/gateways/{type}/delete`.

*Freemarker templates* (under `src/main/resources/templates/`): `layout.ftl` (Pico.css nav macro), `error.ftl`, `firms/list.ftl`, `firms/form.ftl` (create and edit via `<#if firm?>` branching), `comp-ids/list.ftl`, `comp-ids/form.ftl` (create and edit), `comp-ids/set-password.ftl`, `gateway-permissions/list.ftl` (list + inline add form).

*Build tooling added to `pom.xml`:*
- `maven-checkstyle-plugin:3.3.1` — bound to `validate` phase; custom `checkstyle.xml` (unused imports, need braces, empty catch exemption for `ignored`-named variables, etc.). Zero violations.
- `spotbugs-maven-plugin:4.8.6.0` — bound to `verify` phase; effort Max, threshold Medium.
- `dependency-check-maven:10.0.4` (OWASP Dependency Check) — **not** bound to the build lifecycle; run manually with `mvn dependency-check:check`. Requires NVD database download or a local mirror; `failBuildOnCVSS=7`.

Note on startup flow: the auth service never accesses the database directly. Before starting the auth service, a separate export script (not yet written — see item 8 in "What Is Not Yet Done") reads `pubsub_comp_id` from the DB and writes SCRAM credentials into the auth service TOML. Live credential changes made through the admin UI are pushed immediately via `SetCredentialRequest` (PDU 510) without restarting the auth service.

Build: `mvn compile` succeeds (17 source files, 0 errors); `mvn checkstyle:check` passes with 0 violations.

---

### Session 21

**Java DSL code generator — Java backend added.**

The DSL front end (lexer, parser, validator, AST) is language-agnostic and shared. Three new files add Java as a second code-generation target alongside C++:

- `python/dsl/generator_java.py` — `JavaGenerator(class_name, package_name)` dataclass. Emits a single public final outer class that acts as a namespace container for one public enum per DSL enum and one `public static final` inner class per DSL message. Wire format is little-endian throughout, matching the C++ generator; `ByteBuffer.order(ByteOrder.LITTLE_ENDIAN)` is applied at the start of every encode and decode call.

  Each message inner class contains: public fields with zero/empty defaults; `public static int encodedSize(MsgType msg)`; `public static int encode(MsgType msg, ByteBuffer buf)` (returns bytes written, or -1 if buffer too small — checked via a single `encodedSize` pre-flight); `public static MsgType decode(ByteBuffer buf)` (saves position, delegates to `_decodeFields`, resets position and returns null on `BufferUnderflowException`); `static MsgType _decodeFields(ByteBuffer buf)` (package-private, throws on underflow — called by nested-message decode paths so exceptions propagate to the outermost `decode()` catch without corrupting the position).

  Type mappings: DSL `char` → Java `byte` (not Java `char` which is 2-byte UTF-16); `i8`→`byte`, `i16`→`short`, `i32`→`int`, `i64`→`long`, `bool`→`boolean`, `datetime_ns`→`long`, `string`→`String`, `bytes`→`byte[]`. Lists map to Java arrays (`T[]`); nested lists give `T[][]` etc. with correct `new T[n][]` allocation syntax. Enums carry a `public final int value` field (or `long` for i64-backed enums), a `fromValue(v)` factory, and `wireSize()` returning the underlying type's byte count.

- `python/tools/generate_java_from_dsl.py` — thin wrapper script mirroring `generate_cpp_from_dsl.py`. Takes positional `input.dsl` and `output.java` arguments (class name inferred from the output file stem) plus `--package com.example.app` for an optional package declaration.

- `python/tests/test_generator_java.py` — 47 tests covering: outer class and package wrapping, all primitive type mappings, `char`→`byte` (not Java char), enums (entries, value, fromValue, wireSize, i64 long type), optional fields with `has_X` pattern, string/bytes fields, list and array fields, nested messages, nested lists, encode capacity check, `BufferUnderflowException` handling, `ByteOrder.LITTLE_ENDIAN`, and the `_decodeFields` / `decode` method split.

Build: `make all` passes pylint 10.00/10, 203/203 tests.

---

### Session 20

**SCRAM-SHA-256 authentication service — full end-to-end implementation.**

The system now requires every FIX client to complete a SCRAM-SHA-256 challenge-response exchange before a FIX session is established. The exchange happens over the internal PDU path (not over the FIX connection), using four new PDU types defined in `applications/authentication.dsl`:

| PDU ID | Name | Direction |
|---|---|---|
| 500 | `AuthenticationRequest` | gateway → auth service |
| 501 | `AuthenticationChallenge` | auth service → gateway |
| 502 | `AuthenticationProof` | gateway → auth service |
| 503 | `AuthenticationResult` | auth service → gateway |

`AuthenticationRequest` carries `request_id` (= the gateway's internal `ConnectionID` for the FIX session, used to correlate all four messages), `comp_id`, and a random 16-byte `client_nonce`. `AuthenticationChallenge` returns `server_nonce` (server-appended bytes concatenated with client_nonce), `salt`, and `iterations`. `AuthenticationProof` carries the SCRAM `ClientProof`. `AuthenticationResult` carries `outcome` (Granted/Denied), `server_signature` (32 bytes, for mutual authentication), and `force_password_change` flag.

**`ScramCrypto` shared library** (`libraries/scram_crypto/`). Moved from `applications/authentication_service/` into a proper static library so both the authentication service and the gateway can link against it. Namespace `scram_crypto`. Exports: `hmac_sha256`, `sha256`, `pbkdf2_sha256`, `make_scram_credential`, `compute_auth_message`. Links against `OpenSSL::Crypto` (PRIVATE). `find_package(OpenSSL REQUIRED)` added to top-level `CMakeLists.txt`.

**Authentication service** (`applications/authentication_service/`). Stateless: each SCRAM exchange is fully self-contained with no server-side session state between Request and Proof. On receiving an `AuthenticationRequest`, the service generates a random 16-byte server nonce, derives SCRAM parameters from its stored credential for the comp_id, and replies with `AuthenticationChallenge`. On receiving `AuthenticationProof`, it verifies `ClientProof` via `StoredKey`, computes `ServerSignature` via `ServerKey`, and replies with `AuthenticationResult`. Currently uses a single stub credential (`stub_credential_`) for all comp_ids; credential database integration is the next major work item. Two instances are run for HA: primary on port 7070, secondary on port 7071. Both are stateless and independent — no synchronisation between them is needed.

**Gateway SCRAM integration** (`applications/order_gateway/`). On receiving a FIX Logon:
1. Cancels the logon timeout timer.
2. Selects the auth service connection: primary if connected, secondary as fallback (when `ha_enabled`).
3. If neither is connected, sends FIX Logout and disconnects.
4. Generates 16 random bytes as `client_nonce` (via `RAND_bytes`), sends `AuthenticationRequest`, sets `session.auth_pending = true`, and arms a `scram_auth_timeout` timer (default 10 s, configurable).
5. `handle_authentication_challenge`: derives `ClientProof` and `expected_server_signature` locally using `scram_crypto::pbkdf2_sha256`, `hmac_sha256`, `sha256`. Sends `AuthenticationProof` on the same connection the challenge arrived on (correct for HA routing).
6. `handle_authentication_result`: cancels the SCRAM timeout, verifies `ServerSignature` for mutual authentication, then either completes the FIX Logon reply or sends FIX Logout.
7. SCRAM timeout fires: sends FIX Logout and disconnects — session never hangs.

Key configuration additions to `FixGatewaySeqConfiguration`:
- `authentication_service_host / port` (primary, always required)
- `authentication_service_secondary_host / port` (required when `ha_enabled`)
- `scram_password` — the password sent to the auth service on behalf of FIX clients
- `scram_auth_timeout` — maximum time for a SCRAM exchange (default 10 s)

**HA test infrastructure update** (`ha_test.py`). Two authentication service instances (`authentication_service_primary` on port 7070, `authentication_service_secondary` on port 7071) added to the launch table before `order_gateway`. Binary preflight check extended to include `authentication_service`. Auth service log files added to stale-log cleanup. Startup order is now 9 processes: witness → arbiter_primary → arbiter_secondary → authentication_service_primary → authentication_service_secondary → order_gateway → sequencer_primary → sequencer_secondary → matching_engine.

**TLS subsystem added to the framework.** Two sessions of work (both within Session 20) added full TLS support to the raw-bytes connection layer.

*Inbound TLS* (`TlsRawBytesProtocolHandler`, `TlsContext`, `TlsState`, `TlsListenerConfiguration`, `ProtocolType::TlsRawBytes`). The `InboundListenerConfiguration` gains an optional `TlsListenerConfiguration`; when present the `Reactor` builds a `TlsContext` at init (loading certificates once) and constructs a `TlsRawBytesProtocolHandler` for each accepted connection. TLS is handled non-blockingly via OpenSSL memory BIOs — the reactor thread never blocks. `ConnectionEstablished` is not delivered until the handshake completes. Five integration tests cover the happy path, fragmented ciphertext, `close_notify`, mutual TLS, and deliberate handshake failure.

*Outbound TLS* (`TlsClientConfiguration`, `ServiceEndpoints::tls`, `OutboundConnection` and `OutboundConnectionManager` TLS paths). `ServiceEndpoints` gains `std::optional<TlsClientConfiguration> tls`. When present, the outbound manager calls `start_outbound_handshake()` after TCP connect and defers `ConnectionEstablished` until the TLS handshake completes. Four outbound integration tests cover the happy path, mutual TLS, server-initiated disconnect after handshake, and handshake failure with wrong trust anchor.

`ProtocolHandlerInterface` gains three new virtuals: `start_outbound_handshake()`, `is_handshake_complete()`, `is_reads_paused()`. Non-TLS handlers return safe defaults.

Status: framework complete, tested (9 integration tests). Not yet wired to any application — the gateway FIX listener and auth service still use plain TCP. See subsystem section 16 for full detail.

**`build.py` improvements.** Pylint always runs (on `python/dsl/`) before CMake. Pytest runs by default; suppressed by `--no-tests` or the new `--no-pytest` flag. This catches Python DSL regressions before the slower C++ build begins.

**All 13 HA scenarios pass.** `auth_service_test.py` (3 scenarios: single exchange, sequential exchanges on one connection, multiple independent clients) all pass. Build clean, all unit and integration tests pass.

**Database access — design discussion (not yet implemented).** See "Database Access Design" section below.

---

### Session 19

**Quill SPSC queue initial capacity raised to 32 MiB (`PubsubFrontendOptions`).** Under high-throughput runs the default 128 KiB Quill SPSC queue was doubling seven times (reaching 32 MiB) before stabilising, printing an INFO reallocation message on each doubling. A new `PubsubFrontendOptions` struct (`libraries/pubsub_itc_fw/include/pubsub_itc_fw/PubsubFrontendOptions.hpp`) sets `initial_queue_capacity = 32 MiB` to match the observed worst case and eliminates all reallocation. Queue type remains `UnboundedBlocking` so producers block rather than drop if the queue ever exceeds the 2 GiB cap. Physical memory is only touched as the queue fills — the 32 MiB is virtual address space reserved up-front. A companion `QuillLoggerFrontendOptions.hpp` wires the options into the logger type. Files: `PubsubFrontendOptions.hpp` (new), `QuillLoggerFrontendOptions.hpp` (new), `QuillLogger.hpp`, `QuillLogger.cpp`.

**SequencerThread WAL routing map removed from replay.** WAL replay on restart was rebuilding the `seq_no → gateway_session_conn_id` routing map. This was incorrect: after a restart the ME has already sent ERs for any in-flight orders from the previous run and those ERs will not be re-sent. Keeping stale entries caused unbounded heap growth under high throughput. Fix: the WAL `open()` replay callback is now `nullptr`; replay only recovers `next_sequence_number_`. ER PDUs whose seq_no is not in the routing map are handled gracefully by the existing "not in routing map" fallback. The log message on successful WAL recovery was simplified accordingly. File: `applications/sequencer/SequencerThread.cpp`.

**`FixErEncoder` — zero-allocation outbound ER encoder.** The outbound Execution Report path had been using `FixSerialiser`, which builds an `unordered_map<int, string>` inside a `FixMessage` and allocates on every ER sent. Profiling identified this as 3.34% of gateway CPU (heap allocation in libc). Replaced with a new `FixErEncoder` class (`FixErEncoder.cpp`, `FixErEncoder.hpp`) that writes directly to a caller-supplied fixed-size buffer via a `FixWireWriter` cursor/limit helper. No heap allocation on the hot path. `FixGatewaySeqThread` now calls `FixErEncoder::encode()` directly instead of constructing a `FixMessage`. Files: `FixErEncoder.cpp` (new), `FixErEncoder.hpp` (new), `FixGatewaySeqThread.cpp`.

**`ApplicationThreadTest.MessageProcessing` flakiness fix.** The test used a 200 ms startup wait before sending messages. On a loaded CI system this was too tight — the application thread had not yet reached its event loop. Increased to 5000 ms. File: `libraries/pubsub_itc_fw/tests/ApplicationThreadTest.cpp`.

**Performance test infrastructure.** Added `monitor_memory.py` (memory monitor for multi-process test runs) and `perf_run.sh` (shell driver for `perf record` runs). Various fixes to `perf_run.py`: call-graph mode switched from `fp` to `dwarf` (frame-pointer unwinding lost kernel frames), SIGKILL issued immediately on successful test completion.

**Burst=50 end-to-end test.** 50 fix8 clients × burst=50 × 1000 orders = **2,500,000 orders** processed end-to-end with no drops. `matching_engine.log` contained exactly 5,000,000 entries (NOS + ER per order). Pool-exhaustion warnings in the log are benign — they indicate slab chaining working correctly, not allocation failures. Initial "Connection refused" and end-of-run "gateway connection lost" messages are expected startup/shutdown races.

**Build and test status at session end:** all unit tests pass, all integration tests pass. Burst=50 test clean.

---

### Session 18

**Unit and integration test coverage improvements — PduParser, OutboundConnectionManager, and coverage tooling fix.**

**New unit tests: `PduParserTest.cpp`** — 6 tests covering PduParser error paths not exercised by the existing `PduFramerParserTest.cpp`:
- `ZeroLengthPayloadReturnsError` — `byte_count = 0` in header rejected before allocation.
- `OversizedPayloadReturnsError` — `byte_count > slab_size()` rejected before allocation.
- `DisconnectDuringPayloadCallsHandlerAndReturnsFalse` — valid header then peer disconnect calls the disconnect handler and returns `{false, ""}` (empty error = graceful close).
- `ReadErrorDuringPayloadReturnsFalse` — valid header then `ECONNRESET` returns `{false, non-empty}`.
- `EagainDuringPayloadResumesOnNextCall` — EAGAIN on first call returns `{true,""}` with no PDU dispatched; payload arrives on second call and PDU is dispatched correctly.
- `ReadErrorDuringHeaderReturnsError` — `ECONNRESET` with no bytes queued returns `{false, non-empty}`.

A `PduParserTestStream` stub with priority ordering (buffered bytes → disconnect → error → EAGAIN) was written to enable injecting errors at the payload phase after delivering the header. Added to `libraries/pubsub_itc_fw/tests/CMakeLists.txt`.

**New unit tests: `OutboundConnectionTest.cpp`** — 7 additional tests on the `OutboundConnectionManagerTest` fixture covering previously-uncovered paths:
- `RetryFailedConnectionsNoOpWhenEmpty` — calling `retry_failed_connections` with no pending retries is a no-op.
- `ProcessSendRawCommandReturnsFalse` — outbound connections reject SendRaw commands.
- `ProcessCommitRawBytesReturnsFalseForUnknownId` / `ReturnsTrueForKnownId` — commit path.
- `FindByIdReturnsNullForUnknownId` — unknown ConnectionID lookup.
- `ProcessDisconnectCommandTeardownsConnection` — teardown + second call returns false.
- `OnDataReadyParseErrorTeardownsConnection` — bad canary triggers teardown via `on_data_ready`.

**New integration test: `OutboundConnectionRetryIntegrationTest.cpp`** — end-to-end proof that `retry_failed_connections` re-issues `process_connect_command` after a connect timeout. A `RetryCountingThread` accumulates `connection_failed` events without shutting down; the test verifies `failed_count >= 2` (initial timeout + at least one retry). Uses `192.0.2.1:9999` (TEST-NET, non-routable) with a 50 ms `connect_timeout` and 1 ms `connect_retry_interval`. Added to `libraries/pubsub_itc_fw/integration_tests/CMakeLists.txt`.

Note: unit tests and integration tests go in separate directories (`tests/` and `integration_tests/` respectively). The retry test requires a live reactor event loop and belongs in `integration_tests/`.

**Coverage tooling fix — `--erase-functions FMT_COMPILE_STRING` in `build.py`.** The `lcov --omit-lines` option only removes *line* coverage entries (DA records); it does not touch *function* entries (FN/FNDA records). The `FMT_STRING(fmt)` macro used inside `PUBSUB_LOG` for compile-time format validation expands to an immediately-invoked lambda containing a `struct FMT_COMPILE_STRING`. GCC emits these as real callable functions in the object file, and gcov records them as uncovered functions (they live inside `if (false)` blocks). They inflate the function-coverage denominator significantly.

Fix: added `--erase-functions FMT_COMPILE_STRING` to the `lcov --remove` step in `build.py`. This removes all FN/FNDA records whose mangled name contains `FMT_COMPILE_STRING`, eliminating 253 phantom entries project-wide.

Results after fix:

| Class | Before | After |
|---|---|---|
| `TimerHandler` | 4/13 (30.8%) | 4/4 (100%) |
| `InboundConnectionManager` | 16/35 (45.7%) | 16/16 (100%) |
| `OutboundConnectionManager` | 16/35 (45.7%) | 16/16 (100%) |
| `PduParser` | 5/10 (50%) | 5/5 (100%) |
| **Overall** | **~74%** | **93.2% (894/959)** |

`--omit-lines` is retained in the filter step as it still removes phantom line coverage entries from the `if (false)` validate blocks.

Files changed: `libraries/pubsub_itc_fw/tests/CMakeLists.txt`, `libraries/pubsub_itc_fw/tests/PduParserTest.cpp` (new), `libraries/pubsub_itc_fw/tests/OutboundConnectionTest.cpp`, `libraries/pubsub_itc_fw/integration_tests/CMakeLists.txt`, `libraries/pubsub_itc_fw/integration_tests/OutboundConnectionRetryIntegrationTest.cpp` (new), `build.py`.

**Build and test status at session end:** 485 unit tests pass (1 pre-existing skip), 21 integration tests pass. Coverage report: 93.2% function coverage, 86.9% line coverage.

---

### Session 17

**`ExpandableSlabAllocator` SIGSEGV fix — segmented atomic array.** `ConcurrentSmallSlabHighChurn` was crashing with SIGSEGV under ASan / TSan. Root cause: `std::vector<std::unique_ptr<SlabAllocator>>::push_back()` in `append_new_slab()` triggers reallocation — allocates a new backing array, moves elements, frees the old — while worker threads concurrently read raw pointers out of the old array via `deallocate()`. Freed-memory access; undefined behaviour.

Fix: replaced the vector with a two-level segmented atomic array.
- `std::atomic<Page*> pages_[kMaxPages]` (1024 directory slots, in-object, never moves or reallocates).
- Each `Page` is heap-allocated once and holds `std::atomic<SlabAllocator*> slots[kPageSize]` (256 slots). Pages are never freed during the allocator's lifetime.
- Slab id N maps to `pages_[N >> 8]->slots[N & 0xFF]`. Page 0 allocated in constructor; further pages allocated on demand in `append_new_slab`.
- Workers in `deallocate()` load page ptr with `acquire`, slot ptr with `acquire`. Reactor's `release` stores in `append_new_slab` guarantee visibility.
- `drain_empty_slab_queue` and `load_slab_reactor` use `relaxed` loads (reactor-thread only).

The `ConcurrentSmallSlabHighChurn` test runs for `test_duration = std::chrono::seconds(5)` by design — 5-second runtime is intentional.

Files changed: `ExpandableSlabAllocator.hpp`, `ExpandableSlabAllocator.cpp`.

**Slices 1–5 of the WAL+HA plan implemented (spanning earlier sessions, documented here).**

- **Slice 1 — seqNo on wire and in `EventMessage`.** `PduHeader` gains an `int64_t seq_no` field; `EventMessage::create_framework_pdu_message` takes `seq_no`; `SequencerThread` routes `next_sequence_number_` into the field when re-encoding for forwarding. ME and gateway see seqNos.
- **Slice 2 — In-memory WAL.** Sequencer maintains an in-memory log (`SequencerWal`) of every committed order.
- **Slice 3 — mmap'd WAL on disk, segmented, no fsync.** `SequencerWal` writes to `wal_NNNNNN.log` segments. On restart, WAL is replayed from segment 0 (or from snapshot anchor) to rebuild sequencer state. `WalEntryHeader`: `magic(4) | payload_size(4) | seq_no(8) | pdu_id(2) | filler_a(2) | filler_b(4)`. Each entry ends with CRC32. Corrupt/truncated entry stops replay; entries beyond are treated as "did not happen".
- **Slice 4 — Snapshot (single, no rolling).** `SequencerWal::take_snapshot()` writes `snapshot.bin` atomically (write to `.tmp`, rename). `SnapshotHeader`: `magic | version | last_seq_no | record_count | wal_segment | wal_offset`. On open, valid snapshot causes WAL replay to start from the snapshot anchor, bounding restart time to post-snapshot WAL size.
- **Slice 5 — `cl_ord_id → SenderCompID` routing map in sequencer.** Sequencer stamps `routing_comp_id` (the originating FIX client's `SenderCompID`) onto every forwarded ER PDU. Gateway looks up comp-id → current ConnectionID in its small comp-id table and routes the FIX ER accordingly. WAL replay rebuilds the routing map on restart. Gateway's `cl_ord_id_to_session_` map now lives in the sequencer.

**Slice 6 — Leader-follower HA state machine.** Implemented in `SequencerThread`:
- `Role` enum: `unknown`, `leader`, `follower`.
- `adopt_role(role)`: transitions into the assigned role; logs the change. Only the leader forwards order PDUs to ME (`if (role_ != Role::leader) { release_pdu_payload(message); return; }`).
- Startup election via `StatusQuery`/`StatusResponse` on peer connect.
- `peer_heartbeat_timeout` timer: fires if peer does not complete election within the startup window; node self-promotes to leader.
- Epoch tracking: `epoch_` incremented on self-promotion; propagated in `StatusQuery`, `StatusResponse`, `Heartbeat`.
- Fence file written when becoming leader (single-host split-brain protection).
- New `SequencerConfiguration` fields: `ha_enabled{false}`, `startup_election_timeout_seconds{3}`, `heartbeat_timeout_seconds{15}`.

**Slice 6 regression fix — `ha_enabled` flag for single-node mode.** After Slice 6 landed, the sequencer started in `Role::unknown` and silently dropped every inbound order PDU until the election completed. With no secondary running, only the `peer_heartbeat_timeout` path fires — originally 15 seconds. Any NOS within that window received no reply.

Fix: `ha_enabled` flag (default false) added to both sequencer and gateway configs. When false:
- Sequencer `on_initial_event`: immediately `adopt_role(leader)` (epoch 1). No election timer. No arbiter/peer connects.
- Gateway `on_app_ready_event`: skips `connect_to_service("sequencer_secondary")`.
- Gateway `forward_pdu_to_sequencers`: secondary branch guarded by `config_.ha_enabled`.
- Gateway config loader: secondary host/port only parsed when `ha_enabled=true`.
- `SampleFixGatewaySeq.cpp`: secondary service-registry entry only added when `ha_enabled=true`.

Also: three connection-retry/failure log lines in `OutboundConnectionManager.cpp` demoted from `Warning` to `Info`.

Files changed (Slice 6 + regression fix):
- `applications/sequencer/SequencerConfiguration.hpp`
- `applications/sequencer/SequencerConfigurationLoader.cpp` (added `#include <tuple>` for `std::ignore`)
- `applications/sequencer/SequencerThread.cpp`
- `applications/sequencer/SequencerThread.hpp`
- `applications/order_gateway/FixGatewaySeqConfiguration.hpp`
- `applications/order_gateway/FixGatewaySeqConfigurationLoader.cpp`
- `applications/order_gateway/FixGatewaySeqThread.hpp`
- `applications/order_gateway/FixGatewaySeqThread.cpp`
- `applications/order_gateway/SampleFixGatewaySeq.cpp`
- `libraries/pubsub_itc_fw/src/OutboundConnectionManager.cpp`

**Build and test status at session end:** all unit tests pass including `ConcurrentSmallSlabHighChurn`. End-to-end verified: fix8 NOS → gateway → sequencer (immediately leader with `ha_enabled=false`) → ME → sequencer → gateway → fix8 ER. No silent drops.

---

### Session 16

**Slab-queue race condition in `EmptySlabQueue` / `ExpandableSlabAllocator` diagnosed and fixed.** Under heavy fix8 T-burst load on the integrated gateway/sequencer, the reactor would wedge in `ExpandableSlabAllocator::drain_empty_slab_queue` with the tripwire firing after ~1 second. Diagnostic output showed a state that looked structurally impossible: `head_ = &slab_N's_queue_node` (a real slab), `tail_ = &dummy_`, `head_->next = nullptr` — persistent across millions of retry iterations. The consumer was stuck because `head_->next` would never become non-null (no producer can ever set `slab_N's_next` once `tail_` no longer points at `&slab_N's_node`).

**Diagnostic infrastructure added then removed.** Four `peek_*` diagnostic accessors on `EmptySlabQueue` (`peek_head`, `peek_head_next`, `peek_tail`, `peek_dummy`) were added to let the drain function and tests inspect the queue's state without mutating it. The accessors were retained in the final fix; the `fprintf` blocks inside `drain_empty_slab_queue` were used to capture the smoking-gun trace then removed. The trace showed the failing drain's ENTRY line read `head=&dummy_, head->next=&slab_9_node, tail=&dummy_` — proving a producer's `prev->next.store(node)` had set `dummy_.next` but the producer's `tail_.exchange(node)` had been clobbered back to `&dummy_`. Only `reset_to_empty` writes `&dummy_` to `tail_`, so the consumer's `reset_to_empty` must have interleaved with the producer's enqueue.

**Root cause.** `EmptySlabQueue::reset_to_empty()` did three non-atomic stores: `dummy_.next.store(nullptr); head_ = &dummy_; tail_.store(&dummy_)`. Between the consumer's Empty observation in `try_dequeue` and the `tail_.store(&dummy_)` inside `reset_to_empty`, a producer could complete its own `tail_.exchange(node)`. The consumer's subsequent store clobbered the producer's exchange. The producer's later `prev->next.store(node)` then wrote `dummy_.next = node` (because `prev` was `&dummy_` at the producer's exchange time), but `tail_` was back at `&dummy_`. Result: a "ghost-enqueued" slab visible via `head_->next` but unreachable from `tail_`. Over time, multiple such race-clobbered enqueues accumulated; when the consumer finally GotItem'd one, the resulting state was the wedged head=slab/tail=dummy pattern. The `is_enqueued_` one-shot CAS meant the ghost-enqueued slabs could never be re-enqueued either.

**Fix: classical Vyukov sentinel pattern with one-drain-deferred reclamation.**
- `EmptySlabQueue::reset_to_empty()` removed entirely — the function and its declaration. The classical Vyukov pattern doesn't need it.
- `ExpandableSlabAllocator` gained a `deferred_reclaim_slab_id_{-1}` member. The most-recently-popped slab in any drain stays alive as the queue's sentinel; head_ and tail_ point at it after the drain. On the NEXT drain, when a successful GotItem confirms head_ has advanced past the deferred slab, the deferred slab is finally destroyed and the new last-popped becomes the next deferred. This relies on the invariant that for head_ to advance past a node N, the producer who put N's successor in front of head_ must have completed its store (since head_->next.load returned non-null), and that producer is the *only* producer that ever held N's_node as `prev` (the Vyukov MPSC exchange guarantees this).
- The drain-loop tripwire was switched from iteration-count (100,000 — meaningless at modern CPU speeds; iteration time was nanoseconds, so the budget was microseconds) to wall-clock (one second) which is a true safety net for the now-much-rarer case of a genuinely stuck producer.

**Why this fix is correct (not just a workaround).** Producers never need `reset_to_empty` — they thread their nodes onto `tail_` and never read or write `head_`. The only reason `reset_to_empty` was there was to prevent head_/tail_ from pointing at memory that's about to be destroyed during reclamation. The Vyukov sentinel pattern handles that naturally by keeping the most-recently-popped node alive. The author of the original code worried about a self-loop hazard (same node enqueued twice while head_ still pointed at it), but that hazard is already prevented by the `is_enqueued_` one-shot CAS in `SlabAllocator::try_claim_enqueue` — a slab's node can only be enqueued once per slab lifetime, and after destruction a new slab gets a fresh node.

**Reproducer test written.** `ExpandableSlabAllocatorStressTest` (initially a separate fixture, later subsumed into `ExpandableSlabAllocatorTest`) was added: one reactor thread allocates and posts to a bounded blocking work queue; N worker threads pop and deallocate. Small slab and chunk sizes force frequent slab switches and empty-slab notifications, maximising pressure on the lock-free queue. Two test cases (`ConcurrentAllocateAndDeallocateMakesProgress` at slab/chunk 512/64 with 8 workers; `ConcurrentSmallSlabHighChurn` at 1024/32 with 16 workers); each runs for 5 wall-clock seconds and treats any allocator exception as a test failure. The test reproduced the bug in 1.1 seconds on the first run, providing a tight feedback loop for the fix.

**Three pre-existing tests broken by the new two-drain destruction lifecycle, all rewritten.** The fix means a slab destroyed by the queue is destroyed in the drain AFTER the one that popped it. Tests that previously asserted "deallocate against the popped slab's ID throws PreconditionAssertion" needed an additional allocate-deallocate cycle to push the slab through both drain phases. Affected tests: `ExpandableSlabAllocatorTest.OldSlabIsDestroyedAfterChaining`, `ExpandableSlabAllocatorTest.DeallocateDestroyedSlabThrows`, and the merged-in `DestroyedSlabIdThrowsOnDeallocateCrossThread` (formerly `ExpandableSlabAllocatorAdversarialTest.DestroyedSlabIdThrowsOnDeallocate`). All three now use `chunk_size == slab_size` so each `allocate()` chains a fresh slab deterministically, and walk through two drain cycles before asserting destruction.

**Tangential bug found and fixed in `ExpandablePoolAllocatorTest.AbaStressTest`.** Under Valgrind, the test was reporting "free-list corruption: unknown pointer 0x...". Initial theory (pool expansion firing because `pool_slots == num_threads` left no headroom) was partially right but the deeper bug was test-design: with `pool_slots = num_threads`, even after bumping by one to `num_threads + 1`, only `num_threads` slots are ever reachable by the workers (LIFO pop_back from a 5-slot pool with 4 threads holding at most one each leaves slot 0 perpetually untouched at the front of the vector). The drain phase still produced all 5 slots, one of which had never been recorded in `valid_addresses`, giving a phantom-address failure. Fix: pre-touch every slot at test setup (allocate all `pool_slots` in one go before stress so every slot's address is recorded), plus the `pool_slots = num_threads + 1` adjustment to ensure expansion never fires. Diagnosed by adding a one-line stderr print of `number_of_pools`, `total_capacity`, `valid_addresses.size`, and `behaviour_statistics.expansion_events` immediately before the assertion loop, which showed `expansion_events=0` and `valid_addresses.size=4` for a 5-slot pool — confirming the address-coverage gap rather than the expansion race. Test now passes under Valgrind with 500 repeats × 50,000 iterations (~hundred million allocate/deallocate pairs).

**Test file reorganisation.** The user requested two structural rules: (1) one fixture per class under test, (2) one fixture per file. The previous layout was:
- `SlabAllocatorTest.cpp` contained three fixtures: `EmptySlabQueueTest`, `SlabAllocatorTest`, `ExpandableSlabAllocatorTest`.
- `SlabAllocatorAdversarialTest.cpp` contained three more: `EmptySlabQueueAdversarialTest`, `SlabAllocatorAdversarialTest`, `ExpandableSlabAllocatorAdversarialTest`.
- `ExpandableSlabAllocatorTest.cpp` contained `ExpandableSlabAllocatorTest` (a second copy, with different tests) and `ExpandableSlabAllocatorStressTest`.

After reorganisation:
- `EmptySlabQueueTest.cpp` — one fixture, 8 tests (6 original + 2 ex-adversarial).
- `SlabAllocatorTest.cpp` — one fixture, 20 tests (15 original + 5 ex-adversarial).
- `ExpandableSlabAllocatorTest.cpp` — one fixture, 37 tests (12 from the old `SlabAllocatorTest.cpp` + 21 from the previous `ExpandableSlabAllocatorTest.cpp` + 2 ex-stress + 4 ex-adversarial; one duplicate `DeallocateNullptrThrows` vs `DeallocateNullPtrThrows` dropped).
- `SlabAllocatorAdversarialTest.cpp` deleted entirely.

The colliding test names `AdversarialDestroyedSlabIdThrowsOnDeallocate` and `DeallocateDestroyedSlabThrows` were disambiguated by renaming the cross-thread variant to `DestroyedSlabIdThrowsOnDeallocateCrossThread`. All other ex-adversarial tests had the `Adversarial` prefix simply dropped; no other name collisions.

**Documentation correction in `FixedSizeMemoryPool.hpp`.** The `Slot<T>` docblocks (two of them: production and Valgrind paths) and the top-of-file CANARY DESIGN section claimed "Both the Valgrind and production Slot<T> definitions are identical in layout". They are NOT — the production path has `free_next` between `is_constructed` and `canary`; the Valgrind path doesn't (it uses a mutex-protected `std::vector` free list). The pointer arithmetic in `get_is_constructed_for_object` and `get_canary_for_object` works in each build path independently via `offsetof(SlotType, storage)` — it doesn't depend on the two layouts being byte-for-byte equivalent. Updated all three comments to describe the actual invariants: `is_constructed` is first, `canary` is immediately before `storage`, `storage` is last.

**Verification.** All ~65 tests across the three reorganised files pass. The integrated gateway/sequencer/ME runs without the drain tripwire firing under sustained fix8 T-burst load. The user explicitly confirmed: "I also tried to make the gateway fall over with a heavy load via fix8 using loads of T commands. It all stayed up."

**Files changed:**
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/EmptySlabQueue.hpp` — `reset_to_empty` removed; four `peek_*` diagnostic accessors added.
- `libraries/pubsub_itc_fw/src/EmptySlabQueue.cpp` — `reset_to_empty` implementation removed.
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/ExpandableSlabAllocator.hpp` — `deferred_reclaim_slab_id_` member added; class-level docs updated.
- `libraries/pubsub_itc_fw/src/ExpandableSlabAllocator.cpp` — `reset_to_empty` call removed from drain loop; Vyukov sentinel deferred-reclamation logic added; iteration-count tripwire switched to wall-clock; `#include <chrono>` added.
- `libraries/pubsub_itc_fw/include/pubsub_itc_fw/FixedSizeMemoryPool.hpp` — three comment-only edits correcting the misleading "identical layout" claims about `Slot<T>`.
- `libraries/pubsub_itc_fw/tests/ExpandablePoolAllocatorTest.cpp` — `pool_slots = num_threads + 1` and pre-touch loop in `AbaStressTest`.
- `libraries/pubsub_itc_fw/tests/EmptySlabQueueTest.cpp` — new file containing the `EmptySlabQueueTest` fixture only.
- `libraries/pubsub_itc_fw/tests/SlabAllocatorTest.cpp` — replaced wholesale; now contains only the `SlabAllocatorTest` fixture.
- `libraries/pubsub_itc_fw/tests/ExpandableSlabAllocatorTest.cpp` — replaced wholesale; now contains the merged single `ExpandableSlabAllocatorTest` fixture with stress tests subsumed.
- `libraries/pubsub_itc_fw/tests/SlabAllocatorAdversarialTest.cpp` — deleted.

**Smaller deferred items noted during the session (none blocking):**
- `k`-prefix constants (`kSmallSlab`, `kLargeSlab`, `kPartial`) in the `ExpandableSlabAllocatorTest` fixture violate the project rule that constants are snake_case. Left alone in this session to avoid scope creep; mechanical search-and-replace when next convenient.
- The skipped `EmptySlabQueueTest.ReEnqueueOfSameNodeDoesNotCauseInfiniteSpin` test's comment was updated to remove the now-stale reference to `reset_to_empty` (which was supposed to mitigate the hazard but never did — the `is_enqueued_` one-shot CAS does).
- A few existing tests in the new `ExpandableSlabAllocatorTest.cpp` are functional near-duplicates (e.g. `OldSlabIsDestroyedAfterChaining` vs `OldEmptySlabIsDestroyed`; `DeallocateInvalidSlabIdThrows` vs `DeallocateOutOfRangeSlabIdThrows`). All kept per the user's "option (a)" choice — test coverage is cheap; names disambiguated where they collided.

### Session 15

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
- `order_gateway.toml` — `secondary_host`/`secondary_port` lines removed from `[sequencer]`.
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
- `applications/order_gateway/order_gateway.toml`
- `applications/order_gateway/FixGatewaySeqConfiguration.hpp`
- `applications/order_gateway/FixGatewaySeqConfigurationLoader.cpp`
- `applications/order_gateway/FixGatewaySeqConfigurationLoader.hpp`
- `applications/order_gateway/SampleFixGatewaySeq.cpp`
- `applications/order_gateway/SampleFixGatewaySeq.hpp`
- `applications/order_gateway/FixGatewaySeqThread.hpp`
- `applications/order_gateway/FixGatewaySeqThread.cpp`
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
- `applications/order_gateway/FixGatewaySeqThread.cpp` (`on_framework_pdu_message` implemented; `on_connection_lost` map sweep added; BumpAllocator include added)

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

**FIX parsing in `order_gateway`** — `FixParser`, `FixSerialiser`, `FixMessage`, `FixSession` added. Logon handling, heartbeats, preamble checking all working.

### Session 8

**`InboundConnectionManager`** — multi-connection support added (one-connection restriction removed). `on_accept` delivers `ConnectionEstablished` events. `check_for_inactive_connections` implemented.

**`OutboundConnectionManager`** — `check_for_timed_out_connections` implemented. `process_send_pdu_command`, `process_send_raw_command`, `process_disconnect_command` implemented.

**`ReactorConfiguration`** — `connect_retry_interval_` added (later used for retry). `connect_timeout` present.

**Application stubs** — `order_gateway`, `sequencer`, `matching_engine`, `arbiter` — all compiling with correct Aeron topology and startup pattern.

### Session 7

**DSL `char` field type** — added throughout: lexer, parser, validator, generator_cpp, generator_pybind11. Four pybind11 test failures fixed. `fix_equity_orders.dsl` created. Application architecture designed (Aeron sequencer pattern). Four application stubs written and compiling.

### Session 6

**`RawBytesProtocolHandler` bugs fixed** — intermittent `BurstDelivery` test failure resolved. `EventMessage::create_raw_socket_message` carries `tail_position`. Design documentation written. `order_gateway` tested with fix8. All 411 tests passing.

### Session 5

**Logging subsystem rewrite** — `QuillLogger` redesigned. `FwLogLevel` values flipped. `PUBSUB_LOG` and `PUBSUB_LOG_STR` are the only two call-site macros. All 12 test files migrated.

### Session 4

**`MirroredBuffer`**, **`ProtocolType`**, **`PduProtocolHandler`**, **`InboundConnectionManager`**, **`OutboundConnectionManager`**, **`ThreadLookupInterface`** — all implemented. `ExpandableSlabAllocator` use-after-free fixed.

### Session 3

TcpSocket EAGAIN/EOF fix, use-after-free fix, InboundConnection infrastructure, DSL generator fixes, integration test infrastructure, `ApplicationThread::get_reactor()` added.

---

## What Is Done

- Allocator subsystem — complete, tested, all races fixed. Session 16 fixed the `EmptySlabQueue::reset_to_empty` Vyukov-sentinel race (Vyukov deferred-reclaim with `deferred_reclaim_slab_id_`; test files reorganised one-fixture-per-file; ~65 tests). Session 17 fixed a second, independent race in `ExpandableSlabAllocator`: `std::vector::push_back()` reallocation freed the backing array while workers read raw pointers from it; replaced with a two-level segmented atomic array (`pages_[1024]` directory of heap-allocated `Page` structs, each with 256 `atomic<SlabAllocator*>` slots; pages never move; workers load with `acquire`, reactor stores with `release`).
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
- DSL code generator — complete; C++ and Java backends; `enum class` fix; `char` type; 203 tests passing. Java backend: `JavaGenerator(class_name, package_name)`, `generate_java_from_dsl.py` wrapper with `--package` option; 47 Java-specific tests.
- `fix_equity_orders.dsl` — FIX 5.0 SP2 equity order topic registry
- Logging subsystem — complete
- `RawBytesProtocolHandler` — complete; `on_data_ready`/`send_prebuilt`/`continue_send` return `tuple<bool, std::string>`; no disconnect-handler member; no logger member (session 14)
- TLS subsystem — complete, tested (session 20). `TlsContext` (wraps SSL_CTX; `create_server`/`create_client`; TLS 1.2 minimum, TLS 1.3 preferred; AEAD-only ciphers), `TlsState` (per-connection; memory BIOs; pending ciphertext buffer), `TlsRawBytesProtocolHandler` (implements `ProtocolHandlerInterface`; non-blocking handshake; same `RawSocketCommunication` delivery), `TlsListenerConfiguration`, `TlsClientConfiguration`. `ServiceEndpoints` carries `optional<TlsClientConfiguration>`. `ProtocolHandlerInterface` gains `start_outbound_handshake`, `is_handshake_complete`, `is_reads_paused`. `ProtocolType::TlsRawBytes` (value 2). 9 integration tests (5 inbound, 4 outbound). Not yet wired to any application.
- `order_gateway` — FIX session layer complete; PDU encoding to sequencer complete; ER routing back to fix8 complete. Session 17 adds `ha_enabled` flag (default false): when false, secondary sequencer connect is skipped, `forward_pdu_to_sequencers` sends only to primary, and secondary host/port are not required in the toml. When `ha_enabled=true`, dual-publish to primary and secondary is restored. `forward_pdu_to_sequencers` name kept plural — the dual-publish branch returns when leader-follower is fully live.
- `sequencer` — Slices 1–6 complete. PDU forwarding (NOS, OCR, ER), topology, re-encode fixes all from session 15. WAL (`SequencerWal`: mmap'd segments, snapshot, CRC32, replay on restart), seqNo on wire, `routing_comp_id` stamping, and leader-follower state machine (`Role::unknown/leader/follower`, `adopt_role`, `peer_heartbeat_timeout`, epoch, fence file) from sessions covered by the session-17 entry. `ha_enabled=false` (default): sequencer immediately adopts `Role::leader` in `on_initial_event` and skips arbiter/peer connects. Only the leader forwards order PDUs to ME.
- `matching_engine` — complete for the round-trip stub. `on_framework_pdu_message` decodes inbound `NewOrderSingle` PDUs (session 15) and emits a fully-filled `ExecutionReport` over the existing outbound `sequencer_er_conn_id_`. The ER populates every field that `SequencerThread`'s ER decoder reads. No real order book or matching — every order becomes a single fill at its limit price (or a zero sentinel for market orders). `OrderID` and `ExecID` are generated as `ME-ORD-N` / `ME-EXEC-N`. `OrderCancelRequest` is not yet handled (logs and drops at the `else` branch); cancel handling is a small follow-up.
- `arbiter` — stub, compiling
- `start_fix_seq_system.py` — runs primary only (secondary launch removed in session 15 pending leader-follower)
- PostgreSQL schema and migration tooling — complete (session 22). `db/create_db.py` idempotent setup script; Liquibase 5.x changelog; three tables: `pubsub_firm`, `pubsub_comp_id` (SCRAM fields, account status, audit timestamps), `pubsub_comp_id_gateway_permission`. Table prefix configurable (default `pubsub_`).
- Java admin service (`java/admin-service/`) — complete (sessions 22–23). Javalin 6 + Freemarker 2.3 + plain JDBC + Pico.css. Full CRUD for firms, comp_ids, and gateway permissions. Password set path: derives SCRAM-SHA-256 → writes to DB → pushes plaintext password to auth service via `SetCredentialRequest` (PDU 510) over TLS. Credential revocation: `RemoveCredentialRequest` (PDU 512) sent when a firm or comp_id is disabled, locked, or deleted. Maven build with Checkstyle, SpotBugs (exclude filter for DI false positives), JaCoCo (80% threshold), and OWASP Dependency Check. Logging: SLF4J API + Logback 1.2.13 (not Log4j2 — Logback is the native SLF4J implementation and needs only one dependency; Log4j2 requires an additional `log4j-slf4j-impl` bridge adapter with no benefit in this context; Javalin 6.3.0 depends on SLF4J 1.x so Logback 1.5.x is incompatible — 1.2.13 is the correct version). `logback.xml` suppresses Javalin/Jetty/HikariCP noise to WARN. `FreemarkerRenderer` registered via `config.fileRenderer()` (Javalin 6 requires explicit registration). Fat JAR built with maven-shade-plugin including signature-file exclusion and ServicesResourceTransformer. Service starts cleanly and responds on port 8080. Admin UI authentication: Jenkins-style login system backed by a TOML file (`admin_users.toml`) — no database dependency. BCrypt-hashed passwords (jbcrypt 0.4, cost 12). Two roles: ADMIN (full CRUD) and VIEWER (read-only; POST routes blocked with 403 by `AuthFilter`). First-run setup wizard creates the initial ADMIN account. Force-password-change flag set on admin-created accounts; user is redirected to `/change-password` on next login. Session auth via Jetty `SessionHandler`; `AuthFilter` runs as Javalin `before()` handler. Pico.css is bundled in the JAR (`src/main/resources/static/`) — no CDN dependency; works in air-gapped corporate environments. Three branding properties in `application.properties`: `brand.name` (product name shown in titles and nav), `brand.logo-url` (logo image in nav and login page), `brand.css-file` (path to a CSS file inlined into every page for colour overrides). See `java/admin-service/README.md` for deployment and branding instructions. Credential lifecycle gap: re-enabling a firm or comp_id, or unlocking a comp_id, does NOT automatically restore the auth service credential (PDU 510 requires the plaintext password, which is never stored); the operator must reset the password afterwards. The Edit forms display a warning when this applies; the full procedure is documented in the README "Credential Lifecycle" section.
- `db/export_credentials.py` — complete (session 23). Exports SCRAM credentials from `pubsub_comp_id` (enabled comp_ids from enabled firms, not locked) to `credentials.toml` in auth service `[[credential]]` TOML format. Uses `psql --csv --tuples-only` with `PGPASSWORD` env var. Atomic write via temp file + rename.

## What Is Not Yet Done (in dependency order)

1. **Re-verify fix8 wrong-port issue is gone** — session 13 logs showed fix8 reaching the gateway's ER inbound listener (port 7010) rather than the FIX listener (port 9879). Sessions 14 and 15 did not reproduce this; fix8 connected cleanly to 9879 throughout. Worth one explicit check (`f8test -d -c myfix_gateway_client.xml -N GW1`) before retiring the item.
2. **Matching engine — `OrderCancelRequest` handling** — the ME currently logs and drops cancels at the `else` branch. Mirror the NewOrderSingle path: decode, fabricate a `Canceled` ER, send. Small follow-up; will exercise the same plumbing again with a different message type.
3. **Arbiter stub → real** — `ArbitrationReport` → `ArbitrationDecision`
4. **Leader-follower — Slice 7 (network WAL replication)** — the state machine is done (Slice 6, session 17). What remains: follower on another host, leader streams WAL records over a dedicated TCP connection, follower acks, commit-for-ER-emission = follower-acked. When Slice 7 lands, restore the secondary sequencer config/code (set `ha_enabled=true` in both tomls) and the peer Connect (removed in session 13). Port plan needed: 7003 primary peer listener, 7004 secondary peer listener. After Slice 7 the gateway's `forward_pdu_to_sequencers` dual-publish branch is live again.
5. **`SequencedMessage` wrapper** — currently the sequencer forwards raw PDUs to ME without a sequence-number envelope; add this once stable.
6. **Trace logs in `PduParser` and elsewhere** — the two `Info`-level lines in `PduParser::receive` (header decode + raw 16 header bytes), the `Info`-level payload hex dump in `PduParser::dispatch_pdu`, and the short `TRACE` lines in `InboundConnectionManager::on_accept` and `SequencerThread::on_framework_pdu_message` are valuable for diagnosis but noisy at production rates. Drop to `Debug` or wrap behind a compile-time switch when production traffic begins.
7. **Pub/sub WAL** — long-term replacement for direct TCP; eliminates the rendezvous problem and the retry workaround.
8. ~~**Credential export script**~~ — done (session 23). `db/export_credentials.py` exports DB credentials to auth service `credentials.toml`. Live CRUD updates go via PDU 510/512.
9. **`RestoreCredentialRequest` PDU (514/515)** — long-term fix for the re-enable/unlock credential gap. The DB already holds all SCRAM-derived values (`stored_key`, `server_key`, `salt`, `iterations`); a new PDU pair carrying those four fields would let the admin service restore credentials on firm/comp_id re-enable or comp_id unlock without requiring the plaintext password. Work needed: PDU 514 request + PDU 515 result in the auth service (parallel to 510/511 but skip the PBKDF2 step); new `AuthServiceClient.restoreCredential(compId, scramCredential)` method; call it from `FirmHandler.update()` and `CompIdHandler.update()` on the re-enable/unlock branch. The current warning UI and documented password-reset workaround remain valid until this is implemented.

## Immediate Next Task

A WAL+HA design has been worked through in detail (see "WAL and HA Design" section below) and an eleven-slice implementation plan agreed. **Slices 1–6 are complete** (see session-17 entry). The next session takes **Slice 7**:

**Slice 7 — Network WAL replication.** Follower on a second host. Leader streams WAL records over a dedicated TCP connection (replication channel, separate from the order/ER data channels). Follower writes each record to its own WAL and acks. Leader gates ER emission on follower ack — the ER is not sent to the gateway until the follower has durably recorded the order.

Before Slice 7 starts:
- Decide port plan for peer-to-peer replication channel (7003 primary listener, 7004 secondary listener suggested).
- Set `ha_enabled=true` in both sequencer and gateway tomls for the HA test setup; the secondary service-registry entries and dual-publish code are already present, just guarded.
- The peer `Connect` (removed in session 13) returns as the replication channel connect.

**Smaller items deferred but still on the list:**
- Trace log cleanup (item 6 in "What Is Not Yet Done"): demote `Info`-level traces to `Debug`. Mechanical, quick.
- OrderCancelRequest round trip (item 2): mirrors NewOrderSingle path on ME side. Small. Fits in a session corner.
- fix8 wrong-port re-verification (item 1): one `f8test -d` run.
- `k`-prefix constants in `ExpandableSlabAllocatorTest`'s fixture (`kSmallSlab`, `kLargeSlab`) — violates the project rule that constants are snake_case. Mechanical search-and-replace.
- Quill thread-name population — call `quill::Frontend::set_thread_name()` at thread spawn sites so the `thread_name` log column shows something.
- Quill backend CPU tuning — pin the backend thread and tune `BackendOptions::sleep_duration`.
- Sequencer ER inbound 120s idle-timeout killing healthy quiet connections.
- Hex-dump debug logging on hot path needs a compile-time flag gate before production.

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

**Rationale** — this design avoids a common pitfall where logging is unavailable until after config is read (because the log filename comes from the config). Here the log filename comes from the command line, so logging starts immediately and config errors are recorded in the log rather than only printed to stderr.

**Config changes** — all four application configs gain required `[logging]` section:
```toml
[logging]
applog_level = "info"
syslog_level = "info"
```
Both fields are required. There are no optional config fields — making a field optional hides it from operators and makes it unconfigurable in practice.

**FIX parsing implemented in `order_gateway`** — `FixParser`, `FixSerialiser`, `FixMessage`, `FixSession` copied from `order_gateway` with namespace changed to `order_gateway`. `MsgType::OrderCancelRequest` and `Tag::OrigClOrdID` added to `FixMessage.hpp`. Logger threaded through `FixParser` constructor so bad checksums are logged at Debug rather than silently dropped. Full FIX session layer implemented in `FixGatewaySeqThread` (Logon, Heartbeat, TestRequest, Logout, NewOrderSingle, OrderCancelRequest). PDU encoding and ER routing remain TODO.

---

## WAL and HA Design (planned)

> **Topology diagram:** `pubsub_itc_fw_topology.puml` (rendered via PlantUML) is the authoritative single-site, single-instrument deployment diagram for everything described in this section. Its companion explanation is `pubsub_itc_fw_topology.md`.

Designed in conversation, not yet implemented. This section captures the architecture so subsequent sessions can refer back to it. The implementation is staged into vertical slices, listed at the end.

The design follows the convergent pattern that Aeron Cluster, Kafka, Raft, and database checkpointing all arrive at: **separate the irreversible decision (WAL commit) from its replayable effects (ME state, ERs, FIX out).** The WAL is authoritative; everything downstream is reconstructable from it. Followers observe commits, never infer them. Leadership decides who may append; the WAL decides what already happened. Those two concerns must never leak into each other.

### Glossary -- terms that must not be confused

The framework uses two pairs of terms with strict, non-overlapping meanings. Confusing them is a recognised source of bugs in HA systems generally; the discipline matters more than the exact words chosen.

- **primary / secondary**: configured identity, set in the toml at deploy time, never changes for the life of an instance. Primary has the lower `instance_id`. Used only for deterministic tiebreaking on cold start when no instance currently holds a valid lease and the arbiter is being asked to assign initial leadership.
- **leader / follower**: runtime role, determined by the arbiter's lease grant. Either configured primary or configured secondary can be leader at any given moment. Code paths that perform commit / forward / publish actions check the lease state, not the configured identity.
- **active / standby**: NOT USED. These terms ambiguously refer to either configured identity or runtime role and are a permanent source of confusion when discussing HA. Always use one of the two more specific terms above.

In the happy path, primary is leader and secondary is follower. After a primary failure and successful failover, secondary becomes leader (still configured as secondary). After the original primary recovers and rejoins, it becomes follower (still configured as primary). A graceful failback is an operational choice, not automatic.

### Decision log

What is decided, what is leaning, what is open.

**Decided:**

- Per-component HA, no central broker. Each component pair (sequencer pair, ME pair, etc.) has its own primary-secondary instances, its own state replication, its own arbitrated failover. Components do not share a runtime broker; they share framework-level HA *primitives* (data structures and protocols) but compose them independently.
- Lease + epoch arbitration. The arbiter holds leadership state; leaders renew via heartbeat; failover requires arbiter consultation, not unilateral promotion. (See "Leader-Follower Protocol" subsystem section above for the DSL-level mechanism.)
- The arbiter is itself HA, in a Primary+Secondary+Witness (PSA) topology. Two full arbiter instances each hold a copy of the leadership-state map; one third small witness machine holds no state but votes on which of the two arbiters is currently active. The witness machine must be in a failure-independent location relative to the two arbiters: different power supply, different network switch, ideally different network segment. Three votes total means a majority is two; this prevents split-brain in network partitions. Three machines is the structural minimum and stays at three -- adding more witnesses degrades the design rather than improving it (four votes means three needed for majority, so any single failure becomes catastrophic). See "Arbiter PSA topology" section below for the protocol mechanics.
- WAL is segmented, mmap'd, single-writer. Format: `[ magic | length | seqNo | payload | checksum ]`. Replay scans from offset 0 and stops at first failure. Tail corruption equivalent to a clean crash before commit.
- No `fsync` per WAL append. Disk durability is out-of-band (segment rotation, snapshot writes, periodic flusher). Cross-machine durability comes from replication, not from disk.
- Two-tier commit: locally durable (CPU coherence via store-release on commit offset) gates the leader's send to the ME. Replicated (follower has acked over the dedicated replication channel) gates the leader's emission of ERs back to the gateway.
- Every cross-component PDU carries the sender's view of the relevant component pair's leader-epoch. Receivers check the epoch before processing: same/expected = accept, lower = sender is stale (discard with warning), higher = receiver might be stale (re-validate with arbiter, do not silently accept or discard). This is fencing applied to every message rather than only to commits, and is the mechanism that detects split-brain at every cross-component interaction. See "Epoch propagation on every PDU" section below.
- Per-connection isolation in outbound sends. Each TCP connection has its own outbound queue and non-blocking send semantics; a stalled peer cannot block sends to a fast peer. Slow peers that exceed a configured lag threshold are dropped and must reconnect-and-replay. The sequencer-to-follower replication channel is not droppable (it is on the critical path for ER emission); other channels are. See "Per-connection isolation and backpressure" section below.
- Cold-start mmap warm-up via `madvise(MADV_WILLNEED)` on WAL open. Pre-faults the mmap pages so cold-start MTTR is dominated by disk read time done in parallel with snapshot load, not by lazy faulting during replay. Implementation note for slice 3. See "Cold-start MTTR and mmap warm-up" section below.
- Gateway and ME each open TCP connections to **both** sequencer instances at startup, and keep both open. Sends go only to the current leader. Non-leader rejects at the application layer.
- FixSession ↔ ClOrdID mapping moves from gateway to sequencer's WAL. Routing on `(SenderCompID, TargetCompID)` rather than ConnectionID, so that a fix8 client reconnecting (possibly to a different gateway in the gateway pool) is naturally addressable.
- ME failover policy is **cancel-on-failover** as the chosen baseline. ME-secondary maintains a replicated copy of the book; on promotion it reconciles its book against the new sequencer leader's WAL and then issues cancel ERs for all genuinely-outstanding orders. FIX clients receive explicit "Cancelled" messages rather than experiencing a market halt. Halt-on-failure is preserved as a fallback for failure modes that cannot be cleanly reconciled (e.g. WAL corruption, total arbiter unavailability). Seamless lockstep failover (option b) remains a future aspiration. See "ME failover policy" section below for the full rationale and the critical correctness rule about reconciling against the WAL before issuing cancels.
- Integer-only prices and quantities. All price/qty values multiplied by a constant (e.g. 1,000,000) to avoid floating-point determinism hazards. Common practice in matching-engine implementations and a hard rule for this framework.
- Dual rolling snapshots. Truncation gated by the older trusted snapshot, never the newest one just taken. Validation required before promotion.
- Halt as the correct response to several specific failure modes (WAL mid-segment corruption, both arbiter halves unreachable during a failover, snapshot validation failure on the only available snapshot). Halt is conservative and unambiguous; it is preferred over clever recovery in scenarios where correctness cannot be proven.
- Time synchronisation via PTP (IEEE 1588), not NTP. Cross-machine clocks must agree to sub-microsecond accuracy for lease checks, timestamps, and ordering. PTP is operational infrastructure the framework relies on; it is not implemented inside the framework. See "Time synchronisation and clock skew" section below.
- Local interval measurement uses `CLOCK_MONOTONIC` (already in `HighResolutionClock`). `CLOCK_MONOTONIC_RAW` was considered and rejected: it is unaffected by NTP/PTP slewing, but that is a disadvantage rather than an advantage for interval timers, since intervals can drift from real-world expectations on long-running processes if the underlying TSC is inaccurate.
- Clock injection. Components that need to read time will take a `MonotonicClock&` or `WallClock&` constructor parameter rather than calling `HighResolutionClock::now()` directly. Concrete motivator: GTD (Good-Til-Date) order support in the matching engine requires replay-deterministic clock reads, which only injection makes possible. Planned as a dedicated session of work; not blocking any HA slice but to land before the ME grows GTD or any other time-dependent logic. See "Clock injection" section below.
- Two distinct timer mechanisms, kept separate. Local OS `timerfd` for infrastructure timers (idle timeouts, connect retries, lease heartbeats, backstop, FIX logon timeout) -- these are not observable to matching logic, do not need replay determinism, and stay as `timerfd`. Sequencer-mediated timers for ME-domain timer events (GTD expiry, auction expiry, self-trade prevention windows when added) -- these are replay-critical and travel through the WAL alongside orders. See "Timer sourcing" section below.
- Statistics via Prometheus, not via a Kafka publishing chain. Hot-path instrumentation is shared-memory atomic counter/gauge/histogram updates (nanosecond cost). A separate Prometheus gatherer process per machine reads the shared memory and exposes scrape or remote-write endpoints. Cumulative counters in shared memory satisfy the regulatory "no statistic ever gets dropped" requirement: the cumulative count is mathematically complete and durable across gatherer restarts; only fine-grained rate detail within a missed scrape window is lost, which is acceptable. See "Statistics and metrics" section below.
- ME audit log via the existing Quill async logger. The matching engine logs order acceptance, ER emission, and other regulator-relevant events at PTP-disciplined `CLOCK_REALTIME` timestamps. Hot-path cost is sub-100ns per `PUBSUB_LOG` call. The ME audit log is best-effort crash-durable (Quill is async; in-flight records may be lost on a crash), but the WAL is the crash-durable record of order existence -- the ME log is supplementary timing detail. Per-statement synchronous flushing was considered and rejected on latency grounds. See "Statistics and metrics" section below.
- Downstream consumers of order/trade events (Kafka publisher, future broadcast use cases) follow the **WAL-follower pattern**, not topic-based pubsub. Each consumer opens a connection to the sequencer leader, identifies a position cursor, and receives WAL records from cursor onward. The sequencer's WAL replication channel generalises from "one follower (the secondary sequencer)" to "N followers, each with their own cursor". This reuses the framework's existing replication primitive rather than introducing a new pubsub abstraction. A topic-based pubsub primitive may be added later if multiple downstream broadcast consumers with fanout-and-replay semantics emerge; for the current single named consumer (Kafka publisher) it is over-engineering. See "Downstream consumers and broadcast streams" section below.

**Leaning:**

- Per-component HA primitives provided by the framework: a `WAL` data structure, a replication-channel pattern, an arbiter-client API, a fencing-discipline helper. Each component composes these into its own HA strategy. Avoids "every component implements HA differently with different bugs".
- Quill backtrace logging configured on each component's logger: when an `Error` or `Critical` log record fires, a buffered ring of recent diagnostic context is also flushed to the sink. Useful for incident debugging via Splunk. Quill v11 supports this directly. To-do for the framework when convenient; not blocking any HA slice. See "Statistics and metrics" section below.

**Open:**

- Mechanism for the arbiter's own internal HA. The arbiter holds the leadership state for every component pair and is itself HA via a PSA+witness topology (two full arbiters plus one witness, see Decided above). The two arbiter instances must keep their leadership-state cell in sync; the witness must participate in tiebreaking when network partitions affect the arbiter pair. The intent is to build this from scratch using the same lease+epoch pattern the framework uses elsewhere, with replication between arbiter primary and secondary over a dedicated TCP channel and a small voting protocol involving the witness. Consensus libraries (NuRaft, braft) are explicitly *not* the chosen path -- see "Discussion: consensus libraries vs. lease+epoch" below for the trade-off analysis. The decision is recorded as Open because the PSA+witness lease+epoch approach has not yet been designed in detail; if the design surfaces problems that hand-rolled approaches cannot cleanly solve, the consensus-library path may need to be reconsidered.
- Sub-second failover target for the sequencer: how aggressively to tune lease and heartbeat intervals. Tighter intervals trade arbiter availability for failover speed. The framework should make this tunable via `ReactorConfiguration` rather than baking in a number.
- DR site topology. Currently the design is main-site only. DR will require additional design work (a separate site, separate machines, presumably its own arbiter pair, its own sequencer pair, and a cross-site replication strategy). Out of scope until the main-site design is implemented.
- Multi-instrument scaling. A real exchange runs hundreds to thousands of instruments. Single sequencer for everything, sharded sequencer per instrument group, or sequencer per instrument? Each has different failover and replay implications. Not in the immediate slicing plan.
- Sequencer-to-gateway connection direction. The framework currently has the sequencer initiating the outbound connection to the gateway's ER inbound listener (a session-13 finding documented elsewhere in this summary). This is the unusual direction; conventional FIX architectures have the gateway as a client of the core. The trade-off: as designed, the sequencer's configuration must list every gateway address, and adding a gateway requires updating the sequencer configuration. The reverse direction (gateway connects outbound to the sequencer for both order send and ER receive) makes the core "anonymous" and easier to scale horizontally, but requires the sequencer to route ERs by lookup against currently-connected gateway sessions rather than by initiating connections. For the framework's current single-instrument scale this is an acceptable operational cost; for production multi-gateway deployments it likely needs reversing. Open until a deployment scenario forces the choice.
- Market data integration mechanism. The work system at present has a market data system that consumes data published by the order placement system. The exact mechanism and the exact data are not yet known to this project; a conversation with the maintainer of that system is pending. Until that information is available, the framework-side mechanism for delivering equivalent data cannot be decided. Possibilities range from "another WAL follower" (analogous to the Kafka publisher) to "a topic-based pubsub primitive" (if multi-subscriber fanout is genuinely needed) to "a bespoke market-data-specific mechanism". Tracked in the "Open Questions and Items to Investigate" section.

### Architecture: per-component HA with shared primitives

Every component that participates in HA has a primary instance and a secondary instance, each on its own machine. Within the pair, exactly one instance is leader at any moment (when the arbiter is reachable) and the other is follower. Failover between the pair is mediated by the arbiter via lease+epoch.

The framework provides reusable primitives:

- **WAL data structure**: segmented, mmap'd, single-writer, replayable.
- **Replication channel**: leader-pushes-follower-acks pattern over a dedicated TCP connection.
- **Arbiter client API**: `claim_leadership`, `renew_lease`, `query_current_leader`, `release_leadership`.
- **Fencing-discipline helper**: a check-the-lease-before-acting wrapper that components use on every committed action.
- **Snapshot mechanism**: dual rolling snapshots with validation.

Each component composes these primitives into its own HA strategy. The state shape that gets replicated differs by component:

- The **sequencer** has the richest state: order log, FIX session map, per-gateway delivery cursors, sequence-number authority. Its WAL holds an event log plus periodic cursor records. This is the most state-rich case and most of the rest of this section discusses it.
- The **ME** maintains a replicated book between primary and secondary. ME-primary is the active matcher; ME-secondary tails the primary's book updates. On ME-primary failure, ME-secondary reconciles its book against the sequencer's WAL and then issues cancel ERs for outstanding orders (the cancel-on-failover policy). The future seamless-failover case (lockstep) is documented as an aspiration; the cancel-on-failover policy is the chosen baseline.
- A **gateway pool** consists of multiple gateway instances, each fronting some subset of FIX sessions. This is not primary-secondary HA in the same sense; it is N-way pooled. Gateway machine failure causes affected FIX sessions to reconnect to a different gateway. See "Gateway pool" below.
- Future components (downstream forwarders, market-data publishers, risk gateways, etc.) follow the sequencer pattern with simpler state -- typically just a position cursor in the sequencer's output stream.

The arbiter is itself a small distributed system: two full arbiter instances (primary + secondary) plus one witness, in a PSA topology. Each full arbiter holds a copy of the leadership-state map; the witness holds no state but votes in elections of which of the two arbiters is the currently-active one. The arbiter does *not* use the framework's WAL+replication primitives because it would be circular (the primitives depend on the arbiter); the arbiter uses its own internal mechanism, intended to be the same lease+epoch pattern the framework uses elsewhere, with replication between arbiter primary and secondary over a dedicated channel. See "Arbiter PSA topology" section below for the protocol mechanics, and "Discussion: consensus libraries vs. lease+epoch" below for why a third-party consensus library was considered and rejected.

### Authority and roles

- **Sequencer + WAL = authority.** The leader sequencer assigns seqNo and appends to the WAL. The WAL append (via store-release on the commit offset) is the single irreversible act in the system. Before that store, an order does not exist; after it, the order is permanent and globally visible to followers and consumers reading the WAL.
- **Matching Engine = pure consumer with replicated book.** Under the cancel-on-failover baseline, ME-primary receives orders in seqNo order from the leader sequencer, mutates its book in memory, emits ERs back, and pushes book updates to ME-secondary so that ME-secondary maintains a replicated copy of the book. On ME-primary failure, ME-secondary uses its replicated book (after reconciling against the sequencer's WAL) to issue cancel ERs for outstanding orders. The book remains reconstructable by replaying the WAL from any point, which is how the system handles the case where both MEs fail or come up cold; in that situation the surviving instance rebuilds from sequencer-WAL replay. The architectural authority is always the sequencer's WAL. Under the future seamless-ME-failover design (option b in "ME failover policy"), both ME instances build the book in parallel and the secondary's outputs are accepted on primary failure without cancellation.
- **FIX Gateway = edge translator.** Translates FIX wire to/from PDUs. Holds no FixSession→ClOrdID map (that lives in the sequencer's WAL). Maintains a small comp-id → ConnectionID table for the FIX sessions currently established on this gateway. Routes ERs by looking up the comp-id pair the sequencer addresses in the ER PDU.

### State location (changed from current)

In session 15's working system, the gateway holds `cl_ord_id_to_session_` mapping. In the WAL design **this map moves to the sequencer**. Reasoning:

- The sequencer is already the single point that observes every order PDU, so adding "stamp the originating ConnectionID into the WAL entry" is a one-field addition rather than a new mechanism.
- The gateway becomes near-stateless. On a gateway restart, it does not lose ER routing capability, because that information lives in the sequencer's WAL.
- After failover, the new sequencer leader has the routing map by virtue of WAL replay.

**Subtlety -- ConnectionID is not stable across reconnects.** A fix8 client logging out and back in gets a new ConnectionID. The chosen approach is to route on `(SenderCompID, TargetCompID)` -- natural FIX-level addressing. The gateway maintains a small per-comp-id table mapping to the current ConnectionID, and the sequencer addresses ERs by comp-id. The fix8 client reconnecting (possibly to a different gateway in the gateway pool, possibly to the same one with a new socket) is naturally addressable; the new gateway tells the sequencer "I now hold the FIX session for `(CompA, CompB)`" and the sequencer's map updates.

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

### Epoch propagation on every PDU

Every cross-component PDU carries the sender's view of the relevant component pair's leader-epoch. This is fencing applied to every message, not just to commit decisions. The epoch is part of the wire-level framing and is checked by the receiver before the PDU is processed.

The receiver has three responses available:

- **Same or expected epoch:** accept and process normally.
- **Lower epoch than receiver knows:** sender is stale. Discard with a warning log entry. The sender's component pair has had a leadership change that the sender has not yet noticed. This is benign in the short term; the sender will detect its staleness on its next heartbeat to the arbiter and step down or refresh.
- **Higher epoch than receiver expects:** the *receiver* might be stale. Do not silently accept and do not silently discard. Trigger an immediate re-validation with the arbiter to learn the current epoch for that component pair. Hold the PDU until the answer is known. If the receiver's view turns out to be stale, update local state and process the PDU. If the higher-epoch claim turns out to be from a confused or rogue sender, discard.

The case that motivates this discipline is the cancel-on-failover scenario: ME-secondary believes it has been promoted (perhaps due to a transient network partition from the arbiter pair) and emits cancel ERs to the sequencer with what it thinks is the current ME epoch. If the sequencer silently discards (because it knows ME-primary is still leader from the sequencer's view), the cancel ERs are lost without anyone learning that ME-secondary believes it is leader. If the sequencer silently accepts (because the higher epoch is plausible), the cancels reach FIX clients while ME-primary is still actively matching -- a serious split-brain symptom.

Re-validation with the arbiter resolves this correctly: whichever party has the stale view learns it before damage is done.

The cost is one extra small field on every PDU and one re-validation round-trip on the rare case of mismatched epochs. The benefit is split-brain detection at every cross-component interaction, not just at commit.

### Per-connection isolation and backpressure

Each TCP connection from a component to its peers must be **isolated** -- a slow or stalled peer must never block sends to a fast peer. Specifically:

- The sequencer leader sends sequenced order PDUs to both ME-primary and ME-secondary. If ME-secondary stalls (GC pause, page-fault storm, scheduler starvation), its TCP receive window will close and the sequencer's send to ME-secondary will block. This must not block the sequencer's send to ME-primary.
- The sequencer leader sends ER PDUs to multiple gateways. If one gateway stalls, the others must continue to receive their ERs.
- The sequencer leader pushes WAL records to the sequencer follower. This connection is special (see below).

The implementation discipline:

- Per-peer outbound queues, not a shared queue per component.
- Non-blocking send semantics: each connection's send proceeds as far as the peer's window allows and stops without blocking other connections.
- A "lag-acknowledgement" mechanism: each peer reports its current position; the sender knows how far behind each peer is; a peer that exceeds a configured lag threshold is dropped (TCP reset), forcing it to reconnect-and-replay.

**The sequencer-to-follower replication channel is not droppable.** It is on the critical path for ER emission to gateways, since the sequencer leader does not emit an ER until the follower has acked the underlying order's WAL record. If the follower lags, the leader cannot drop it without violating the two-machine durability rule. Instead, the leader stops emitting ERs (the gateway sees order receipt but no fills until replication catches up). If the follower fails entirely, leadership decisions become single-machine-durable until a new follower is paired in, which is itself an arbiter-mediated event.

**ME-secondary lag handling under cancel-on-failover.** ME-secondary needs a current copy of ME-primary's book, not just the sequencer's order stream, to do cancel-on-failover correctly. So ME-secondary has two inputs: the sequencer's order stream (same as ME-primary receives) and ME-primary's book-update stream. If ME-secondary lags on the book-update stream, the cancel-on-failover policy may fail at promotion time (the secondary's book is stale, and reconciliation against the WAL has more catching-up to do). This is acceptable as long as the lag is bounded -- the reconciliation step handles the catch-up. If the ME-secondary lags egregiously (more than a configured threshold), it is dropped; on restart it cold-starts via WAL replay rather than via lagged replication.

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

### Gateway pool

Gateway HA differs from sequencer HA. A FIX session is a TCP connection with a counterparty; that connection is intrinsically tied to the gateway machine that holds it. When a gateway dies, the connection is gone and the FIX client must re-establish.

This is not catastrophic, because:

- FIX has built-in resend semantics. The user reconnects with their last received sequence number; the gateway side replays anything missed.
- A pool of gateway machines shares the FIX session load. Each FIX session is pinned to one gateway, but if that gateway dies, the user reconnects to a different gateway in the pool.
- The user-visible interruption is the time taken for the FIX client to detect the dead connection and re-establish. Typically seconds, not transparent failover.

So the gateway pool is **N-way pooled redundancy**, not primary/secondary HA. Failure mode is "user reconnects to a different gateway"; not "gateway-secondary becomes leader transparently". The downstream sequencer must be designed for this: when a user's FIX session lands on a different gateway after a gateway failure, the sequencer's `(SenderCompID, TargetCompID)` routing naturally points the next ER to the new gateway, since the new gateway has registered the FIX session with the sequencer on the user's reconnect.

This is why the FixSession ↔ ClOrdID mapping must live in the sequencer rather than the gateway: gateway-pool failures must not lose ER routing.

### Failover speed targets

What "fast" means depends on the component. Targets below are aspirational; the framework should expose tuning parameters via `ReactorConfiguration` rather than hard-coding.

| Component | Failover target | Notes |
|---|---|---|
| Sequencer leader → follower | Sub-second; tens of milliseconds aspirational | Drives lease length (~200-500ms) and heartbeat interval (~50-100ms). Tighter intervals trade arbiter availability for failover speed. |
| ME crash | Cancel-on-failover baseline; latency = sequencer-failover time + book reconciliation | ME-secondary reconciles its replicated book against the sequencer's WAL, then issues cancel ERs for outstanding orders. Depends on sequencer being up (or having failed over). Seamless ME failover (sub-millisecond, no cancels) is a future feature. Halt-on-failure remains the fallback for failure modes that cannot be cleanly reconciled. |
| Gateway machine failure | Seconds (FIX reconnect to another gateway in the pool) | Inherent to the gateway-pool design. FIX-level resend covers any gap. |
| Arbiter failure | Tolerated for the lease window | If primary arbiter dies and secondary is up, secondary takes over via internal arbiter HA. If both arbiters are unreachable, leader continues operating until lease expiry; system becomes unavailable for new orders thereafter. |
| WAL disk full | Immediate halt | Sequencer gates ingress instantly. No "best effort" continuation. Operator action required. |

Fast failover for the sequencer requires active health-checking, not just TCP-level dead-peer detection. The gateway and ME must heartbeat to the sequencer at sub-lease-length intervals, and treat heartbeat loss as failure rather than waiting for TCP to notice. This is a per-component cost on every healthy interval, traded for fast detection of failure.

### Time synchronisation and clock skew

Several mechanisms in this design depend on clocks across different machines agreeing closely:

- **Lease expiry checks.** The arbiter grants a lease with an absolute expiry time. The leader checks its own clock against that expiry. Skew between leader and arbiter can produce two failure modes: a leader stepping down too early (skew makes its clock think the lease has expired before the arbiter does), or worse, a leader continuing to act past expiry (skew makes its clock think the lease is still valid after the arbiter has already granted leadership to the secondary). The latter is a split-brain risk.
- **TransactTime on ERs.** Auditors and downstream consumers expect timestamps from different machines to be in a sensible total order. Skew between the sequencer, the ME, and any timestamping component produces audit anomalies.
- **Heartbeat liveness detection.** "I haven't heard from you in N milliseconds" means roughly the same thing on both endpoints only if their clocks agree.
- **WAL entry and snapshot boundary timestamps.** Replay across machines (including cross-site DR replay in the future) needs timestamps to order events meaningfully.

The intended solution is **PTP (Precision Time Protocol, IEEE 1588)**, not NTP. PTP delivers sub-microsecond synchronisation across a properly configured local network, vs NTP's millisecond-to-tens-of-milliseconds best case. With PTP, the lease length needs only a small safety margin above expected maximum skew, and the split-brain risk from skew effectively vanishes for any realistic lease length.

PTP works best with:

- Hardware-timestamped NICs (the timestamp is captured by the NIC at packet ingress/egress, not by software).
- A grandmaster clock on the local network, typically GPS-disciplined.
- Boundary or transparent clocks on switches between grandmaster and consumers.

This is operational infrastructure that real exchanges already run as standard. The framework relies on it being present and working, but does not implement PTP itself.

**Framework discipline around clocks:**

- Use `CLOCK_MONOTONIC` for hot-path interval measurement (heartbeat timers, timeout checks). It never goes backwards and is unaffected by wall-clock adjustments such as leap seconds. Its rate is gently slewed by NTP/PTP to track real wall-clock time, which keeps interval expectations accurate even on long-running processes. (`CLOCK_MONOTONIC_RAW` is *not* slewed, but is rarely the right choice -- intervals measured against it can drift from what one would expect of a "second" if the underlying TSC is inaccurate.) The framework's `HighResolutionClock` already uses `CLOCK_MONOTONIC`.
- Use `CLOCK_REALTIME` (PTP-disciplined) for cross-machine timestamps such as lease expiry, WAL entry timestamps, and TransactTime fields.
- Lease grants from the arbiter should ideally include both an absolute expiry time *and* a TTL ("expires at T, or after N seconds from when you receive this, whichever comes first"). The leader can use whichever frame is safer locally. Open question whether to implement both.

**Operational monitoring:**

The intent is a Nagios-based health monitor (or equivalent) that checks PTP status on every machine and alerts on:

- PTP offset exceeding a threshold (e.g. 10 microseconds).
- Loss of PTP synchronisation (the local `ptp4l` or `phc2sys` is not synchronised to a master).
- PTP master reselection events (a boundary clock has flipped to a different master), which can briefly disturb the local clock.
- Drift between `CLOCK_REALTIME` and the PTP hardware clock.

This is *monitoring*, not *prevention*. The framework still needs to tolerate skew within a documented bound. If skew exceeds that bound, the alert escalates and operators intervene before the system fails. This is the same operational discipline that exchanges already apply to their existing infrastructure.

**What the framework does NOT do:**

- It does not implement PTP. It assumes PTP is in place.
- It does not attempt to detect or correct skew at runtime. That is the operating-system and PTP daemon's job.
- It does not fail over because of clock skew alone. Skew is detected by monitoring; the framework's failure detection is heartbeat-based, and heartbeats are tolerant of small clock differences.

This thinking is embryonic and will be revisited when slice 8 (the arbiter) lands and lease-expiry handling is implemented.

### Statistics and metrics

The framework's approach to observability splits into three categories with three different mechanisms. Each addresses a question regulators and operators ask, but the questions are different and the right mechanism is different.

**Aggregate statistics: Prometheus.** "How many orders per second? What's the latency distribution? How many open positions?" These are aggregate questions answered by counters, gauges, and histograms.

The chosen mechanism is Prometheus instrumentation reading from shared memory. The application (e.g. the matching engine on order placement) does an atomic counter increment, gauge update, or histogram observation -- nanosecond-scale operations that don't allocate, don't lock, and don't do I/O. A separate Prometheus gatherer process runs on the same machine, reads the shared-memory counters, and exposes them via standard Prometheus scrape (or pushes via remote-write). Grafana or equivalent draws graphs.

This is intentionally cheaper than the work system's UDP-based stats publication, which is fundamentally unreliable. Cumulative counters in shared memory are durable across gatherer restarts; the regulator's "no statistic ever gets dropped" requirement is satisfied because cumulative counts are mathematically complete -- a missed scrape window can be reconstructed from the cumulative values at scrape time before and after the gap.

The trade-off accepted: fine-grained rate detail within a missed scrape window is not preserved. The cumulative count is. For the regulator's intent (auditing the cumulative count of regulated events), this is sufficient.

Prometheus histograms have a known limitation: observations beyond the configured bucket range fall in the `+Inf` bucket and the exact value is lost. For latency monitoring of P99, P99.9 etc this is fine. For "give me the actual value of every observation above some threshold" it isn't, and that question is answered by the WAL or by the ME audit log, not by Prometheus.

**Per-event timing observability: ME audit log via Quill.** "When exactly did the ME accept order N?" Pcap-based reconstruction at the network layer gets close but doesn't capture the inside-process timing. The right answer is for the ME to log the event itself, with a high-precision timestamp from PTP-disciplined `CLOCK_REALTIME`, at order acceptance time.

The framework already uses Quill for logging. Quill's hot-path cost is sub-100ns per call: structured records go to a lock-free queue and a separate logger thread drains the queue to disk. The ME's order acceptance path can emit an audit log line with no meaningful latency cost.

The framework's `PUBSUB_LOG` macros are the existing entry point. ME audit records go through the same mechanism. A dedicated audit log level (separate from the existing `Info`/`Warning`/`Error` levels) is worth adding when this work lands, so audit records are filtered separately from diagnostic traces and never accidentally suppressed.

**Crash durability: a documented gap.** Quill is async. On a process crash, in-flight log records that have been queued but not yet flushed by the logger thread are lost. For ME audit records this is a real concern -- "the ME crashed mid-trade and we have no record of what it was processing" is a bad audit answer if the audit log is the sole record.

The mitigation is that **the audit log is not the sole record.** The sequencer's WAL records every order before the ME sees it. The ME's audit log adds timing detail to events already durably recorded in the WAL. If the ME crashes with audit records in flight:

- The WAL still has the order. "Did this order exist?" → yes.
- The ME audit log may be missing the last few microseconds of timing detail. "Exactly when did the ME accept it?" → "the audit log shows acceptance at T, with the caveat that records within the last flush interval may have been lost in the crash."

This is a defensible position. The trade-off (acceptable async latency vs perfect crash durability) is the right one given the WAL covers existence.

The alternative -- synchronous flushing per audit record -- was considered and rejected. Quill v11 does not provide a per-statement-priority flush; the `set_immediate_flush` API is global and would couple all logging to disk-flush latency. A dedicated synchronous logger for audit records was considered but rejected on the same grounds: per-record disk-flush latency on the ME's hot path is unacceptable, and the WAL already covers the audit-existence requirement.

**Future enhancement: Quill backtrace logging.** When an `Error` or `Critical` log record fires, a buffered ring of recent diagnostic context is also flushed to the sink. This is useful for incident debugging via Splunk: support staff see the full picture at the moment something goes wrong, not just the error in isolation. Quill v11 supports this directly. To-do for the framework: configure backtrace logging on each component's logger, so recent operational context is preserved for incident analysis.

**The Kafka stats chain at the work system is intentionally not replicated.** That chain (ME stats → broker → Kafka publisher → external Kafka consumers) addresses the same question Prometheus addresses, less efficiently, with worse reliability semantics, and with more moving infrastructure. Prometheus replaces it cleanly. This is a hard break from the work system's pattern but a beneficial one.

### Downstream consumers and broadcast streams

The framework needs to support delivery of order/trade events to downstream applications outside the framework's order-flow components. The named use case is a Kafka publishing component: a process that consumes order and trade events from the framework and publishes them to an external Kafka cluster. Other potential consumers (market data publishers, surveillance systems, post-trade settlement) are forward-looking and not yet specified.

**The chosen pattern: WAL followers, not pubsub.**

The sequencer's WAL is the durable record of every order and ER. Components that need to consume order/trade events do so by becoming **WAL followers** -- they open a connection to the sequencer leader, request records from a position cursor onward, and receive a stream of WAL records.

This is a generalisation of the existing replication channel. Currently the sequencer has one follower (the secondary sequencer). Generalising to N followers, each with their own position cursor, is an extension rather than a new mechanism. Each follower:

- Opens a TCP connection to the sequencer leader.
- Identifies itself and its starting position cursor.
- Receives WAL records in seqNo order from cursor onward.
- Tracks its position locally; persists it via its own primary/secondary HA replication so failover doesn't lose position.

The Kafka publisher is the first such follower (other than the secondary sequencer itself). It reads the WAL, derives trades from matched orders, and publishes to Kafka via Kafka's idempotent producer protocol. Kafka idempotency handles the brief overlap window during Kafka publisher failover so duplicates do not reach external consumers.

**Why not a generic pubsub primitive.**

A topic-based pubsub primitive (publish to topic, N subscribers receive, framework handles fanout and replay) was considered. For the named requirement -- one Kafka publisher consuming the order/trade stream -- it is over-engineering. The WAL-follower pattern reuses what the framework already has to build for sequencer replication, generalises naturally to additional followers, and adds no new primitive.

A pubsub primitive becomes justified when there are multiple downstream consumers with similar fanout-and-replay semantics that don't fit cleanly as WAL followers. Currently there are not enough such use cases to justify it. If future use cases accumulate (market data dissemination at scale, multiple surveillance subscribers, etc.), a pubsub primitive can be added at that point. For now: WAL followers.

**Snapshot-and-truncate is multi-subscriber-aware.** With multiple WAL followers each at their own position, the sequencer cannot truncate WAL beyond the slowest follower's position. The sequencer tracks each follower's position; the truncation target is the minimum-of-all-follower-positions ∧ snapshot-anchor-position. A persistently-slow follower (e.g. Kafka publisher backed up by Kafka cluster issues) extends WAL retention until it catches up. This is a known cost of the WAL-follower pattern and is acceptable.

**Open: the market data system at work.**

The market data system at work currently consumes data published by the order placement system. The exact mechanism and the exact data are not yet known to this project; a conversation with the developer who maintains the market data system is pending. Until that conversation happens, the appropriate framework-side mechanism for delivering equivalent data to a market data system cannot be decided. Possibilities:

- Market data system becomes another WAL follower (option α extended to a second consumer). Likely sufficient if market data consumes the same per-event stream the Kafka publisher does.
- Market data system requires topic-based fanout to multiple downstream subscribers (e.g. tens of trading firms each subscribing to specific instrument feeds). In that case a pubsub primitive becomes justified -- not just for market data but as a framework primitive.
- Market data system has its own bespoke requirements that don't fit either pattern.

The investigation is recorded in the "Open Questions and Items to Investigate" section near the end of this summary.

**On the framework's "pubsub" name.**

The framework's name (`pubsub_itc_fw`) was chosen at a time when topic-based pubsub seemed central to the design. Looking at what the framework actually builds and what its named use cases need, the WAL is the more central abstraction. Components that need to consume broadcast-like streams do so by being WAL followers, not by subscribing to topics on a broker. The "pubsub" framing is more idiomatic for systems like Kafka and the work system's broker; "log + followers" is the framing that fits this design. The name can stay; the architecture is what it is.

### Clock injection

**Planned.** Components that need to read time will take a clock interface as a constructor parameter rather than calling `HighResolutionClock::now()` directly. Aeron uses this pattern (an `EpochClock` / `NanoClock` interface) and it pays back in several ways:

- **Testability.** Tests inject a mock clock whose `now()` is controllable. The lease-expiry boundary, heartbeat-loss detection, and any other time-sensitive code path can be exercised deterministically without relying on real wall time.
- **Replay determinism.** Replay uses the timestamps recorded in the WAL, not the current wall time, by injecting a clock that returns the recorded timestamps. The replayed ME sees the same view of time as the original ME.
- **Lockstep ME operation.** If ME-primary and ME-secondary process the same stream in parallel (the future seamless-failover model), both must see identical timestamps for any operation that touches a clock. Injection lets both read from the same authority rather than each calling its own local clock.
- **Single point of swap.** Changing the underlying clock implementation later (e.g. switching to a hardware PTP timestamp source) is one class change rather than touching every call site.

**Concrete near-term motivator: Good-Til-Date (GTD) orders.** The matching engine is expected to support GTD: orders that remain on the book until a specified date and then cancel. GTD requires the ME to consult time, and that consulting must be replayable -- which means the clock the ME reads must be injectable, so that replay can substitute a clock returning the WAL-recorded timestamp rather than the current wall-clock time. Without injection, GTD orders cancel at *replay time* rather than at the *original time*, and the rebuilt book diverges from the original.

The framework currently calls `HighResolutionClock::now()` directly in roughly a hundred places (timer arming, idle-connection checks, lifecycle timestamps). The retrofit involves introducing a `MonotonicClock` and `WallClock` interface pair, a `SystemMonotonicClock` / `SystemWallClock` default implementation, and threading clock references through component constructors. This is a dedicated session of work; it does not block any HA slice but should land before the matching engine grows GTD or any other time-dependent logic.

### Timer sourcing

Two distinct timer mechanisms are needed, with different requirements and different implementations.

**Local OS `timerfd` events** -- the existing mechanism, fired in the reactor thread of whichever component owns them -- remain the right and intended choice for **infrastructure timers**. These are timers whose firing is *not* observable to the matching logic and which therefore do not need replay determinism or cross-machine consistency. They have legitimate, ongoing use:

- Idle-connection timeout in `InboundConnectionManager`.
- Connect-retry backoff in `OutboundConnectionManager`.
- Backstop timer in the `Reactor`.
- Lease-heartbeat in arbiter clients (when the arbiter slice lands).
- Logon timeout in FIX session establishment.

These timers fire when the local kernel decides, with no need for the wider system to know. Replacing them with sequencer-mediated timers would add latency and complexity without any correctness gain. They stay as `timerfd`.

**Sequencer-mediated timers** are needed for **ME-domain timers** -- events whose firing *is* observable to the matching logic and which therefore must be replayable. The timer event itself becomes part of the sequencer's input stream: the sequencer assigns it a seqNo, appends it to the WAL, replicates it, forwards it to the ME alongside orders. The ME sees it as just another ordered event in the stream.

The concrete near-term motivator is **Good-Til-Date (GTD) orders**: an order that remains on the book until a specified date and then cancels. The cancellation event must be replayable -- if it isn't, replay produces a different book state than the original (the order would still be on the replayed book if the replay is mid-stream, or would cancel at the wrong seqNo if local clocks were used during replay). Other examples that will eventually need this pattern:

- Auction expiry events.
- Self-trade prevention windows where orders within a small time delta cannot match.

For sequencer-mediated timers, the cost is higher latency on the timer fire (the event has to go through the sequencer's WAL, replication, and forward to ME) but the gain is total determinism: timer events are in the WAL alongside orders, in deterministic seqNo order, and replay produces identical book state.

The framework currently has no ME-domain timers. When GTD or any other time-dependent ME logic is added, the implementation will need: a way to register pending timer events with the sequencer, a sorted-by-time data structure in the sequencer holding pending events, and a wake-up mechanism that translates "this pending timer's fire time has arrived" into "inject this event into the order stream at the next available seqNo". The wake-up mechanism itself can be a local `timerfd` inside the sequencer -- the local timerfd fires "wake up and check pending timers", and the sequencer responds by appending one or more timer events to the WAL. The fire of the local timerfd is not observable to anyone outside the sequencer; it is just the mechanism by which the sequencer notices that time has passed.

Out of scope until the ME grows GTD or another time-dependent feature, but the design is recorded here so it is not forgotten when the time comes.

### ME failover policy

The ME failover policy is the most consequential design choice in the HA architecture, because it determines what state the ME-secondary maintains and the determinism investment required of the matching-engine logic. Four options exist:

- **(a) Slow seamless failover.** ME-secondary cold-starts, loads state from the sequencer's WAL, replays. Tens of seconds to minutes depending on instrument count and warm-up cost. Rejected: too slow for an exchange.
- **(b) Fast lockstep failover.** Both ME instances process the sequencer's input stream in parallel using deterministic logic. Both produce identical book state at every instant. Failover is "stop discarding the secondary's outputs and start using them". Sub-millisecond failover possible. Requires deterministic ME logic: no system clocks on the hot path, no random numbers, no FP edge cases, no parallelism that doesn't preserve ordering.
- **(c) Halt-on-failure.** ME-primary dies, secondary is not used (or doesn't exist), market halts for the affected partition. Operators perform orderly cancel-and-restart. Trading resumes after a recovery window. Matches the published behaviour of LSE Millennium when its mirror process does not take over.
- **(d) Cancel-on-failover.** ME-primary dies, ME-secondary is promoted, and on promotion it uses its replicated book state to issue cancellation messages for all outstanding orders. The gateway delivers FIX `ExecutionReport` messages with `OrdStatus=Canceled` to the affected clients. Trading resumes immediately on the secondary, but all in-flight orders at the moment of failure are explicitly cancelled rather than allowed to silently disappear. This is the policy used by some real exchanges and is the chosen direction for this framework.

**The chosen baseline is (d) cancel-on-failover.** It avoids the long downtime of (c) and the determinism investment of (b), while giving FIX clients an unambiguous "your order has been cancelled" outcome rather than the silence of (c) or the seamless-but-determinism-fragile guarantees of (b). The ME-secondary maintains a replicated copy of the book, fed from the ME-primary via a dedicated replication channel (shown in the topology diagram). On promotion, the secondary walks its book and emits cancel ERs for every outstanding order.

**Critical correctness rule -- reconcile against the WAL before issuing cancels.** A naive implementation of cancel-on-failover has a serious race condition:

1. ME-primary matches a trade and emits an ER (e.g. `OrdStatus=Filled`) to the sequencer.
2. The sequencer commits the ER to its WAL.
3. The sequencer crashes before forwarding the ER to the gateway. (Or the ME-primary crashes before its book-replication push to the secondary catches up.)
4. ME-secondary is promoted. Looks at its replicated book; sees the order is still open (the match wasn't reflected in its replicated state in time).
5. ME-secondary issues a Cancel for that order.
6. The new sequencer leader, after recovery, sees the original Fill ER in the WAL. But by now the FIX client has already received "Cancelled" for an order that legally executed.

The result is a wrongly-cancelled trade. This is unambiguously worse than a delayed cancel.

The correctness rule is therefore: **on promotion, ME-secondary does not issue cancels until it has reconciled its book against the new sequencer leader's WAL.** The order of operations is:

1. ME-secondary detects ME-primary failure (heartbeat loss).
2. ME-secondary requests promotion via the arbiter; receives an `ArbitrationDecision` with new epoch.
3. ME-secondary stops tailing the (presumed-dead or now-failed-over) sequencer's stream.
4. ME-secondary connects to the new sequencer leader and exchanges position cursors: "my book reflects events up to seqNo M; what does your WAL go to?"
5. New sequencer leader replays events `M+1..N` from its WAL. ME-secondary applies them to its book.
6. **Now** the book is consistent with the sequencer's authoritative state. Any order still on the (reconciled) book is genuinely outstanding -- the WAL has no later ER for it.
7. ME-secondary issues cancel ERs for the genuinely-outstanding orders, via the new sequencer leader, which forwards them to gateways.

This adds latency to the cancel-on-failover path: the cancels cannot fire until the new sequencer leader is up and reconciliation is complete. In return it eliminates the race and guarantees that no legally-executed trade is wrongly cancelled. This trade-off is the right one: a wrongly-cancelled trade is a far worse outcome than a delayed cancel.

**Sequencer failover and ME failover may be ordered.** Note that step 4 above requires the new sequencer leader to be up. If the failure event is "ME-primary died" alone with sequencer pair healthy, the new sequencer leader is the existing sequencer leader, and reconciliation is fast. If the failure event takes out both ME-primary and a sequencer, the ME failover waits for sequencer failover to complete first. The cancel-on-failover latency is bounded by the sum of the two failover times, not by either individually.

**(b) lockstep failover is documented as a future aspiration** beyond cancel-on-failover. When the framework is mature and a specific deployment requires *seamless* ME failover (no cancels, in-flight orders survive the failover), the determinism work for option (b) can be undertaken at that point. The integer-only price/qty rule (already adopted) removes one major hazard. Other hazards -- timestamping, hash-iteration order, parallel work scheduling -- would need careful design. Until then, (d) cancel-on-failover is the right balance of correctness and recovery speed.

**(c) halt-on-failure is preserved as a fallback** for failure modes that go beyond what cancel-on-failover can handle. WAL mid-segment corruption, both halves of the arbiter pool unreachable simultaneously, or any scenario where reconciliation cannot be completed correctly -- all of these escalate to halt-on-failure. The operator deals with the situation manually after a clean halt.

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

### Cold-start MTTR and mmap warm-up

A sequencer that starts cold (process restart, machine reboot, or a previously-down instance rejoining) loads its most recent snapshot, replays the WAL tail, and rebuilds in-memory state. Three components contribute to the recovery time:

- **Snapshot load.** Single-digit milliseconds for a small snapshot, low hundreds of ms for a large one. Bounded.
- **WAL tail replay.** From the snapshot's seqNo to the current end-of-WAL. With snapshots taken regularly the tail is small (seconds-to-minutes of orders); replay is bounded by the tail size.
- **mmap page-in.** The WAL is mmap'd. On cold start the OS page cache holds nothing for the file. First reads cause page faults that pull pages from disk. With a 1GB WAL tail this is roughly 1GB / disk-read-rate -- in the order of low single-digit seconds for a fast SSD, longer for slower storage.

The mmap page-in cost is what most people forget. It does not appear in microbenchmarks of WAL replay because microbenchmarks usually run on warm caches. It appears in production cold starts and is the dominant component of recovery time when the WAL tail is non-trivial.

This matters mostly for *cold* starts. In a normal failover the secondary is already running, its mmap'd WAL is paged in (because tailing the leader's WAL records and writing them to its own WAL has caused the relevant pages to be touched), and the recovery time is the time to apply any not-yet-applied records plus the time to switch into leader role. No mmap warm-up cost. This is one of the reasons failover from a warm secondary is faster than cold start.

Two specific recoveries warrant the mmap-warm-up consideration:

1. **A previously-down sequencer being restarted to rejoin as follower.** Cold-start for that instance. Snapshot load + WAL replay + mmap page-in. Latency a function of how long it was down and how much WAL has accumulated.
2. **Both sequencer instances down at the same time, one being brought up first to elect itself leader.** Same cold-start cost, plus this is the path on the critical recovery from a total outage.

The mitigation is straightforward: pre-fault the mmap pages by issuing `madvise(MADV_WILLNEED)` on the mmap region right after opening it, or by reading the file sequentially in a background thread. The kernel does the I/O while the rest of the startup proceeds. A 1-2 second wall-clock cost becomes a parallel 1-2 seconds of disk I/O that overlaps with snapshot deserialisation and WAL replay setup.

**Implementation note** for slice 3 (mmap'd WAL on disk): include the `madvise(MADV_WILLNEED)` call in the WAL open path, so the page-in cost is paid eagerly rather than lazily during replay. Also worth measuring cold-start MTTR in benchmarks at slice 3 -- the number is likely surprising the first time.

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

The arbiter is **off the critical data path**. It holds leadership state for each component pair: `(component_id, leader_instance_id, epoch, lease_expiry)`. Leaders heartbeat to the arbiter to renew their lease. The arbiter never participates in order processing.

**The arbiter is itself HA.** It is deployed as a PSA+witness three-machine topology (two full arbiter instances and a witness) so that the arbiter can survive a single-machine failure without losing the leadership-state map. See "Arbiter PSA topology" subsection below for the protocol details.

On leader-failover, the surviving instance of a component pair contacts the arbiter and requests promotion via an `ArbitrationReport`. The arbiter performs an atomic compare-and-swap: if the old leader's lease has expired, it grants the request, bumps the epoch, and records the new leader. The old leader on revival sees its epoch is stale and steps down. (See "Leader-Follower Protocol" subsystem section above for the wire-level protocol.)

This is the **fencing token / epoch / leader-epoch** pattern (same family as Raft term, Kafka leader-epoch, Chubby/ZooKeeper epoch).

### Arbiter PSA topology

> **Topology diagram:** `pubsub_itc_fw_topology.puml` (rendered via PlantUML) shows the complete deployment topology for the single-site, single-instrument case: FIX clients, gateway pool, sequencer pair, ME pair, arbiter pair, and witness. Its companion explanation is `pubsub_itc_fw_topology.md`. This section describes the arbiter pool portion of that diagram.

The arbiter is itself HA, using a Primary-Secondary-Arbiter (PSA, also called Primary-Secondary-Witness) topology with three machines:

- **Arbiter primary** -- a full arbiter instance, holds the leadership-state map for every component pair.
- **Arbiter secondary** -- a full arbiter instance, holds a replicated copy of the leadership-state map.
- **Witness** -- a small process that holds no state but participates in elections by voting on which of the two full arbiters is currently the active one.

Three votes total; majority is two; any single failure can be tolerated; split-brain is prevented by majority arithmetic.

This is the same shape MongoDB uses for its replica sets when there are only two data-bearing members. The pattern is well-understood and operationally familiar.

**Failure-independence requirement (critical):**

The witness machine must be in a *failure-independent* network and power location relative to the two arbiter machines. Specifically:

- Different power supply / UPS / power circuit. A single power event must not be able to take down a full arbiter and the witness simultaneously.
- Different network switch. A single switch failure must not isolate a full arbiter and the witness from each other.
- Ideally a different network segment / VLAN / rack. A single rack-level event (top-of-rack switch failure, rack power failure) must not affect the witness if it affects an arbiter.

A common Mongo PSA failure mode is the witness sharing infrastructure with one of the data-bearing nodes: when the shared component fails, the cluster effectively becomes 1-of-2 instead of 2-of-3, the surviving arbiter cannot reach a majority, and failover is impossible at the moment it is most needed. This must not happen here. The witness's value depends entirely on it being in a failure-independent location.

**Why exactly three machines, and not four or more:**

PSA is a 3-vote design and stays 3-vote. Adding a second witness would make it 4 votes, requiring 3 for majority, meaning *any* single failure leaves the cluster below majority. This is worse, not better, than the 3-machine configuration. Counterintuitive but mathematically correct: the right way to add redundancy is to upgrade to 5 machines (full Raft cluster of 4 data-bearing + 1 witness, or 5 data-bearing nodes), not to bolt extra members onto a PSA. For this framework's purposes, 3 machines is the chosen design and stays at 3.

**The witness must run the protocol correctly:**

The witness machine is small but it is on the critical path for failover decisions. It must be reliable, well-monitored, on a healthy network, and running the same operational discipline (PTP synchronisation, monitoring alerts, etc.) as the arbiter machines. Treating the witness as second-class hardware is a common Mongo PSA mistake -- the cluster makes the wrong decision at election time because the witness is too slow or unreliable to participate properly.

**External protocol (components contacting the arbiter pool):**

- Components have a configured list of arbiter contact addresses: arbiter primary, arbiter secondary. (The witness is *not* in this list. Components never contact the witness directly.)
- Components try each address in turn until one responds.
- The arbiter that accepts the connection either grants the leadership decision (if it is the currently-active arbiter) or replies "I am not the active arbiter; the active arbiter is X" (if it is the passive arbiter, knowing this from the internal protocol below).
- If the named active arbiter is unreachable (e.g. it has just died), the requester retries the original list, eventually finding the new active arbiter after the internal failover completes.

**Internal protocol (within the arbiter pool):**

- Active arbiter heartbeats to passive arbiter (carrying state replication: the leadership-state map kept in sync) and to witness (liveness only).
- Passive arbiter heartbeats to witness (liveness only).
- On active-arbiter heartbeat loss as observed by passive arbiter: passive arbiter does *not* promote unilaterally. It asks the witness "have you also lost the active arbiter?".
  - If witness confirms ("yes, I have also not heard from active in N seconds"): passive arbiter promotes itself to active, notifies witness of the new state, and begins serving leadership decisions.
  - If witness disagrees ("I just heard from active recently"): passive arbiter does not promote. The active arbiter is still alive, just unreachable from passive's specific network position. Passive enters a degraded state and continues to retry.
  - If witness is unreachable from passive's perspective: passive arbiter cannot promote. The system cannot fail over until either witness contact is restored or active arbiter contact is restored. This is the operational limitation noted below.

**Symmetric-partition handling:**

If the two arbiters lose contact with each other but both can reach the witness:
- Each asks the witness who is currently active.
- Witness's view is authoritative. Whichever arbiter was active continues; the other stays passive.

If only one arbiter can reach the witness:
- That arbiter becomes (or remains) active. The other cannot promote without witness contact and stays passive.

If neither arbiter can reach the witness:
- Neither can promote. The system continues with the previously-active arbiter for the remainder of its lease window, then becomes unavailable for new failover decisions.

**Operational limitation:**

Witness outage during arbiter failover means the system cannot fail over the arbiter pool. The witness is itself a SPOF for the *arbiter-pool failover decision* (not for steady-state operation, where the active arbiter continues to serve leadership decisions to component pairs as normal). A complete answer to this would require redundancy in the witness, but adding witness redundancy degrades the topology mathematically as described above. The accepted operational discipline is: monitor the witness aggressively, repair witness outages quickly, and accept that during a witness outage the arbiter pool itself cannot fail over.

The arbiter slice (slice 8) is where this gets implemented. Earlier slices treat the arbiter as a single-instance external service for testing purposes; the PSA+witness topology only matters once the arbiter itself is real.

**Continuing operation when the arbiter pool is unavailable:**

If the leader of a non-arbiter component is alive and the active arbiter is unreachable, the component leader continues for the lease window (typically tens of seconds; possibly sub-second for tighter tuning). If the entire arbiter pool is unavailable -- both full arbiters and witness all unreachable -- no failover decision can be made and a failure of any component leader during this period leaves the system unavailable until arbiter contact is restored. This is the correct behaviour: no leader can claim leadership without arbiter authorisation, and split-brain is therefore prevented.

### Discussion: consensus libraries vs. lease+epoch

The leader-election problem the arbiter solves is well-studied. Several mature consensus protocols exist (Raft, Multi-Paxos, Viewstamped Replication) and several open-source implementations of these protocols exist in various languages. The framework's chosen approach -- primary/secondary/arbiter with hand-rolled lease+epoch -- deliberately avoids using these. This section names the trade-off honestly.

**Why not use a Raft library:**

- C++ Raft implementations are less mature than their Java or Go counterparts. NuRaft (eBay) and braft (Baidu) are the most credible; both are in production use at their respective companies. NuRaft v3 is approximately a year old at time of writing and contains significant fixes from earlier versions, suggesting the implementation is still evolving in non-trivial ways. Whether this counts as "not yet mature enough" depends on the bar one is setting; for a personal project that aspires to production-grade exchange semantics, the bar is high.
- A library dependency means inheriting bugs the project did not write. A subtle Raft bug discovered at 3 a.m. in production would require debugging code the project's author did not author, in a protocol the project's author cannot easily reason about from first principles. This is a real operational cost.
- Implementing Raft from scratch is harder than it looks. The original Raft paper is unusually clear, but the protocol has many corner cases (log compaction, snapshot installation, configuration changes, lost-vote handling under partition) and the canonical TLA+ model has been refined multiple times. A from-scratch implementation by one person, part-time, is a multi-year project on its own and would likely contain bugs that only surface under specific failure modes.
- The framework only needs leader election for one tiny replicated cell (the arbiter's leadership-state map). Pulling in a full consensus library for one cell is overkill in code-size terms even if it would be sound in correctness terms.

**Why the lease+epoch design is reasonable:**

- The state to replicate is genuinely small: a handful of records (one per component pair). Hand-rolled replication of a small cell over TCP is tractable.
- The deployment topology is fixed and known in advance (two full arbiter instances plus a witness, all on dedicated hardware at the main site, with the witness in a failure-independent location). Many of Raft's complexities (dynamic membership changes, joint consensus during reconfiguration, log catch-up over wide ranges) do not arise.
- The lease+epoch pattern itself is well-understood and is used in production by Chubby, ZooKeeper sessions, etcd leases, Kubernetes leader election. The pattern is not an invention; only the specific implementation here is hand-rolled.
- The framework's overall HA design is per-component, with each component pair having its own arbitration. The arbiter is the leadership-decision service for all of them, but it is not itself on the data path. Performance constraints on the arbiter are mild.

**Limitations of the lease+epoch design (named honestly):**

- **No formal correctness proof.** Raft has a TLA+ model that proves safety. The lease+epoch design as constructed here has no such proof. Subtle bugs are harder to rule out by reasoning alone.
- **The arbiter pool is a PSA+witness three-machine design, not a multi-node Raft cluster.** A 5-node Raft cluster tolerates two simultaneous node failures and continues. The PSA+witness design tolerates one. For most operational scenarios this is fine, but a correlated failure that affects two of the three machines (e.g. the rack containing both an arbiter and the witness) leaves the system unable to make new leadership decisions. Mitigated by failure-independent placement (different power, different network segment) but not eliminated.
- **The arbiter pool's leader election protocol is hand-rolled, not Raft.** Raft has a TLA+ model that proves safety. The PSA+witness lease+epoch protocol as constructed here has no such proof. Subtle bugs in the symmetric-partition handling or the witness-vote logic are harder to rule out by reasoning alone.
- **The arbiter pool is required for new leadership decisions.** If all three arbiter pool members (both full arbiters and the witness) are unreachable, no component pair can fail over until at least the active arbiter or a quorum capable of electing a new active arbiter is restored. This is not a single-point-of-failure (no single machine failure causes it) but it is a service-availability dependency: the design requires the arbiter pool to be running for failover decisions to happen. The lease window mitigates this for healthy component leaders (they continue operating until lease expiry) but not for actually-failed component leaders during arbiter-pool outage. In a Raft-based design with five or more cluster members, two simultaneous member failures are invisible.
- **Sub-second failover is harder.** A short lease (for fast failover) means the arbiter must respond to heartbeats reliably at sub-lease-length intervals. Operationally this puts the arbiter on a tight SLA. Raft has a similar consideration (election timeout) but handles it with quorum-based voting rather than a single-arbiter dependency.
- **Hand-rolled means hand-tested.** Every failure mode (clock skew, network partition between arbiters, simultaneous-restart, message reordering, duplicated messages, slow links) must be designed for and tested for explicitly. Raft libraries have these tests baked in from years of operational use.

**The decision the project makes:**

Build the lease+epoch arbiter from scratch, with the limitations above named and accepted as the engineering cost. This is the realistic choice for a personal-project framework where library-dependency risk is judged greater than implementation-from-scratch risk. If the design surfaces problems that hand-rolled lease+epoch cannot cleanly solve, this decision can be revisited at slice 8 -- but the default is hand-rolled.

The decision should be re-examined under two conditions:

1. If the project's aspiration shifts from "personal framework" to "candidate for production exchange use", the maturity calculus changes. A library with thousands of production-hours has fewer unknown unknowns than a hand-rolled implementation by one person, even if the library is younger.
2. If the lease+epoch design hits a corner case that requires solving a sub-problem of consensus (e.g. needing the arbiter pair to agree on something more complex than a single state cell), the cost-benefit shifts toward the library.

### Topology diagram

The diagram below shows a single instrument's components. Real exchanges run many instruments; the multi-instrument scaling story is open (see Decision Log).

```
                    ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
                    │   fix8      │  │   fix8      │  │   fix8      │
                    │  client A   │  │  client B   │  │  client C   │
                    └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
                           │                │                │
                           │ FIX wire (TCP, port 9879)
                           │ orders, ERs, heartbeats
                           ▼                ▼                ▼
                    ┌─────────────────────────────────────────────┐
                    │             FIX Gateway POOL                 │
                    │  (N gateways; clients pin to one each)       │
                    │                                              │
                    │  - parses FIX, encodes PDU                   │
                    │  - maintains comp-id ↔ ConnectionID table    │
                    │    for FIX sessions on this gateway          │
                    │  - tracks per-leg cursors                    │
                    │  - reconnects on sequencer leader change     │
                    │  - on gateway machine failure, affected      │
                    │    FIX clients reconnect to a different      │
                    │    gateway (FIX-level resend covers any gap) │
                    └────┬─────────────────────────────────────┬───┘
                         │                                     │
              order PDUs │     ER PDUs                         │
              (one TCP   │     (from current leader)           │
              conn each; │                                     │
              gateway    │                                     │
              sends only │                                     │
              to leader; │                                     │
              both kept  │                                     │
              open)      │                                     │
                         ▼                                     ▼
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │  Sequencer: PRIMARY      │         │  Sequencer: SECONDARY    │
        │  (config'd identity)     │         │  (config'd identity)     │
        │  currently LEADER        │         │  currently FOLLOWER      │
        │                          │         │                          │
        │  - assigns seqNo         │         │  - tails leader's WAL    │
        │  - appends to WAL        │         │  - applies records to    │
        │  - state:                │         │    its own WAL & state   │
        │    · FixSession          │         │  - never sends to ME     │
        │      routing tables      │         │  - never sends to GW     │
        │      keyed on            │         │  - heartbeats to arbiter │
        │      (SenderCompID,      │         │  - on promotion: stops   │
        │       TargetCompID)      │         │    tailing, replays its  │
        │    · per-GW cursors      │         │    own WAL, becomes      │
        │  - heartbeats to arbiter │         │    leader (after arbiter │
        │  - sends to ME (leader)  │         │    grants ArbDecision)   │
        │  - sends ERs to GW       │         │                          │
        │                          │         │                          │
        │  ┌────────────────────┐  │         │  ┌────────────────────┐  │
        │  │ WAL on local disk  │  │         │  │ WAL on local disk  │  │
        │  │ (mmap, segmented)  │  │         │  │ (mmap, segmented)  │  │
        │  │ - order records    │  │         │  │ - mirror of leader │  │
        │  │ - cursor records   │  │         │  │   (one record      │  │
        │  │ - dual rolling     │  │         │  │   behind, due to   │  │
        │  │   snapshots        │  │         │  │   ack RTT)         │  │
        │  └────────────────────┘  │         │  │ - own snapshots    │  │
        │                          │         │  └────────────────────┘  │
        └────┬──────────────────┬──┘         └──────────────────────────┘
             │                  ▲                      ▲
             │ order PDUs       │ ER PDUs              │ WAL replication
             │ (only from       │                      │ (TCP, dedicated;
             │  leader)         │                      │  leader pushes,
             │                  │                      │  follower acks;
             │                  │                      │  ER not sent to
             │                  │                      │  GW until follower
             │                  │                      │  has acked the
             │                  │                      │  underlying order)
             ▼                  │                      │
        ┌──────────────────────────┐  ┌──────────────────────────┐
        │  ME: PRIMARY             │  │  ME: SECONDARY           │
        │  (config'd identity)     │  │  (config'd identity)     │
        │  currently LEADER        │  │  currently FOLLOWER      │
        │                          │  │                          │
        │  - receives orders in    │  │  Cancel-on-failover:     │
        │    seqNo order from      │  │  - tails book updates    │
        │    LEADER sequencer      │  │    from primary;         │
        │  - mutates book          │  │    maintains replicated  │
        │  - emits ERs back to     │  │    book                  │
        │    leader sequencer      │  │  - on primary failure,   │
        │  - pushes book updates   │  │    reconciles against    │
        │    to secondary          │  │    sequencer WAL, then   │
        │                          │  │    issues cancel ERs for │
        │                          │  │    outstanding orders    │
        │                          │  │                          │
        │                          │  │  Future seamless model:  │
        │                          │  │  - lockstep parallel     │
        │                          │  │    book; no cancels      │
        │                          │  │  - requires deterministic│
        │                          │  │    ME logic              │
        └──────────────────────────┘  └──────────────────────────┘

        ╔══════════════════════════════════════════════════════════════════╗
        ║                  ARBITER POOL (PSA + witness)                    ║
        ║                                                                  ║
        ║  ┌────────────────────┐  ┌────────────────────┐  ┌────────────┐  ║
        ║  │ Arbiter PRIMARY    │  │ Arbiter SECONDARY  │  │  WITNESS   │  ║
        ║  │ (config'd identity)│  │ (config'd identity)│  │  (no state │  ║
        ║  │ currently ACTIVE   │  │ currently PASSIVE  │  │   votes in │  ║
        ║  │                    │  │                    │  │  elections)│  ║
        ║  │ holds leadership   │  │ replicates from    │  └────────────┘  ║
        ║  │ state map for all  │  │ active; awaits     │                  ║
        ║  │ component pairs    │  │ promotion          │                  ║
        ║  └────────────────────┘  └────────────────────┘                  ║
        ║                                                                  ║
        ║  Internal protocol:                                              ║
        ║   - active heartbeats to passive (state replication) and         ║
        ║     to witness (liveness)                                        ║
        ║   - passive heartbeats to witness (liveness)                     ║
        ║   - on active loss, passive asks witness "do you also see        ║
        ║     active as gone?"; promotes only if witness confirms          ║
        ║   - 3 votes total; majority is 2; split-brain prevented          ║
        ║                                                                  ║
        ║  External protocol (from components):                            ║
        ║   - components contact arbiter primary or secondary by config'd  ║
        ║     list; passive arbiter redirects to active                    ║
        ║   - components NEVER contact the witness directly                ║
        ║                                                                  ║
        ║  Failure-independence requirement (CRITICAL):                    ║
        ║   - witness must be on different power, different switch,        ║
        ║     ideally different network segment from both arbiters         ║
        ║   - otherwise witness provides appearance of redundancy          ║
        ║     without the reality                                          ║
        ║                                                                  ║
        ║   - NEVER on the order/ER data path                              ║
        ║                                                                  ║
        ║  Internal HA mechanism: lease+epoch hand-rolled with witness     ║
        ║  voting. See "Arbiter PSA topology" section for protocol         ║
        ║  mechanics; "Discussion: consensus libraries vs. lease+epoch"    ║
        ║  for trade-off analysis.                                         ║
        ╚══════════════════════════════════════════════════════════════════╝
              ▲                              ▲
              │ heartbeats / lease renewal   │ heartbeats from passive
              │ from each component leader   │ follower instances
              │                              │
              └──────────┬───────────────────┘
                         │
              every component knows about the arbiter pool's contact list;
              only one instance per component pair is leader at any moment

        Channels summary:
        ─────────────────────────────────────────────────────────────────────
        FIX              fix8 ↔ Gateway pool     (TCP, FIX wire)
        Orders           Gateway → Sequencer     (TCP, PDU; only to leader)
        ERs              Sequencer → Gateway     (TCP, PDU; only from leader)
        ME orders        Sequencer → ME          (TCP, PDU; only from leader)
        ME ERs           ME → Sequencer          (TCP, PDU; only to leader)
        WAL replication  Leader → Follower       (TCP, dedicated; bidirectional acks)
                         (per component pair)
        Arbiter control  Components ↔ Active arbiter (TCP; not on data path)
        Arbiter HA       Arbiter↔Arbiter, Arbiter↔Witness
                         (internal lease+epoch + witness vote)
        ─────────────────────────────────────────────────────────────────────
```

A representative deployment requires roughly: 2 sequencer machines, 2 ME machines, N gateway machines, 2 arbiter machines, 1 witness machine. The witness must be in a failure-independent location (different power, different switch, ideally different network segment). Real exchanges have additional components (downstream forwarders, market-data publishers, risk gateways, etc.), each typically a primary-secondary pair on dedicated hardware.

### Implementation staging

Each slice is shippable -- the system works at the end of each. Each slice is a session or two. The whole programme is roughly a year of part-time work but never leaves the system in a half-built state.

1. ~~**Add seqNo to `EventMessage` and to wire format.**~~ ✓ Done (session 17).
2. ~~**In-memory WAL.**~~ ✓ Done (session 17).
3. ~~**mmap'd WAL on disk, single-host, no fsync.**~~ ✓ Done (session 17). `SequencerWal`: mmap'd segments, CRC32 per entry, WAL replay on startup.
4. ~~**Snapshot (single, no rolling).**~~ ✓ Done (session 17). `SequencerWal::take_snapshot()` writes atomically; WAL replay starts from snapshot anchor on restart.
5. ~~**Move FixSession ↔ ClOrdID mapping into sequencer's state.**~~ ✓ Done (session 17). `routing_comp_id` stamping on forwarded ERs; WAL replay rebuilds routing map.
6. ~~**Single-host failover infrastructure.**~~ ✓ Done (session 17). Leader-follower state machine (StatusQuery/StatusResponse, heartbeat, Role enum, fence file, epoch). `ha_enabled=false` (default) makes sequencer start immediately as leader in single-node mode.
7. **Network replication.** ← **Next task.** Follower on another host. Leader streams WAL records over a dedicated TCP connection. Follower acks; commit-for-ER-emission = follower-acked.
8. **Arbiter implementation.** Replaces file-based fencing with the real arbiter (lease+epoch handshake protocol from the Leader-Follower Protocol subsystem section). At this slice, the arbiter's own internal HA is implemented via the PSA+witness topology: two full arbiter instances with hand-rolled lease+epoch replication between them, plus one witness machine in a failure-independent location to break ties. See "Arbiter PSA topology" section.
9. **Dual snapshots, snapshot validation, polish.** The safety-net layer that turns the system from "works" into "operationally bullet-proof".
10. **WAL multi-subscriber generalisation.** Generalise the sequencer's WAL replication channel from "one follower (the secondary sequencer)" to "N followers, each with their own position cursor". Snapshot-and-truncate becomes follower-position-aware: truncation target is the minimum across all follower positions ∧ the snapshot anchor position. This slice unblocks slice 11 and any future WAL-follower components.
11. **Kafka publishing component.** A new component that tails the WAL via the multi-subscriber mechanism from slice 10, derives trades from matched orders, and publishes to an external Kafka cluster via Kafka's idempotent producer protocol. Primary/secondary HA pair using the same arbiter+lease+epoch pattern as other components; cursor replication between Kafka publisher's own primary and secondary preserves position across failover. Kafka idempotency handles the brief overlap window during failover so external Kafka consumers see no duplicates.

Slices 12+ are forward-looking and not yet planned in detail:

- **ME primary-secondary pair** (cancel-on-failover baseline). The ME-secondary maintains a replicated book; on promotion it reconciles against the sequencer's WAL before issuing cancel ERs. Includes the book-replication channel and the reconciliation-on-promotion logic.
- **Gateway pool**. Multiple gateway instances; clients pin to one each; FIX-level reconnect on gateway machine failure.
- **Market data integration mechanism.** Deferred pending requirements clarification with the maintainer of the work system's market data system. Possibilities: another WAL follower (analogous to the Kafka publisher); a topic-based pubsub primitive (only if multiple downstream subscribers with fanout-and-replay semantics emerge); a bespoke mechanism specific to market data's needs. See the "Open Questions" section.
- **Seamless ME failover**. Lockstep or near-lockstep replication. Determinism work. Far in the future.
- **DR site**. A second site with its own arbiter pair, its own component pairs, cross-site replication. Far in the future.
- **Multi-instrument scaling**. Sharded sequencer, instrument groups, etc. Out of scope until single-instrument is operationally proven.

### Production-readiness gap

The framework aspires to be something that could, in theory at least, serve as a foundation for a real exchange system. This section honestly documents the gap between "works for one fix8 client doing one order" and "could plausibly be a candidate for production exchange use", so that the gap is roadmap rather than blind spot.

**Architecturally addressed in the design above (will be addressed in implementation):**

- Single-writer log discipline.
- Decision (commit) separated from effects (ME, ER, FIX out).
- Replayable consumer (ME).
- WAL = truth.
- Snapshot-and-replay.
- Fencing via lease+epoch.
- Per-component HA with shared primitives.
- Halt-on-failure as a correctness boundary, not a bug.

**Designed but not yet on the slicing plan:**

- Sub-second sequencer failover with active heartbeating.
- Tail-latency engineering (P99.9, P99.99). The framework needs measurement infrastructure built in from the start (histograms, jitter sources logged) and discipline against unbounded operations on the hot path.
- Backpressure semantics under load (WAL writer slower than ingress; ME queue full; follower can't keep up).
- Hot-restart and rolling upgrades (swap binary while in flight; standby takes over; upgrade standby; fail back; upgrade now-standby).
- Multi-instrument scaling.
- Auditable historical replay at arbitrary points (regulators ask "what was the order book at 14:32:17.450?").
- Crash-injection testing as continuous integration: random SIGKILL at every WAL append boundary, every commit, every replication ack, run for days.

**Not architectural -- operational and organisational requirements:**

- Operational maturity. Years of incident debugging, runbook accumulation, monitoring rules, deployment automation. Cannot be designed; only earned.
- Regulatory acceptance. Replacing an exchange's order processing infrastructure requires regulator engagement, audit, change-management process measured in years.
- Disaster recovery testing on a regulator's schedule (quarterly site failovers, annual full-day participant tests).
- Compatibility with everything else (existing market data, risk, settlement, back-office). Each downstream consumer depends on quirks not documented anywhere.

The realistic path, if the aspiration is real:

1. Build the framework to the quality where it could plausibly be considered. Sound architecture, all slices, careful tail-latency engineering, comprehensive correctness tests, documented failure modes. Several years of part-time work.
2. Get a venue to take it seriously. Technical merit alone is rarely enough. It requires the alignment of (a) a real pain point that this framework specifically addresses, (b) a sympathetic decision-maker willing to stake reputation on the choice, (c) a graceful migration story that doesn't ask anyone to risk everything at once.
3. Pilot in a low-stakes environment. Test exchange, one non-critical instrument, parallel-run for months. Prove it. Then maybe migrate one real instrument. Then maybe more.
4. Maintain it forever. Once in production, it's a commitment: operational ownership, on-call, regulatory compliance evidence, customer support during failovers.

This section exists to keep the aspiration honest and to stop the project drifting into "builds more features" rather than also "becomes more trustworthy in the specific ways that matter for serious use".

---

## Open Questions and Items to Investigate

This section tracks specific unresolved questions whose answers will inform future design decisions. It is distinct from the "Open" entries in the WAL+HA Design's Decision Log: those are architectural decisions deferred until a slice forces them. The items here are research items -- things the project author needs to find out about, often by talking to people or reading documentation, before the answer can be committed to code.

Each item names what is unknown, what would change once the answer is known, and roughly when the answer needs to be available.

### Market data integration mechanism

**What is unknown:** Exactly what the market data system at the work site consumes from the order placement system. Specifically: what data fields, at what frequency, with what delivery semantics (per-event, batched, snapshot-plus-deltas), with what subscriber count and how subscribers identify themselves, with what gap/reconnection handling, with what regulatory constraints on the delivery path.

**What changes once known:** The framework-side mechanism for delivering equivalent data. Three candidate shapes:
- WAL follower: market data system becomes another consumer of the sequencer's WAL, analogous to the Kafka publisher.
- Topic-based pubsub primitive: justified if the market data side has multiple downstream subscribers with fanout-and-replay semantics that don't fit cleanly as WAL followers.
- Bespoke mechanism: if the market data system has specific requirements that don't fit either of the above.

**Plan:** A conversation with the maintainer of the market data system at work, who has the deepest understanding of what that system does. Communication may proceed through written follow-ups (email or chat) to allow careful confirmation of technical points.

**When needed:** Before slice 12+ designs market data delivery. Slice 11 (Kafka publisher) is unaffected. Earlier slices are unaffected.

### Long-term retention and archival of audit-relevant artefacts

**What is unknown:** Specific regulatory retention requirements for the WAL, the ME audit log, and the Prometheus shared-memory counter files. Periods are typically multi-year (5-7 years for financial trading records is common) but vary by jurisdiction and venue type. The framework's regulatory environment for any deployment scenario isn't yet defined.

**What changes once known:** Operational tooling for log rotation, archival, offsite copies, tamper-detection, and recovery from archives. None of this is core framework code, but the framework's design must support it (e.g. the WAL's segment file format must be archive-friendly; segments must be self-describing enough to restore from archive without the live system being available).

**Plan:** Investigated when a deployment scenario emerges. For the personal-project phase, this is documented but not actively researched.

**When needed:** Before any production deployment. Not for any current slice.

### Operational monitoring of PTP, leases, and arbiter health

**What is unknown:** Specific Nagios (or equivalent) check definitions for the framework's HA and time-sync state. Sketched in the "Time synchronisation and clock skew" and "Arbiter PSA topology" sections but not yet specified at the level of "here is the check_ptp config that this framework requires".

**What changes once known:** A library of Nagios checks shipped alongside the framework, or pointers to standard-issue checks with framework-specific configuration recipes.

**Plan:** Drafted alongside slice 8 (arbiter implementation) when the lease/epoch state actually exists to monitor.

**When needed:** Before any deployment that goes beyond the personal-project test setup.

## HA Architecture (legacy stub -- predates the WAL+HA design above)

The legacy stub described two sequencer instances with the gateway dual-publishing every order PDU to both, so a follower stayed in sync and failover would be gap-free. That stub never fully landed: session 15 removed the secondary sequencer and the dual-publish mechanism because their semantics under the "behaves as unconditional leader" stub were broken (both sequencers would forward to the ME, producing duplicate fills). The full WAL+HA design above replaces this stub. When the design lands, the secondary returns as a passive follower (not a parallel publisher), order PDUs go only to the leader, and the WAL replication channel keeps the follower in sync.

For the framework's *generic* leader-follower DSL protocol (separate from the sequencer-specific design above),
the five-node topology described in subsystem 12 still applies.
The sequencer-specific design uses a simpler topology (two sequencers + one arbiter, single site)
because matching-engine workloads have different durability constraints than the framework's generic streaming use case.

---

## Running and Testing the System

### Scripts

Five Python scripts live in the project root.

**`devenv.py`** — developer sandbox management. Subcommands: `start`, `stop`, `status`, `restart [name]`. Reads component definitions from the env TOML (`--env`, default `environments/dev.toml`). Starts components in dependency order, stops in reverse. `--no-ha` skips `ha_only=true` components. Exports credentials before start; re-exports on auth service restart.

**`release.py`** — assembles a versioned deployment artefact. Reads version from `CMakeLists.txt`, git hash from `git rev-parse --short HEAD`. Stages `bin/` (deployment binaries), `lib/` (`.so` + jars), `etc/` (config templates from `applications/`), `db/`, `environments/`, `devenv.py`, `deploy.py`, `release.json`. Output: `build/release/pubsub-<version>-<hash>.tar.gz`.

**`deploy.py`** — deploys a release artefact or expands an in-place install. Steps: (1) unpack artefact if `--artefact` given; (2) expand `${...}` placeholders in `etc/**/*.toml` using the env TOML flattened into a substitution namespace; (3) generate self-signed TLS certs via `openssl req -x509` (skip with `--skip-certs` for production CA certs); (4) run `db/create_db.py`; (5) run `db/export_credentials.py`. Use `--skip-db` to skip database steps on re-deploy.

**`start_fix_seq_system.py`** — starts the full system for interactive testing.

```
./start_fix_seq_system.py installed
./start_fix_seq_system.py installed --startup-delay 2.0
./start_fix_seq_system.py installed --valgrind --valgrind_command "valgrind"
```

Starts 7 processes in dependency order: witness → arbiter-primary → arbiter-secondary → order_gateway → sequencer-primary → sequencer-secondary → matching_engine. Monitors for unexpected exits. Ctrl-C sends SIGTERM to all processes.

**`perf_run.py`** — starts the full system, attaches `perf record` to gateway and ME, fires fix8 NOS orders, waits for completion, SIGTERMs everything, then produces per-process perf reports and flamegraph SVGs.

```
./perf_run.py                              # 1 client, 1 burst (1 000 orders)
./perf_run.py --burst=5                    # 1 client, 5 000 orders
./perf_run.py --clients=3 --burst=4        # 3 clients × 4 bursts = 12 000 orders
./perf_run.py installed --burst=2    # explicit install prefix
```

Output goes to `installed/perf/<YYYYMMDD_HHMMSS>/`. Requires `perf` in PATH and the FlameGraph scripts at `/home/marlowa/mystuff/FlameGraph`.

### Manual fix8 testing

fix8 is installed at `/home/marlowa/mystuff/fix8_install`. The test binary and config must be run from that directory:

```
cd /home/marlowa/mystuff/fix8_install
./bin/f8test -c myfix_gateway_client.xml -N GW1
```

`-N GW1` selects the session name from the XML config. Once the FIX Logon is established, interactive commands at the prompt:

| Command | Effect |
|---|---|
| `T` | Send 1 000 NewOrderSingle messages |
| `T` repeated | Each `T` sends another 1 000; type it N times for N × 1 000 orders |
| `d` | Toggle debug output |
| `q` | Quit (sends FIX Logout) |

Add `-d` on the command line for verbose debug output from startup:

```
./bin/f8test -d -c myfix_gateway_client.xml -N GW1
```

The gateway listens for FIX connections on port 9879. The matching engine log at `installed/log/matching_engine.log` contains `ME-ORD-N` entries that confirm each order was processed.

---

## Application Architecture — Sequencer-Based Order Flow

Inspired by the Aeron sequencer pattern. The sequencer is the **sole writer** to the matching engine's input stream, imposing total order on all messages.

**Current state (session 15 end -- single sequencer, no HA):**

```
FIX client
    | raw FIX bytes (RawBytesProtocolHandler)
    v
order_gateway          (single instance)
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
order_gateway --> FIX ER --> FIX client (via cl_ord_id_to_session_)
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
| 7070 | gateway → authentication_service_primary (PDU, ProtocolType::FrameworkPdu) |
| 7071 | gateway → authentication_service_secondary (PDU, ProtocolType::FrameworkPdu) |
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

---

## Gateway Performance Analysis

Run identifier: **20260525_220617**.
Profiling flags: `perf record --call-graph dwarf -F 999`.
Kernel tuning: `/proc/sys/kernel/kptr_restrict = 0`, `/proc/sys/kernel/perf_event_paranoid = -1`.
Binary: `order_gateway` (RelWithDebInfo, full DWARF).
Workload: fix8 sending 100,000 NewOrderSingles + OrderCancelRequests over loopback (127.0.0.1).

> **Why dwarf instead of fp?**
> With `--call-graph fp` the call chain was lost whenever a sample landed inside a syscall or a kernel function that did not preserve the frame pointer register. This caused 53 % of gateway samples to appear as `[unknown] [k] 0xffffffff…` (genuine kernel addresses hidden by the default `kptr_restrict=1`). Switching to `--call-graph dwarf` records the full register state at sample time and unwinds both userspace and kernel stacks offline using DWARF unwind tables. Setting `kptr_restrict=0` then resolved the kernel symbol names. Data file size grew from ~550 KB to ~12 MB reflecting the richer per-sample data.

### Category breakdown — gateway reactor thread (`sample_fix_gate`)

| Category | % of samples | Notes |
|---|---|---|
| Kernel TCP / net stack (`kernel.kallsyms`) | 39.24 % | Normal for TCP I/O — send/recv, SKB management, scheduler |
| **Netfilter** (`nf_tables` / `nf_conntrack` / `nf_nat`) | **13.04 %** | **Surprise: loopback traffic goes through the full nftables chain** |
| Framework (`libpubsub_itc_fw`) | 8.80 % | Dominated by `ReactorControlCommand` slab operations |
| Application binary (`order_gateway`) | 8.14 % | FIX parsing, serialisation, hashtable, PDU send |
| libc | 7.82 % | Heap allocation (3.34 %), timestamp (0.86 %), memchr/memmove |
| libstdc++ | 1.84 % | |
| vdso | 0.68 % | `gettimeofday` fast-path |

### Netfilter — the most important finding

13 % of all gateway CPU is consumed by nftables/conntrack/NAT processing **loopback packets** (source and destination 127.0.0.1). This is not obvious: nftables hooks fire on every packet regardless of interface, including `lo`. The fix8 test client connects over the loopback interface, so every NOS and ER traverses the full netfilter chain.

Top netfilter symbols:

| Symbol | % |
|---|---|
| `nft_do_chain` | 4.56 % |
| `nft_counter_eval` | 2.82 % |
| `nft_immediate_eval` | 1.39 % |
| `expr_call_ops_eval` | 0.94 % |
| `nf_nat_*` (combined) | 0.60 % |
| `nft_meta_get_eval` | 0.38 % |
| `__nf_conntrack_find_get` | 0.33 % |
| `nf_conntrack_tcp_packet` | 0.39 % |

**Remediation**: flush nftables rules (`nft flush ruleset`) or disable conntrack for loopback before benchmark runs. This recovers the full 13 % at zero code cost.

### Application binary symbols

| Symbol | % | Interpretation |
|---|---|---|
| `parse_fields` | 1.04 % | FIX tag/value scanning (string_view, no copies) |
| `from_chars<int>` | 1.03 % | Integer tag parsing inside `parse_fields` |
| `FixSerialiser::append_field` | 0.77 % | Outbound ER field serialisation |
| `validate_checksum` | 0.75 % | Checksum verification on inbound messages |
| `on_framework_pdu_message` | 0.69 % | ER dispatch from sequencer |
| `handle_new_order_single` | 0.67 % | NOS handler including ER routing setup |
| `unordered_map::operator[]` | 0.55 % | ClOrdID → session routing hashtable |
| `try_extract_message` | 0.48 % | Message boundary detection in parser |
| `send_pdu<NewOrderSingle>` | 0.33 % | PDU encoding to sequencer |
| `_Hashtable::find` | 0.28 % | Hashtable probe (ER routing) |

The inbound path (parse_fields + from_chars + validate_checksum + try_extract_message = **3.30 %**) and the outbound ER path (append_field + unordered_map + _Hashtable = **1.38 %**) are the two addressable clusters within application code.

### Framework symbols — ReactorControlCommand queue

| Symbol | % | Notes |
|---|---|---|
| `pop_slot_from_free_list` | 3.34 % | Slab allocator freelist pop per NOS |
| `run_internal` | 0.95 % | Reactor main loop |
| `deallocate` | 0.73 % | Slab return after command processed |
| `dequeue` | 0.66 % | Lock-free queue dequeue |
| `allocate` | 0.32 % | Slab allocation for outbound PDU |
| `enqueue` | 0.29 % | Lock-free queue enqueue |
| **Total** | **~5.34 %** | Structural cost of app-thread → reactor crossing |

Every NOS crossing the app-thread → reactor boundary allocates and frees a `ReactorControlCommand` slot. This is structural: eliminating it would require batching PDUs or merging the app thread with the reactor thread.

### libc symbols

| Category | Symbols | % |
|---|---|---|
| Heap allocation | `cfree` 1.05 % + `_int_malloc` 1.04 % + `_int_free` 0.92 % + `malloc` 0.33 % | **3.34 %** |
| Timestamp formatting | `__strftime_internal` 0.50 % + `__tz_convert` 0.23 % + `__offtime` 0.13 % | **0.86 %** |
| Memory operations | `__memchr_avx2` 0.78 % + `__memmove_avx_unaligned_erms` 0.55 % | **1.33 %** |

The heap cost (3.34 %) is driven by the outbound `FixMessage` — `unordered_map<int, string>` inside `FixSerialiser` allocates on every ER sent. Replacing it with a flat fixed-size structure would eliminate this.
The timestamp cost (0.86 %) comes from `FixSerialiser::current_utc_timestamp()` being called once per ER; caching it at second resolution would reduce this to near zero.

### Quill backend thread (`Quill_Backend`)

The logger backend thread is a separate profiling process. Top symbols:

| Symbol | % |
|---|---|
| `fmtquill::write` | 4.68 % |
| `fmtquill::write` (lambda) | 1.86 % |
| `vformat_to` | 1.29 % |
| `copy_noinline` | 0.97 % |
| `_populate_transit_event` | 0.97 % |
| `sanitize_non_printable_chars` | 0.71 % |

GW-NOS-RECV and GW-ER-SENT are logged at `Info` level, generating approximately 1 M Quill queue writes per 100 K order run. Dropping these to `Debug` level would eliminate almost all Quill backend activity during benchmarks.

### Kernel TCP symbols (selected)

| Symbol | % | Notes |
|---|---|---|
| `native_queued_spin_lock_slowpath` | 1.99 % | Lock contention in network stack |
| `__memcpy` | 1.18 % | SKB data copy |
| `__tcp_transmit_skb` | 1.17 % | TCP transmit path |
| `_copy_to_iter` | 1.00 % | Scatter-gather copy to userspace |
| `entry_SYSRETQ_unsafe_stack` | 0.99 % | syscall return overhead |
| `net_rx_action` | 0.87 % | Receive softirq processing |
| `tcp_rcv_established` | 0.84 % | TCP fast-path receive |
| `tcp_sendmsg_locked` | 0.69 % | TCP send path |

These are normal for a TCP-over-loopback workload and cannot be reduced without switching to a shared-memory transport (e.g. Unix domain sockets or a custom ring buffer between processes).

### Priority list for further optimisation

1. **Flush nftables rules before benchmarking** — recovers 13 % at zero code cost.
2. **Reduce GW-NOS-RECV / GW-ER-SENT to Debug level** — eliminates ~1 M Quill writes and reduces Quill backend load substantially.
3. **Replace `FixMessage` (outbound ER path) with a flat fixed-size structure** — eliminates 3.34 % heap allocation from libc.
4. **Cache `FixSerialiser::current_utc_timestamp()` at second resolution** — eliminates 0.86 % strftime cost.
5. **Batch `ReactorControlCommand` allocations** — reduces 5.34 % framework overhead; requires API change.
6. **Switch to Unix domain sockets for intra-host connections** — bypasses kernel TCP entirely (39 % of samples); largest possible gain but highest effort.
