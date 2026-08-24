# Reactor

## Design Goals

The reactor is the single-threaded epoll event loop that owns all socket I/O, timer
delivery, and control-command processing. No other thread ever touches a socket.
Application threads send requests to the reactor via a lock-free command queue
(`ReactorControlCommand`); the reactor delivers results back via the threads' ITC queues.

This separation keeps all file-descriptor state — connection maps, timer fds, listen sockets
— on one thread and eliminates the need for locks around I/O paths.

---

## Key Classes

| Class | Description |
|-------|-------------|
| `Reactor` | epoll event loop; owns all threads, timers; implements `ThreadLookupInterface`; delegates I/O to `InboundConnectionManager` and `OutboundConnectionManager` |
| `ThreadLookupInterface` | Pure abstract interface: single method `get_fast_path_thread(ThreadID)`; allows connection managers to deliver events to threads without a direct `Reactor` dependency |
| `InboundConnectionManager` | Owns all inbound connection state: listener registry, accepted-connection maps, accept/read/write/teardown/idle-timeout logic |
| `OutboundConnectionManager` | Owns all outbound connection state: connection maps, connect/read/write/teardown/timeout logic |
| `ReactorConfiguration` | All config: timeouts, slab sizes, HA topology, command queue capacity, `connect_timeout` (default 5 s), `socket_maximum_inactivity_interval_` (default 60 s) |
| `ReactorControlCommand` | Commands sent from application threads to the reactor: `AddTimer`, `CancelTimer`, `Connect`, `Disconnect`, `SendPdu`, `SendRaw`, `CommitRawBytes` |
| `ServiceRegistry` | Static service catalog; interns each service to a stable `ServiceID` at registration and maps id→(name, `ServiceEndpoints`); populated before threads start; no file I/O at runtime. `connect_to_service(name)` resolves the name to its `ServiceID` up front (fail-fast on an unknown name), so a `Connect` command carries the integer id, not a `std::string` |
| `ServiceEndpoints` | Primary + secondary `NetworkEndpointConfig`; secondary `port==0` means not configured |
| `ConnectionID` | Strongly-typed connection identifier; 0 = invalid; monotonically increasing from 1; allocated by `Reactor::allocate_connection_id()`, shared across both managers |
| `OutboundConnection` | Per-connection state for reactor-managed outbound TCP connections |
| `InboundConnection` | Per-connection state for reactor-managed inbound TCP connections |

**Design rules:**
- All socket I/O on the reactor thread only.
- `fast_path_threads_` written only during init/shutdown; read-only while running.
- `ConnectionID` space is shared between inbound and outbound managers: the reactor
  allocates the ID and passes it in, so neither manager couples to the other.

---

## Event Loop

The reactor's main loop calls `epoll_wait` and dispatches each ready event:

1. **Timer fds** — delegated to `TimerHandler::handle_event()`.
2. **Listen socket fds** — delegated to `InboundConnectionManager::on_accept_ready()`.
3. **Established connection fds** — dispatched by fd to the owning manager
   (`InboundConnectionManager` or `OutboundConnectionManager`) via the `connections_by_fd_`
   maps.
4. **Control command queue** — `process_control_commands()` drains the `ReactorControlCommand`
   queue from application threads, calling the appropriate manager method for each command.

`process_control_commands()` calls `OutboundConnectionManager::drain_pending_send()` at its
start to retry any `SendPdu` that was stashed because a partial write was in flight.

---

## Timers

Timers are backed by Linux `timerfd`. Each `Timer` object carries an interval, a type
(`OneShot` or `Recurring`), and an owner `ThreadID`.

`TimerHandler` is created when an `AddTimer` command is processed:
- Calls `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)`.
- Arms the fd via `timerfd_settime`; for recurring timers `it_interval` is also set.
- Registers the fd with epoll.

On expiry (`EPOLLIN` on the timerfd), `TimerHandler::handle_event()`:
1. Reads the 8-byte expiration count (draining the fd non-blocking).
2. If the owner `ThreadID == 0`, calls `reactor_.on_housekeeping_tick()` — this is the
   reactor's internal backstop timer, not an application-owned timer.
3. Otherwise checks the owner thread's lifecycle state. If not `Operational`, all pending
   expirations are dropped (prevents delivering timer events to a shutting-down thread).
4. Coalesces multiple expirations into one `EventMessage` of type `Timer` and delivers it to
   the owner thread's ITC queue via `reactor_.route_message()`.

`CancelTimer` removes the `TimerHandler` from epoll and destroys it.

---

## Housekeeping Tick

The reactor registers a recurring backstop timerfd (owner `ThreadID == 0`) that fires
periodically. Each tick calls `on_housekeeping_tick()`, which:

- Calls `OutboundConnectionManager::check_for_timed_out_connections()` — any outbound
  connection that has been in the connecting phase longer than `connect_timeout` is torn
  down and a `ConnectionFailed` event is delivered to the owning thread.
- Calls `InboundConnectionManager::check_for_inactive_connections()` — any inbound
  connection with no activity for longer than `socket_maximum_inactivity_interval_` is torn
  down and a `ConnectionLost` event is delivered.
- Calls `check_for_stuck_threads()` — see [Threading](threading.md) for details.
- Calls `OutboundConnectionManager::retry_failed_connections()` — reconnection attempts for
  connections configured with auto-retry.

---

## InboundConnectionManager

Manages accepted TCP connections. Owns three maps:

| Map | Type | Notes |
|-----|------|-------|
| `inbound_listeners_` | `fd → InboundListener` | Owning; one entry per listening socket |
| `connections_` | `ConnectionID → unique_ptr<InboundConnection>` | Owning |
| `connections_by_fd_` | `fd → InboundConnection*` | Non-owning; for epoll dispatch |

Each accepted connection gets an `InboundConnection`, which owns:
- `TcpSocket` — the accepted socket (`TCP_NODELAY` set)
- `unique_ptr<ProtocolHandlerInterface>` — strategy (PDU or raw bytes)
- `last_activity_time_` — updated on each read; used by idle-timeout check
- `target_thread_id_` — for `ConnectionLost` delivery on teardown

**Protocol handler strategy:**

| Class | Strategy | Description |
|-------|----------|-------------|
| `PduProtocolHandler` | A | Owns `PduParser` + `PduFramer` + pending-send slab state; handles framework-native PDU framing |
| `RawBytesProtocolHandler` | B | Owns `MirroredBuffer`; delivers raw byte streams to the application thread (see below) |
| `TlsRawBytesProtocolHandler` | C | TLS variant of B; uses OpenSSL memory BIOs; reactor thread never blocks on SSL I/O |

All handler methods return `[[nodiscard]] tuple<bool, std::string>`. On `!ok`, the manager
calls `teardown_connection()` directly — the handler never destroys the connection itself.

**Inbound PDU path (zero-copy):**
1. epoll signals `EPOLLIN` on accepted fd.
2. `InboundConnectionManager::on_data_ready()` → `InboundConnection::handle_read()` →
   `PduProtocolHandler::on_data_ready()` → `PduParser::receive()`.
3. `PduParser` reads the 16-byte `PduHeader` (validates canary `0xC0FFEE00`).
4. `PduParser` allocates a slab chunk: `auto [slab_id, chunk] = inbound_slab_allocator_.allocate(byte_count)`.
5. `PduParser` reads the payload **directly from the socket into the slab chunk** — zero copy.
6. Dispatches `EventMessage::create_framework_pdu_message(payload, size, slab_id)` to the
   target thread queue. The application thread **must** call
   `inbound_slab_allocator().deallocate(msg.slab_id(), msg.payload())` after processing.

---

## OutboundConnectionManager

Manages outbound TCP connections initiated by application threads via `Connect` commands.
Owns two maps:

| Map | Type | Notes |
|-----|------|-------|
| `connections_` | `ConnectionID → unique_ptr<OutboundConnection>` | Owning |
| `connections_by_fd_` | `fd → OutboundConnection*` | Non-owning; for epoll dispatch |

Each `OutboundConnection` has two lifecycle phases:

| Phase | Indicator | Active members |
|-------|-----------|----------------|
| Connecting | `is_connecting()` | `connector_`, `connect_started_at_`, `trying_secondary_` |
| Established | `is_established()` | `socket_`, `framer_`, `parser_` |

**Connection flow:**
1. `Connect` command → `process_connect_command()` → `TcpConnector::connect(primary)` →
   register fd for `EPOLLOUT`.
2. `EPOLLOUT` fires → `on_connect_ready()` → `finish_connect()`:
   - Success → create `PduFramer` + `PduParser` → re-register for `EPOLLIN` → deliver
     `ConnectionEstablished`.
   - Failure + secondary configured → `retry_with_secondary()` → repeat from step 1.
   - Both fail → `teardown_connection()` → deliver `ConnectionFailed`.
3. Connect timeout detected by housekeeping tick → `teardown_connection()` → deliver
   `ConnectionFailed`.
4. `EPOLLIN` fires → `on_data_ready()` → `PduParser::receive()` → zero-copy into slab →
   dispatch `FrameworkPdu` event to thread queue.
5. `SendPdu` command → `process_send_pdu_command()` → `PduFramer::send_prebuilt()`
   (zero-copy from slab).
6. Partial send → store in `current_*` fields + register `EPOLLOUT` → `on_write_ready()` →
   `continue_send()` → deallocate slab when complete.
7. `Disconnect` or peer close → `teardown_connection()` → deliver `ConnectionLost`.

**`pending_send_` pattern:** if a `SendPdu` cannot proceed (partial write in flight or
connection not yet established), the command is stashed in the manager's
`std::optional<ReactorControlCommand> pending_send_`. `drain_pending_send()` is called at
the start of `process_control_commands()` each tick to retry it.

---

## Raw Socket Communication

For alien protocols (e.g. ASCII FIX), the framework delivers raw byte streams rather than
decoded PDUs. This is the most complex inbound path because the application thread handles
its own message framing.

**`MirroredBuffer`** — a stream-oriented ring buffer using virtual-memory mirroring:

| Detail | |
|--------|-|
| Backing | `memfd_create` + double `mmap` into adjacent virtual address ranges |
| Purpose | Contiguous view of unprocessed bytes even when data wraps the ring boundary |
| Head | Advanced by the reactor thread only, on each `recv()` |
| Tail | Advanced by the reactor thread only, in response to `CommitRawBytes` |
| Exposed to app | `read_ptr()`, `bytes_available()`, `tail()` |
| Backpressure | If `space_remaining() == 0` on `EPOLLIN`, the connection is torn down |

**Inbound raw path:**
1. `RawBytesProtocolHandler::on_data_ready()` → `recv()` into buffer, advancing head.
2. Enqueues `RawSocketCommunication` event carrying `connection_id`, `payload()` (pointer
   to first unprocessed byte), `payload_size()` (ALL unprocessed bytes, not just newly
   arrived), and `tail_position()` (buffer tail at enqueue time).
3. Application thread implements `on_raw_socket_message()`, decodes what it can, then sends
   a `CommitRawBytes` reactor control command with `bytes_consumed`.
4. Reactor processes `CommitRawBytes` → `RawBytesProtocolHandler::commit_bytes(n)` →
   `buffer_.advance_tail(n)`.

**Why `tail_position()` is needed:** without it, the application cannot distinguish "more
data arrived" from "tail advanced and window shifted" when both happen simultaneously — both
can cause `payload_size()` to increase or decrease in the same direction.

---

## Outbound PDU Ownership

Application thread allocates slab from `outbound_slab_allocator()` → writes `PduHeader` +
encoded payload → enqueues `SendPdu` → reactor sends via `PduFramer::send_prebuilt()` →
reactor deallocates slab on send completion (or teardown).

---

## Shutdown Sequence

1. Reactor's `run()` exits the epoll loop (SIGTERM or `stop()` called).
2. `finalize_threads_after_shutdown()`:
   a. Cancels all timerfd handlers (removes from epoll, closes fds).
   b. Calls `thread->shutdown(reason)` on every registered `ApplicationThread` — sets
      lifecycle state to `ShuttingDown` and writes to `notify_fd_` for immediate wakeup.
   c. Waits for all threads to reach `Terminated` state (polling with timeout).
   d. Calls `join_with_timeout()` on each thread.
3. Managers tear down remaining connections.

There is an **outstanding timing issue**: after the SIGSEGV fix that added the `shutdown()`
call in step 2b, "did not stop within shutdown_timeout" and "failed to join within
shutdown_timeout" log entries still appear. Despite `is_running()` returning false
immediately, threads take the full 200 ms timeout before exiting. Root cause not yet
identified.

---

## See Also

- [Threading](threading.md) — `ApplicationThread`, ITC queues, lifecycle states, stuck-thread detection
- [CPU Pinning](cpu_pinning.md) — how each thread claims a dedicated CPU at startup
- [Allocators](allocators.md) — slab and pool allocators backing the PDU paths
- [Socket Comms](socket_comms.md) — `TcpSocket`, `TcpAcceptor`, `TcpConnector`, `PduFramer`, `PduParser`
- [Secure Comms](../operations/secure_comms.md) — TLS subsystem and `TlsRawBytesProtocolHandler`
