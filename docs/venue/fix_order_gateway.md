# Order Gateway {#fix_order_gateway}

## Role

The FIX order gateway is the venue's FIX 5.0 SP2 session layer. It accepts FIX connections from
members, in plain TCP on port 9879 and with TLS on port 9880 for instance `a` (9881 and 9882 for
instance `b`), authenticates each member with SCRAM-SHA-256, checks every order and cancel, and
sends those it accepts to the sequencers. It turns the execution reports the leading sequencer
sends back into FIX ExecutionReports and OrderCancelRejects for the member's session.

What the gateway holds is the state of the sessions connected to it: each session's message
numbering, its open orders (for cancel-on-disconnect), and its throttles. It holds nothing the
venue needs after the gateway dies. Where each session can be reached, and which session placed
each order, are held by the sequencer, so a member that reconnects to the other instance of the
gateway finds its orders and reports there; see [Gateway High Availability](../availability/gateway_ha.md).

## FIX session management

The FIX listener uses `RawBytesProtocolHandler` (`TlsRawBytesProtocolHandler` on the TLS port),
because FIX is a text protocol whose message boundaries the framework does not know. The reactor
hands the gateway's thread the raw bytes, and `FixParser` frames and checks them (below). Outbound
messages are written by `FixErEncoder` and `FixSerialiser` straight into a fixed-size buffer, with
no heap allocation.

Each session has 16 MiB of receive buffer (`raw_buffer_capacity`). When it is three quarters full
the reactor stops reading the socket until the gateway has taken enough out, so a member sending
faster than the gateway can process is slowed by TCP rather than disconnected.

**Startup order does not matter.** Each sequencer connects to the gateway's execution report
listener (`er_listen_port`, 11010 for instance `a`) and retries every two seconds until it answers,
without limit. Starting the sequencers first costs at most one retry interval before reports can
flow, and loses nothing, because there are no orders yet to report on.

**Every order goes to both sequencers.** With `ha_enabled = true`, the gateway sends each order and
cancel to the primary and the secondary sequencer. Only the leading sequencer acts on it; the
follower discards its copy. With `ha_enabled = false` only the primary is used.

## How the gateway uses `fix_codec` {#gw_fix_codec_migration}

<!-- verify: present applications/fix_order_gateway/FixParser.cpp "fix_codec::FixMessageReader" -->
<!-- verify: present applications/fix_order_gateway/FixParser.cpp "FixMessageValidator" -->
<!-- verify: present applications/fix_order_gateway/FixMessage.hpp "namespace Tag = fix_codec::tag;" -->
<!-- verify: present applications/fix_order_gateway/FixErEncoder.cpp "fix_codec::FixMessageWriter" -->
<!-- verify: exists applications/fix_order_gateway/FixSerialiser.hpp -->
<!-- verify: exists applications/fix_order_gateway/FixErEncoder.hpp -->
The gateway reads and writes FIX with the [FIX codec](../fix/codec.md) library. The tags and
message types come from the dictionary generated from the FIX data dictionary, so the gateway has
no tables of its own:

```cpp
namespace MsgType = fix_codec::msg_type;   // FixMessage.hpp
namespace Tag     = fix_codec::tag;
```

- **Inbound:** `FixParser` frames each message with `fix_codec::FixMessageReader` and checks it with
  `FixMessageValidator` before anything acts on it.
- **Outbound:** `FixErEncoder` (execution reports and cancel rejects) and `FixSerialiser` (session
  messages) write through `fix_codec::FixMessageWriter`, which computes the body length and checksum
  in place.

### Framing

`FixParser::feed` takes every complete message from the receive buffer. It constructs a
`FixMessageReader` at the current position; on a valid message, or one whose checksum is wrong, it
moves past the message; on an incomplete one it stops and leaves the bytes for the next read; on a
malformed one it skips one byte and tries again, so the stream resynchronises past garbage. It
returns the number of bytes consumed, which the gateway commits back to the reactor.
`FixMessageReader::error()` gives the specific reason a message was malformed, for the log.

A well-framed message with no MsgType (tag 35) is discarded and logged at Info. It cannot be
rejected, because a Reject names the message type it refers to and there is none to name, and a
received message must never be dropped without a trace.

### Checks, and how a failure is answered

Two layers of checks, in this order:

| Layer | Checks | Answer |
|---|---|---|
| `FixMessageValidator`, before dispatch | InvalidTagNumber (0), RequiredTagMissing (1), TagNotDefinedForThisMessage (2), ValueIsIncorrect (5), IncorrectDataFormat (6), TagAppearsMoreThanOnce (13) | A FIX **Reject (35=3)**: `373` the reason, `371` the tag, `372` the message type, `45` the message's sequence number, `58` a description. The session stays up and the message is not acted on. A failed Logon disconnects instead |
| The order and cancel handlers | The lengths of `ClOrdID`, `Symbol` and `OrderQty`; whether the venue is accepting orders; the session's throttles | An order is answered with an `ExecutionReport` with OrdStatus Rejected; a cancel with an `OrderCancelReject` that says the order is still open. The text names what was wrong |

A message missing a field the dictionary requires is answered with a Reject, not dropped. A cancel
does not need `OrderQty`. The refusals while the venue is not accepting orders are described in
[Order acceptance](../availability/order_acceptance.md), and the throttles in
[Gateway throttles](gateway_throttles.md).

The binary order gateway makes the same checks of the venue's rules, so an order is refused the
same way whichever gateway it arrives at.

### Which order fields flow end to end

The generated dictionary covers every FIX 5.0 SP2 field, so reading another field of a
NewOrderSingle is one call (`reader.find(fix_codec::tag::MinQty)`) and costs no copy. Whether a
field reaches the matching engine is still decided in three places:

1. **The gateway** reads the field and puts it in the order message.
2. **The order message** in `fix_orders.dsl` carries it. It already carries `price`, `stop_px`,
   `time_in_force`, `account`, `ex_destination`, `exec_inst`, `min_qty`, `max_floor`,
   `expire_time` and `text`, among others.
3. **Something sends it,** so that a test can exercise it. The
   [FIX Test Client](fix_test_client.md#ftc_advanced_nos) has an Advanced section on its order form
   for the optional fields, and raw FIX and Groovy scripting for anything else.

## Authentication

SCRAM-SHA-256 authentication runs on each FIX Logon:

1. The gateway receives the Logon and takes its `SenderCompID`.
2. It sends `AuthenticationRequest` (PDU 500) to the authentication service, instance `a` on port
   11070 or, failing that, instance `b` on 11071.
3. It receives `AuthenticationChallenge` (PDU 501) and passes the server nonce, salt and iteration
   count to the member.
4. The member returns its proof, and the gateway sends `AuthenticationProof` (PDU 502).
5. It receives `AuthenticationResult` (PDU 503) and verifies the `ServerSignature`, which proves the
   service is genuine.
6. It checks that the member is provisioned for this gateway instance, and applies the member's
   cancel-on-disconnect settings and throttles, all of which arrive in the `AuthenticationResult`.
7. On success it completes the Logon and tells the sequencers where the session is
   (`SessionBound`). On failure it sends a Logout, saying why, and disconnects.

The `request_id` in the four messages is the gateway's connection id for the session, so the gateway
can match each answer to the right logon when several are in progress at once.

## FIX capture

`FixCapture` records raw FIX bytes to a file for later analysis, at three points: inbound messages
after framing, outbound session messages, and outbound execution reports. With capture disabled,
which is the default, each point is one comparison of a null pointer.

**No heap allocation and no blocking on the gateway's thread.** `capture()` copies each record into
a buffer allocated once at start (`ring_bytes`, 64 MB by default) and never resized. A background
thread drains the buffer to disk. The two sides coordinate through two atomic positions on separate
cache lines, one written only by the gateway's thread and one only by the writer, so neither waits
for the other. A record that would not fit before the end of the buffer is preceded by a marker
(`payload_size = 0xFFFFFFFF`) and written at the start, so every record is contiguous and the writer
can pass a pointer straight to `fwrite()`. If the buffer fills because the writer has fallen behind,
the record is dropped and a Warning is logged; the gateway's thread is never held up.

**Record format (little-endian):**
```
uint32_t payload_size  -- byte count of raw FIX data
int64_t  timestamp_ns  -- nanoseconds since the Unix epoch (wall clock)
uint8_t  direction     -- 0 = inbound, 1 = outbound
uint8_t  data[...]     -- raw FIX wire bytes
```
Each record is padded to a multiple of four bytes. `scripts/read_fix_capture.py` prints a capture
file.

## Configuration

The main sections of `fix_order_gateway_a.toml` and `_b.toml`; the values come from the environment
file.

| Section and key | Purpose |
|---|---|
| `[network] listen_port`, `tls_listen_port` | The member listeners, plain and TLS (9879 and 9880 for instance `a`) |
| `[network] er_listen_port` | Where the sequencers connect to send execution reports (11010 for instance `a`) |
| `[network] raw_buffer_capacity` | Receive buffer per session, 16 MiB |
| `[sequencer] primary_host/port`, `secondary_host/port`, `ha_enabled` | The two sequencers' order listeners (11001 and 11002); with `ha_enabled = false` only the primary is used |
| `[authentication_service] host/port`, `secondary_host/port` | The two authentication service instances (11070 and 11071) |
| `[gateway] instance_id` | Which instance of the FIX gateway this is, stamped on every order |
| `[fix_tls] enabled`, `cert`, `key` | The TLS listener and its certificate |
| `[cancel_on_disconnect] enabled`, `grace_period` | The defaults, which each comp id may override; see [Gateway High Availability](../availability/gateway_ha.md) |
| `[fix_limits] max_symbol_length`, `max_order_qty_length` | The field lengths the gateway accepts |
| `[timeouts] logon_timeout`, `scram_auth_timeout` | How long a logon, and its authentication, may take |
| `[fix_capture] enabled`, `file`, `ring_bytes` | FIX capture, off by default |
| `[open_order_pool]` | The pool from which open-order entries are taken, so tracking an order allocates nothing |
| `[metrics]` | This process's Prometheus endpoint |

## See Also

- [FIX Codec](../fix/codec.md) — the library the gateway reads and writes FIX with
- [FIX Test Client](fix_test_client.md) — the order entry form and blotter driven against this gateway
- [Gateway throttles](gateway_throttles.md) — how many orders, amends and cancels a session may send each second
- [Secure Communications](../operations/secure_comms.md) — SCRAM-SHA-256 protocol detail
- [Socket Communications](../framework/socket_comms.md) — `RawBytesProtocolHandler`, `PduFramer`/`PduParser`
- [Gateway High Availability](../availability/gateway_ha.md) — two instances, provisioning, reconnecting to a backup, resend, and cancel-on-disconnect
- [WAL and High Availability](../availability/wal_and_ha.md) — how the sequencers and matching engines are led and replicated
