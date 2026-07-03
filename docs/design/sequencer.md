# Sequencer Design

## Role

The sequencer is the **authority** for message ordering. It is the sole writer to the
matching engine's input stream, assigning a monotonically increasing sequence number (`seqNo`)
to every order and appending it to its Write-Ahead Log before forwarding it to the ME.

The two concerns that the sequencer separates:

- **Leadership decides who may append.** The leader sequencer is the only instance that
  assigns seqNos and writes to the WAL.
- **The WAL decides what already happened.** Once a record is committed to the WAL, the
  order exists permanently, regardless of what happens to the process afterwards.

These two concerns must never leak into each other. See
[WAL and High Availability](wal_and_ha.md) for the full commit and replication semantics.

---

## Message Flow

For each order PDU received from a gateway:

```
Gateway sends NewOrderSingle/OrderCancelRequest PDU
    │  TCP connection (port 7001 to primary, 7002 to secondary)
    ▼
SequencerThread::on_framework_pdu_message()
    1. Role guard: if not leader, release PDU payload and return
    2. Decode NOS/OCR from PDU payload (BumpAllocator arena)
    3. Stamp sequenced_at = wall_clock.now_ns()  ← same value used everywhere
    4. wal_.append(wall_time_ns, pdu_id, payload) ← local WAL commit (store-release)
    5. Set sequenced_at field on the decoded struct
    6. Re-encode NOS/OCR with seq_no in PduHeader
    7. send_pdu(me_outbound_order_conn_id_, ...)  ← gates on local commit
    8. send_wal_record(wall_time_ns, pdu_id, payload) ← streams to follower
    9. Buffer ER in pending_er_[seq_no] when ME replies
    10. Gate ER forwarding to gateway on WalAck receipt from follower
```

**`wall_time_ns` is computed once per order PDU** and used identically for three purposes:
the local WAL `append()`, the `sequenced_at` field in the forwarded PDU, and the
`WalRecord.wall_time_ns` sent to the follower. All three values are guaranteed identical.

---

## Routing Map

The sequencer maintains a `(SenderCompID, TargetCompID)` → current gateway `ConnectionID`
map. When the ME returns an ER, the sequencer looks up which gateway connection to forward
it to.

**Why this map lives in the sequencer, not the gateway:**

- The sequencer sees every order PDU and is therefore the natural place to record which
  gateway originated each order.
- After a gateway restart or failure, the new gateway registers its FIX sessions with the
  sequencer on reconnect, and the map updates. The gateway itself is near-stateless.
- After sequencer failover, the new leader has the routing map by WAL replay.

**`ConnectionID` instability across reconnects:** a FIX client logging out and back in gets
a new `ConnectionID`. Routing is therefore on the FIX-level comp-id pair
`(SenderCompID, TargetCompID)`, not on `ConnectionID`. The gateway maintains a small table
mapping the current `ConnectionID` for each active FIX session; the sequencer addresses ERs
by comp-id.

**WAL routing map and replay:** on restart, WAL replay intentionally does **not** rebuild
the `seq_no → gateway_session_conn_id` map. After a restart the ME has already processed
and returned ERs for any in-flight orders from the previous run; those ERs will not be
re-sent. Rebuilding a stale routing map would cause unbounded heap growth under high
throughput and would never be used. The "ER seq_no not found in routing map" fallback
handles any edge cases gracefully.

---

## WAL Record Format

On-disk layout per record (as stored in `SequencerWal`):

```
[ magic | length | seqNo | wall_time_ns | pdu_id | payload | checksum ]
```

| Field | Type | Notes |
|-------|------|-------|
| `magic` | 4 bytes | Validates record boundary |
| `length` | 4 bytes | Byte count of `wall_time_ns + pdu_id + payload` |
| `seqNo` | 8 bytes | Monotonically increasing sequence number |
| `wall_time_ns` | 8 bytes | `CLOCK_REALTIME` ns when the sequencer processed this order |
| `pdu_id` | 2 bytes | Message type (1000=NOS, 1001=OCR) |
| `payload` | variable | DSL-encoded PDU payload |
| `checksum` | 4 bytes | CRC32 of the entire record |

`wall_time_ns` was added to support replay determinism: when the sequencer replays a WAL
record, it stamps `sequenced_at = record.wall_time_ns` on the forwarded PDU so the ME
produces ERs with the same `TransactTime` as the original live run.

The WAL holds two kinds of records:
1. **Order/ER event records** — the committed orders (the data).
2. **Delivery cursor records** — per-gateway progress (written periodically, not per-record).
   These allow a reconnecting gateway to exchange cursor positions with the sequencer and
   replay any gap.

---

## `sequenced_at` Field

`optional datetime_ns sequenced_at` appears on `NewOrderSingle` and `OrderCancelRequest`.
The sequencer sets it; gateways never set it (the field is absent on the inbound PDU from
the FIX client).

**Purpose:** gives the ME a replay-deterministic clock source. In live operation,
`has_sequenced_at` is always `true`; the ME uses `sequenced_at` as `TransactTime` on the
ER rather than calling the wall clock. In replay mode, the sequencer stamps the original
`wall_time_ns` from the WAL record, so the replayed ERs carry the same `TransactTime` as
the originals without any `ReplayClock` injection.

---

## `pending_er_` Buffer

The leader buffers each ER received from the ME in `pending_er_` (a `seq_no → slab-allocated
PDU payload` map) until the corresponding `WalAck` arrives from the follower. This gates ER
emission on two-machine durability.

**Burst risk:** under a heavy burst the ME returns ERs faster than WalAcks arrive, so
`pending_er_` can grow to hundreds of entries simultaneously. If the slab backing those
payloads exhausts, the sequencer stalls. This failure mode did not exist before WAL
replication. A `seq_pending_er_count` Prometheus gauge (planned) would expose this in
production before it becomes critical.

On follower disconnect, the leader flushes all buffered ERs immediately and continues as
single-machine-durable.

---

## Inline WAL Handler

### The Three-Wakeup Problem

Without the inline handler, the WAL round-trip (primary → secondary → primary) involves
three sequential `epoll_wait` wakeups:

1. Secondary `SequencerThread` wakes to receive `WalRecord` via ITC.
2. Secondary reactor wakes to execute the resulting `SendPdu(WalAck)` command.
3. Primary `SequencerThread` wakes to receive `WalAck` via ITC.

At ~200 µs p50 per wakeup on a development machine, these compound to a ~600 µs floor
that cannot be improved by OS tuning alone.

### Solution: Option C (Inline Handler)

The `WalRecord` PDU is handled entirely on the secondary reactor thread with no ITC hop.
When a complete `WalRecord` frame arrives at the secondary's `PduParser`, the inline handler:

1. Decodes the `WalRecord` (stack-allocated 4 KiB arena, no heap allocation).
2. Calls `wal_.append()` directly.
3. Encodes an 8-byte `WalAck` with `encode_fast()` and calls `framer->send()` on the same
   connection.
4. Returns `true`, consuming the PDU (no `EventMessage` is enqueued).

This eliminates wakeups 1 and 2. Only wakeup 3 remains. If backpressure is active
(`framer->has_pending_data()`), the handler returns `false` and the PDU falls through to
the normal ITC path as a fallback.

### Threading Invariant

`SequencerWal::append()` is single-writer at any given time:
- **Leader:** all writes happen on `SequencerThread`. The role guard in
  `on_framework_pdu_message` prevents a follower from writing.
- **Follower:** after the inline handler is installed, all writes happen on the **reactor
  thread**. `SequencerThread::handle_wal_record()` is bypassed (the inline handler returns
  `true` first). No concurrent access.

The handler is re-installed on each peer reconnect, so a stale `framer*` is never held.

### Latency Results (12 Manual Orders)

First measurement with Option C active (dev machine, no CPU pinning):

| Metric | Before Option C | After Option C |
|--------|----------------|----------------|
| Minimum | 389 µs | **160 µs** |
| Median | ~590 µs | ~769 µs |
| Maximum | 1769 µs | 1420 µs |

The sub-389 µs results (160, 297, 329 µs) represent cases where the inline WAL path
completed before the ME returned the ER. The primary's `wal_acked_seq_nos_` already held
the ack when the ER arrived, so the ER was forwarded with no WAL wait at all. The measured
latency was purely the ME round-trip plus gateway wakeup.

The tail (650–1420 µs) is driven by OS scheduler jitter on the one remaining wakeup.
`SCHED_FIFO` + `isolcpus` on production hardware would reduce this from ~200 µs p50 to
~5 µs, making sub-100 µs median achievable.

---

## Replay Mode

The `--replay` flag allows the sequencer to replay its WAL to the matching engine, producing
the same ERs with the same timestamps as the original live run.

**Behaviour:**
1. Opens the WAL with `full_replay = true` — skips the snapshot so `WalReader::replay()`
   visits every record from segment 0 (not from the snapshot anchor).
2. Buffers all records into `replay_buffer_` (`seq_no`, `pdu_id`, `wall_time_ns`, payload).
3. Connects to the ME only — no gateway, arbiter, or peer connections.
4. Waits for both ME connections to be ready (`replay_me_order_ready_` AND
   `replay_me_er_ready_`) before dispatching. This prevents the ME from dropping orders
   because its outbound ER connection is not yet established.
5. Dispatches each buffered record with `sequenced_at = record.wall_time_ns`, producing
   identical ERs to the original run.

The ME and gateway binaries are unchanged — replay is entirely a sequencer concern.

**`full_replay = true` vs normal `open()`:** normal WAL open uses the snapshot's anchor
position to skip already-applied records (correct for crash recovery). Full replay ignores
the snapshot so every record is revisited. Passing `full_replay = false` is the default
and is unchanged.

---

## Downstream Consumers (WAL-Follower Pattern)

The sequencer's WAL replication channel generalises from "one follower (the secondary
sequencer)" to "N external subscribers, each with their own cursor". External consumers
(e.g. a Kafka publisher, the Matching Engine Publisher) connect to the sequencer, identify
a position cursor via `WalSubscribeRequest` / `WalSubscribeAck`, and receive WAL records
from that cursor onward.

WAL truncation is gated on the minimum cursor across all connected followers (secondary
sequencer + external subscribers). A persistently slow subscriber is disconnected when its
lag exceeds a configured threshold.

This is a planned extension (slice 10); the current system has only one follower (the
secondary sequencer).

---

## See Also

- [WAL and High Availability](wal_and_ha.md) — WAL format, two-tier commit, replication protocol, failover
- [Sequencer Application](../applications/sequencer_app.md) — operational detail: port allocation, TOML config, startup order, implemented slices
- [Socket Comms](socket_comms.md) — inline PDU handler mechanism (`InlinePduHandler`, `InstallInlinePduHandler`)
