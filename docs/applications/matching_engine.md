# Matching Engine

## Role

The matching engine receives sequenced order PDUs from the sequencer leader, matches orders
against the book, and emits execution report PDUs back to the sequencer. The sequencer
routes those ERs to the correct gateway by `SenderCompID`.

**This is a framework-validation stub, not a production matching engine.** The goal is to
exercise the framework under load — correct HA behaviour, WAL replication, slab backpressure,
ITC latency — not to implement real matching semantics.

## Order Book

The ME maintains a primitive in-memory order book. Stub behaviour:

- Every `NewOrderSingle` (PDU 1000) is immediately fully filled at its limit price (or a
  zero sentinel for market orders).
- Every `OrderCancelRequest` (PDU 1001) is unconditionally confirmed with a `Canceled` ER
  (`ExecType::Canceled`, `OrdStatus::Canceled`, `LeavesQty=0`, `CumQty=0`).
- No partial fills, no price-time priority, no GTD or IOC logic.

`OrderID` and `ExecID` are generated as monotonically increasing `ME-ORD-N` /
`ME-EXEC-N` strings.

## Execution Report Generation

`MatchingEngineThread::on_framework_pdu_message()` dispatches by `pdu_id`:

| pdu_id | Message | Handler |
|--------|---------|---------|
| 1000 | `NewOrderSingle` | `handle_new_order_single()` — fabricates a fully-filled ER |
| 1001 | `OrderCancelRequest` | `handle_order_cancel_request()` — fabricates a cancel-confirmed ER |

Each handler encodes an `ExecutionReport` (PDU 1002) and sends it to both sequencer ER
listener connections — port 7021 (primary) and port 7022 (secondary, used when
`ha_enabled = true`). The inbound `seq_no` from the PDU header is carried forward as the
transport sequence number on the ER.

The `sequenced_at` field (an `optional datetime_ns` on NOS and OCR) is stamped by the
sequencer when it sequences the PDU. The ME reads this value and uses it as `TransactTime`
on the ER, ensuring that replayed ERs carry the same timestamp as the originals. When the
field is absent, the ME falls back to the current wall clock.

## HA and Failover

The ME participates in leader-follower HA via the cancel-on-failover policy:

- ME-primary is the active matcher; ME-secondary tails the primary's book updates via a
  dedicated replication channel.
- On ME-primary failure, ME-secondary is promoted via the arbiter, reconciles its book
  against the sequencer's WAL, then issues cancel ERs for all genuinely-outstanding orders.
- Halt-on-failure is preserved as a fallback for failure modes that cannot be cleanly
  reconciled (WAL corruption, arbiter unreachable).

See [WAL and High Availability](../design/wal_and_ha.md) for the full cancel-on-failover
correctness rule and the 7-step promotion sequence.

**Current status:** ME HA wiring is planned but not yet implemented. The ME currently runs
as a single instance. The cancel-on-failover design is agreed; implementation is deferred
until sequencer HA slices 8+ land.

## Configuration

Key `matching_engine.toml` sections:

| Key | Purpose |
|-----|---------|
| `[network] sequencer_order_listener_port` | Port on which ME accepts sequenced order PDUs (default 7020) |
| `[network] sequencer_er_host / er_port` | Sequencer ER listener endpoint (default port 7021) |
| `ha_enabled` | When true, ME also sends ERs to secondary sequencer ER listener (port 7022) |

## See Also

- [WAL and High Availability](../design/wal_and_ha.md) — cancel-on-failover policy, correctness rule, ME failover options
- [Sequencer Application](sequencer_app.md) — the sequencer that feeds the ME and routes ERs
