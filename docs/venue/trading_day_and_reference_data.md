# Trading day start and reference data {#trading_day_and_reference_data}

**Status: partly built.** The trading phases are described in [`trading_phases.md`](trading_phases.md) and
built, and so is the record that announces a phase, `TechnicalEvent`. The trading date is built as this note
describes: each sequencer is started with `--trading-day D`, and the start-of-day event at the head of the WAL
fixes the date for the day. `scripts/trading_day.py start` writes the run manifest and starts the day
([`start_and_end_of_day.md`](start_and_end_of_day.md), section 3), and `scripts/trading_phase.py` sends the
requests that move the venue from one phase to the next. The replay of an archived day, described under
"Trading date, run manifest, and replay", is not built. Reference data is left until something in the venue
uses it; this note keeps only the requirement any design for it must meet. Some of the choices listed under
"Decisions still required" have since been settled in [`start_and_end_of_day.md`](start_and_end_of_day.md) and
[`trading_phases.md`](trading_phases.md); this note has not yet been checked against them.

## Purpose

The venue needs a defined way to begin a trading day and to allow order entry only when the venue is ready.
The design must support the existing high availability model: either sequencer can lead, processes can
restart, and a restarted process must reconstruct the same trading state as its peer. A developer must be
able to select a saved production trading day and replay its events without the local calendar changing
the result.

The trading date is therefore part of the identity of a run and of the replay contract. It is not a value
that each process derives from its own clock.

## Terms and separation of concerns

- **Trading day** is the venue's business date, represented as `YYYYMMDD`.
- **Trading phase** is the current state of the venue: `sod`, `pre_open`, `open`, `closed`, or `eod`. In
  the DSL these are the `TradingPhase` values `StartOfDay`, `PreOpen`, `Open`, `Closed`, and two for `eod`:
  `EndOfDayBegin`, which ends trading, and `EndOfDayEnd`, which the leading sequencer writes by itself once
  every copy of the WAL is complete ([start_and_end_of_day.md](start_and_end_of_day.md), section 4).
- **Phase change** is a sequenced `TechnicalEvent` record that states the resulting phase. A
  `TechnicalEvent` with phase `StartOfDay` moves the venue into `sod`; the record announces the state, it
  is not the state itself.
- **Reference data** is the day-specific set of instruments and attributes needed to check orders and
  operate the venue, such as reference prices and price limits used by the matching engine. The venue has
  none today, and its fields are not defined.

The sequencer owns the authoritative trading state because it owns the WAL order. The day's
schedule, recorded in the WAL, or an operator's request causes a transition; the sequencer decides whether it is allowed and records it; gateways learn
the resulting phase through their existing control connection to the sequencer. These are separate roles.

## Trading date, run manifest, and replay

Every live or replay run has an explicit trading date `D`, supplied by the procedure that starts the run.
The source of `D` is the venue's trading calendar or a run parameter an operator controls. Processes do not
infer `D` from local time.

A run manifest identifies at least:

- the trading date `D`;
- whether the run is live or replay;
- the WAL, or archived WAL set, for `D`;
- once reference data exists, the reference data used for `D`, by an identifier that never changes and a
  digest;
- any other versioned input needed to interpret the day's records.

For a live start, the start-of-day procedure checks that the fresh daily files and configured inputs match
`D` before it starts the venue. The request to start the day carries `D`, and the sequencer accepts it only
for the date in the run manifest. This check matters on a fresh WAL: with no earlier record, the WAL alone
cannot tell that a first start-of-day request names yesterday by mistake.

**The sequencer takes the date from the start-of-day event, never from its clock.** Each sequencer is started
with `--trading-day D`. The first record of a day's WAL is the start-of-day `TechnicalEvent`, which the leading
sequencer writes only for `D` and only on a WAL that holds no record; its `trading_day` and its envelope's
`WalRecord.trading_date` both carry `D`. Every later record takes its trading date from that event
(`SequencerThread::trading_date_to_write`, and `TradingPhaseState`, which `VenueStateFromTheWal` keeps). The
matching engine derives the keys of its order identifiers from that field, so it does not change during a day
and is the same in every replay. The follower learns the date from the replicated record, and a replay reads it
from the saved one. Until the WAL holds a start of day, the sequencer refuses every member's command
(`CommandRefusedReason::TradingDayNotStarted`) and writes nothing of its own; during the start of day it refuses
them too (`MarketNotOpen`).

For replay, the developer selects `D` and the archived run for `D`. The replay checks the manifest and the
WAL records against that date, then applies the existing WAL in sequence. It does not generate a replacement
start-of-day event: the WAL supplies the order of events, and the manifest selects and checks the run's
inputs. A replay must not send orders to live gateways or start any work with an external system.

Every phase change carries `D`. A component applies phase changes in WAL order and checks that each one's
date is the date of the run it is following: an older date is stale, and a different later date cannot skip
the current day's phases. Each daily file records its trading date, and a process at startup refuses a file
whose date does not match the run manifest. This extends principle V12 in
[`design_principles.md`](../design_principles.md), every trading day starts fresh.

Replaying the WAL alone reproduces the day only if every external input that affected processing is
preserved, or identified by something that never changes. Reference data, once it exists, is such an input:
the exact data is archived, or kept under an identifier, and its digest checked on replay.

## State transitions and WAL records

The sequencer's trading state, derived from its WAL, is at least:

```text
(trading_day, phase, halted)
```

`halted` remains separate from the phase, as set out in [`trading_phases.md`](trading_phases.md). The
phases of a day follow one another in one order:

```text
sod -> pre_open -> open -> closed -> eod
```

A fresh daily WAL starts with no trading state at all. The start-of-day `TechnicalEvent` establishes the
date and moves the state to `sod`. Each trading day starts with a fresh WAL, so a new day is started by the
procedure that starts the run, never by changing the date inside yesterday's WAL.

The WAL record for a phase change is the existing `TechnicalEvent` (PDU 153), defined in
[`leader_follower.dsl`](../../libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl). It already
carries the resulting `TradingPhase`, the halted flag, the trading day as `YYYYMMDD`, and a reason. No second
record for phase changes is needed. Start of day is a `TechnicalEvent` with phase `StartOfDay`; the end of
start of day is a `TechnicalEvent` with phase `PreOpen`.

The record exists, but most of what a phase means is not implemented. The leading sequencer writes a
`TechnicalEvent` only to halt trading or lift a halt, always with phase `Open`, and derives only whether
trading is halted from the ones it holds. The matching engine applies each one in WAL order, takes whether
trading is halted from it, and refuses one whose trading day is older than the trading date of the record
carrying it. Neither keeps or enforces the phase. A phase implementation extends these handlers and this
state; it does not add a second record.

`TechnicalEventReason` currently names only the causes of a halt and of lifting one. Phase changes need
reasons of their own, such as one for a scheduled transition; the exact values are to be decided with the
request below.

## Moving from one phase to the next

### The sequencer's rule

The leading sequencer checks each requested transition against its current state before writing anything:

| Requested phase for day `D` | Current state | Further condition | What the sequencer does |
|---|---|---|---|
| `StartOfDay` | No trading state: the WAL is fresh | `D` is the run manifest's date | Writes `TechnicalEvent(StartOfDay, D)` as the first record of the WAL |
| `PreOpen` | `(D, sod)` | Everything that must be applied before members may enter orders has been applied (see below) | Writes `TechnicalEvent(PreOpen, D)` |
| The phase after the current one | That phase's predecessor, on day `D` | None yet; the phase table may add some | Writes the `TechnicalEvent` for it |

**A repeated request is always harmless, and needs no request identity.** Within one trading day the phases
only move forward, so the sequencer answers every request from its current state:

- **the venue is already in the requested phase, on day `D`:** nothing is written, and the answer says so;
- **the venue is already past the requested phase, on day `D`:** nothing is written, and the answer says
  so and gives the current phase;
- **the requested phase is the next one, and its conditions are met:** the transition is written;
- **anything else, including a request for a different trading day or a phase further ahead than the next
  one:** refused, with the reason, and nothing is written.

An operator or procedure whose answer was lost asks again and learns where the venue now is, whether or not its first
request took effect. This is the rule `OperatorHaltRequest` already follows: halting a venue that is halted
writes nothing and answers `AlreadyHalted`, and lifting a halt that is not in force answers `NotHalted`.

Every phase change carries the resulting phase and `D`, so a component never has to infer the venue's state
from the name of a transition. A component can still replay the changes to rebuild its state; a record that
states its result makes it explicit and supports inspection and late recovery.

### The request and its answer

An operator, or the run procedure at the start of the day, sends a phase request to both
sequencers over their existing listener for operators. This is the routing and authentication of
`OperatorHaltRequest` (161): the listener accepts only TLS with a client certificate signed by the
operator's certificate authority, and a sequencer that does not lead answers that it does not, so the sender
never needs to know which sequencer leads. Conceptually:

```text
TradingPhaseRequest {
    trading_day
    requested_phase
    operator_name
    schedule         # with the start of day only: the day's schedule (trading_day_plan.md, section 3.2)
}

TradingPhaseAnswer {
    outcome          # written, already in phase, already past it, refused with a reason, not the leader, not written
    trading_day      # the trading state the sequencer holds once the request is dealt with
    phase
    halted
    technical_event_seq_no   # the position of the TechnicalEvent written, or 0 when none was
    schedule_seq_no          # the position of the TradingSchedule written, or 0 when none was
    explanation      # the outcome in words, as the sequencer logged it
}
```

Only the request and the answer are new DSL messages; the phase change itself is a `TechnicalEvent`. The
request is not the phase change: the leading sequencer decides, writes the `TechnicalEvent`, and the
follower receives it through the existing replication of the WAL.

The phase changes during the day are normally written by the leading sequencer itself, when each phase's
time in the day's schedule has come; the schedule is a record in the WAL, sent with the start-of-day request
([trading_day_plan.md](trading_day_plan.md), section 3). The phase request is how an operator moves the venue
on at once, ahead of the schedule or in place of it, and the same rule answers both: a change is written only
while the venue is in the predecessor of the phase asked for.

### The condition for entering pre-open

Before the venue enters `pre_open`, the matching engine must have applied everything that has to be in
place before members may enter orders. Today that is nothing beyond the start-of-day event itself. Reference
data, when it exists, and long orders, if they are ever brought into scope, each add records that must be
applied first.

The sequencer can establish this with the existing `EnginePositionQuery` (118) and `EnginePosition` (119),
which report the highest sequence number the leading engine has acted on. That position already counts
`TechnicalEvent` records, not only orders and cancels: `MatchingEngineStateMachine::apply_technical_event`
raises it by the same checks an order or a cancel goes through, and does so even for an event it refuses as
being for an older day. A new kind of record counts in the same way if the engine applies it through the
same two checks, `sequence_goes_backwards` when live and `applied_before` during catch-up. The comment on
`EnginePosition.highest_applied` in the DSL says "order or cancel", which is narrower than what the engine
does.

The follower matching engine catches up from the same WAL before it can be promoted, so it applies the same
records in the same order.

## Reference data: the requirement, and the rest left until later

The venue has no reference data today. Its design (what it holds, where it comes from, how it is
transferred, and which component checks instruments against it) is left until something in the venue uses
it. One requirement is fixed now, because it constrains the phases:

**Any data the matching engine uses to decide what to do with an order is applied in WAL order, by every
matching engine, before the venue enters `pre_open`.** That means:

- the venue stays in `sod` while the data is being transferred or checked;
- the primary and secondary matching engines apply the same data at the same position in the WAL, so a
  promotion or a catch-up can never change the decision on an order;
- a replay uses exactly the data production used for that day.

The likeliest way to meet it is for the data to enter the WAL as sequenced records, in bounded pieces,
before the `PreOpen` event. The WAL's replication then gives the data to the follower sequencer and to both
matching engines, so the process that loads it holds nothing that must survive its own failure, and can
simply be run again. That is a direction, not a decision: the amount of data and the size of each record
are not known.

## Orders that outlive the trading day

Every order is a day order, and orders that outlive the trading day are out of scope under principle V2.
If they are ever brought into scope, the start of the day gains one step: the surviving orders from the
previous day are re-entered through the sequencer as commands in the new day's WAL, after the reference data
and before the `PreOpen` event, and the sequencer waits for the matching engine to apply them as it waits for
everything else. Nothing in this note's design has to change for that. How they would be kept between days,
and the decisions they would need, are in [`long_orders.md`](long_orders.md).

## Gateways and order admission

Gateways need the current phase to tell members why orders are or are not accepted. The proposed path is the
existing control connection on which the leading sequencer sends each gateway `OrderAcceptance` (127),
extended with the trading day and the phase, and keeping the existing acceptance flag and reason. The
leading sequencer sends it when a gateway connects and whenever it changes. On promotion, the new leader
sends the state it derived from its WAL. A reconnecting gateway receives the current state rather than
waiting for the next transition.

Gateways refuse orders locally when the phase does not allow them, but the sequencer also enforces the phase
when an order reaches it, so that a gateway with an out-of-date phase cannot cause an order to be accepted.

Gateways do not follow the WAL to learn the phase; their control connection is the more direct path. Other
applications may follow the WAL if they need the history of events or need to reconstruct state.

## Following the WAL

The existing protocol for following the WAL uses unicast TCP. A subscriber connects to both sequencers and
sends `WalFollowRequest` (134) with its last durable WAL position. The leader sends each later record as a
`WalRecord` (103), catches the subscriber up to the committed end, then sends `WalFollowCaughtUp` (136) and
continues with new committed records. A sequencer that does not lead declines with `WalFollowDeclined`
(135); after a change of leader, the subscriber asks again from its last durable position.

Phase changes reach a subscriber this way as sequenced records among the orders and execution reports. No
component sends a phase change to subscribers directly.

## Recovery behaviour

Each sequencer rebuilds `(trading_day, phase, halted)` by applying WAL records in order:

1. At startup, it reads its own daily WAL and derives the state.
2. As leader, it applies each record as it writes it.
3. As follower, it applies each record from the leader as it writes it to its own WAL.

The WAL's commit and replication rules decide when a phase change is safe to act on. Once a phase change is
committed, both sequencers hold the same record and derive the same phase. A process that restarts as
follower catches up from its WAL position before it is treated as current. A WAL subscriber catches up from
its own durable position, and the hand-over from catching up to following live leaves no gap.

A snapshot of the state could make recovery faster later, but it would have to record the WAL position it
represents. The first implementation replays the WAL, unless measured recovery times call for a snapshot.

## What each phase allows, and the end of the day

Recording phases does not settle what members and components may do in each one. The table in
[`trading_phases.md`](trading_phases.md) still has open entries, including whether members may cancel orders
in `closed` or `eod`. Those permissions must be agreed before the phases are enforced, because the sequencer
and the gateways must apply the same rules.

The end of the day also has a constraint from recording: the order activity recorder must confirm that every
event of the day has been recorded, and no order record may follow the `eod` event. Every cancellation at the
close, including those of orders still open ([BUG-0132](../bug_list.md#bug_0132)), must therefore be
sequenced before the `eod` event; the recorder can treat the day as complete only once it has confirmed
everything through that event. This constrains the phase table and the exact order of closing,
cancellation and end of day.

## Decisions still required

1. Who supplies the expected trading date, and who writes and checks the run manifest?
2. The exact fields and outcomes of the phase request and its answer, and the `TechnicalEventReason` values
   for phase changes.
3. What each phase allows: the open entries in the table in [`trading_phases.md`](trading_phases.md), and the
   reason a member is given for each refusal.
4. Whether `OrderAcceptance` gains the trading day and the phase, or another message on the same connection
   carries them.
5. Which phase changes the end of the day sequences, and in what order: the cancellation of orders still
   open, the `eod` event, the recorder's confirmation, archiving, and the run manifest.
6. What must be archived to reproduce a day: the WALs, the run manifest, the configuration, and any other
   external input.

The decisions about reference data (its fields, its source, how it is transferred, and which component
checks orders against it) are left until something in the venue uses it. Those about orders that outlive
the day are in [`long_orders.md`](long_orders.md).

## Related design

- [`trading_phases.md`](trading_phases.md) — the phases, a halt as a separate flag, and the open questions
  about what each phase allows.
- [`sequencer.md`](sequencer.md) — WAL sequencing, replication, downstream subscribers, and the current
  derivation of whether trading is halted from WAL records.
- [`sequencer_app.md`](sequencer_app.md) — the sequencer's listeners and deployment configuration.
- [`mep_oar.md`](../pubsub/mep_oar.md) — the protocol for following the WAL, and a subscriber's catch-up.
- [`voided_commands.md`](../availability/voided_commands.md) — section 6, halting and lifting a halt,
  including the operator's request that phase requests follow.
- [`design_principles.md`](../design_principles.md) — V2, every order is a day order; V12, every trading day
  starts fresh.
- [`long_orders.md`](long_orders.md) — orders that outlive the trading day, deferred.
- [`bug_list.md`](../bug_list.md) — BUG-0137, the missing start-of-day and end-of-day procedure.
