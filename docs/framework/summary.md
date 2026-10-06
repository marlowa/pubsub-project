# pubsub_itc_fw — Project Summary

## Quick Facts

| Item | Detail |
|---|---|
| Language | C++17 |
| Namespace | `pubsub_itc_fw` |
| Dev compiler | gcc-13 / Linux Mint 22.2 |
| Target compiler | gcc-8.5 / RHEL 8 |
| Build system | CMake, driven by `scripts/build.sh`; the developer loop is `scripts/devsetup.sh` |
| Logging | Quill 11.0.2 |
| Test framework | GoogleTest (C++), pytest (Python DSL tests) |
| License | Apache-2.0 |
| Max line width | 160 characters (clang-format enforced) |
| Documentation | `docs/README.md` — start here; chapters under `docs/`, application docs in `docs/applications/` |

---

## What It Is

A low-latency, multi-threaded, event-driven application framework using the **reactor pattern**. It provides:

- Inter-thread communication (ITC) via lock-free MPSC queues
- Inter-process communication (IPC) via unicast TCP
- Lock-free thread-safe pool allocators
- Topic-based publish/subscribe with fan-out and replay, backed by a per-publisher WAL
(the MEP publishes topics; `topic_probe` and the `TopicSubscriberThread` base subscribe) —
see [Pub/Sub](../pubsub/pubsub.md)
- Timers (timerfd, via epoll)
- High availability via instance pairs, each led by whichever instance holds a majority lease
- A DSL-based binary serialisation layer replacing protobuf/SBE
- Applications built on it, which together form a simplified exchange: FIX 5.0 SP2 and binary order
  gateways with SCRAM authentication, a sequencer that numbers and logs every command, a matching engine,
  a publisher of topics, the arbiters and witness that take part in deciding which instance of each pair
  leads, an authentication service, and an administration service written in Java. They are described in
  [Architecture](../orientation/architecture.md)

Target environment is **low-latency** (sub-100ns encode/decode). Heap allocation is avoided on all hot paths.

---

> For instructions on building, deploying and running the system, see [Running and Testing the System](#fw_running_and_testing) below.

## Architectural Goals

- Threads pinned to specific CPUs
- Lock-free fast paths throughout
- Predictable memory allocation (pool allocators, bump allocators, slab allocators)
- Zero-copy on all PDU paths (inbound and outbound)
- Deterministic shutdown
- Message ordering preserved

---

## Running and Testing the System {#fw_running_and_testing}

### Scripts

The scripts live in `scripts/`; the repository root holds none. The ones below are the
entry points a newcomer needs; `scripts/` holds others for narrower jobs.

**`devsetup.sh`** — the developer loop. Builds, releases, deploys into `installed/`, and runs every
test suite. Use it rather than `build.sh` followed by `deploy.py`, which skips the release step.

**`release.py`** — assembles a versioned deployment artefact. Reads version from `CMakeLists.txt`, git hash from `git rev-parse --short HEAD`. Stages `bin/` (deployment binaries), `lib/` (`.so` + jars), `etc/` (config templates from `applications/`), `db/`, `environments/`, `devenv.py`, `deploy.py`, `release.json`. Output: `build/release/pubsub-<version>-<hash>.tar.gz`.

**`deploy.py`** — deploys a release artefact or expands an in-place install. Steps: (1) unpack artefact if `--artefact` given; (2) expand `${...}` placeholders in `etc/**/*.toml` using the env TOML flattened into a substitution namespace; (3) generate self-signed TLS certs via `openssl req -x509` (skip with `--skip-certs` for production CA certs); (4) run `db/create_db.py`; (5) run `db/export_credentials.py`. Use `--skip-db` to skip database steps on re-deploy.

**`devenv.py`** — manages the developer sandbox: `start`, `stop`, `status`, `restart [name]`. Reads the environment TOML for the install tree, the component list and the startup order, so it starts the C++ components and the Java ones (admin service, FIX test client) alike. `--supervised` runs each under `launch.py` so a component that dies is restarted; `--no-ha` skips the secondary instances; `--debug` overrides the log level before starting.

```
./scripts/devenv.py start
./scripts/devenv.py --no-ha --debug start
./scripts/devenv.py status
```

Startup order is dependency-driven and counterintuitive in one place: the gateway must be listening before the sequencers start, because the sequencers connect outbound to its execution-report listener. Under valgrind, use `callgrind_run.py`.

**`ha_test.py`** — the high availability suite. Each scenario starts the whole venue, sends orders,
kills, stops or restarts processes, and checks what follows: `./scripts/ha_test.py --scenario N`, or
`--scenario all`. A scenario marked as expected to fail records a known gap: it does not fail the
suite while it fails, and does fail it once it passes.

**`perf_run.py`** — starts the full system, attaches `perf record` to gateway and ME, fires fix8 NOS orders, waits for completion, SIGTERMs everything, then produces per-process perf reports and flamegraph SVGs.

```
./scripts/perf_run.py                              # 1 client, 1 burst (1 000 orders)
./scripts/perf_run.py --burst=5                    # 1 client, 5 000 orders
./scripts/perf_run.py --clients=3 --burst=4        # 3 clients × 4 bursts = 12 000 orders
./scripts/perf_run.py installed --burst=2          # explicit install prefix
```

How to make a latency measurement that can be trusted, and what has been measured, is in
[latency_findings.md](../operations/latency_findings.md).

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
| `ThreadLifecycleState` | NotCreated, Created, Started, InitialProcessed, Operational, ShuttingDown, Terminated |

**Virtual callbacks on `ApplicationThread`:**
- `on_initial_event()`, `on_app_ready_event()`, `on_termination_event(reason)`
- `on_itc_message(msg)` — pure virtual
- `on_timer_event(id)` — the `TimerID` of the timer that fired
- `on_pubsub_message(msg)` — topic records are delivered here (see "Topic pub/sub delivery" below), `on_raw_socket_message(msg)`
- `on_framework_pdu_message(msg)` — **the thread must call `release_pdu_payload(msg)` once it has finished with the payload**
- `on_connection_established(id)`, `on_connection_failed(reason)`, `on_connection_lost(id, reason)`
- `on_connection_writable(id)` — a connection whose writable notification the thread asked for can accept another frame

**Topic pub/sub delivery.** A subscriber receives topic records through `on_pubsub_message(msg)`. The reusable header-only `TopicSubscriberThread` base owns the connect/subscribe handshake and inbound topic-PDU routing (delegating to `TopicSubscriberChannel`: dedup by seq_no, periodic truncation ack), decodes each `TopicPage`, and calls `on_pubsub_message` once per fresh record — so a concrete subscriber (`topic_probe`, later OAR) overrides `on_pubsub_message` only and handles no PDUs itself. The delivered `EventMessage` carries `seq_no()` and `pdu_id()`; its `payload()` is a **borrowed zero-copy view valid only for the duration of the call** — the framework owns and releases the underlying inbound slab once the page is delivered, so a subscriber that needs the bytes must copy them. Delivery is synchronous; the record is not re-enqueued as a `PubSubCommunication` reactor event.

---

### 4. Reactor Subsystem

| Class | Description |
|---|---|
| `Reactor` | epoll event loop; owns all threads, timers; inherits `ThreadLookupInterface`; delegates inbound and outbound connection management to dedicated managers |
| `ThreadLookupInterface` | Pure abstract interface with single method `get_fast_path_thread(ThreadID)`; implemented by `Reactor`; allows connection managers to deliver events to threads without depending on `Reactor` |
| `InboundConnectionManager` | Owns all inbound connection state: listener registry, accepted connection maps, accept/read/write/teardown/idle-timeout logic |
| `OutboundConnectionManager` | Owns all outbound connection state: connection maps, connect/read/write/teardown/timeout logic |
| `ReactorConfiguration` | All config: timeouts, slab sizes, HA topology, command queue config, `connect_timeout` (default 5s), `socket_maximum_inactivity_interval_` (default 60s) |
| `ReactorControlCommand` | Commands: `AddTimer`, `CancelTimer`, `Connect`, `Disconnect`, `SendPdu`, `SendRaw`, `CommitRawBytes`, `InstallInlinePduHandler`, `RequestWritableNotification`, `PauseReading`, `ResumeReading` |
| `ServiceRegistry` | Static service catalog; interns each service to a stable `ServiceID` at registration and maps id→(name, `ServiceEndpoints`); populated before threads start; no file I/O. `connect_to_service(name)` resolves the name to its id up front (fail-fast on unknown), so a `Connect` command carries the integer id, not a `std::string` |
| `ServiceEndpoints` | Primary + secondary `NetworkEndpointConfig`; secondary port==0 means not configured |
| `ConnectionID` | Strongly-typed connection identifier; 0 = invalid; monotonically increasing from 1; allocated by `Reactor::allocate_connection_id()` which is shared between both managers |
| `OutboundConnection` | Per-connection state for reactor-managed outbound TCP connections (see below) |
| `InboundConnection` | Per-connection state for reactor-managed inbound TCP connections (see below) |

**Key reactor design rules:**
- All socket I/O on reactor thread only
- `fast_path_threads_` written only during init/shutdown, read-only during running
- Connect timeout checked by `on_housekeeping_tick()` via backstop timer, which calls `OutboundConnectionManager::check_for_timed_out_connections()`
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

**`pending_send_` pattern:** `OutboundConnectionManager::drain_pending_send()` is called by the Reactor at the start of `process_control_commands()`. If a `SendPdu` cannot proceed (partial write in flight or connection not yet established), it is stashed in the manager's `pending_send_`. Cleared when `on_write_ready()` completes the send. While a send waits there the reactor takes no further command, so the waiting send is never replaced (BUG-0117), and one waiting send holds up every connection (BUG-0112).

---

### 6. InboundConnection and Protocol Handler Strategy

**`InboundConnection`** is a thin transport shell representing one accepted TCP connection. It owns:
- `TcpSocket` — the accepted socket
- `unique_ptr<ProtocolHandlerInterface>` — the protocol handler (Strategy pattern)
- `last_activity_time_` — for idle timeout enforcement
- `target_thread_id_` — for `ConnectionLost` delivery

**Protocol handler strategy.** There are two because there are two kinds of peer, and the
difference is not a detail of either one.

A peer built on this framework sends framework PDUs, which carry their own length and so can
be split into messages by generic code that need not know what any of them mean. A peer that
is not — an *alien* protocol such as ASCII FIX, or any third-party binary protocol — marks its
message boundaries its own way, and the ways do not resemble each other: FIX carries a body
length and terminates on a checksum field, another protocol will do something else entirely.

The framework could be taught to find a FIX message boundary. It deliberately is not. Framing
knowledge for a particular protocol would make the framework partly a FIX implementation, and
the next protocol would add its own case beside it, in a layer whose whole value is that it
does not care what it is carrying. So alien streams are handed to the application thread as
raw bytes and framed there, by code that is allowed to know what it is reading. That division
is what the two strategies are:

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
| Backpressure | When the buffer is three quarters full, the reactor stops reading the socket, so the kernel's buffer fills and TCP slows the sender; it reads again when the application thread has brought the buffer down to half. A buffer that is completely full when data arrives means the application is not consuming at all, and the connection is closed. [BUG-0104](../bug_list.md#bug_0104) records a stall that can follow a pause |

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

`EventMessage::create_raw_socket_message(connection_id, data, size, tail_position)` — the `tail_position` parameter exists to give the application thread an unambiguous way to detect when the reactor has advanced the tail between two deliveries. Without it, the app cannot reliably distinguish "more data arrived" from "tail advanced and the window shifted", because both can cause `payload_size()` to change in the same direction.

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
- A client that sends part of a message and goes silent leaves the partial message in the buffer; it is closed by the idle timeout.

**Why `tail_position()` is needed**

Without it, the app uses `available < last_available_` to detect a tail advance. This fails when new data arrives simultaneously: the tail advances (shrinking the window) but new bytes also arrive (growing it), so `available` may increase rather than decrease. The `tail_position()` field makes the detection exact and unambiguous.

**Failure handling in `InboundConnectionManager`**

`on_data_ready()`, `on_write_ready()`, `process_send_pdu_command()`, and `process_send_raw_command()` each inspect the `tuple<bool, std::string>` returned by the handler call and call `teardown_connection(id, reason, true)` directly on `!ok`. The handler does not destroy the connection synchronously, so no re-lookup of the connection in the map is required after the call returns.

---

### 8. Messaging Subsystem

| Class | Description |
|---|---|
| `EventMessage` | Move-only envelope; `EventType` tag, payload pointer, `slab_id`, `TimerID`, reason string, originating `ThreadID`, `ConnectionID` |
| `EventType` | None, Initial, AppReady, Termination, InterthreadCommunication, Timer, PubSubCommunication, RawSocketCommunication, FrameworkPdu, ConnectionEstablished, ConnectionFailed, ConnectionLost, ConnectionWritable |

**Key factory methods:**
- `create_framework_pdu_message(data, size, slab_id, connection_id, ...)` — the receiving thread must call `release_pdu_payload`
- `create_raw_socket_message(connection_id, data, size, tail_position, ...)` — for alien protocol byte streams
- `create_connection_established_event(connection_id)`
- `create_connection_failed_event(reason)`
- `create_connection_lost_event(connection_id, reason)`
- `create_connection_writable_event(connection_id)`

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
| `PduHeader` | 24-byte wire header: `byte_count` (u32), `pdu_id` (i16), `version` (i8), `filler_a` (u8), `seq_no` (i64, the sequence number the sequencer gave the record, 0 if none), `canary` (u32), `filler_b` (u32); all multi-byte fields network byte order |
| `PduFramer` | Two-mode send: `send()` builds frame internally (small fixed PDUs, max 256 bytes payload); `send_prebuilt()` zero-copy from slab chunk (large PDUs); both share `continue_send()` / `has_pending_data()` |
| `PduParser` | Zero-copy receive: phase 1 reads the 24-byte header; phase 2 allocates slab chunk and reads payload directly from socket into it; dispatches `FrameworkPdu` EventMessage with slab_id |

**Inbound PDU ownership:** reactor allocates slab → PduParser reads into it → EventMessage carries ptr+slab_id → app thread must call `release_pdu_payload(msg)` after processing.

**Outbound PDU ownership:** app thread allocates slab from `outbound_slab_allocator()` → writes PduHeader + encoded payload → enqueues `SendPdu` → reactor sends via `send_prebuilt()` → reactor deallocates slab when send complete.

---

### 11. DSL Subsystem

Python code generator producing C++17 headers for zero-copy binary encode/decode.

The full reference, including the wire format and the benchmark figures, is
[serialisation_dsl.md](serialisation_dsl.md).

**DSL types:** `i8`, `char`, `i16`, `i32`, `i64`, `bool`, `datetime_ns`, `string`, `array<T>[N]`, `list<T>`, `optional T`, `enum : base`, named message references.

**`char` field type** — single-byte wire format, C++ type `char`. Distinct from `i8` (maps to `int8_t`). For FIX protocol char fields. Enum underlying type `char` generates C++ `char`. Character literals (e.g. `'A'`, `'1'`) accepted in enum entry values.

**`fix_orders.dsl`** — the FIX 5.0 SP2 order messages, generated into the build tree (`generated_dsl/`) from the FIX data dictionary rather than written by hand: `NewOrderSingle` (1000), `OrderCancelRequest` (1001), `ExecutionReport` (1002) and the rest. Prices and quantities are `string`; `TransactTime` is `datetime_ns`; conditionally required fields are `optional`. The hand-written message definitions are `applications/authentication.dsl`, `binary_session.dsl`, `pubsub.dsl` and `topics.dsl`, and `libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl`.

**generate_cpp_from_dsl.py** — takes input DSL path and output **file path** (not directory) as positional arguments, plus `--namespace` and `--topics` flags.

---
### 12. Leader-Follower Protocol

Each component that needs a single writer runs as **a pair of instances**: the sequencer, the
matching engine, the matching engine publisher, and the arbiters. Each instance has a fixed
configured identity -- the **primary** (`instance_id` 1) or the **secondary** (`instance_id` 2) --
and at any moment **one instance holds the leader role and the other the follower role**. The leader
does the component's work; the follower receives the leader's state (the sequencer's log, the
matching engine's book) and does nothing on the order path, ready to take over. The primary normally
leads, because the secondary gives it a head start when both start together, but either can hold
either role: after a failover the secondary leads, and the lead does not move back on its own.
[WAL and High Availability](../availability/wal_and_ha.md) walks through the life of a pair.

How the roles are decided: **an instance leads only while a majority of three voters agrees**: itself, its peer, and a third voter that never
leads -- the active arbiter for a component pair, the witness for the arbiters. The agreement is a
lease that runs for a fixed period and must be renewed; a leader whose lease runs out stops acting at
once. Losing every arbiter does not stop trading, because a leader can renew with its peer alone.

Every leadership generation has an epoch number, which also records which instance leads in it, so
no two instances ever lead at the same epoch. The epoch travels on the lease messages and on the
matching engine's announcement of its role.

| Message | ID | Purpose |
|---|---|---|
| `StatusQuery` | 100 | Identity and epoch, sent when two instances connect |
| `StatusResponse` | 101 | Identity, epoch and current role, in reply |
| `RoleAnnouncement` | 117 | A matching engine tells the sequencers which role it holds and at which epoch |
| `LeaseRequest` | 130 | Ask a voter for a lease, or renew one |
| `LeaseGrant` | 131 | A voter grants it |
| `LeaseRefusal` | 132 | A voter refuses, saying the highest epoch it has granted |
| `ArbiterStateRecord` | 400 | The active arbiter tells the passive one the highest epoch granted in each group |

The rules, why each is needed, the failure table and the model checking are in
[Majority leases](../availability/majority_leases.md); the code is `applications/fix_common/PairLeaseAgent.hpp`
and the classes it uses.

### 13. Authentication Service and SCRAM-SHA-256

**Overview.** A standalone application (`applications/authentication_service/`) that authenticates the members who log on to either order gateway, using SCRAM-SHA-256 (RFC 5802 variant). SCRAM is chosen because it provides mutual authentication without ever transmitting the password in plaintext or storing it in recoverable form — the server stores only derived key material (`StoredKey`, `ServerKey`), so a database breach does not expose client passwords and cannot be used to impersonate the server. The service is stateless: each four-message exchange is self-contained. Two instances serve at once (ports 11070 and 11071 for the gateways in the development environment); they share no state and require no synchronisation.

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

**Credential store.** Each instance reads `credentials.toml` when it starts, a file exported from the database by `db/export_credentials.py`, and the administration service sends it every change afterwards. It holds no database connection; see section 15.

**Mutual authentication.** The gateway verifies the `ServerSignature` in `AuthenticationResult` before completing the FIX Logon. This confirms the service is genuine (not an impostor). If verification fails the gateway sends FIX Logout and disconnects.

---

### 14. Logging Subsystem

Several C++ logging libraries were evaluated, including spdlog, fmtlog, Boost.Log, and log4cxx. Quill was selected primarily for its throughput and latency characteristics: it uses a wait-free single-producer-single-consumer queue per caller thread, offloads all formatting and I/O to a dedicated backend thread, and imposes sub-100 ns overhead on the hot path. The async-first design aligns well with the reactor pattern: `ApplicationThread`s never block on I/O when logging.

`QuillLogger` wrapping `quill::Logger*`. `PUBSUB_LOG(logger, level, fmt, ...)` for format args; `PUBSUB_LOG_STR(logger, level, str)` for single string (required by `-Werror=variadic-macros`).

Log levels: `FwLogLevel::Alert`, `Critical`, `Error`, `Warning`, `Notice`, `Info`, `Debug`, `Trace`. Each component's level is set by `applog_level` in its configuration. Messages that arrive continually, such as heartbeats and lease renewals, are logged at `Debug`, and a change of state at `Info` or above.

Any class that needs to log receives a `QuillLogger&` in its constructor and stores it as a member. The Reactor does not own all logging — each class logs for itself.

**Console output (stdout/stderr).** There is no in-process console-capture facility. Console output is captured by the launcher scripts (`devenv.py`, `perf_run.py`, `callgrind_run.py`), which redirect each component's stdout — with stderr merged — to a per-component `{name}.stdout` file in truncate mode, kept separate from the Quill `{name}.log`.

---

### 15. Database Access

No C++ component uses the database, and the framework contains no database code. The work the
venue does with the database at runtime is done in Java, over JDBC, by the administration service
(`java/admin-service/`), which is the only component that writes it.

What a C++ component needs from the database is read from an export when it starts, and each later
change is sent to it. The authentication service is the example: it reads `credentials.toml`,
which `db/export_credentials.py` exports from the database when the venue is deployed, and the
administration service sends it every change afterwards.

The reason is that a store the venue must be able to reach in order to work turns every outage of
that store into a trading outage, for a cause that has nothing to do with trading.

---

### 16. TLS Subsystem

The framework's raw-bytes connection layer supports TLS, for both inbound (server-side) and outbound (client-side) connections. The FIX order gateway's member listener uses it when `fix_tls_enabled` is set, which it is in the development environment, and the authentication service's administration listener uses it. Connections between the venue's own components are plain TCP.

**New `ProtocolType` value:** `ProtocolType::TlsRawBytes` (value 2). Selecting this type on an `InboundListenerConfiguration` causes `InboundConnectionManager` to create a `TlsRawBytesProtocolHandler` for each accepted connection instead of a `RawBytesProtocolHandler`. The application thread receives the same `RawSocketCommunication` events as it does for plain `RawBytes`; TLS is transparent above the protocol-handler boundary.

**`TlsContext`** (`TlsContext.hpp` / `.cpp`). Wraps an `SSL_CTX`. Non-copyable. Factory methods:
- `create_server(cert_path, key_path, ca_path, require_client_cert)` — server-side context. `ca_path` empty disables client certificate verification.
- `create_client(ca_path, cert_path, key_path)` — client-side context. `ca_path` empty skips server verification.

Both enforce **TLS 1.2 only** -- `TlsContext::apply_common_tls_options()` sets min *and* max proto version to `TLS1_2_VERSION`. This is a deliberate cap, not a floor: QuickFIX/J's MINA `SslFilter` mishandles TLS 1.3 `NewSessionTicket` records and deadlocks, timing the fix-test-client out on logon. Lifting it means removing those two calls and adding the TLS 1.3 cipher groups. Ciphers: AEAD only (`ECDHE-RSA-AES256-GCM-SHA384` and similar). See docs/design/secure_comms.md. One `TlsContext` per listener or outbound service; certificate loading happens once at construction, not per-connection.

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

**Relationship to SCRAM-SHA-256.** SCRAM (see Section 13) provides *authentication* — proof that the client knows the correct password — but does not encrypt the channel. TLS provides *confidentiality and integrity* for the byte stream. In the full production design, the gateway's inbound FIX listener should use `TlsRawBytes` so that both the FIX messages and the SCRAM exchange over that channel are protected in transit. The two mechanisms are complementary: SCRAM authenticates the SCRAM exchange itself (mutual authentication via `ServerSignature`), TLS prevents the exchange from being observed or tampered with by a network eavesdropper. The binary order gateway has no TLS listener, so its SCRAM exchange travels over plain TCP; whether to add one is an open item in the [roadmap](../roadmap.md).

**Where it is used.** The FIX order gateway's member listener (`TlsRawBytes` when `fix_tls_enabled` is set) and the authentication service's administration listener. The binary order gateway and every connection between the venue's components use plain TCP.

---

### 17. FIX Codec Library (`fix_codec`)

An **application-tier** FIX codec library (`libraries/fix_codec/`). It sits *above* pubsub, parallel to `scram_crypto`, and is never part of `pubsub_itc_fw` — the framework stays protocol-agnostic (it only mentions FIX in comments as an example "alien protocol"); FIX belongs at the application level.

The design borrows the ideas of the hffix library (zero-copy, no heap allocation on any path, field/tag metadata generated from the FIX data dictionary) **without depending on it**: hffix is old, has a Haskell code generator, and pulls in Boost. A pure-Python generator fits this project's use of Python for scripts and tools.

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
   - `FixMessageReader` — constructed over a borrowed `(const char*, size_t)` window, the shape the gateway receives from a `MirroredBuffer`. Frames one message via BodyLength (never scans for the checksum tag), reports a `Status` (`Valid`/`Incomplete`/`Malformed`/`ChecksumError`), and exposes the fields as a forward range of `FixField` with a `find(tag)` / `find(tag, hint)` (hffix's `find_with_hint`). The iterator honours the generated data-length tags so DATA values may contain SOH. Checksum validation is allocation-free.
   - `FixMessageWriter` — builds a message directly into a caller buffer (e.g. a slab chunk) with no intermediate `std::string`: body fields first, then the tag 8/9 header written backward into a reserved prefix and the tag 10 checksum appended, computing BodyLength and Checksum in place (hffix's `push_back_header`/`push_back_trailer`).
   - `FixChecksum` — allocation-free `compute_checksum` and `checksum_matches` (parses the received three digits with `from_chars` and compares numerically).

**Where it is used.** The FIX order gateway parses, checks and writes FIX messages with it (`FixParser`, `FixErEncoder`, `FixSerialiser`), and the generated `fix_orders.dsl` comes from the same data dictionary. Its tests are `libraries/fix_codec/tests/` and `python/tests/test_fix_dictionary.py`.

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

---

## ReactorControlCommand Payload Fields by Tag

| Tag | Fields |
|---|---|
| `AddTimer` | `owner_thread_id_`, `timer_id_`, `timer_name_`, `interval_`, `timer_type_` |
| `CancelTimer` | `owner_thread_id_`, `timer_id_` |
| `Connect` | `requesting_thread_id_`, `service_id_` (a `ServiceID`; the name is resolved to this id by `connect_to_service`, fail-fast on unknown) |
| `Disconnect` | `connection_id_` |
| `SendPdu` | `connection_id_`, `slab_id_`, `pdu_chunk_ptr_`, `pdu_byte_count_` |
| `SendRaw`, `CommitRawBytes`, `InstallInlinePduHandler`, `RequestWritableNotification`, `PauseReading`, `ResumeReading` | See the comments on each tag in `ReactorControlCommand.hpp`, and for the last two [reactor.md](reactor.md) |

---

## DSL Subsystem — Full API

Full API reference, wire format table, BumpAllocator two-pass pattern, DSL files in the
project, and benchmark results: **[serialisation_dsl.md](serialisation_dsl.md)**.

---

## Allocator Subsystem — Full Table

Full class table including `AllocatorConfig`, `PoolStatistics`, and
`AllocatorBehaviourStatistics`: **[allocators.md](allocators.md)**.

---

## Miscellaneous / Support

| Class/File | Description |
|---|---|
| `CacheLine<T>` | Aligns `T` to cache line boundary to prevent false sharing |
| `PreconditionAssertion` | Exception thrown on precondition violations (not `assert`) |
| `PubSubItcException` | Framework-level exception base |
| `WrappedInteger<Tag, T>` | Type-safe integer wrapper; base for `ThreadID`, `TimerID`, `ConnectionID`; `is_valid()` returns `value != 0` |
| `BackoffWithYield`, `BackoffWithoutYield` | Spin-wait backoff helpers, with and without yielding the processor |
| `HighResolutionClock` | Clock alias used for event timing in `ApplicationThread` |
| `MillisecondClock` | Millisecond-precision clock used for inactivity checks and connect timeout |
| `StringUtils` | `get_error_string(int)`, `get_errno_string()`, `leafname()`, `starts_with()` |
| `SimpleSpan<T>` | Minimal non-owning span (pre-C++20 compatibility) |
| `FileLock` | File-based lock |
| `MemoryMappedFile` | `mmap`-backed file wrapper |
| `UseHugePagesFlag` | Enum: `DoUseHugePages` / `DoNotUseHugePages` |
| `CoverageDummy` | Compilation unit to satisfy coverage tooling |
| `WallClock` | The clock a component reads the time of day from; injected, so that a test can set the time |
| `FileSystemUtils` | Helpers for files and directories |

**Test infrastructure:**

| Class | Description |
|---|---|
| `LoggerWithSink` | Logger wired to `TestSink`; in the `pubsub_itc_fw::tests_common` namespace |
| `TestSink` | In-memory log sink for test assertions |
| `MisbehavingThreads` | Test helpers that simulate stuck/crashed threads |
| `LatencyRecorder` | Nanosecond-bucket histogram recorder; thread-safe; dump to file |
| `UnitTestLogger` | Logger configured for unit tests |

---

## Earlier designs, plans and measurements

The project's working notebook, which holds earlier designs, plans, measurements and session notes kept
for reference, is [framework_notebook.md](../history/framework_notebook.md). It is not maintained as a
description of the code; this document and the chapters it links to are.
