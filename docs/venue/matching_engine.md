# Matching Engine {#matching_engine}

## Role

The matching engine receives sequenced orders and cancels from the leading sequencer, keeps the
book of open orders, and sends an execution report for each command back to the sequencers, which
deliver it to the member that placed the order.

**It does not match.** An order the engine accepts rests on the book until its member cancels it,
or until a failover or a halt cancels it (below). No order is ever filled, partly filled or
expired, and there is no price-time priority. What the engine does implement is everything around
matching that the venue depends on: the book survives the engine's death, a second instance keeps a
copy of it, leadership passes between the two instances safely, and every member is told what
became of each of its orders.

## Orders and cancels

The sequencer sends each command wrapped in a `WalRecord` envelope (PDU id 103), whose `pdu_id`
field says what is inside: a `NewOrderSingle` (1000) or an `OrderCancelRequest` (1001). The envelope
also carries the identity of the session that placed the command (its comp id and gateway protocol),
the time the sequencer sequenced it, and the time the gateway read it from the member.
`MatchingEngineThread::on_framework_pdu_message()` unwraps the envelope and calls
`handle_new_order_single()` or `handle_order_cancel_request()`.

An order is identified on the book by its session and its `ClOrdID` together (`OrderKey`), so two
members, or the same comp id through the two gateway protocols, can use the same `ClOrdID` without
colliding.

### A new order

In order, the engine:

1. **Refuses an order whose sequence number is lower than one it has already handled**, which would
   mean the sequencer had sent the venue's commands out of order.
2. **Refuses an order whose `ClOrdID` is longer than 64 bytes** (`fix_order_limits::max_cl_ord_id_length`).
   Both gateways refuse these already; the engine checks again because the book's key is that size.
3. **Refuses every order while trading is halted**, with OrdRejReason Exchange closed and the text
   "trading is halted". This reply is how a member trading at the time learns of the halt.
4. **Refuses an order whose `ClOrdID` is already open on the book for the same session**, with
   OrdRejReason Duplicate order.
5. **Refuses an order when the book is full**, with OrdRejReason Other and the text "the venue is
   holding as many open orders as it can". The book has a fixed number of records
   (`[order_book] region_capacity`: two million in the development environment, one million in the
   others), and it is never grown on the engine's thread.
6. **Otherwise accepts it**: the order is written to the book, a copy is sent to the standby engine
   (`BookUpdate`, below), and the member is sent an execution report with ExecType and OrdStatus New,
   LeavesQty equal to OrderQty, and CumQty 0. The report echoes the order's price, order type, time
   in force, expiry time, and its party and underlying repeating groups.

Every refusal is an execution report with ExecType and OrdStatus Rejected and OrderID "NONE".

### A cancel

The engine looks for the order named by `OrigClOrdID` under the same session. If it is open, the
engine removes it from the book, sends the removal to the standby engine, and reports it with
ExecType and OrdStatus Canceled. If it is not open, the engine sends a rejected execution report with
OrdRejReason Unknown order. The gateways turn that report into an `OrderCancelReject` for the member;
see [fix_order_gateway.md](fix_order_gateway.md) and [binary_order_gateway.md](binary_order_gateway.md).

### Identifiers and times

`OrderID` and `ExecID` are `ME-ORD-N` and `ME-EXEC-N`, from counters that only increase. After a
restart the order counter starts above the highest order number found in the book, so an order
number is never issued twice.

`TransactTime` on every report is the time the sequencer sequenced the command, carried in the
envelope, so a report produced again during a catch-up carries the same time as the original. When
the envelope has no such time the engine uses its own clock.

### Where the reports go

Each report is wrapped in a `WalRecord` envelope that carries the sequence number of the command it
answers and the identity of the session that placed the order. It is sent to both sequencers' report
listeners: the primary's on port 11021 and the secondary's on port 11022. The leading sequencer
delivers it to wherever that session is connected when the report arrives; see
[Sequencer Application](sequencer_app.md) and [Gateway High Availability](../availability/gateway_ha.md).

A report produced while the engine has no connection to either sequencer is held, and sent when a
connection is established, because the order it reports is in the sequencer's log and on the book,
and the member must be told.

## The book survives the process

The book is a memory-mapped file of fixed-size records (`[order_book] region_path`), and there is one
copy of each order, in that file (`OrderBook`). The engine writes an order's record, then sends the
report, then records in the file that it is current up to that command's sequence number. A death
before that last step leaves the change above the recorded position: a restart ignores it, and the
sequencer sends the command again. See
[Open order checkpoint](../durability/open_order_checkpoint.md).

On starting, an engine that finds the file reads back every order at or below the recorded position.
It then asks the leading sequencer for everything after that position, and applies it (the
catch-up, below). An engine that finds no file holds nothing and never did: it has no position, and
it is placed at the sequencer's current position without a catch-up. An order the sequencer had taken
and no engine had yet applied is not recovered by an engine that starts that way, and both sides log
it.

**A long absence cancels everything.** The engine writes the time into the file every so often while
it is able to match. If a restarted engine finds that no engine was able to match for longer than
`[order_book] absence_limit_seconds` (300 seconds in every environment), and the book holds orders,
it cancels every order, reports each cancel to its member, and halts trading. Those orders were
priced for a market that moved while their members could not cancel them. An empty book is not
cancelled and does not halt.

## High availability

With high availability on, two instances run: `matching_engine` (the primary, instance 1) and
`matching_engine_secondary` (instance 2).

- **Leadership is decided by leases.** An instance leads only while a majority of itself, its peer
  and the arbiter pool has granted it a lease; see
  [Deciding leadership by majority, with leases](../availability/majority_leases.md). An instance that
  holds no lease discards the orders the sequencer sends it, and marks itself as needing a catch-up
  before it may act. Nothing is lost by discarding them: they are in the sequencer's log.
- **The standby keeps a copy of the book.** The leader sends each order it accepts and each order it
  removes to the standby over the book replication connection (`BookUpdate`, ports 11025 and 11026 in
  the development environment). The standby discards the orders the sequencer sends it.
- **Every engine catches up before it acts**, whether it has just been promoted or has just started.
  It sends the sequencer the position it has reached (`MePositionRequest`), and the sequencer sends
  every command after it, then says where it stopped and how many records it sent (`MePositionAck`).
  The engine checks that it received them all before it acts. Each command applied during a catch-up
  is reported to the member that placed it, marked `PossResend`, because the engine cannot tell a
  command an earlier engine already reported from one no engine ever saw. This is what recovers an
  order the sequencer took while no engine was running
  ([BUG-0064](../bug_list.md#bug_0064)).
- **What a promotion does with the orders it inherits** is set by
  `[order_book] open_orders_on_promotion`. Under `"cancel"`, the promoted engine cancels every order on
  its book, reports each cancel to its member, and continues on an empty book. Under `"keep"` it
  carries the book across, so an order open before the promotion is still open after it and can be
  cancelled by its member. **`"cancel"` is set in every environment**, because nothing yet checks that
  the copy of the book the standby kept from `BookUpdate` is the book the leader held. See R-0073 and
  R-0101 in the functional specification.
- **When the book file could not be used**, the engine builds its book from the sequencer's log
  alone. If the log still reaches back to the first command the venue ever took, the engine cancels
  every order, reports each cancel, and halts trading, whatever `open_orders_on_promotion` says. If
  the log has been trimmed so it no longer reaches back that far, the engine cannot name every order
  it held, so it halts without cancelling: cancelling only the orders it can name would leave the
  others unmentioned.

A halt stays until a person lifts it; see [Trading phases](trading_phases.md). The whole promotion
sequence, and why cancelling is safe, are in
[WAL and High Availability](../availability/wal_and_ha.md).

`ha_test.py` scenario 16 kills the primary engine and checks that the secondary takes over.

## Configuration

`matching_engine_primary.toml` and `matching_engine_secondary.toml`. Ports in the development
environment, primary first:

| Section and key | Purpose |
|---|---|
| `[network] listen_port` | Where the sequencers connect to send commands: 11020 and 11023 |
| `[sequencer_er] host`, `port` | The primary sequencer's report listener, 11021 |
| `[sequencer_er_secondary] host`, `port` | The secondary sequencer's report listener, 11022 |
| `[ha] enabled`, `role` | The venue-wide high availability switch, and which instance this is |
| `[book_replication] listen_port`, `port` | The book replication connection between the two instances: each listens on one and connects to the other's (11025 and 11026) |
| `[ha_instance] instance_id`, `epoch_state_file` | This instance's number, and the file holding the highest leadership epoch it has seen |
| `[arbiter_primary]`, `[arbiter_secondary]` | The arbiters, 11200 and 11201 |
| `[lease]` | The lease period, the allowance for clock drift, and how often a lease is renewed, shared by every component that takes part in leases |
| `[ha_timing] catch_up_retry_seconds` | How long to wait before asking for a catch-up again when no answer came, 15 seconds |
| `[order_book] region_path`, `region_capacity` | The book's file, and the most orders it can hold |
| `[order_book] initial_capacity`, `growth_report_threshold_bytes` | The size reserved for the map from order identity to record, and when its growth is reported |
| `[order_book] absence_limit_seconds` | How long the venue may be unable to match before a restart cancels everything and halts |
| `[order_book] open_orders_on_promotion` | `"cancel"` or `"keep"`, as above |
| `[event_queue_pool]`, `[command_queue_pool]` | The pools the reactor's queues take their entries from |
| `[reactor]`, `[logging]`, `[metrics]` | CPU pinning and waiting, log levels, and the Prometheus endpoint |

The preprod, prod and test-1 environment files define a single engine, named `matching_engine`,
whose configuration file is `etc/matching_engine/matching_engine.toml`. The build installs no
template of that name: it installs only `matching_engine_primary.toml` and
`matching_engine_secondary.toml`.

## See Also

- [WAL and High Availability](../availability/wal_and_ha.md) — the promotion sequence and the cancel-on-failover rule
- [Open order checkpoint](../durability/open_order_checkpoint.md) — the book's file, and recovery from it
- [Sequencer Application](sequencer_app.md) — the sequencer that feeds the engine and delivers its reports
- [Trading phases](trading_phases.md) — halting and lifting a halt
