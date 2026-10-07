# Socket Communications

## Design Goals

All socket I/O is owned by the reactor thread. No application thread ever touches a file
descriptor. The two goals on every I/O path are zero-copy (no intermediate buffers between
socket and application memory) and correct backpressure (a stalled peer must never block
the reactor from servicing other connections).

---

## TCP Socket Primitives

| Class | Description |
|-------|-------------|
| `TcpSocket` | Non-blocking TCP socket; `TCP_NODELAY` set on every socket; `get_file_descriptor()` for epoll registration |
| `TcpAcceptor` | Non-blocking listening socket; wraps `accept4(SOCK_NONBLOCK)` |
| `TcpConnector` | Stateless non-blocking connector; `connect()` starts the attempt; `finish_connect()` completes it on `EPOLLOUT`; `get_connected_socket()` transfers ownership |
| `ByteStreamInterface` | Abstract base: `send()`, `receive()`, `close()`, `get_peer_address()` |
| `InetAddress` | Concrete IP address; factory from host+port string via `getaddrinfo` |

`TCP_NODELAY` is set on all sockets. The framework sends small-to-medium fixed-size PDUs
and relies on the reactor's non-blocking loop to coalesce writes naturally; Nagle's
algorithm would only add latency.

---

## Protocol Handler Strategy

When an inbound connection is accepted, `InboundConnectionManager` creates an
`InboundConnection` with a `ProtocolHandlerInterface` chosen by the listener's
`ProtocolType`:

| `ProtocolType` | Value | Handler created | Use case |
|----------------|-------|-----------------|----------|
| `FrameworkPdu` | 0 | `PduProtocolHandler` | All inter-component PDU connections |
| `RawBytes` | 1 | `RawBytesProtocolHandler` | Alien-protocol byte streams (e.g. ASCII FIX) |
| `TlsRawBytes` | 2 | `TlsRawBytesProtocolHandler` | TLS-wrapped byte streams (see [Secure Comms](../operations/secure_comms.md)) |

`ProtocolType` is set per listener at `register_inbound_listener()` call time. An outbound
connection carries framework PDUs with its own `PduFramer` and `PduParser`, or, when the service's
endpoint is configured for TLS, uses a `TlsRawBytesProtocolHandler` as the client side.

All handler methods return `[[nodiscard]] tuple<bool, std::string>`. On `!ok` the owning
manager calls `teardown_connection()` directly; the handler never destroys the connection
itself.

---

## PDU Framing

### Wire Format

Every framework PDU begins with a fixed 24-byte header (`PduHeader`):

| Field | Type | Notes |
|-------|------|-------|
| `byte_count` | u32 | Length of the payload, not counting this header; network byte order |
| `pdu_id` | i16 | The message's id in its DSL file; network byte order |
| `version` | i8 | Message version |
| `filler_a` | u8 | Reserved, zero |
| `seq_no` | i64 | Sequence number stamped by the sequencer; 0 when not stamped; network byte order |
| `canary` | u32 | Fixed value `0xC0FFEE00`; validated on receive; network byte order |
| `filler_b` | u32 | Reserved, zero |

All multi-byte fields are in network byte order. The canary is validated immediately on
receive; a mismatch causes the connection to be torn down.

### PduFramer

`PduFramer` has two send modes:

| Mode | Method | When to use |
|------|--------|-------------|
| Built internally | `send()` | Small fixed PDUs; at most 256 bytes of payload (`PduFramer::max_payload_size`); header + payload assembled by the framer |
| Zero-copy from slab | `send_prebuilt()` | Large PDUs; caller writes `PduHeader` + encoded payload into a slab chunk and passes the pointer |

Both modes share `continue_send()` and `has_pending_data()` for partial-send handling.

### PduParser

`PduParser` receives PDUs in two phases:

1. **Header phase:** reads 24 bytes into `header_buffer_`; validates the canary. A payload of zero
   bytes, or one larger than an inbound slab (`ReactorConfiguration::inbound_slab_size`), is refused
   and the connection torn down.
2. **Payload phase:** allocates a slab chunk (`auto [slab_id, chunk] = inbound_slab_allocator_.allocate(byte_count)`), then reads the payload **directly from the socket into the slab chunk** — zero copy.

On completion, dispatches `EventMessage::create_framework_pdu_message(payload, size, slab_id)` to the target thread's ITC queue.

### Inline PDU Handler

For latency-critical paths where the ITC queue hop is undesirable, `PduParser` supports an
optional `InlinePduHandler`:

```cpp
using InlinePduHandler = std::function<bool(int16_t pdu_id, int64_t seq_no,
                                             const uint8_t* payload, size_t size)>;
void set_inline_handler(InlinePduHandler handler);
```

In `dispatch_pdu()`, if the inline handler is installed and returns `true`, the slab chunk
is freed immediately and no `EventMessage` is enqueued — the PDU is handled entirely on the
reactor thread. If it returns `false`, the normal ITC path is used as fallback (e.g. when
the framer has pending data and backpressure applies).

The handler is installed via `InstallInlinePduHandler` reactor control command; the
application thread calls `install_inline_pdu_handler(connection_id, installer_fn)`.

The installer function receives both the `PduParser*` and the `PduFramer*`, giving access
to the outbound channel on the same connection (e.g. to send acks in-line).

**Use in sequencer:** a follower sequencer installs an inline handler on its connection to the
leader, which writes each incoming `WalRecord` to the local log and sends the `WalAck` straight back,
without a turn of the sequencer thread. See [Sequencer Design](../venue/sequencer.md).

### PDU Ownership

**Inbound:** reactor allocates slab → `PduParser` reads into it → `EventMessage` carries
`ptr + slab_id` → application thread **must** call
`inbound_slab_allocator().deallocate(msg.slab_id(), msg.payload())` after processing.

**Outbound:** `ApplicationThread::send_pdu()`, on the application thread, takes a chunk from the
outbound slab allocator, writes the `PduHeader` and the encoded payload into it, and enqueues a
`SendPdu` reactor control command; the reactor sends it via `send_prebuilt()` and frees the chunk
when the send is complete.

---

## Raw Socket Communication

For alien protocols (e.g. ASCII FIX), the framework delivers raw byte streams rather than
decoded PDUs. The application thread is responsible for its own message framing.

### MirroredBuffer

A stream-oriented ring buffer backed by virtual-memory mirroring.

| Detail | |
|--------|-|
| Backing | `memfd_create` + two `mmap` calls into adjacent virtual address ranges |
| Purpose | Provides a contiguous view of unprocessed bytes even when data wraps the ring boundary, eliminating split-packet edge cases |
| Head | Advanced by the reactor thread only, on each `recv()` |
| Tail | Advanced by the reactor thread only, in response to `CommitRawBytes` |
| API exposed to app | `read_ptr()`, `bytes_available()`, `tail()` |
| Backpressure | Reading pauses at three quarters full and resumes at half; see *Backpressure* below |

### RawBytesProtocolHandler

Inbound path:
1. `EPOLLIN` fires → `on_data_ready()` → `recv()` into buffer, advancing head.
2. Enqueues `RawSocketCommunication` event carrying:
   - `connection_id` — for demultiplexing multiple raw connections
   - `payload()` — `read_ptr()` into the buffer at enqueue time
   - `payload_size()` — ALL currently unprocessed bytes (not just newly arrived ones)
   - `tail_position()` — buffer tail value at enqueue time

Outbound path: the bytes to send are in a slab chunk, which the handler writes with its own
`send_prebuilt()`, finishing a partial write in `continue_send()` and freeing the chunk when done.

### Reactor Control Commands for Raw Bytes

| Command | Direction | Meaning |
|---------|-----------|---------|
| `CommitRawBytes` | App thread → Reactor | Finished processing `bytes_consumed` bytes; advance the buffer tail |
| `SendRaw` | App thread → Reactor | Send pre-built raw bytes on `connection_id` |

`CommitRawBytes` is processed by `InboundConnectionManager::process_commit_raw_bytes()` →
`RawBytesProtocolHandler::commit_bytes(n)` → `buffer_.advance_tail(n)`.

### Application Thread Responsibilities

The application thread implements `on_raw_socket_message()`. Each call receives ALL
currently unprocessed bytes from the tail, not just the newly arrived bytes. Tail advance
only happens when the reactor processes a `CommitRawBytes` command.

Recommended application pattern (as used by `BurstListenerThread` in
`integration_tests/RawBytesProtocolHandlerIntegrationTest.cpp`):
- Track `bytes_decoded_` (decoded since last tail advance) and `last_tail_` (tail position
  from last delivery).
- On each call: if `message.tail_position() != last_tail_`, the tail has advanced — reset
  `bytes_decoded_` to 0.
- Decode from `data + bytes_decoded_` for `available - bytes_decoded_` bytes.
- Call `commit_raw_bytes()` only when `bytes_decoded_ == available` (entire window consumed)
  so no partial message bytes are stranded after the commit.

### Why `tail_position()` Is Needed

Without it, the application would use `available < last_available_` to detect a tail
advance. This fails when new data arrives simultaneously: the tail shrinks the window but
new bytes enlarge it, so `available` may increase rather than decrease. `tail_position()`
makes tail-advance detection exact and unambiguous.

---

## Backpressure

### Read Backpressure

**Raw-bytes connections** pause reading when the application falls behind. When the unprocessed
bytes in a connection's `MirroredBuffer` reach three quarters of its capacity, the reactor stops
watching the socket for incoming data, so the kernel's receive buffer fills and TCP slows the peer.
When the application's `CommitRawBytes` commands bring the unprocessed bytes down to half, the
reactor reads again. Only that connection is affected. A connection whose buffer is nevertheless
completely full when data arrives is torn down; with the pause in place that should not happen. The
FIX gateway gives each session 16 MiB, so a burst that fits in 12 MiB never slows the member.

**PDU connections** do not pause on their own. The inbound slab allocator adds a new slab when the
current one is full, so `PduParser` always has somewhere to put the next payload. An application
thread that needs to slow a peer asks the reactor to stop reading that connection, with
`pause_reading()` and `resume_reading()`; see [Reactor](reactor.md).

### Write Backpressure (EPOLLOUT / Partial Sends)

Non-blocking `send()` may write fewer bytes than requested when the kernel TCP send buffer
is full. `PduFramer` handles this:

1. Initial `send_prebuilt()` or `send()` writes as much as the kernel accepts.
2. If partial: stores `current_*` fields (pointer, remaining bytes, slab id) and registers
   `EPOLLOUT` on the fd.
3. `EPOLLOUT` fires → `on_write_ready()` → `continue_send()` resumes writing.
4. On completion: `release_pending_send()` deallocates the slab chunk.

A send that arrives while its connection is still writing an earlier one waits in that
connection's own queue (`WaitingSendQueue`), and is started, in order, when the send before it
completes in `on_write_ready()`. Other connections are not held up. A connection whose queue
reaches `connection_waiting_sends_maximum` sends or `connection_waiting_bytes_maximum` bytes is
closed, with a Warning, and its application thread is told the connection was lost: a peer that
has stopped reading would otherwise hold the sending thread's slab memory without limit. See
`docs/framework/reactor.md` for the defaults.

### Idle Connection Timeout

`InboundConnectionManager::check_for_inactive_connections()` (called on each housekeeping
tick) uses the two-phase identify-then-process pattern. Any inbound connection that has had
no read activity for longer than `socket_maximum_inactivity_interval_` (default 60 s from
`ReactorConfiguration`) is torn down.

Individual connections or entire listeners can be exempted by passing
`IdleTimeoutFlag::BypassIdleTimeout` to `InboundConnection` and `register_inbound_listener()`. This is used for connections
such as the peer-to-peer HA link that have their own heartbeat-based liveness detection.

---

## Failure Handling

All handler methods return `[[nodiscard]] tuple<bool, std::string>`. Any `!ok` result
causes the owning manager to call `teardown_connection()` immediately, telling the application
thread the connection was lost.
The handler never destroys the connection itself, so the connection is still valid when the
handler returns, and the manager does not have to look it up again.

On graceful peer close, `PduParser::receive()` returns `(false, "")` (empty reason string)
to distinguish a clean close from a protocol error.

---

## See Also

- [Reactor](reactor.md) — how the event loop drives `on_data_ready`, `on_write_ready`, and connection lifecycle
- [Secure Comms](../operations/secure_comms.md) — `TlsRawBytesProtocolHandler` and OpenSSL memory BIOs
- [Allocators](allocators.md) — the slab allocators that back the zero-copy inbound PDU path
