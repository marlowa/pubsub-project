# Order Gateway {#order_gateway}

## Role

The order gateway is the FIX 5.0 SP2 session layer. It accepts raw FIX byte streams from
FIX clients (`RawBytesProtocolHandler`, port 9879), parses inbound messages, authenticates
clients via SCRAM-SHA-256, encodes order PDUs for the sequencer, and decodes execution
report PDUs from the sequencer back into outbound FIX messages.

The gateway holds no business state beyond what is needed for the current FIX session. The
`(SenderCompID, TargetCompID)` → `ClOrdID` routing map lives in the sequencer's WAL, not
in the gateway. This makes the gateway near-stateless and safe to replace or restart without
losing ER routing capability.

## FIX Session Management

The gateway uses `RawBytesProtocolHandler` for the FIX listener because FIX 5.0 SP2 is an
ASCII text protocol. `OrderGatewayThread` implements `on_raw_socket_message()` to run a
hand-written FIX parser (`parse_fields`, `try_extract_message`). Outbound FIX messages are
serialised by `FixErEncoder`, which writes directly to a caller-supplied fixed-size buffer
via a `FixWireWriter` cursor — no heap allocation on the ER path.

**Startup order:** the gateway must start before the sequencer. The sequencer connects
outbound to the gateway's ER inbound listener (port 7010); if the sequencer starts first it
retries at 2-second intervals. The long-term fix is the WAL+HA topology where the gateway
and ME keep connections open to both sequencer instances at all times.

**Dual sequencer publishing:** `forward_pdu_to_sequencers()` sends the encoded PDU to both
the primary and secondary sequencer connections when `ha_enabled = true`. With
`ha_enabled = false` (the default for single-instance dev runs) only the primary connection
is used.

## Planned Migration to `fix_codec` (not yet done)

The gateway currently carries its own hand-written FIX layer: `FixMessage.hpp` (which also
holds the `Tag::` and `MsgType::` tables), `FixParser`, `FixSerialiser`, and `FixErEncoder`.
The [FIX Codec](../design/fix_codec.md) library exists to replace these with a generated
dictionary plus a zero-copy reader/writer. This is a deliberate later pass; the codec is
tested and ready, the gateway is not yet rewired.

**Migration outline:**

1. Replace `Tag::`/`MsgType::` in `FixMessage.hpp` with the generated `fix_codec::tag` /
   `fix_codec::msg_type` constants — deleting the hand-maintained tables, not translating them.
2. Replace `FixParser` with `fix_codec::FixMessageReader` over the same
   `on_raw_socket_message()` byte window (the reader was deliberately given the shape
   `FixParser::feed` already receives, so this is close to a drop-in).
3. Replace `FixSerialiser` / `FixErEncoder` with `fix_codec::FixMessageWriter` writing into
   the existing fixed-size ER buffer. This removes the per-message `std::string` allocation
   the current `validate_checksum` path incurs.
4. Keep `FixSession` and `FixCapture` as they are — neither is a codec concern.

### Impact on large, complex messages (NewOrderSingle)

This is the concern worth stating plainly, because it is easy to over- or under-estimate what
the codec buys.

**What `fix_codec` changes.** Its generated dictionary covers the *entire* FIX 5.0 SP2 field
set, so the gateway is no longer limited by which tags someone remembered to hand-add to
`FixMessage.hpp`. Reading a further NewOrderSingle field becomes a one-liner
(`reader.find(fix_codec::tag::MinQty)`) rather than a table edit, and the read is zero-copy.
For a large message like a NOS — many optional, conditionally-required fields — this makes
*fuller* coverage cheap where it was previously fiddly.

**What it does not change.** The codec is only the mechanism. Whether a given NOS field flows
end to end is still a three-layer decision, and the codec touches only the first:

1. **Gateway** — parse, validate, and map the field into the order PDU (the `fix_codec` work).
2. **DSL topic** — the `NewOrderSingle` message in `fix_equity_orders.dsl` must carry the
   field. It already carries most of them: `price`, `stop_px`, `time_in_force`, `account`,
   `ex_destination`, `exec_inst`, `min_qty`, `max_floor`, `expire_time`, `text`. So for those,
   only steps 1 and 3 remain.
3. **A producer** — something must actually set the field for a test to exercise it.

**Impact on the blotter entry screen.** The [FIX Test Client](fix_test_client.md) is
Java/QuickFIX and does **not** use `fix_codec` (a C++ library), so the migration does not
touch the entry screen directly. The effect is indirect but real: by making richer NOS
handling cheap on the gateway side, the migration shifts the limiting factor onto the entry
form. Today the NOS send form exposes only **ClOrdID, Symbol, Side, OrdType, Qty, Price** —
a small subset of what the topic already carries. Once the gateway can accept the fuller NOS,
that simple form becomes the thing that can no longer drive it. The recommended response,
tracked as a follow-up to the migration rather than part of it:

- Keep the six-field form as the default fast path for the common order.
- Add an **Advanced fields** collapsible section exposing the optional tags
  (`TimeInForce`, `Account`, `ExDestination`, `StopPx`, `ExpireTime`, `ExecInst`, `MinQty`,
  `MaxFloor`, `Text`) for interactive testing.
- Lean on the form's existing **raw-FIX** escape hatch and Groovy scripting for exhaustive or
  malformed-input coverage, so the UI does not have to grow a control for every tag.

This form change is sketched in
[FIX Test Client → Proposed: Advanced NOS Fields](fix_test_client.md#proposed-advanced-nos-fields-not-yet-built).

The sequencing that falls out of this: migrate the gateway to `fix_codec` first (mechanism and
full-dictionary access), then widen NOS field coverage as a coordinated change across the
gateway map, the DSL topic (where a field is not already present), and the entry form.

## Authentication Flow

SCRAM-SHA-256 authentication is triggered on each FIX Logon:

1. Gateway receives FIX Logon; extracts `SenderCompID`.
2. Sends `AuthenticationRequest` (PDU 500) to the authentication service (port 7070
   primary, 7071 secondary).
3. Receives `AuthenticationChallenge` (PDU 501); forwards server nonce, salt, and
   iterations to the FIX client.
4. FIX client returns proof; gateway sends `AuthenticationProof` (PDU 502).
5. Receives `AuthenticationResult` (PDU 503); verifies `ServerSignature`.
6. On success: completes FIX Logon, registers the session.
7. On failure: sends FIX Logout and disconnects.

`request_id` in the SCRAM PDUs carries the gateway's `ConnectionID` for the FIX session,
allowing the gateway to correlate the result with the correct pending session even if
multiple logons are in progress concurrently.

## FIX Capture

`FixCapture` records raw FIX bytes to a binary file for post-hoc analysis.

**Three capture points:**
- **Inbound** — after `parser.feed()`, captures consumed bytes of complete FIX messages.
- **Outbound session messages** — after `FixSerialiser` serialises a session-level message.
- **Outbound ERs** — after `FixErEncoder` encodes an execution report.

When `capture_` is `nullptr` (capture disabled, the default), all three call sites are
guarded by `if (capture_ != nullptr)` — zero overhead when not in use.

**Hot-path design — SPSC ring buffer, no heap allocation:**

`capture(Direction, data, size, timestamp_ns)` packs the record directly into a
pre-allocated byte buffer using `memcpy`. There is no mutex, no heap allocation,
and no file I/O on the gateway thread. A background writer thread drains the buffer and
writes to disk.

The backing store is a `std::vector<uint8_t>` (`ring_` in `FixCapture.hpp`) sized to
`ring_bytes` at construction and **never resized or reallocated**. The ring-buffer behaviour
— wrap-around, concurrent access — is implemented on top of it via two cache-line-aligned
atomics: `write_offset_` (written by the producer only) and `read_offset_` (written by the
consumer only). Actual positions are `offset % capacity_`. This is classic SPSC lock-free
synchronisation — no CAS, no contention.

When a record would not fit contiguously before the end of the ring, a sentinel record
(`payload_size = 0xFFFFFFFF`) is written at the current position and both pointers wrap
to the start. This ensures records are always stored linearly and the writer can pass a
direct pointer into the ring to `fwrite()` without copying.

If the ring fills (writer thread falling behind), `capture()` drops the record and logs
a Warning. The gateway thread is never blocked.

**Record format (little-endian):**
```
uint32_t payload_size  -- byte count of raw FIX data
int64_t  timestamp_ns  -- nanoseconds since Unix epoch (wall clock)
uint8_t  direction     -- 0 = inbound, 1 = outbound
uint8_t  data[...]     -- raw FIX wire bytes
```
Each record is padded to 4-byte alignment. The file format is identical to what was
written when the ring-buffer implementation replaced the original mutex design.

**Config:** `[fix_capture] enabled`, `file`, and `ring_bytes` in `order_gateway.toml`.
Disabled by default in `dev.toml`.

`read_fix_capture.py` is available for inspecting capture files in production.

## Configuration

Key `order_gateway.toml` sections:

| Key | Purpose |
|-----|---------|
| `[network] fix_listener_port` | Inbound FIX port (default 9879) |
| `[network] sequencer_primary_*` | Host/port for sequencer primary order channel (port 7001) |
| `[network] sequencer_secondary_*` | Host/port for sequencer secondary (port 7002; used when `ha_enabled=true`) |
| `[network] er_listener_port` | Inbound ER port from sequencer (default 7010) |
| `[network] auth_service_a_*` | Authentication service endpoint A (port 7070) |
| `[network] auth_service_b_*` | Authentication service endpoint B (port 7071) |
| `[fix_capture] enabled / file` | FIX capture on/off and output file path |
| `[fix_capture] ring_bytes` | Ring buffer capacity in bytes (default 64 MB); increase if the writer thread falls behind under heavy load |
| `ha_enabled` | When false, secondary sequencer connect is skipped |

## See Also

- [FIX Codec](../design/fix_codec.md) — the library this gateway's FIX layer will migrate onto
- [FIX Test Client](fix_test_client.md) — the NOS entry form / blotter driven against this gateway
- [Secure Communications](../design/secure_comms.md) — SCRAM-SHA-256 protocol detail
- [Socket Communications](../design/socket_comms.md) — `RawBytesProtocolHandler`, `PduFramer`/`PduParser`
- [WAL and High Availability](../design/wal_and_ha.md) — gateway pool design and sequencer reconnection
