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
6. **Otherwise accepts it**: the order is written to the book, and the member is sent an execution report with ExecType and OrdStatus New,
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

`OrderID` and `ExecID` are made from positions in the sequencer's WAL. An order is named by the
sequence number of the new order that placed it, and a report by the sequence number of the command
that caused it and its place among that command's reports. The two numbers are scrambled with FF1,
with keys derived from a secret both engines hold and the trading date the sequencer writes on each
command, and written as a letter and 16 hexadecimal digits: `O` for an `OrderID`, `E` for an
`ExecID`, for example `E3F9A0C21D4B7E605`. So every engine that applies the same commands names
every report the same, whether it applied them live or while catching up, however often it has
restarted; identifiers are unique for the day; and a member cannot read the venue's sequence numbers
from them. `identifier_to_position` turns one back for support staff. See
[deterministic_matching_engine.md](../availability/deterministic_matching_engine.md), section 3.

`TransactTime` on every report is the time the sequencer sequenced the command, carried in the
envelope, so a report produced again during a catch-up carries the same time as the original. A
command whose envelope carries no time, or no trading date, is a defect in the sequencer: the engine
refuses it, logs the refusal at Error, and writes on the refusal the time the command carried, zero
where it carried none.

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

**A long absence cancels everything.** When no engine could trade for longer than the venue allows,
the leading sequencer writes a `CancelBook` command into the WAL, and every engine that applies it
cancels every order, reports each cancel to its member, and halts trading if it cancelled any (R-0117).
Those orders were priced for a market that moved while their members could not cancel them. An empty
book is not cancelled and does not halt. The engine does not measure the absence itself, so an engine
that was down while the other traded cancels nothing. See [Sequencer](sequencer.md#sequencer_absence).

**A file that cannot be used is rebuilt from the WAL.** A file that is damaged, or was written by a
different build, is moved aside and kept, and a new one started (R-0102). The engine asks the leading
sequencer for every record from the first, and where the WAL reaches the first command of the trading
day, the book it rebuilds is exactly the venue's: it keeps it, cancels nothing, and says at Error that
it rebuilt its record. Where the WAL does not reach that far, an engine about to lead asks the leading
sequencer to halt trading, by a `TechnicalEvent` in the WAL, and cancels nothing (R-0123).

**Whether trading is halted is kept in the file** with the orders, beside the recorded position, so
an engine that restarts after a halt is still halted. It is set and cleared by commands: a `CancelBook`
that asks to halt and cancels an order, and a `TechnicalEvent`, which says whether trading is halted.
While halted, the engine refuses every new order, in the reply to that order, and still carries out
every cancel. The engine that leads tells the follower of each change to the halt as it tells it of
each change to the book.

## High availability

With high availability on, two instances run: `matching_engine` (the primary, instance 1) and
`matching_engine_secondary` (instance 2).

- **Both instances start alike**, whichever is configured as primary: in the unknown role, neither
  leading nor following. Each catches up from the sequencer's log when the first sequencer connects,
  and says it leads or follows only once a lease settles which.
- **Leadership is decided by leases.** An instance leads only while a majority of itself, its peer
  and the arbiter pool has granted it a lease; see
  [Deciding leadership by majority, with leases](../availability/majority_leases.md). A sequencer sends
  orders only to the instance that leads (`EngineOrderRouting`).
- **The instance that does not lead follows the sequencer's log.** It asks the leading sequencer to
  catch it up and to go on sending it every committed record, and applies every command as the leader
  does, so its book is the leader's after the same record. It checks that the records' numbers run on
  with none missing, and asks again if not. It sends no reports: it keeps each until the log holds the
  leader's report of the same command, and when it takes the lead it sends those the log lacks, each
  marked `PossResend`. See
  [Running the matching engine pair in lockstep](../availability/matching_engine_lockstep.md).
- **The two instances are linked** (`[peer]`, ports 11025 and 11026 in the development environment):
  the link carries the lease requests between them, their role announcements, and the check that
  their settings agree.
- **Every engine catches up before it acts**, whether it has just been promoted or has just started.
  It sends the sequencers the position it has reached (`WalFollowRequest`), and the leading sequencer
  sends every command after it, then says where it stopped and how many records it sent
  (`WalFollowCaughtUp`). A sequencer that does not lead declines (`WalFollowDeclined`) and says when it
  takes the lead (`WalLeaderChanged`), and the engine then asks again.
  The engine checks that it received them all before it acts. Each command applied during a catch-up
  is reported to the member that placed it, marked `PossResend`, because the engine cannot tell a
  command an earlier engine already reported from one no engine ever saw. This is what recovers an
  order the sequencer took while no engine was running
  ([BUG-0064](../bug_list.md#bug_0064)).
- **A promotion keeps every order.** The promoted engine followed the log, so an order open before the
  promotion is still open after it and can be cancelled by its member (R-0073).
- **When the book file could not be used**, the engine rebuilds its book from the sequencer's log,
  from the first command of the trading day. The engine is deterministic, so that book is exactly the
  venue's: it keeps it, cancels nothing, and says at Error that it rebuilt its record (R-0123). If the
  log no longer reaches back to the first command, the engine cannot account for its book. An engine
  about to lead, or the only engine, then asks the leading sequencer for a halt, and the sequencer
  writes a `TechnicalEvent` that halts trading; nothing is cancelled. An engine that follows asks for
  no halt, because the engine that leads goes on trading.

A halt stays until a person lifts it; see [Trading phases](trading_phases.md).

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
| `[peer] listen_port`, `port` | The link between the two instances: each listens on one and connects to the other's (11025 and 11026) |
| `[ha_instance] instance_id`, `epoch_state_file` | This instance's number, and the file holding the highest leadership epoch it has seen |
| `[arbiter_primary]`, `[arbiter_secondary]` | The arbiters, 11200 and 11201 |
| `[lease]` | The lease period, the allowance for clock drift, and how often a lease is renewed, shared by every component that takes part in leases |
| `[ha_timing] catch_up_retry_seconds` | How long to wait before asking for a catch-up again when no answer came, 15 seconds |
| `[order_book] region_path`, `region_capacity` | The book's file, and the most orders it can hold |
| `[order_book] initial_capacity`, `growth_report_threshold_bytes` | The size reserved for the map from order identity to record, and when its growth is reported |
| `[event_queue_pool]`, `[command_queue_pool]` | The pools the reactor's queues take their entries from |
| `[reactor]`, `[logging]`, `[metrics]` | CPU pinning and waiting, log levels, and the Prometheus endpoint |

The preprod, prod and test-1 environment files define a single engine, named `matching_engine`,
whose configuration file is `etc/matching_engine/matching_engine.toml`. The build installs no
template of that name: it installs only `matching_engine_primary.toml` and
`matching_engine_secondary.toml`.

## See Also

- [Running the matching engine pair in lockstep](../availability/matching_engine_lockstep.md) — how the follower follows the log, and what a promotion does
- [WAL and High Availability](../availability/wal_and_ha.md) — the sequencer pair and the log
- [Open order checkpoint](../durability/open_order_checkpoint.md) — the book's file, and recovery from it
- [Sequencer Application](sequencer_app.md) — the sequencer that feeds the engine and delivers its reports
- [Trading phases](trading_phases.md) — halting and lifting a halt
