# Order Gateway {#fix_order_gateway}

## Role

The FIX order gateway is the FIX 5.0 SP2 session layer. It accepts raw FIX byte streams from
FIX clients (`RawBytesProtocolHandler`, port 9879), parses inbound messages, authenticates
clients via SCRAM-SHA-256, encodes order PDUs for the sequencer, and decodes execution
report PDUs from the sequencer back into outbound FIX messages.

The gateway holds no business state beyond what is needed for the current FIX session. The
`(SenderCompID, TargetCompID)` → `ClOrdID` routing map lives in the sequencer's WAL, not
in the gateway. This makes the gateway near-stateless and safe to replace or restart without
losing ER routing capability.

## FIX Session Management

The gateway uses `RawBytesProtocolHandler` for the FIX listener because FIX 5.0 SP2 is an
ASCII text protocol. `FixOrderGatewayThread` implements `on_raw_socket_message()` to run a
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

## Planned Migration to `fix_codec` (not yet done) {#gw_fix_codec_migration}

The gateway currently carries its own hand-written FIX layer: `FixMessage.hpp` (which also
holds the `Tag::` and `MsgType::` tables), `FixParser`, `FixSerialiser`, and `FixErEncoder`.
The [FIX Codec](../fix/codec.md) library replaces the **inbound** half of this with a
generated dictionary, a zero-copy reader, and a dictionary-driven validator
(`FixMessageReader` + `FixMessageValidator`, added 2026-07-21).

**Scope of this pass — inbound parse + validate only.** The outbound `FixSerialiser` /
`FixErEncoder` stay as they are; this pass replaces `FixParser` and adds validation.
Validation **enforces immediately** — a non-conforming inbound message is rejected, not
logged-and-accepted — which the capture-driven reconciliation below confirms is safe for the
current test client. `FixSession` and `FixCapture` are untouched (neither is a codec concern).

**Staged plan (each stage builds and tests independently):**

- **Stage 0 — wiring.** Add `fix_codec` to the gateway's includes and a
  `fix_dictionary_generated` build dependency in `applications/fix_order_gateway/CMakeLists.txt`.
  No code change.
- **Stage 1 — tag tables.** Replace the hand-written `Tag::`/`MsgType::` in `FixMessage.hpp`
  with the generated `fix_codec::tag` / `fix_codec::msg_type`, deleting the tables rather than
  translating them. Mechanical; no behaviour change.
- **Stage 3 — inbound framing.** Replace `FixParser` with a stream driver around
  `fix_codec::FixMessageReader` (see *Framing* below), producing the same `ParsedFixMessage`
  so the `handle_*` dispatch (`FixOrderGatewayThread.cpp` msg-type switch) is untouched.
  Behaviour-preserving; framing/checksum/resync parity must be proven against `FixParser`'s
  current test cases.
- **Stage 4 — validation.** Run `fix_codec::FixMessageValidator` at the dispatch point, before
  any `handle_*`. On a `FixReject`, respond per the reject map below and skip dispatch. This is
  the behaviour change.

Stage 2 (outbound writer swap) and Stage 5 (deleting `FixSerialiser`/`FixErEncoder`) are
deliberately out of scope for this pass.

### Framing: from `FixParser` to a stream driver

`FixParser::feed` extracts *every* complete message from the MirroredBuffer window and
resynchronises byte-by-byte past garbage; `FixMessageReader` frames *one* message at the
window start and reports `Malformed` / `Incomplete`. The migration keeps the loop and resync
in a thin driver:

- Loop: construct a `FixMessageReader` at the cursor; on `Valid`/`ChecksumError` advance by
  `message_size()`; on `Incomplete` stop (leave the partial bytes for the next `recv`); on
  `Malformed` skip one byte and retry — reproducing `FixParser`'s current resync.
- Return the total consumed byte count for the `commit_raw_bytes` contract exactly as today.
- `FixMessageReader::error()` supplies a specific per-cause reason for the `Malformed` log
  line, replacing today's hand-written warnings.

### Validation layer and reject mapping

Protocol validation (fix_codec) sits in front of the gateway's existing business validation:

| Layer | Checks | Response |
|---|---|---|
| `FixMessageValidator` (new, pre-dispatch) | InvalidTagNumber (0), RequiredTagMissing (1), TagNotDefinedForThisMessage (2), ValueIsIncorrect (5), IncorrectDataFormat (6), TagAppearsMoreThanOnce (13) | FIX **Reject (35=3)** built from the `FixReject`: `373`=reason, `371`=`ref_tag`, `372`=`ref_msg_type`, `45`=RefSeqNum, `58`=`describe()` text. Session stays up; the message is not dispatched. |
| existing `handle_*` (unchanged) | symbol/qty length, ClOrdID limit, sequencer availability, risk | existing `ExecutionReport(Rejected)` / `BusinessReject` |

The gateway *gains* a small `35=3` builder and stops silently dropping malformed inbound
messages. Session-level messages (Logon) keep their current disconnect-on-failure semantics
rather than emitting `35=3`.

### Capture-driven reconciliation (why enforce-immediately is safe)

Before committing to enforce-immediately, the current test client's real traffic was validated
against the generated dictionary. A live NOS round-trip (client `APM001` → gateway → matching
engine → ExecutionReport back) was captured and every field checked:

```
8=FIXT.1.1 9=131 35=D 34=5 49=APM001 52=20260721-...218 56=GATEWAY
11=LIVECAP-1 21=1 38=100 40=2 44=100 54=1 55=AAPL 60=20260721-...217 10=124
```

Result: **no rejects** under all six rules for Logon (A), NewOrderSingle (D), and
OrderCancelRequest (F). In particular the DD-required NOS tags the gateway does not read today
— notably `TransactTime` (60) — *are* sent by the client (QuickFIX plus `FixHelper` /
`MessagesHandler`), and QuickFIX renders `OrderQty`/`Price` as plain integers (`38=100`,
`44=100`) which pass the decimal-format check. Enforcing immediately therefore does not reject
the existing client. This reconciliation must be re-run if the client's field set or the
dictionary changes.

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
2. **DSL topic** — the `NewOrderSingle` message in `fix_orders.dsl` must carry the
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
[FIX Test Client → Proposed: Advanced NOS Fields](fix_test_client.md#ftc_advanced_nos).

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

**Config:** `[fix_capture] enabled`, `file`, and `ring_bytes` in `fix_order_gateway.toml`.
Disabled by default in `dev.toml`.

`read_fix_capture.py` is available for inspecting capture files in production.

## Configuration

Key `fix_order_gateway.toml` sections:

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

- [FIX Codec](../fix/codec.md) — the library this gateway's FIX layer will migrate onto
- [FIX Test Client](fix_test_client.md) — the NOS entry form / blotter driven against this gateway
- [Secure Communications](../operations/secure_comms.md) — SCRAM-SHA-256 protocol detail
- [Socket Communications](../framework/socket_comms.md) — `RawBytesProtocolHandler`, `PduFramer`/`PduParser`
- [WAL and High Availability](../availability/wal_and_ha.md) — gateway pool design and sequencer reconnection
