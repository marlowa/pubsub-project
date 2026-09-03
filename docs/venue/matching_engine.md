# Matching Engine {#matching_engine}

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

The ME participates in leader-follower HA, and what a promotion does with the orders it
inherits is a stated policy rather than a fixed behaviour — `order_book.open_orders_on_promotion`,
either `"cancel"` or `"keep"`:

- ME-primary is the active matcher; ME-secondary tails the primary's book updates via a
  dedicated replication channel.
- **Every engine catches up before it acts**, whether it was promoted or has just started. It
  presents the position it has reached — a promoted follower knows it from the replica it was
  maintaining, a starting instance from the region it recovered — and the sequencer sends
  everything after it. Each record applied is reported to the member that placed it, marked
  `PossResend`, because the engine cannot tell a record an earlier engine already reported from
  one no engine ever saw. This is what recovers an order the sequencer deferred while no engine
  was running; see `docs/bug_list.md` BUG-0064.
- An instance that finds **no region at all** has no position rather than a position of zero, and
  is placed at the sequencer's head without a catch-up. It holds nothing and never did, so
  replaying the venue's retained record into it would build a book out of orders it never had.
  The cost is real and is logged by both sides: an order taken and not yet applied is not
  recovered by an instance that starts that way.
- On ME-primary failure, ME-secondary is promoted via the arbiter and reconciles its book
  against the sequencer's WAL. Under `"cancel"` it then issues a cancel ER for every order it
  inherited and resumes on an empty book; under `"keep"` it carries the book across, so an
  order open before the promotion is open after it and still cancellable by its owner.
- **`"cancel"` is the deployed default in every environment.** Keeping the book is the better
  outcome for a member and rests on that book being the one the venue had, which nothing
  establishes yet: neither the replica maintained by `BookUpdate` nor the completeness of the
  catch-up that follows it. See R-0073 and R-0101 in the functional specification, and
  `docs/bug_list.md` BUG-0074.
- Halt-on-failure is preserved as a fallback for failure modes that cannot be cleanly
  reconciled (WAL corruption, arbiter unreachable), and cancelling is unconditional there
  whatever the policy says.

See [WAL and High Availability](../availability/wal_and_ha.md) for the full cancel-on-failover
correctness rule and the 7-step promotion sequence.

**Current status:** Implemented (slices A–D) and verified. On ME-primary loss the secondary
detects the dropped book-replication channel, waits out the promotion timeout, requests
arbitration, reconciles its replicated book against the sequencer's WAL
(`MePositionRequest`/`MePositionAck`), issues cancel ERs for genuinely-outstanding orders,
and adopts leader; the leader sequencer promotes its standby connection so sequenced orders
route to the promoted ME. Verified by `ha_test.py` scenario 16 (ME failover), a live perf run
through a failover, and an orders-in-flight-during-the-gap run (gap orders are WAL-committed
and recovered — none dropped). Halt-on-failure remains the fallback for irreconcilable
failure modes (WAL corruption, arbiter unreachable).

## Configuration

Key `matching_engine.toml` sections:

| Key | Purpose |
|-----|---------|
| `[network] sequencer_order_listener_port` | Port on which ME accepts sequenced order PDUs (default 7020) |
| `[network] sequencer_er_host / er_port` | Sequencer ER listener endpoint (default port 7021) |
| `ha_enabled` | When true, ME also sends ERs to secondary sequencer ER listener (port 7022) |

## See Also

- [WAL and High Availability](../availability/wal_and_ha.md) — cancel-on-failover policy, correctness rule, ME failover options
- [Sequencer Application](sequencer_app.md) — the sequencer that feeds the ME and routes ERs
