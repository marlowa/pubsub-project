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
| Documentation | `docs/index.md` — start here; design docs in `docs/design/`, application docs in `docs/applications/` |

---

## What It Is

A low-latency, multi-threaded, event-driven application framework using the **reactor pattern**. It provides:

- Inter-thread communication (ITC) via lock-free MPSC queues
- Inter-process communication (IPC) via unicast TCP
- Lock-free thread-safe pool allocators
- Topic-based publish/subscribe with fan-out and replay, backed by a per-publisher WAL
(the MEP publishes topics; `topic_probe` and the `TopicSubscriberThread` base subscribe) —
see [Pub/Sub](docs/design/pubsub.md)
- Timers (timerfd, via epoll)
- High availability via primary/secondary instance pairs with arbitration
- A DSL-based binary serialisation layer replacing protobuf/SBE
- Sample applications demonstrating framework usage: a simple order gateway (FIX 5.0 SP2 client connectivity, SCRAM authentication) and a matching engine (order book, execution report generation), forming a minimal exchange system skeleton

Target environment is **low-latency** (sub-100ns encode/decode). Heap allocation is avoided on all hot paths.

---

> For instructions on building, deploying and running the system, see [Running and Testing the System](#running-and-testing-the-system) below.

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

| Class | Description |
|---|---|
| `FixedSizeMemoryPool<T>` | Single fixed-capacity pool backed by `mmap`; Treiber stack free-list with 128-bit tagged CAS; `std::atomic<Slot<T>*> free_next` field in `Slot<T>` (outside union, before canary) eliminates data race on next pointer; `deallocation_count_` atomic for safe statistics without list traversal; Valgrind/TSan mutex fallback |
| `ExpandablePoolAllocator<T>` | Chains `FixedSizeMemoryPool<T>` instances; lock-free fast path, mutex on expansion; pools never removed |
| `BumpAllocator` | Non-owning bump allocator; snprintf contract — always advances `bytes_used()`; `nullptr`+0 = measuring mode; not thread-safe |
| `SlabAllocator` | Single `mmap`-backed slab; bump allocation (reactor thread only); atomic outstanding count; notifies reactor on last-chunk free |
| `ExpandableSlabAllocator` | Chains `SlabAllocator` instances; demand-driven reclamation (no GC thread); Vyukov sentinel deferred-reclamation (`deferred_reclaim_slab_id_`) so popped slabs are destroyed one drain after they are popped, safe against producers still mid-enqueue; wall-clock drain tripwire; returns `std::tuple<int, void*>` for structured bindings |
| `EmptySlabQueue` | Intrusive Vyukov MPSC queue of slab IDs used by `ExpandableSlabAllocator` to collect exhausted `SlabAllocator` instances for reclamation. One node is embedded per slab — no separate allocation needed. Used exclusively by the reactor thread as the consumer; multiple ApplicationThreads may produce concurrently. Consumer never resets head_/tail_ — the Vyukov sentinel pattern relies on the most-recently-popped slab staying alive as the queue's sentinel (deferred-reclaim by one drain cycle, managed by `ExpandableSlabAllocator::deferred_reclaim_slab_id_`). Four `peek_*` const accessors for diagnostics. |

**`Slot<T>` layout (production path, not valgrind):**
```
[ is_constructed (atomic) ][ free_next (atomic) ][ canary (u64) ][ storage (alignas T) ]
```
`free_next` before `canary` — canary remains adjacent to storage for underrun detection.

---

### 2. Lock-Free Queue Subsystem

| Class | Description |
|---|---|
| `LockFreeMessageQueue<T>` | Vyukov MPSC queue; nodes from `ExpandablePoolAllocator<Node>`; watermark hysteresis callbacks; shutdown semantics |
| `QueueConfig` | Watermark thresholds and callbacks |

---

### 3. Threading Subsystem

| Class | Description |
|---|---|
| `ApplicationThread` | Abstract base; owns queue and thread; timer APIs enforced from owning thread; `connect_to_service()` for outbound TCP; pure virtual `on_itc_message()` |
| `ThreadWithJoinTimeout` | Wraps `std::thread`; `join_with_timeout()` |
| `ThreadID` | Strongly-typed thread identifier |
| `ThreadLifecycleState` | NotCreated, Started, InitialProcessed, Operational, ShuttingDown, Terminated |

**Virtual callbacks on `ApplicationThread`:**
- `on_initial_event()`, `on_app_ready_event()`, `on_termination_event(reason)`
- `on_itc_message(msg)` — pure virtual
- `on_timer_event(name)`
- `on_pubsub_message(msg)` — topic records are delivered here (see "Topic pub/sub delivery" below), `on_raw_socket_message(msg)`
- `on_framework_pdu_message(msg)` — **caller must call `allocator.deallocate(msg.slab_id(), msg.payload())` after processing**
- `on_connection_established(id)`, `on_connection_failed(reason)`, `on_connection_lost(id, reason)`

**Topic pub/sub delivery.** A subscriber receives topic records through `on_pubsub_message(msg)`. The reusable header-only `TopicSubscriberThread` base owns the connect/subscribe handshake and inbound topic-PDU routing (delegating to `TopicSubscriberChannel`: dedup by seq_no, periodic truncation ack), decodes each `TopicPage`, and calls `on_pubsub_message` once per fresh record — so a concrete subscriber (`topic_probe`, later TAP) overrides `on_pubsub_message` only and handles no PDUs itself. The delivered `EventMessage` carries `seq_no()` and `pdu_id()`; its `payload()` is a **borrowed zero-copy view valid only for the duration of the call** — the framework owns and releases the underlying inbound slab once the page is delivered, so a subscriber that needs the bytes must copy them. Delivery is synchronous; the record is not re-enqueued as a `PubSubCommunication` reactor event.

---

### 4. Reactor Subsystem

| Class | Description |
|---|---|
| `Reactor` | epoll event loop; owns all threads, timers; inherits `ThreadLookupInterface`; delegates inbound and outbound connection management to dedicated managers |
| `ThreadLookupInterface` | Pure abstract interface with single method `get_fast_path_thread(ThreadID)`; implemented by `Reactor`; allows connection managers to deliver events to threads without depending on `Reactor` |
| `InboundConnectionManager` | Owns all inbound connection state: listener registry, accepted connection maps, accept/read/write/teardown/idle-timeout logic |
| `OutboundConnectionManager` | Owns all outbound connection state: connection maps, connect/read/write/teardown/timeout logic |
| `ReactorConfiguration` | All config: timeouts, slab sizes, HA topology, command queue config, `connect_timeout` (default 5s), `socket_maximum_inactivity_interval_` (default 60s) |
| `ReactorControlCommand` | Commands: `AddTimer`, `CancelTimer`, `Connect`, `Disconnect`, `SendPdu` |
| `ServiceRegistry` | Static service catalog; interns each service to a stable `ServiceID` at registration and maps id→(name, `ServiceEndpoints`); populated before threads start; no file I/O. `connect_to_service(name)` resolves the name to its id up front (fail-fast on unknown), so a `Connect` command carries the integer id, not a `std::string` |
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

| Class | Description |
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

| Class | Description |
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

| Class | Description |
|---|---|
| `TcpSocket` | Non-blocking TCP socket; `TCP_NODELAY` on all sockets; `get_file_descriptor()` for epoll |
| `TcpAcceptor` | Non-blocking listening socket |
| `TcpConnector` | Stateless non-blocking connector; `connect()` + `finish_connect()` + `get_fd()` + `get_connected_socket()` |
| `ByteStreamInterface` | Abstract base: `send()`, `receive()`, `close()`, `get_peer_address()` |
| `InetAddress` | Concrete IP address; factory from host+port string via `getaddrinfo` |

---

### 10. PDU Framing Subsystem

| Class | Description |
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
### 12. Leader-Follower Protocol

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

The epoch is a generation counter that identifies which leadership generation the cluster is currently in. A *stale node* is a node that has been isolated from the cluster (e.g. due to a crash or network partition) and has since missed one or more leadership transitions. When it rejoins, its epoch is lower than the current generation's epoch. The epoch comparison allows the cluster to recognise and correctly demote a returning node without manual intervention, regardless of how it believes it left.

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

- **HA has not considered DR (Disaster Recovery) yet; the design at the moment is for the main site only.**
- **Heartbeat interval and loss threshold:** Implemented. `heartbeat_interval_seconds` (default 5 s) and `heartbeat_timeout_seconds` (default 15 s) are configurable fields in `SequencerConfiguration` and `ArbiterConfiguration`. The ha_test.py HA scenarios rely on these values: scenarios 1–15 all exercise the heartbeat timeout path and confirm correct failover behaviour within the expected window.

---

### 13. Authentication Service and SCRAM-SHA-256

**Overview.** A standalone application (`applications/authentication_service/`) that authenticates FIX gateway clients using SCRAM-SHA-256 (RFC 5802 variant). SCRAM is chosen because it provides mutual authentication without ever transmitting the password in plaintext or storing it in recoverable form — the server stores only derived key material (`StoredKey`, `ServerKey`), so a database breach does not expose client passwords and cannot be used to impersonate the server. The service is stateless: each four-message exchange is self-contained. Two instances run for HA (primary port 7070, secondary port 7071); they share no state and require no synchronisation.

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

**`ScramCrypto` static library** (`libraries/scram_crypto/`). Static library linked by both the authentication service and the gateway. Namespace `scram_crypto`. Free functions: `hmac_sha256`, `sha256`, `pbkdf2_sha256`, `make_scram_credential`, `compute_auth_message`. Depends on `OpenSSL::Crypto` (PRIVATE linkage). `find_package(OpenSSL REQUIRED)` in top-level CMakeLists.

**Current credential store.** A single stub `ScramCredential` (`stub_credential_`) hardcoded in `AuthenticationThread`. Credential database integration is the next major work item; see "Database Access Design" below.

**Mutual authentication.** The gateway verifies the `ServerSignature` in `AuthenticationResult` before completing the FIX Logon. This confirms the service is genuine (not an impostor). If verification fails the gateway sends FIX Logout and disconnects.

---

### 14. Logging Subsystem

Several C++ logging libraries were evaluated, including spdlog, fmtlog, Boost.Log, and log4cxx. Quill was selected primarily for its throughput and latency characteristics: it uses a wait-free single-producer-single-consumer queue per caller thread, offloads all formatting and I/O to a dedicated backend thread, and imposes sub-100 ns overhead on the hot path. The async-first design aligns well with the reactor pattern: `ApplicationThread`s never block on I/O when logging.

`QuillLogger` wrapping `quill::Logger*`. `PUBSUB_LOG(logger, level, fmt, ...)` for format args; `PUBSUB_LOG_STR(logger, level, str)` for single string (required by `-Werror=variadic-macros`).

Log levels: `FwLogLevel::Alert`, `Critical`, `Error`, `Warning`, `Notice`, `Info`, `Debug`, `Trace`. Currently everything is logged at `Info`; level differentiation is a future task.

Any class that needs to log receives a `QuillLogger&` in its constructor and stores it as a member. The Reactor does not own all logging — each class logs for itself.

**Console output (stdout/stderr).** There is no in-process console-capture facility. A `ConsoleCapture` class (fd-level `dup2` redirect into Quill with a signal-safe crash-drain path) once existed but was removed (commit `8609599`): it was never wired into any application, and it only earned its keep in a hostile target environment that masks fatal signals on its own threads. Console output is instead captured by the launcher scripts (`devenv.py`, `start_fix_seq_system.py`, `perf_run.py`), which redirect each component's stdout — with stderr merged — to a per-component `{name}.stdout` file in truncate mode, kept separate from the Quill `{name}.log`.

---

### 15. Database Access Design from C++ (discussed, not yet implemented)

**Note: the RDBMS is in use today.** Credential and access-control data (firms, comp_ids, gateway permissions) are managed via the Java admin service (`java/admin-service/`) using plain JDBC. The `db/export_credentials.py` script exports SCRAM credentials from the database to `credentials.toml` for the authentication service. The design described in this section concerns a future C++ `DatabaseThread` that would allow C++ components to query the database directly; that has not yet been implemented. The principle is to limit direct database access to as few components as possible — currently only the Java admin service and the credential export script touch the database, and that is the preferred architecture.

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

### 17. FIX Codec Library (`fix_codec`)

An **application-tier** FIX codec library (`libraries/fix_codec/`), added 2026-07-17. It sits *above* pubsub, parallel to `scram_crypto`, and is never part of `pubsub_itc_fw` — the framework stays protocol-agnostic (it only mentions FIX in comments as an example "alien protocol"); FIX belongs at the application level. It is the first of a likely small family of exchange-protocol codecs (a binary protocol built on the PDU framing work is intended later as a sibling).

The design borrows the ideas of the hffix library (zero-copy, no heap allocation on any path, field/tag metadata generated from the FIX data dictionary) **without depending on it** — hffix is 12+ years old, has a Haskell code generator, and pulls in Boost. Notably hffix's own generator was Python before 2018, so a pure-Python generator is faithful to its history and fits this project's Python-for-scripts, pylint-10/10 conventions.

**Two parts:**

1. **Generated dictionary header** — a pure-Python generator (`python/tools/generate_fix_dictionary.py` plus the `python/fix_dictionary/` package: `model.py`, `parser.py`, `emitter.py`) parses the QuickFIX-style FIX XML dictionaries (`libraries/fix_codec/data_dictionary/FIXT11.xml` for the session/transport layer and `FIX50SP2.xml` for application messages), merges them by tag number, and emits `fix_dictionary.hpp` (namespace `fix_codec`):
   - `namespace tag` — every field as `inline constexpr int` keyed by canonical FIX name.
   - `namespace msg_type` — message types as `std::string_view` (multi-character msgtypes exist).
   - `namespace enum_values` — enumerated field values grouped by field.
   - `tag_name(int)` — constexpr binary-search tag→name lookup for diagnostics.
   - `is_data_length_tag(int)` / `data_field_for_length_tag(int)` — the DATA↔LENGTH pairing, derived **by name** (`RawData`↔`RawDataLength`, `Signature`↔`SignatureLength` — numeric adjacency fails for Signature, tags 89/93). A FIX parser uses this so a binary DATA field, whose bytes may contain the SOH delimiter, is read by exact length rather than by scanning for SOH.

   The header is generated into the build tree (`${CMAKE_BINARY_DIR}/generated_fix/fix_codec/`) by the `fix_dictionary_generated` target (which depends on `check_standards`), never written into the source tree. Output is deterministic (no embedded timestamp).

2. **Hand-written zero-copy runtime** (header-only `INTERFACE` library):
   - `FixField` — one parsed field (tag + `string_view` value) with lazy typed accessors mirroring hffix's `field_value`: `as_int`/`as_int64`/`as_uint`, `as_char`, `as_bool`, `as_decimal` (mantissa/exponent — no float), and `as_utc_timestamp_ns` (Howard Hinnant's days-from-civil algorithm, no `timegm`). All via `std::from_chars`; no allocation, no locale.
   - `FixMessageReader` — constructed over a borrowed `(const char*, size_t)` window (the same shape `FixParser::feed` receives from a `MirroredBuffer`, so a later gateway migration is a drop-in). Frames one message via BodyLength (never scans for the checksum tag), reports a `Status` (`Valid`/`Incomplete`/`Malformed`/`ChecksumError`), and exposes the fields as a forward range of `FixField` with a `find(tag)` / `find(tag, hint)` (hffix's `find_with_hint`). The iterator honours the generated data-length tags so DATA values may contain SOH. Checksum validation is allocation-free.
   - `FixMessageWriter` — builds a message directly into a caller buffer (e.g. a slab chunk) with no intermediate `std::string`: body fields first, then the tag 8/9 header written backward into a reserved prefix and the tag 10 checksum appended, computing BodyLength and Checksum in place (hffix's `push_back_header`/`push_back_trailer`).
   - `FixChecksum` — allocation-free `compute_checksum` and `checksum_matches` (parses the received three digits with `from_chars` and compares numerically).

**Status.** Library, generator, and tests are complete and green: 21 GoogleTest cases (`libraries/fix_codec/tests/`) and 6 pytest cases (`python/tests/test_fix_dictionary.py`) pass; `fix_codec_tests` is wired into `build.py`'s C++ test run; the `fix_dictionary` package is in the pylint gate (10.00/10); `check_standards` and clang-format are clean. **The `order_gateway` FIX code is intentionally not yet migrated** — that is a later pass which will replace the gateway's hand-maintained `FixMessage`/`FixParser`/`FixSerialiser` and `Tag::`/`MsgType::` tables with this library (and, in doing so, remove the per-message `std::string` allocation the current `FixParser::validate_checksum` still incurs).

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

## Development Sessions

The full session-by-session narrative is in **[SESSIONS.md](SESSIONS.md)**. That file
records what was built, fixed, or investigated in each session and is the primary source
for "how did we get here" questions. Sessions are referenced by number throughout this
summary (e.g. "session 16", "session 25") to indicate when work was completed.

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
- `sequencer` — Slices 1–7 complete. PDU forwarding (NOS, OCR, ER), topology, re-encode fixes all from session 15. WAL (`SequencerWal`: mmap'd segments, snapshot, CRC32, replay on restart), seqNo on wire, `routing_comp_id` stamping, and leader-follower state machine (`Role::unknown/leader/follower`, `adopt_role`, `peer_heartbeat_timeout`, epoch, fence file) from sessions covered by the session-17 entry. Slice 7 (session 18): network WAL replication — leader streams `WalRecord` (id=103) PDUs to follower over peer TCP; follower appends to its WAL and replies `WalAck` (id=104); leader buffers ERs in `pending_er_` keyed by seq_no and gates gateway ER emission on WalAck; follower WAL written exclusively from WalRecord (not from direct gateway PDU); `flush_pending_er()` releases all buffered ERs on peer disconnect (degraded mode). `ha_enabled=false` (default): sequencer immediately adopts `Role::leader` in `on_initial_event` and skips arbiter/peer connects. Both TOMLs now have `ha_enabled=true`.
- `matching_engine` — complete for the round-trip stub. `on_framework_pdu_message` decodes inbound `NewOrderSingle` PDUs (session 15) and emits a fully-filled `ExecutionReport` over the existing outbound `sequencer_er_conn_id_`. The ER populates every field that `SequencerThread`'s ER decoder reads. No real order book or matching — every order becomes a single fill at its limit price (or a zero sentinel for market orders). `OrderID` and `ExecID` are generated as `ME-ORD-N` / `ME-EXEC-N`. `OrderCancelRequest` is not yet handled (logs and drops at the `else` branch); cancel handling is a small follow-up.
- `arbiter` — complete. Implements the `ArbitrationReport`/`ArbitrationDecision` PDU exchange. End-to-end arbiter-mediated election verified by ha_test.py scenario 15 (session 2026-06-03).
- `start_fix_seq_system.py` — runs primary only (secondary launch removed in session 15 pending leader-follower)
- PostgreSQL schema and migration tooling — complete (session 22). `db/create_db.py` idempotent setup script; Liquibase 5.x changelog; three tables: `pubsub_firm`, `pubsub_comp_id` (SCRAM fields, account status, audit timestamps), `pubsub_comp_id_gateway_permission`. Table prefix configurable (default `pubsub_`).
- Java admin service (`java/admin-service/`) — complete (sessions 22–23). Javalin 6 + Freemarker 2.3 + plain JDBC + Pico.css. Full CRUD for firms, comp_ids, and gateway permissions. Password set path: derives SCRAM-SHA-256 → writes to DB → pushes plaintext password to auth service via `SetCredentialRequest` (PDU 510) over TLS. Credential revocation: `RemoveCredentialRequest` (PDU 512) sent when a firm or comp_id is disabled, locked, or deleted. Maven build with Checkstyle, SpotBugs (exclude filter for DI false positives), JaCoCo (80% threshold), and OWASP Dependency Check. Logging: SLF4J API + Logback 1.2.13 (not Log4j2 — Logback is the native SLF4J implementation and needs only one dependency; Log4j2 requires an additional `log4j-slf4j-impl` bridge adapter with no benefit in this context; Javalin 6.3.0 depends on SLF4J 1.x so Logback 1.5.x is incompatible — 1.2.13 is the correct version). `logback.xml` suppresses Javalin/Jetty/HikariCP noise to WARN. `FreemarkerRenderer` registered via `config.fileRenderer()` (Javalin 6 requires explicit registration). Fat JAR built with maven-shade-plugin including signature-file exclusion and ServicesResourceTransformer. Service starts cleanly and responds on port 8080. Admin UI authentication: Jenkins-style login system backed by a TOML file (`admin_users.toml`) — no database dependency. BCrypt-hashed passwords (jbcrypt 0.4, cost 12). Two roles: ADMIN (full CRUD) and VIEWER (read-only; POST routes blocked with 403 by `AuthFilter`). First-run setup wizard creates the initial ADMIN account. Force-password-change flag set on admin-created accounts; user is redirected to `/change-password` on next login. Session auth via Jetty `SessionHandler`; `AuthFilter` runs as Javalin `before()` handler. Pico.css is bundled in the JAR (`src/main/resources/static/`) — no CDN dependency; works in air-gapped corporate environments. Three branding properties in `application.properties`: `brand.name` (product name shown in titles and nav), `brand.logo-url` (logo image in nav and login page), `brand.css-file` (path to a CSS file inlined into every page for colour overrides). See `java/admin-service/README.md` for deployment and branding instructions. Credential lifecycle gap: re-enabling a firm or comp_id, or unlocking a comp_id, does NOT automatically restore the auth service credential (PDU 510 requires the plaintext password, which is never stored); the operator must reset the password afterwards. The Edit forms display a warning when this applies; the full procedure is documented in the README "Credential Lifecycle" section.
- `db/export_credentials.py` — complete (session 23). Exports SCRAM credentials from `pubsub_comp_id` (enabled comp_ids from enabled firms, not locked) to `credentials.toml` in auth service `[[credential]]` TOML format. Uses `psql --csv --tuples-only` with `PGPASSWORD` env var. Atomic write via temp file + rename.

## What Is Not Yet Done (in dependency order)

1. ~~**Re-verify fix8 wrong-port issue is gone**~~ — DONE (session 2026-06-03). `f8test` connected to port 9879, SCRAM auth succeeded, FIX session established. Closed.
2. ~~**Matching engine — `OrderCancelRequest` handling**~~ — DONE (session 2026-06-03). ME now decodes `OrderCancelRequest`, fabricates a `Canceled` ER, and sends it back via the sequencer to the gateway. Verified end-to-end via fix-test-client (place order, cancel order, Execution Report received with `OrdStatus=Canceled`).
3. ~~**Arbiter — end-to-end failover verification**~~ — DONE (session 2026-06-03). Added `VerifyStep` NamedTuple to `ha_test.py` and scenario 15 (`arbiter_mediated_election`). Scenario 15 explicitly verifies each step of the ArbitrationReport/Decision PDU exchange after killing `sequencer_primary`: (a) `sequencer_secondary` sends `ArbitrationReport` to the arbiter pool (confirmed in log in 5.8 s — well within the 15 s `peer_heartbeat_timeout`); (b) `arbiter_primary` sends `ArbitrationDecision` back (confirmed in 0.0 s); (c) `sequencer_secondary` receives the decision (0.0 s); (d) `sequencer_secondary` transitions to leader (0.0 s). Recovery orders flow. Scenario 15 PASS confirmed.
4. ~~**Leader-follower — Slice 7 (network WAL replication)**~~ — DONE. Slice 7 complete (session 18): leader streams `WalRecord` PDUs to follower; follower acks with `WalAck`; leader gates ER emission on ack. WAL sequence number continuity across failover verified by scenario 14 (`wal_recovery`) added in session 2026-05-30: primary kills → secondary becomes leader → 1000 interim orders (seq 1001–2000) → primary restarts, reads WAL, syncs from peer, rejoins as follower → secondary kills → primary re-elected leader → recovery orders continue from seq 2001 with no reset or gap.
5. ~~**`SequencedMessage` wrapper**~~ — the sequencer already forwards to the ME with `send_pdu(me_conn, pdu_id, seq, nos)` where `seq` is the WAL sequence number encoded into `PduHeader.seq_no`; the ME reads `message.seq_no()` to retrieve it. The envelope is already explicit. **Open question for WAL replay:** when a downstream consumer (Kafka publisher, future broadcast) connects with a position cursor, the seq_no in each replayed `WalRecord` already serves as the cursor position identifier. Verify at implementation time that no additional wrapper is needed for the replay/Aeron-style consumer path.
6. ~~**Trace logs in `PduParser` and elsewhere**~~ — DONE. All five `PduParser.cpp` log lines (header fields, raw 16 header bytes, slab alloc, alloc result, payload hex dump) are at `Debug`. `InboundConnectionManager::on_accept` TRACE is `Debug`. `SequencerThread::on_framework_pdu_message` TRACE is `Debug`. Per-PDU `Info` hot-path logs in `SequencerThread` (WAL append, ER forwarding) and `MatchingEngineThread` (sequenced PDU received) also demoted to `Debug` in the same pass.
7. ~~**Pub/sub WAL**~~ — DONE as slice 10. Topic-based fan-out over the WAL, streamed and socket-paced, with replay from a cursor; the MEP is the reference publisher and `topic_probe` the reference subscriber. See [Pub/Sub](docs/design/pubsub.md). This is the long-term replacement for direct TCP and eliminates the rendezvous problem and the retry workaround; the retry logic can be removed as consumers migrate onto topics.
8. ~~**Credential export script**~~ — done (session 23). `db/export_credentials.py` exports DB credentials to auth service `credentials.toml`. Live CRUD updates go via PDU 510/512.
9. ~~**`RestoreCredentialRequest` PDU (514/515)**~~ — DONE (session 2026-06-03). PDU 514/515 added to `authentication.dsl`; `handle_restore_credential_request()` implemented in `AuthenticationThread` (decodes pre-derived SCRAM binary fields, validates sizes, installs into `credentials_` map, persists). `AuthServiceClient.restoreCredential()` added in Java (hex→binary conversion; sends PDU 514, validates PDU 515). `CompIdHandler.update()` now calls `restoreCredential` when transitioning from disabled/locked → enabled+unlocked. `FirmHandler.update()` now calls `restoreCredential` for all enabled+unlocked comp_ids when a firm is re-enabled. Warning notices removed from both Edit form templates. README credential lifecycle table updated to include the new restore actions.
10. ~~**FIX message capture**~~ — DONE (session 2026-06-04). `FixCapture` class (`applications/order_gateway/FixCapture.hpp/.cpp`): gateway thread calls `capture(Direction, data, size, timestamp_ns)` which enqueues a record onto a `std::vector<Record>` queue (protected by mutex; short critical section, no file I/O). A background `std::thread` drains the queue via `condition_variable` and writes binary records to disk. Record format (little-endian): `uint32_t payload_size | int64_t timestamp_ns | uint8_t direction(0=in,1=out) | bytes`. Three capture points in `OrderGatewayThread`: (1) inbound — after `parser.feed()`, captures the consumed bytes of all complete FIX messages; (2) outbound session messages — in `send_fix_to_session`, after serialise; (3) outbound ERs — after `encode_execution_report`. Config: mandatory `[fix_capture] enabled` + `file` fields in `order_gateway.toml`; `enabled=false` in `dev.toml` by default. `capture_` member is `nullptr` when disabled; all three capture calls are guarded by `if (capture_ != nullptr)` so there is zero overhead when capture is off.
11. ~~**WAL replication jitter — Option B fix**~~ — DONE (2026-07-03). Added `prioritise_data_over_timers()` virtual hook to `ApplicationThread` (default `false`). When `true`, the drain loop buffers any `Timer` events encountered and processes them only after all non-timer events in the same drain cycle are exhausted. `SequencerThread` overrides to return `true`. This prevents a heartbeat or snapshot timer from adding latency before a `WalAck` that arrived in the same `epoll_wait` wakeup. All other `ApplicationThread` subclasses are unaffected (FIFO ordering preserved). 583 unit + 33 integration tests pass.
12. ~~**cpu_registry_shm_path configurable from TOML**~~ — DONE (2026-07-03). Added `cpu_registry_shm_path` to all seven application `*Configuration.hpp`, `*ConfigurationLoader.cpp`, and `*.cpp` wiring files. Added `cpu_registry_shm_path = "${shared_reactor_cpu_registry_shm_path}"` to all eleven application TOML templates and `reactor_cpu_registry_shm_path` to all four environment TOMLs. `ReactorConfiguration::cpu_registry_shm_path` is now populated from the TOML rather than falling back to the hardcoded `/dev/shm/pubsub_cpu_registry` default. `deploy.py` already injected the correct install-dir-relative path; the C++ side now reads it. 583 unit + 33 integration tests pass.
13. ~~**FixCapture: replace mutex with SPSC lock-free queue**~~ — DONE (implemented before session log coverage; discovered 2026-07-04). `FixCapture` uses a pre-allocated SPSC ring buffer (`ring_bytes` allocated once at construction, default 64 MB), not a mutex or `std::vector<Record>`. `capture()` packs records directly into the ring via `memcpy` with no malloc. `write_offset_` / `read_offset_` are cache-line-aligned atomics for lock-free SPSC coordination. A sentinel record handles ring wrap-around so records are never split. The writer thread drains via `fread` from the ring. No per-record heap allocation; no mutex. The chosen approach (contiguous ring buffer) is more efficient than `LockFreeMessageQueue` + pool allocator for this use case because it eliminates per-record node allocation/deallocation entirely.
15. ~~**fix-test-client smoke test (Python script driving the Groovy scripting API).**~~ — DONE (2026-07-05). `fix_client_smoke_test.py` (stdlib only) drives the running fix-test-client's REST API. It clears the blotter, then submits two Groovy scripts — phases 1–3 (rapid NOS + a few cancels + ~3 s settle) and phase 4 (a heavy loop of 100+ orders using `fix.uniqueId()` for idempotent, re-runnable ClOrdIDs) — polling `GET /api/script` to `COMPLETED`/`FAILED`. Each script echoes `SENT_NOS`/`SENT_CANCEL` ClOrdIDs in its output. The blotter (`GET /api/messages`) records inbound ERs only, so the validator matches sent-vs-acked from the script output: (a) every sent NOS has a matching New ER (none dropped), (b) every cancel produced a `Canceled` ER (matched via `origClOrdId`), (c) no ER carries an unexpected OrdStatus (only New/Canceled — the ME stub never matches, so no fills). Reports PASS/FAIL with counts; exit 0/1/2. Verified live end-to-end: 128 orders + 3 cancels → PASS, and re-runnable with different counts. pylint 10/10. Depended on item 14 (`fix.uniqueId()`), which is done.
16. **Prometheus metrics.** Add continuous observability so latency analysis does not require post-hoc log archaeology. Currently, measuring phases such as WAL replication lag vs ME processing time requires grepping log timestamps and computing differences by hand. Prometheus histograms would give live p50/p90/p99 breakdowns per phase, updating every scrape interval.

*C++ instrumentation (`prometheus-cpp` or a minimal bespoke atomic-based implementation):*
Hot-path metrics must use only `std::atomic` increments or thread-local accumulators — no locks, no allocations on the measurement path. A dedicated metrics-serving thread (pinned to a non-hot CPU outside the hot-path CPU range) formats and serves the Prometheus text endpoint on HTTP GET `/metrics`. The scrape thread reads the atomics off the hot path entirely. Cost per observation on a cache-warm atomic: 3–5ns, negligible even under heavy load.

Priority metrics:
- `order_latency_ns` histogram labelled by phase: `gw_nos_received`, `seq_wal_roundtrip`, `me_roundtrip`, `gw_er_sent`. This is the single most valuable metric — right now we can only see the total (GW-NOS-RECV to GW-ER-SENT); breaking it into phases would immediately show whether the bottleneck is ME processing or WAL replication without any log mining.
- `app_thread_wakeup_ns` histogram per thread — the ITC latency measurement we currently derive manually by pairing heartbeat timer log lines, made automatic and continuous.
- `seq_pending_er_count` gauge — number of ERs buffered in `pending_er_` on the sequencer primary waiting for a WalAck. Should normally be 0 or 1; a rising value under load indicates replication is falling behind.
- `seq_wal_replication_lag_records` gauge — difference between the leader's current `next_sequence_number` and the last seq_no the follower has acked. Currently invisible.
- `seq_sequence_number` counter — gives throughput directly in Grafana without log parsing.
- Queue depth gauges per `ApplicationThread` — early warning for backpressure situations.

**Note (added 2026-07-26): metrics needed to compare NOS handling between the FIX and binary
gateways.** A day spent comparing the two by profiling and log timestamps produced one solid
number and three things that could not be concluded, and every one of the failures is a case
this item exists to fix. What follows is what that exercise says the instrumentation must
provide.

*What the comparison actually needs.* The two gateways differ only in a bounded segment: what
happens between the client's bytes arriving and the order PDU leaving for the sequencer. The
FIX gateway parses, validates, extracts repeating groups and builds the PDU; the binary gateway
forwards the client's bytes as they arrived. Everything downstream -- sequencer, WAL, matching
engine -- is common. So the metric that answers the question is not end-to-end latency but the
gateway-internal segment, timed identically on both:

- `gw_ingress_ns` histogram, from the reactor delivering client bytes to the envelope being
  handed to the sequencer. This is the segment in which the protocols differ, and the only one
  where a difference can legitimately be attributed to FIX.
- `gw_egress_ns` histogram, from an execution report arriving from the sequencer to the bytes
  being written to the client socket. The FIX gateway decodes the PDU and encodes FIX text; the
  binary gateway relays the payload untouched.
- `gw_decode_ns` and `gw_encode_ns` histograms on the FIX gateway only, as sub-phases of the
  above. On the binary gateway these do not exist rather than reading zero -- a metric that is
  structurally absent is clearer than one that is always empty.

*Every one of these must carry a `gateway` label* (`order_gateway`, `binary_gateway`) and use
**identical bucket boundaries**, so a single Grafana panel can overlay them. Different buckets
would make the two incomparable in exactly the way that is hardest to notice.

*Metrics that make a latency figure interpretable.* Two measurements from 2026-07-26 mean a
gateway latency number is close to meaningless on its own:

- `gw_sessions_active` gauge, per gateway. Median latency was measured at 119us with 4 sessions,
  356us with 20 and 528us with 40, at an identical offered rate -- roughly `sessions^0.7`. Any
  latency figure quoted without the session count that produced it is not reproducible.
- `gw_orders_received_total` and `gw_reports_sent_total` counters, per gateway. Their *rates*
  give throughput directly, which is the number the current harness cannot produce for the FIX
  gateway at all: `perf_run.py` infers FIX completion by polling log lines, and in a fast run
  the poller observes the finish long after it happened. A counter removes the inference. The
  divergence between the two rates is also the live backlog signal -- when orders are offered
  faster than reports come back, latency is queueing and any percentile is measuring queue
  depth, not service time.

*The trap this exercise fell into, which the instrumentation must not repeat.* The first
comparison was dominated not by protocol but by **logging**: the FIX gateway logged a line per
order at Info and the binary gateway logged none, which put 32% of the FIX gateway's samples in
Quill against 11% for the binary one -- more than three times the cost of FIX parsing itself.
Demoting both to Debug and adding a shared `GW-PROGRESS` total halved it. The same asymmetry is
easy to reproduce with metrics: **if one gateway is instrumented more heavily than the other,
the comparison measures the instrumentation.** Both gateways must observe the same histograms at
the same points, and the per-observation cost must stay in the few-nanosecond range the design
above requires.

*What Prometheus will not settle.* Around 70% of both gateways' samples are in the kernel --
TCP, epoll, syscalls -- and that is a deliberate property of the measurement environment, which
must stay comparable with a development VM that has no kernel bypass (see the note on item 6).
Wall-clock histograms of the segments above do include the syscall time inside them, so they
measure the real cost; but no amount of instrumentation will make the userspace difference
between the two gateways larger than the roughly 11% of samples that FIX parsing occupies. The
value of the metrics is to measure that 11% continuously and reliably, not to find something
larger.

**Note (added 2026-07-03 re item 11):** `seq_wal_roundtrip` latency in the histogram above is also the metric needed to measure the practical benefit of the Option B (`prioritise_data_over_timers`) fix. The unit test (`ApplicationThreadTest.PrioritisesDataOverTimers`) proves the ordering guarantee mechanistically, but the magnitude of improvement in the tail distribution is only visible under load. When Prometheus is wired up, compare `seq_wal_roundtrip` p99 before and after to quantify how often a heartbeat or snapshot timer was previously competing with a WalAck in the same drain cycle.

*Java instrumentation (admin service, fix-test-client):* Micrometer with the Prometheus registry. Gauges for FIX session state, counters for messages sent/received. A few lines of Javalin integration per service.

*Deployment:* Prometheus server and Grafana added to the environment configuration. The metrics HTTP endpoint for each C++ process should be on a configurable port, added to the TOML config templates and `dev.toml`. The scrape thread's CPU pinning must be excluded from the hot-path CPU registry so it does not collide with `OrderGatewayThread`, `SequencerThread`, or `MatchingEngineThread`.

**Note (added 2026-07-26): that last sentence is a whole design problem, not a detail.** Keeping the
scrape thread off the pinned cores requires knowing which cores are pinned, and the CPU registry
cannot say reliably: it records what has claimed so far, and no process knows when machine-wide
claiming has finished. A component that starts early computes a mask that silently permits cores
pinned moments later. The same root cause also decides which components get P-cores, so it bears
directly on the gateway comparison above. The design is now agreed -- see the TODO section
"CPU core layout" below and
[docs/design/cpu_pinning_anti_affinity.md](docs/design/cpu_pinning_anti_affinity.md) -- but it is
not built, and it should land before the endpoint does, or the first thing the endpoint measures
will be its own scheduling.

17. ~~**Burst test with WAL replication active.**~~ — DONE (2026-07-05). `fix_client_burst_test.py` (stdlib only) drives the fix-test-client's Groovy API with a tight loop of N NewOrderSingles (default 20,000; `fix.uniqueId()` for idempotent ClOrdIDs), against the current system with `ha_enabled = true` and WAL replication live. It waits for every order's New ER (zero drops), checks for unexpected OrdStatus, and scans the delta of `sequencer_primary.log`/`sequencer_secondary.log` for WAL-path distress (slab/pool exhaustion, errors, drops; backpressure/EPOLLOUT reported as informational since they are handled). Verified live: **50,000 orders submitted at ~34,500 orders/s, all 50,000 acked (zero drops), no slab/pool exhaustion, no distress in the sequencer logs** — the `pending_er_` buffer and WAL TCP channel absorbed the burst. Reports throughput + PASS/FAIL; exit 0/1/2. pylint 10/10. For million-scale peak load, `perf_run.py`'s multi-client fix8 driver is the complement (a 1M-order burst with WAL active also passed this session). Original risk analysis retained below.

The gateway thread is not the bottleneck: it is fully non-blocking (parse NOS → encode PDU → SendPdu command → return to event loop; the ER arrives as a separate later event). Multiple orders are genuinely in-flight simultaneously at different pipeline stages. Under a 1,000-order burst the gateway thread spends nearly all its time parsing and forwarding, not waiting.

The real risks under burst load with WAL active are both in the sequencer:

- **`pending_er_` accumulation.** The sequencer primary buffers each ER in `pending_er_` (a `seq_no → slab-allocated PDU payload` map) until the corresponding WalAck arrives from the follower. Under a burst the ME returns ERs faster than WalAcks arrive, so the map can grow to hundreds of entries simultaneously. If the slab backing those payloads fills up, the sequencer stalls. This is a new failure mode that did not exist before WAL replication. It is exactly what the `seq_pending_er_count` gauge in item 16 (Prometheus) would catch in production; the burst test will expose it first.

- **WAL channel backpressure.** The sequencer reactor streams one `WalRecord` per order over a dedicated TCP connection to the follower. If the follower's `SequencerThread` cannot drain fast enough (measured p90 wakeup latency: 354 µs), the follower's socket receive buffer fills, which fills the sequencer's TCP send buffer, which causes the reactor's `SendPdu` for the next `WalRecord` to block on a partial write. The reactor's `EPOLLOUT`-based partial-send path handles this correctly but adds queueing delay that compounds across the burst.

The burst test is a natural companion to item 15 (fix-test-client smoke test): the smoke test verifies correctness at moderate load; the burst test verifies the WAL replication path does not saturate or exhaust slab memory under peak load. A Groovy script submitted via the fix-test-client scripting API (item 14/15) is the natural driver.

19. ~~**Matching Engine HA wiring.**~~ — DONE (2026-07-05, session HA work). Implemented as four slices: (A) role config + second ME instance; (B) book-replication channel (ME-primary streams `BookUpdate` PDUs to ME-secondary); (C) arbiter-mediated promotion (`ArbitrationReport`/`ArbitrationDecision`, `group=matching_engine`); (D) WAL reconciliation (`MePositionRequest`/`MePositionAck`) + cancel-on-failover, with the leader sequencer promoting its standby connection to re-route sequenced orders to the promoted ME. All three of the original requirements — (a) book-update tailing, (b) arbiter-mediated promotion, (c) WAL reconciliation before cancel ERs (the correctness rule) — are met. Commits: `c7f8b9d` (A+B), `dbbb91b` (C+D), plus `dddb415` (arbiter keyed by `(component-group, instance_id)` so the sequencer/ME/MEP pairs don't alias onto shared `{1,2}` slots) and `b6451d8` (catch-up served by the leader sequencer, not a follower). Verified: `ha_test.py` scenario 16 PASS, a live perf run through a failover, and an orders-in-flight-during-the-gap run (15,000 orders WAL-committed during the ~15 s gap and all replayed to the promoted ME — none dropped). Docs updated: `docs/applications/matching_engine.md`, `docs/design/wal_and_ha.md`. Halt-on-failure remains the fallback for irreconcilable failure modes.

18. ~~**Doxygen navigation layer — clickable architecture maps.**~~ — DONE (2026-07-05).
Implemented with the Graphviz DOT approach chosen below. `docs/architecture.dot` is a
component-topology digraph in which each node carries `URL="\ref <page-label>"`; Doxygen
rewrites the `\ref` into a real link when the graph is embedded via `\dotfile` in
`docs/architecture_map.dox`, producing a clickable client-side image map over the rendered
PNG — click any component box to open that component's documentation. The map links
**directly to the existing markdown docs** via their Doxygen-generated page labels
(e.g. `md_docs_2applications_2matching__engine`), so no per-component stub pages were needed
and the markdown stays pristine for GitHub. Wired in with `DOTFILE_DIRS = docs` in the
`Doxyfile` and a dedicated **System Architecture** section on the mainpage linking both the
map (`\ref architecture_map`) and a maintenance guide (`\ref architecture_map_howto`) that
documents the `URL="\ref …"` mechanism, the label-encoding rule, and an "adding a new
component" recipe. As part of this the docs were made to build cleanly under the existing
`WARN_AS_ERROR = FAIL_ON_WARNINGS`: four pre-existing/introduced warnings were fixed (two
unresolved `#anchor` links in `authentication_service.md`, a `DESIGN.md` link outside the
Doxygen input in `fix_test_client.md`, and an undocumented `logger` constructor parameter in
`TlsRawBytesProtocolHandler.hpp`). Verified: `doxygen Doxyfile` exits 0 with 0 warnings, all
nine component boxes resolve to real doc pages, and both mainpage links render. Sub-maps for
complex components (Reactor, Sequencer) remain an optional future extension. The design
rationale and tool trade-off analysis below are retained for reference.

**Documentation restructure (markdown docs/) — DONE 2026-07-03.** A `docs/` hierarchy was
created and fully populated as a human-navigable alternative to this summary file. Entry
point: `docs/index.md`. Design subsystem docs in `docs/design/` (threading, reactor,
allocators, WAL+HA, serialisation DSL, socket comms, secure comms, CPU pinning, sequencer,
MEP/TAP). Application docs in `docs/applications/` (order gateway, sequencer, matching
engine, admin service, FIX test client). `pubsub_itc_fw_summary.md` is NOT deleted — it
remains the authoritative narrative and session log. The remaining part of item 18 is
specifically the **Graphviz DOT clickable maps in Doxygen**, described below.

### Problem

Doxygen's generated output is comprehensive but navigable only as a tree: classes, files, namespaces. Developers do not naturally think in document trees. When a developer wants to understand `ReferencePriceDataInterface`, they do not want to browse `Architecture → Core → Framework → Reactor → Services`; they want to click on a picture of the system and land in the right place. The documentation tree is fine as a reference index once you know where you are, but it is a poor entry point for orientation.

The root insight (from a design discussion on 2026-06-25): the SVGs are not illustrations — they are part of the navigation layer. A clickable architecture map is a first-class navigation mechanism, not decoration.

### What we are building

A hierarchy of SVG architecture maps embedded directly in the Doxygen HTML output. Each map is a set of labelled rectangles, one per major component. Each rectangle is a hyperlink. Clicking it lands the developer on a curated landing page for that component, from which they can drill into the auto-generated API reference for the relevant classes.

The top-level map covers the whole system. Complex components (Reactor, Sequencer) may have their own sub-maps linking to their internal subsystems.

### Tool choice: Graphviz/DOT

**Why DOT was chosen:**
- Eclipse Public License — genuinely free software with no proprietary hosted component.
- Native Doxygen support: the `\dotfile` command and `@dot`...`@enddot` inline blocks embed DOT diagrams directly into Doxygen HTML. No extra tooling step, no build pipeline change beyond what is already there.
- Clickable links are trivial: adding `URL="doxygen_page_id"` to any node causes `dot -Tsvg` to wrap that node in an SVG `<a href>` element. No JavaScript required; the links work in static HTML.
- Text-based source: the `.dot` files are version-controlled plain text alongside the code they describe. Diffing, reviewing, and updating is the same workflow as editing any other source file.
- Auto-layout: when a component is added or removed, Graphviz recalculates the layout automatically. There is no need to manually reposition boxes.

**Alternatives considered and rejected:**

*draw.io* — Explicitly excluded. Although the desktop application source is Apache-licensed, draw.io is fundamentally a hosted service with a proprietary back end. The project requirement is free software only.

*Mermaid (MIT)* — Free software, Doxygen support since v1.9.3 via `\mermaid` blocks. Syntactically simpler than DOT for some diagram types. Rejected because Mermaid's `click` directive for interactive links is JavaScript-driven: the links only work when the Mermaid JS runtime is present in the browser context. In static Doxygen HTML output (e.g. viewed from a file system or offline) this is not guaranteed. DOT's URL attribute produces native SVG `<a>` elements that work unconditionally.

*PlantUML (GPL)* — Free software, native Doxygen support, SVG with clickable links via `[[URL]]` syntax. Rejected for two reasons: it requires a Java runtime as a separate build dependency, and for simple box-and-arrow architecture maps its syntax is more verbose than DOT with no compensating benefit. DOT is already available on the build machine (Doxygen depends on it).

*Inkscape (GPL)* — Free software, excellent SVG editor, full support for adding hyperlinks to any element via Object Properties. Rejected because it produces a GUI-authored file rather than a text description. The result is harder to maintain in version control, harder to diff and review, and — critically — has no auto-layout: every time a component is added the developer must manually reposition boxes. The maintenance cost over time outweighs the advantage of a visual editor.

*Hand-written SVG XML* — Maximally flexible but completely impractical. As noted in the design discussion: "creating and maintaining a hierarchy of clickable architecture maps by hand in SVG XML would be miserable." Rejected immediately.

### Link target choice: dedicated .dox files

Each rectangle links to a dedicated `.dox` file rather than to an auto-generated class page or a running service URL.

**Why not auto-generated class pages:**
- Each architectural component (Gateway, Reactor, Transport) spans many classes across many files. There is no single class that is the natural landing point for someone trying to understand the component.
- Auto-generated Doxygen page names can shift when Doxygen changes its naming or hashing scheme. URL attributes in `.dot` files would break silently.
- Auto-generated pages show API reference (the *what*). They do not contain architecture rationale, design decisions, invariants, or the *why*.

**Why not service URLs (e.g. `http://localhost:8080`):**
- Only work when the service is running. Documentation should be readable offline and independently of runtime state.

**Why dedicated .dox files:**
- User-defined page IDs are stable: `\page gateway_overview "Order Gateway"` gives the page the ID `gateway_overview`, which never changes unless you rename it deliberately.
- Each `.dox` page is a curated landing page under the author's control: a component overview, design notes, non-obvious invariants, explicit `\ref` links to the key classes within the component.
- The `.dox` files form the translation layer between visual navigation (the SVG maps) and API reference (the auto-generated pages). Each layer serves its purpose without collapsing into another.
- Maintenance of the `.dox` files is forced to be conscious: when the architecture changes, the developer must update both the `.dot` diagram and the relevant `.dox` file. This is a feature — it prevents the navigation layer from silently drifting away from the implementation.

### Intended hierarchy

```
Doxygen mainpage
    └── architecture.dot (top-level SVG, one rectangle per major component)
            │
            ├── docs/gateway.dox          — Order Gateway landing page
            │       overview, design notes, \ref GatewaySession, \ref FixParser, ...
            │
            ├── docs/reactor.dox          — Reactor Framework landing page
            │       overview, \ref Reactor, \ref ApplicationThread, \ref SlabAllocator, ...
            │       (may include a sub-diagram of reactor subsystem internals)
            │
            ├── docs/sequencer.dox        — Sequencer landing page
            │       overview, WAL design, HA state machine, \ref SequencerThread, ...
            │
            ├── docs/matching_engine.dox  — Matching Engine landing page
            │       overview, order book design, \ref MatchingEngineThread, ...
            │
            ├── docs/admin_service.dox    — Admin Service landing page
            │       overview, auth flow, DB schema summary, link to Javadoc
            │
            └── docs/fix_test_client.dox  — FIX Test Client landing page
                    overview, scripting API, link to web UI
```

### Implementation plan

1. Create `docs/architecture.dot` with component nodes, directed edges showing data flow, and `URL` attributes linking to `.dox` page IDs.
2. Write `docs/<component>.dox` stubs with `\page` declarations, one-paragraph overviews, and `\ref` links to key classes. Stubs can be expanded over time.
3. Embed `architecture.dot` in a Doxygen `.dox` page (e.g. `docs/overview.dox` with `\mainpage` or `\page overview "System Overview"`) using `\dotfile docs/architecture.dot`.
4. Build Doxygen and verify: open the generated HTML, click each rectangle, confirm navigation lands on the correct component page and that all `\ref` links resolve.
5. Add sub-diagrams to complex component pages as needed.

14. ~~**fix-test-client scripting: idempotent ClOrdID and example script**~~ — DONE (2026-07-03). Added `uniqueId()` to `FixHelper`: returns `System.currentTimeMillis() + "-" + counter.getAndIncrement()` where `counter` is an `AtomicLong` that never resets, guaranteeing uniqueness across all script runs for the lifetime of the process. Updated `example.groovy` and `buys_sells_and_cancels.groovy` to use `fix.uniqueId()` for all ClOrdIDs. Both scripts are now safely re-runnable without generating duplicate IDs.

## TODO — transport encryption on the binary gateway, and on the PDU paths generally (raised 2026-07-26)

**Undecided. To be discussed with a security specialist before anything is built.**

The binary gateway authenticates with SCRAM-SHA-256 but has **no TLS listener**, so a
client's password crosses the client-to-gateway hop in clear text. SCRAM limits the damage --
the password is never stored, never forwarded, and never leaves the gateway process -- but
that is a property of the credential handling, not of the transport. The FIX gateway does
offer TLS, so the two gateways are not equivalent on this point, and a venue offering one
encrypted and one unencrypted front door would be hard to defend.

The wider question is the more interesting one. The argument for encrypting the binary
gateway is not only the password: **PDUs carry order flow**, and some venues would treat that
as sensitive in its own right -- who is trading what, in what size, at what price, ahead of
anyone else seeing it. On that reading the case for encryption does not stop at the client
edge. It applies to the internal PDU paths too: gateway to sequencer, sequencer to matching
engine, WAL replication between sequencer instances, and the topic streams external
subscribers read. Those are all currently plain TCP.

Points to settle:
- Whether client-edge TLS on the binary gateway is required, optional, or configured per
  deployment as the FIX gateway's is.
- Whether the internal PDU hops need it, and if so whether that is a deployment concern
  (trusted network segment, IPsec) or an application one.
- What it costs. The framework's TLS path exists and is used by the FIX listener and the
  admin channel, so the mechanism is not new -- but encryption on the hot path is exactly
  the kind of thing the perf work in this document is measuring, and the two goals pull
  against each other.

## Measured: latency grows with sessions per gateway (2026-07-26)

Five runs against the binary gateway with `perf_run.py`, all on the Mint dev box, plain
loopback TCP, no kernel bypass. Directly relevant to the fairness question below, because it
shows the problem arising *inside* a single gateway before any question of balancing across
several.

### Throughput ceiling

Offering orders as fast as the socket accepts them (no `--rate`), 20 sessions, 200,000 full
DD-derived orders including both repeating groups:

| Run | Round-trip throughput |
|-----|----------------------|
| First | 73,860 orders/s |
| Second | 57,930 orders/s |

Two runs 16,000/s apart, so **treat the ceiling as "roughly 60-75k/s" rather than a figure**.
Nothing changed between them that should have slowed the order path. Repeats are needed before
quoting a number.

Latency cannot be read from these runs: at 10-15 million orders/s offered into a pipeline
draining at ~60-75k/s, everything queues, and the reported p50 of 2.7-3.4 **seconds** is queue
depth. `binary_load_client` says so in its own output. Use `--rate` for latency.

### Latency versus session count

All three runs offered 10,000 orders/s and achieved ~9,990, all drained in 20ms after the last
send, so throughput is identical and the only variable is the number of client sessions:

| Sessions | min | p50 | p99 | p99.9 | max |
|---------:|----:|----:|----:|------:|----:|
| 4 | 92.8 us | **118.5 us** | 310 us | 1,214 us | 2,368 us |
| 20 | 94.1 us | **356.1 us** | 650 us | 4,651 us | 14,229 us |
| 40 | 96.5 us | **527.9 us** | 1,579 us | 6,799 us | 14,761 us |

Two conclusions, and the first is what makes the second interesting:

- **The pipeline itself is unaffected by session count.** The minimum is flat -- 92.8, 94.1,
  96.5 us across a tenfold change in sessions. One order's round trip through gateway,
  sequencer, matching engine and back costs the same however many sessions exist, which rules
  out per-session state, lookup cost, or anything on the order path.
- **Median latency grows with session count, sub-linearly.** 4 to 20 sessions (5x) costs 3.0x;
  20 to 40 (2x) costs 1.48x. That fits roughly `sessions^0.7`. The tail is worse than the
  median: p99 goes 310 to 650 to 1,579 us, the last step being 2.4x for 2x the sessions, so
  tail latency degrades slightly *super*-linearly even as the median flattens.

The cause is reactor-turn contention: one thread multiplexes every client socket, so an
order's report waits behind other sockets' processing within the same epoll iteration. The
profile agrees -- no hot function, work spread thin, around 75% of samples in the kernel. It is
structural, not a hotspot to optimise away.

**Load and session count compound.** Adding rate to the picture: 10,000/s with 4 sessions gives
p50 119 us; 10,000/s with 20 sessions gives 356 us (3x from sessions); 50,000/s with 20 sessions
-- about 70-86% of the ceiling -- gives 3,962 us, a further 11x from running near saturation.

### Why this belongs next to the fairness TODO

A member's latency depends on how many other members happen to share its gateway: 119 us median
on a quiet gateway, 528 us on one carrying 40 sessions. That is a 4.4x disadvantage arising from
nothing the member did or can control, which is the equitable-access problem in miniature. It
suggests **capacity per gateway is part of the fairness design, not only the number of gateways
or how orders are steered between them**. If sessions per gateway are to be capped to keep
latency uniform, this is the data that says where the cap bites.

### Caveats

One run per data point, on a shared desktop rather than an isolated machine, and the throughput
figures above show run-to-run variance of over 20%. The session-count trend is consistent across
three points and the flat minimum is a strong internal control, but the absolute numbers should
be re-measured before any of them is quoted as a result.

## TODO — gateway availability, fairness and identity (raised 2026-07-26)

Adding the binary gateway made the venue multi-gateway for the first time, and that has
surfaced three questions the current design does not answer. None is a defect in what is
built; all three need deciding before this could be called an exchange design.

**1. The gateway is a single point of failure.** `docs/design/wal_and_ha.md` records the
model as "N-way pooled redundancy": the gateway elects nothing and holds no WAL, so
redundancy is meant to come from running several and letting clients reconnect. Only one is
ever run. If its process or host dies, every member loses connectivity at once — unable to
enter orders, and unable to manage the risk already on the book. A single reactor also puts
a ceiling on inbound TCP, frame decode and dispatch that will be met under volatility.

**2. Load balancing across gateways must preserve deterministic fairness.** The usual
answers — round-robin, least-connections, random — are not available to an exchange. If one
member's order is served in 50us on gateway 1 while an identical order queues 200us on
gateway 2 through uneven balancing or an asymmetric network path, the venue has failed its
duty of fair and equitable access. Participants detect that delta immediately via drop
copies and market data, and the consequence is regulatory rather than technical. Any pooling
scheme here has to be argued for on determinism, not throughput.

**3. Pooled redundancy is weaker than the document claims, because ER routing changed.**
`wal_and_ha.md` still says routing is on the FIX comp-id pair and "not on ConnectionID".
The code routes on `gateway_session_conn_id`; `OrderGatewayThread` marks the comp-id lookup
"legacy -- no longer used for ER routing". That change was right and must be kept: it fixed
ClOrdID collisions between sessions sharing a comp id. But a connection id is gateway-local
and unstable across reconnects, so a client reconnecting to a *different* pool member can no
longer be handed its in-flight reports. Either the document records the trade-off, or
pooled redundancy needs a comp-id-based path back alongside the connection-id one.

**The intended answer: stateless front-end gateways behind the sequencer.** Rather than
load-balancing orders across independent matching gateways, split the tiers: several
redundant gateway processes accept member connections and do only session-layer work
(protocol parse, validation, authentication), and every message they accept is funnelled
into the one strictly-ordered sequencer. The sequencer stamps a monotonic sequence number at
the moment the exchange accepts the message, and that number — not the arrival gateway —
dictates processing order. Fairness then comes from a single ordering authority rather than
from trying to balance load evenly, which is what makes it workable for a venue.

**This system already has that shape**, which is the useful part: the gateways are already
near-stateless and own no book state, the sequencer already assigns the monotonic number, and
`origin_gateway_id` already lets any number of gateways funnel into it while their execution
reports still find their way home. What is missing is only running more than one instance of
a gateway, and the front-end fairness work below.

Note what the sequencer does and does not settle. It makes *processing order* fair and
deterministic however many gateways feed it. It does not equalise the time a message takes to
*reach* it: two members on different gateway instances, or different network paths, can still
see different latencies to the sequencing point. So Pattern A removes the ordering-fairness
problem but leaves the access-latency one, which is what has to be argued about symmetric
paths, pinned cores and per-gateway capacity rather than about sequencing.

**Decided, not yet implemented: one comp id may be logged on only once, venue-wide.**
Today each gateway checks for a duplicate comp id only among its own sessions, so the same
identity can hold a session on the FIX gateway and the binary gateway simultaneously. This
cannot be fixed inside a gateway: two processes cannot see each other's sessions. It needs a
shared authority, and the sequencer is the natural one — it is arbiter-elected so there is
exactly one leader, and both gateways already connect to it. That means a logon round trip
through the sequencer and a new PDU pair, so it is a cross-component protocol change rather
than a local edit.

## TODO — CPU core layout: declared allocation and background by default (raised 2026-07-26, design agreed 2026-07-27)

**Design agreed. Nothing implemented.** Full treatment, including the rejected approaches and the
specific flaw in each, is in
[docs/design/cpu_pinning_anti_affinity.md](docs/design/cpu_pinning_anti_affinity.md). Summary here
so the problem and its answer are visible from this file.

The Prometheus endpoint (item 16) runs its HTTP server on a civetweb background thread that must
not share a core with a hot-path thread. That needs the inverse of the current facility: not "pin
thread T to core C" but "restrict thread T to whatever cores nobody has pinned". Applying such a
mask is easy -- `sched_setaffinity` takes a set, and the civetweb thread id is obtainable.
Determining the set is the problem.

**The registry is a record, not a plan.** `claim_cpus()` reports what has claimed so far. The
anti-affinity mask needs to know what *will* claim, and a process that has not started yet leaves
no trace. So a component that starts early computes a complement covering nearly every core --
including the ones pinned seconds later -- and applies it successfully. The failure is silent.

**The same root cause misallocates P-cores.** Claiming is greedy and follows `devenv.py` start
order, so the gateways, which start last, get what is left. Measured 2026-07-26, same binaries and
configuration: under full HA `OrderGatewayThread` landed on CPU 19 and `BinaryGatewayThread` on
CPU 22, both E-cores; under `devenv.py --no-ha` they landed on CPU 10 and CPU 13, both P-cores.
Nothing in the output records which regime produced a measurement. This is the scheduling form of
the trap already noted under item 16 -- and the symmetry that makes the gateway comparison valid at
all is an accident of arithmetic, since the P/E boundary falls wherever the cumulative thread count
reaches 15 and one extra thread anywhere upstream would split the pair.

**HA narrows the options, but less than first thought.** 24 threads want 15 claimable P-cores, so
nine must sit on E-cores regardless. The Quill backend threads (eight, genuinely off the hot path)
are fair game. Among the followers, only the sequencer's must stay on the hot-path tier: it is
synchronously inside the client round trip, because the leader parks each ER in `pending_er_` and
releases it only on the matching WalAck. The matching engine secondary is not -- `send_book_update()`
is fire-and-forget -- so it can be demoted, and the objection that a promoted follower instantly
becomes latency-critical is answered by re-pinning on promotion, since the dead leader's cores are
freed by the registry's dead-pid eviction the moment it goes. That brings hot-path demand to about
ten threads against fifteen P-cores. **"Mandatory for an operational system" and "latency-critical"
are orthogonal, and the sequencer secondary is the counterexample that proves it** -- the venue
trades without it, yet its ack gates every ER.

**Development is the environment that matters.** Production, preprod and test-1 all run one
component per dedicated host, where there is no contention and anti-affinity is trivial. Only
`dev.toml` puts eight claimants on one workstation -- and that is where every latency measurement
and every protocol comparison is taken. A design that is sound in production and indeterminate in
development yields a system that cannot be characterised.

**Not viable, for the record:** a configured cap on how many programs may run. A maximum is an
upper bound, not an expectation; the two coincide only at exactly full capacity, and `--no-ha`
drops three of the eight claimants so a full-HA count is never reached and waiters block forever.
"Programs running" is also the wrong quantity -- five components never claim at all -- and program
count is a lossy proxy for core demand while `register_extra_thread()` exists.

### The agreed design

**Rank and cut point are two quantities, not one.** "Should this instance get a hot-path core" splits
into a **rank** -- machine-invariant, declared, the order in which entitlement is surrendered when a
machine is short -- and a **cut point** -- computed per machine from its population and real
topology. On a dedicated production host the cut point falls below everything, so a secondary gets
exactly what its primary gets with no special case; on the dev workstation the cut point binds and
the rank decides. A *class* ("this component is background") would have been wrong: there is no
reason to withhold a P-core from `matching_engine_secondary` on a host where nothing else wants one.
Demotion is a consequence of contention, not a property of the component.

**Two declarations, both machine-invariant.** A `[machines.*]` section in the environment TOML lists
which components run on each host (`localhost` recognised, used by `dev.toml`) plus an absolute
`minimum_background_cores` floor; and `hot_path_rank` on `[components.*]` alongside `ha_only`.
  The two tiers are two *ways of using a core* — hot-path is dedicated, one thread per core;
  background is ordinary shared multitasking — so background cores are where everything else runs,
  not idle. `minimum_background_cores` is a small floor (2-6 on any machine) whose first job is
  correctness: the background pool must never be empty, since an empty affinity mask is `EINVAL`
  and every process has at least a Quill backend needing somewhere to run. It binds only on
  uniform-core machines; omitted on the hybrid dev workstation, where the 15-P-core ceiling binds
  first, and inert in production, where a host runs one component wanting two cores out of twenty.
Absence of a rank means background, so forgetting is harmless. The machine list names **every**
process on the host, not only those that pin -- the seven dev components with
`cpu_pinning_enabled = false` currently run anywhere at all, including on the gateways' cores, which
is the hole they were missing.

**Ties are a constraint, not just an ordering.** A rank group is admitted whole or not at all. Both
gateways are rank 1, so they can never be split across core types -- turning the gateway symmetry
from an accident of arithmetic into a structural guarantee.

**`deploy.py` resolves rank to core ids on the target host**, which it can do because it takes no
host argument and already runs there. Same declaration, different resolution on a 32-core
workstation, a 20-core work machine or a small VM. Admission stops (not skips) at the first group
that does not fit, subject to two constraints: enough P-cores on a hybrid machine, and enough cores
left to meet the background reserve. On the dev box that admits ranks 1-4 (14 of 15 P-cores); on a
20-core uniform machine with reserve 6 it admits ranks 1-3 -- reproducing, unprompted, the tiering
that was chosen by hand when the shortfall was first analysed.

**Runtime: background by default, promotion by exception.** Every process starts masked to the
background pool, every thread it creates inherits that mask (verified -- `pthread_create` copies the
creator's), and the Reactor explicitly promotes only the `ApplicationThread`s and the reactor thread.
Promotion works because an affinity mask is not a ratchet -- `sched_setaffinity` is bounded by the
cgroup cpuset, not the current mask. That is also why the design uses `taskset` and not cpusets.
Library threads -- civetweb, a future Kafka client -- never need to be known about. Quill's backends
are still pinned explicitly via `quill::Backend::get_thread_id()`, but to background cores.

The prompt for this was how the author's workplace does it: a per-thread CPU bitmask config
variable, differing per environment, where threads spawned inside libraries get forgotten and then
interfere with important ones. That is a **default-value bug**, not a diligence failure -- absence
of configuration means "run anywhere", and you cannot enumerate what a library will spawn next
release. A bitmask also declares the answer rather than the intent, so it is machine-specific;
declaring a rank and resolving it at deploy time survives a hardware change.

**The mask is applied by a `deploy.py`-generated wrapper script**, not by a particular launcher --
production launch is still undecided (schedulix is one candidate). Whatever invokes
`bin/run_binary_gateway` gets identical behaviour. This covers the JVM components (an affinity mask
is preserved across `execve`, so `taskset` constrains a JVM and every thread it will ever create,
with no Java code), closes the pre-`main()` static-initialiser hole, and doubles as an interposition
point for `perf` / `valgrind` / `gdb`. The wrapper is not the guarantee, though: each C++ component
also self-masks in `main()`, so nothing in the production hot path depends on launcher cooperation.

**`fix_test_client` matters more than it looks.** It pins nothing, is Java, and was written off as
"only a test program". But it drives *both* gateways (`dev.toml:437`), it exists in dev, FT and
**NFT** -- where the numbers are taken -- and it is a load generator, so it saturates cores by
design at exactly the moment of measurement. Pinning restricts the pinned thread and excludes
nobody, and the dev box has no `isolcpus`, so today it can be scheduled straight onto CPUs 19 and 22
while the gateway threads are pinned there. Same shape as the logging asymmetry under item 16.

**`--no-ha` needs no special handling.** The assignment is computed at deploy time over the full
manifest; `--no-ha` is a runtime flag and the skipped components' cores simply sit idle. Identical
assignment across both deployments falls out for free, and nothing in the allocation path needs to
know what an HA component is. (A naming-suffix heuristic for that was considered and rejected: it
misclassifies `witness` and `arbiter_primary`, both `ha_only = true` while being nobody's
secondary.)

**Verification:** a machine-wide affinity audit -- read `Cpus_allowed_list` from
`/proc/*/task/*/status` and report anything overlapping the hot-path pool that is not a declared
hot-path thread. In-process `/proc/self/task` is not enough now that JVMs are in scope.

**Thread counts stay in the code; the TOML never declares them.** Hot-path demand is the reactor
thread plus the registered `ApplicationThread`s — every component registers exactly one, so every
ranked component wants two cores. Threads from `register_extra_thread()` are background by default
and do not count, which removes a real hazard: `OrderGatewayThread` registers `FixCaptureWriter`
*conditionally* on `fix_capture_enabled`, so today the same binary has different core demand
depending on a config flag, and any figure in the TOML would be silently wrong for one setting.
`deploy.py` asks the binary (`--hot-path-thread-count`) rather than reading a number, backed by a
fail-loud startup check when fewer cores are assigned than needed.

**Two points remain flagged as proposals, not settled:** `CpuRegistry` becoming a runtime record and
collision detector rather than the allocator; and one machine-wide layout file written into `run/`.
**Deferred:** re-pin-on-promotion, which under a declared layout becomes "adopt the dead leader's
assignment". **Withdrawn:** a `required_hot_path_rank` per machine — a global rank scale does not
compose with a per-machine assertion, and on the deployments in hand it was unreachable.

## Immediate Next Task

**The CPU core layout, then item 16 — Prometheus metrics.** Every
other near-term item on the roadmap's "Active / Next" list is now done (items 11, 12, 13, 14, 15,
17, 18, 19), which leaves Prometheus as the head of the queue — with one piece of work in front of
it: the metrics thread must be kept off the pinned cores, and the design for that was agreed on
2026-07-27 but not built. It is not a detail of the endpoint; it reaches into the claiming model,
into which components get P-cores at all, and into how every component is launched. Prometheus is
in turn a prerequisite rather than a nicety: gateway performance work is paused
until it lands, because the FIX-versus-binary comparison cannot be settled by profiling and log
timestamps — the 2026-07-26 note under item 16 records exactly which measurements failed and
which metrics would fix each one. The two design constraints that matter most are that both
gateways must be instrumented at the same points with identical histogram buckets (otherwise the
comparison measures the instrumentation), and that no observation may lock or allocate.

Two design questions are open but not blocking: gateway availability/fairness/identity, and
whether the internal PDU hops need transport encryption. Both are recorded as TODO sections
above; the second is waiting on a security specialist and nothing should be built for it yet.

**Item 18 — Doxygen navigation layer (clickable architecture maps) is DONE (2026-07-05).**
The Graphviz DOT clickable maps are implemented: `docs/architecture.dot` (component-topology
digraph with `URL="\ref <page-label>"` nodes) is embedded via `\dotfile` in
`docs/architecture_map.dox`, producing a clickable client-side image map that links directly
to the existing markdown docs. `doxygen Doxyfile` builds clean (0 warnings under
`WARN_AS_ERROR = FAIL_ON_WARNINGS`). See item 18 in "What Is Not Yet Done" for the full record.

**Library unit test coverage is adequate and is NOT a pending task** (confirmed 2026-07-15).
The current LCOV report for `libraries/pubsub_itc_fw/src` shows 80.8% lines (3424/4239) and
72.7% functions (408/561). Some individual files remain low (e.g. `OutboundConnectionManager.cpp`,
`TimerHandler.cpp`, `Reactor.cpp`), but overall coverage is considered sufficient. (An earlier
draft of this section cited 74.4% / 50.3% from 2026-07-09 and named coverage as the next task;
that is superseded.)

## RT scheduling and CPU isolation: machine assessment guide

Option B (item 11) reduces latency outliers caused by timer events competing with WalAck events, but it does not move the median. The median WAL round-trip latency is dominated by three sequential `epoll_wait` wakeups — one per `ApplicationThread` hop — and on a normal Linux desktop or server kernel each wakeup carries 50–200µs of scheduler jitter. To move the median below 100µs consistently, the threads need `SCHED_FIFO` scheduling on CPUs removed from the general scheduler pool.

The feasibility and cost of doing this depends entirely on the target machine. The following assessment procedure determines where a given machine stands and what the improvement path looks like.

### Step 1 — CPU governor

The CPU frequency governor controls whether the CPU boosts to its rated clock. On `powersave` the CPU runs at its minimum frequency most of the time and boosts opportunistically; on `performance` it runs at maximum rated clock at all times. Frequency transitions add jitter to any latency measurement.

```bash
# Show the governor for every CPU
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort -u

# Show the available governors
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
```

If `powersave` is shown, switch to `performance` before drawing any conclusions from latency measurements. This requires no reboot and no code change:

```bash
# Set all CPUs to performance governor (requires root)
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$cpu" > /dev/null
done
```

On cloud instances or VMs the governor may be absent (`no cpufreq`); this is normal and means the hypervisor controls the clock — governor tuning is unavailable.

### Step 2 — CPU topology: P-cores vs E-cores

Intel hybrid CPUs (12th generation / Alder Lake and later) have two core types: Performance cores (P-cores) and Efficiency cores (E-cores). E-cores have lower single-thread performance and higher wakeup latency. If hot-path threads land on E-cores, latency measurements are misleading and CPU pinning must be revised.

```bash
# Show the core type for each logical CPU (Intel hybrid only)
# P-cores report "Intel Core Processor" or similar; E-cores report "Atom"
for d in /sys/devices/system/cpu/cpu*/topology; do
    cpu=$(basename $(dirname $d))
    core_id=$(cat $d/core_id 2>/dev/null)
    echo "$cpu core_id=$core_id $(cat $d/../cpufreq/scaling_driver 2>/dev/null)"
done

# More direct: check /sys/devices/system/cpu/cpuX/acpi_cppc/highest_perf
# P-cores have higher values than E-cores
for cpu in /sys/devices/system/cpu/cpu*/; do
    hp=$(cat ${cpu}acpi_cppc/highest_perf 2>/dev/null)
    [ -n "$hp" ] && echo "$(basename $cpu): highest_perf=$hp"
done | sort -t= -k2 -rn
```

If two distinct `highest_perf` values appear, the higher value is a P-core, the lower is an E-core. On an i9-14900F for example, P-cores are typically CPUs 0–15 and E-cores are 16–31. The CPU registry must restrict hot-path threads to P-cores. This is a configuration change (adjust the `available_cpus` range in the TOML), not a code change.

On a uniform-core machine (AMD, older Intel, server CPUs) this step is a no-op.

### Step 3 — RT priority budget

`SCHED_FIFO` requires the process to have real-time priority capability. Without it, `pthread_setschedparam` will return `EPERM`.

```bash
# Show the current RT priority ceiling for the shell's user
ulimit -r

# Try to actually set SCHED_FIFO at priority 1 (the minimum)
chrt -f 1 echo "SCHED_FIFO works"

# Show what limits.conf grants
grep -r rtprio /etc/security/limits.conf /etc/security/limits.d/ 2>/dev/null
```

If `ulimit -r` returns `0` and `chrt` fails with `Operation not permitted`, the process has no RT priority budget. To grant it without running as root, add a line to `/etc/security/limits.conf` and re-login:

```
# /etc/security/limits.conf
yourusername   -   rtprio   99
```

Alternatively, grant `CAP_SYS_NICE` to the specific binary (survives across logins without a limits.conf change, appropriate for a deployed install):

```bash
sudo setcap cap_sys_nice+ep /path/to/sequencer
sudo setcap cap_sys_nice+ep /path/to/order_gateway
# etc. for each binary that uses ApplicationThread
```

The `sched_rt_runtime_us` throttle (`/proc/sys/kernel/sched_rt_runtime_us`) defaults to 950000 out of a 1000000µs period (95%). Under sustained RT load the kernel enforces this ceiling; threads stall for the remaining 5%. For testing, disable the throttle:

```bash
echo -1 | sudo tee /proc/sys/kernel/sched_rt_runtime_us
```

Disabling RT throttle is safe on a dedicated benchmarking or trading machine but unwise on a shared development machine — a runaway RT thread can make the machine unresponsive. Re-enable it with:

```bash
echo 950000 | sudo tee /proc/sys/kernel/sched_rt_runtime_us
```

### Step 4 — Kernel preemption model

Even with `SCHED_FIFO`, a non-RT kernel can preempt a user-space thread in response to a hardware interrupt. The preemption model determines whether this matters in practice.

```bash
# Show the kernel preemption configuration
grep -E "^CONFIG_PREEMPT" /boot/config-$(uname -r) 2>/dev/null \
    || zcat /proc/config.gz 2>/dev/null | grep -E "^CONFIG_PREEMPT"

# Check for the PREEMPT_RT indicator file
cat /sys/kernel/realtime 2>/dev/null && echo "PREEMPT_RT kernel" || echo "not a PREEMPT_RT kernel"

# Show the running kernel string
uname -r
```

Interpret the results:

| `CONFIG_PREEMPT_*` value | Meaning | Impact on SCHED_FIFO |
|---|---|---|
| `CONFIG_PREEMPT_NONE=y` | Server kernel, no voluntary preemption | Worst; IRQs and long kernel paths can delay RT threads |
| `CONFIG_PREEMPT_VOLUNTARY=y` | Desktop kernel, explicit preemption points | IRQs still preempt; moderate jitter |
| `CONFIG_PREEMPT=y` | Full preemption (non-RT) | IRQs still preempt; better than voluntary |
| `CONFIG_PREEMPT_RT=y` | Full RT preemption | Hardware IRQs handled as threaded IRQs; best determinism |

On Ubuntu, `linux-lowlatency` installs a kernel with `CONFIG_PREEMPT=y`; `linux-rt` (or `linux-realtime` on some releases) installs a `PREEMPT_RT` kernel. Neither requires a hardware change; both require a package install and reboot.

```bash
# Check what lowlatency/RT kernels are available (Ubuntu/Debian)
apt-cache search linux-image | grep -E "lowlatency|realtime|rt-"
```

For the purposes of this system, `linux-lowlatency` is a practical middle ground: eliminates most OS-induced jitter without the operational overhead of a full `PREEMPT_RT` deployment.

### Step 5 — CPU isolation (`isolcpus`)

`isolcpus` is a kernel boot parameter that removes named CPUs from the general scheduler pool. Once isolated, the kernel will not schedule any process or thread on those CPUs unless explicitly assigned. This eliminates the primary source of scheduler interference on the hot-path threads.

```bash
# Check whether isolcpus is already configured
grep isolcpus /proc/cmdline

# Check whether nohz_full is set (stops timer ticks on idle isolated CPUs)
grep nohz_full /proc/cmdline

# Check whether rcu_nocbs is set (moves RCU callbacks off isolated CPUs)
grep rcu_nocbs /proc/cmdline
```

If none of these appear, `isolcpus` is not configured. To add it, edit the kernel boot parameters. On Ubuntu with grub:

```bash
sudo nano /etc/default/grub
# Find the line: GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
# Add to it:    isolcpus=A,B,C nohz_full=A,B,C rcu_nocbs=A,B,C
# where A,B,C are the CPU IDs to reserve for hot-path threads

sudo update-grub
sudo reboot
```

The CPUs listed must match those used by the `ApplicationThread` CPU registry — whichever CPUs the sequencer, gateway, and matching engine are pinned to. `nohz_full` stops the per-CPU timer tick on the isolated CPUs when they have exactly one runnable thread; `rcu_nocbs` moves RCU grace-period processing off those CPUs. Both are recommended alongside `isolcpus` for lowest jitter; without them, isolated CPUs still receive periodic kernel timer interrupts.

After reboot, verify isolation took effect:

```bash
cat /proc/cmdline | grep isolcpus
# The isolated CPUs should no longer appear in the scheduler's runqueue
taskset -c <cpu_id> stress-ng --cpu 1 --timeout 5s &
# Check with htop that only the pinned process runs on that CPU
```

### Summary: what a machine needs for sub-100µs median wakeup

All five steps are independent but cumulative:

| Step | Requires reboot | Expected benefit |
|---|---|---|
| 1. Set governor to `performance` | No | Eliminates clock-scaling jitter; free baseline win |
| 2. Pin hot-path threads to P-cores only (hybrid CPUs) | No | Avoids E-core latency inconsistency |
| 3. Grant RT priority (`rtprio` in limits.conf or `CAP_SYS_NICE`); set `SCHED_FIFO` in `ApplicationThread`; disable RT throttle for benchmarking | No | Prevents userspace preemption; reduces scheduler jitter |
| 4. Install `linux-lowlatency` or `PREEMPT_RT` kernel | Yes | Reduces IRQ preemption; moves from ~50–200µs jitter range to ~5–20µs range |
| 5. Add `isolcpus`+`nohz_full`+`rcu_nocbs` to boot params | Yes | Removes all kernel scheduler interference from hot-path CPUs; the dominant final improvement |

Steps 1–3 can be applied on any machine and verified without disruption. Steps 4–5 require a reboot and are most valuable on dedicated hardware. On a shared development machine, steps 1–3 alone typically bring median wakeup from 150–200µs down to 80–120µs; the full five steps on dedicated hardware with a PREEMPT_RT kernel and isolcpus can reach consistent 5–15µs wakeup latency.

---

A WAL+HA design has been worked through in detail (see "WAL and HA Design" section below) and an eleven-slice implementation plan agreed. **Slices 1–7 are complete** (session-18 entry for Slice 7):

**Slice 7 — Network WAL replication — COMPLETE (session 18).** Leader streams `WalRecord` (pdu_id=103) PDUs to follower over the existing peer TCP connection (7003/7004). Follower appends each record to its own WAL and replies with `WalAck` (pdu_id=104). Leader buffers ERs from the ME in `pending_er_` (keyed by seq_no) and only forwards them to the gateway once the corresponding WalAck arrives. On peer disconnect the buffered ERs are flushed immediately (degraded mode). The follower no longer writes its WAL from the direct gateway PDU path; it writes exclusively from WalRecord, ensuring WAL contents are byte-for-byte identical to the leader's. WAL sequence number continuity across failover verified by scenario 14 (`wal_recovery`) — added and passing.

**Smaller items deferred but still on the list:**
- ~~OrderCancelRequest round trip (item 2)~~ — DONE (session 2026-06-03).
- ~~fix8 wrong-port re-verification~~ — DONE (session 2026-06-03).
- ~~`k`-prefix constants in `ExpandableSlabAllocatorTest`'s fixture~~ — checked clean; no k-prefix violations anywhere in the codebase.
- ~~Quill thread-name population~~ — DONE (session 2026-06-03). `ApplicationThread::run_internal()` calls `pthread_setname_np(pthread_self(), os_name.c_str())` before first log; Quill captures the OS thread name on first log call. Names longer than 15 chars are truncated by the OS limit.
- ~~Quill backend CPU pinning~~ — DONE. Confirmed in startup logs: `CPU pinning: Quill backend thread pinned to CPU N`.
- ~~Sequencer ER inbound idle-timeout killing healthy quiet connections~~ — DONE (session 2026-06-03). Gateway ER inbound listener registered with `idle_timeout_exempt=true`; the 600s timeout no longer applies to this framework-internal connection.
- ~~Hex-dump debug logging on hot path~~ — DONE (session 2026-06-04). All hex dump calls moved to `FwLogLevel::Trace` with level-check guards around string construction; zero evaluation cost at Info/Debug level.

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
- Downstream consumers of order/trade events (Kafka publisher, future broadcast use cases) follow the **WAL-follower pattern**, not topic-based pubsub. Each consumer opens a connection to the sequencer leader, identifies a position cursor, and receives WAL records from cursor onward. The sequencer's WAL replication channel generalises from "one follower (the secondary sequencer)" to "N followers, each with their own cursor". This reuses the framework's existing replication primitive rather than introducing a new pubsub abstraction for a single named consumer. Where multiple downstream consumers with fan-out-and-replay semantics are needed, the framework now provides a **topic-based pub/sub primitive** built on the same WAL (the MEP publishes topics; see [Pub/Sub](docs/design/pubsub.md)) — the two patterns coexist, chosen per consumer.

**Leaning:**

- Per-component HA primitives provided by the framework: a `WAL` data structure, a replication-channel pattern, an arbiter-client API, a fencing-discipline helper. Each component composes these into its own HA strategy. Avoids "every component implements HA differently with different bugs".
- Quill backtrace logging configured on each component's logger: when an `Error` or `Critical` log record fires, a buffered ring of recent diagnostic context is also flushed to the sink. Useful for incident debugging with any log aggregation tool. Quill v11 supports this directly. To-do for the framework when convenient; not blocking any HA slice. See "Statistics and metrics" section below.

**Open:**

- Mechanism for the arbiter's own internal HA. **DONE.** Implemented in `applications/arbiter/` (active/passive pair) and `applications/witness/` (tiebreaker). The active arbiter manages the leadership-state map and replicates to passive via `ArbiterStateRecord`(400)/`ArbiterStateAck`(401). Arbiter active/passive election uses the same StatusQuery/StatusResponse/Heartbeat protocol as the sequencer. When both arbiters are undecided, each sends `ArbiterVoteRequest`(301) to the witness; the witness grants the vote to the lower `instance_id` via `ArbiterVoteResponse`(302). The witness sends no state; it only breaks ties. This is the PSA+witness lease+epoch design from scratch, avoiding consensus library dependencies.
- Sub-second failover target for the sequencer: how aggressively to tune lease and heartbeat intervals. Tighter intervals trade arbiter availability for failover speed. The framework should make this tunable via `ReactorConfiguration` rather than baking in a number.
- DR site topology. Currently the design is main-site only. DR will require additional design work (a separate site, separate machines, presumably its own arbiter pair, its own sequencer pair, and a cross-site replication strategy). Out of scope until the main-site design is implemented.
- Multi-instrument scaling. A real exchange runs hundreds to thousands of instruments. Single sequencer for everything, sharded sequencer per instrument group, or sequencer per instrument? Each has different failover and replay implications. Not in the immediate slicing plan.
- Sequencer-to-gateway connection direction. The framework currently has the sequencer initiating the outbound connection to the gateway's ER inbound listener (a session-13 finding documented elsewhere in this summary). This is the unusual direction; conventional FIX architectures have the gateway as a client of the core. The trade-off: as designed, the sequencer's configuration must list every gateway address, and adding a gateway requires updating the sequencer configuration. The reverse direction (gateway connects outbound to the sequencer for both order send and ER receive) makes the core "anonymous" and easier to scale horizontally, but requires the sequencer to route ERs by lookup against currently-connected gateway sessions rather than by initiating connections. For the framework's current single-instrument scale this is an acceptable operational cost; for production multi-gateway deployments it likely needs reversing. Open until a deployment scenario forces the choice.
- Market data integration mechanism. The reference system has a downstream market data consumer that consumes data published by the order placement system. The exact mechanism and the exact data are not yet settled for this project; the requirements for that consumer are still pending. Until that information is available, the framework-side mechanism for delivering equivalent data cannot be decided. Possibilities range from "another WAL follower" (analogous to the Kafka publisher) to "a topic-based pubsub primitive" (if multi-subscriber fanout is genuinely needed) to "a bespoke market-data-specific mechanism". Tracked in the "Open Questions and Items to Investigate" section.

The detailed design — architecture, authority and roles, WAL format and segmentation,
commit semantics, epoch propagation, per-connection isolation, replication channel,
gateway reconnection, failover targets, ME failover policy, snapshots, WAL truncation,
cold-start MTTR, failure-handling boundaries, arbiter PSA topology, and the
consensus-libraries discussion — is in **[docs/design/wal_and_ha.md](docs/design/wal_and_ha.md)**.

---

## Open Questions and Items to Investigate

This section tracks specific unresolved questions whose answers will inform future design decisions. It is distinct from the "Open" entries in the WAL+HA Design's Decision Log: those are architectural decisions deferred until a slice forces them. The items here are research items -- things the project author needs to find out about, often by talking to people or reading documentation, before the answer can be committed to code.

Each item names what is unknown, what would change once the answer is known, and roughly when the answer needs to be available.

### Market data integration mechanism

**What is unknown:** Exactly what the downstream market data consumer consumes from the order placement system. Specifically: what data fields, at what frequency, with what delivery semantics (per-event, batched, snapshot-plus-deltas), with what subscriber count and how subscribers identify themselves, with what gap/reconnection handling, with what regulatory constraints on the delivery path.

**What changes once known:** The framework-side mechanism for delivering equivalent data. Three candidate shapes:
- WAL follower: the market data consumer becomes another consumer of the sequencer's WAL, analogous to the Kafka publisher.
- Topic-based pubsub primitive: justified if the market data side has multiple downstream subscribers with fanout-and-replay semantics that don't fit cleanly as WAL followers.
- Bespoke mechanism: if the market data consumer has specific requirements that don't fit either of the above.

**Plan:** Gather the requirements for the downstream market data consumer, from whoever has the deepest understanding of what that consumer does. Communication may proceed through written follow-ups to allow careful confirmation of technical points.

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
| `Connect` | `requesting_thread_id_`, `service_id_` (a `ServiceID`; the name is resolved to this id by `connect_to_service`, fail-fast on unknown) |
| `Disconnect` | `connection_id_` |
| `SendPdu` | `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_` |

---

## DSL Subsystem — Full API

Full API reference, wire format table, BumpAllocator two-pass pattern, DSL files in the
project, and benchmark results: **[docs/design/serialisation_dsl.md](docs/design/serialisation_dsl.md)**.

---

## Allocator Subsystem — Full Table

Full class table including `AllocatorConfig`, `PoolStatistics`, and
`AllocatorBehaviourStatistics`: **[docs/design/allocators.md](docs/design/allocators.md)**.

---

## Miscellaneous / Support

| Class/File | Description |
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

| Class | Description |
|---|---|
| `LoggerWithSink` | Logger wired to `TestSink`; in `pubsub_itc_fw` namespace (NOT `test_support`) — important for test compilation |
| `TestSink` | In-memory log sink for test assertions |
| `MisbehavingThreads` | Test helpers that simulate stuck/crashed threads |
| `LatencyRecorder` | Nanosecond-bucket histogram recorder; thread-safe; dump to file |
| `UnitTestLogger` | Logger configured for unit tests |

---

## Gateway Performance Analysis

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

   **Do not pursue this.** (Decided 2026-07-26.) Performance studies here must stay
   comparable with the work development environment, which is a VM with no kernel
   bypass -- no onload, full networking. Removing kernel TCP would make the numbers
   measure a system nobody runs. Both gateways currently spend around 70% of their
   samples in the kernel (binary 68.3%, FIX 71.8%), and that is the floor, not a
   defect: the remaining ~30% of userspace is the whole space in which gateway design
   choices can move the number. Optimise within it, and read any gateway comparison
   with that ceiling in mind.

---

## Session Log (2026-06-02 onwards)

Named session entries are in **[SESSIONS.md](SESSIONS.md)**.
