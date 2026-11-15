# The trading day: design plan {#trading_day_plan}

**Status: agreed; steps 1 to 6 are done.** This note covers the whole trading day: every phase,
how the venue moves from one to the next, what each component does in each, and how each change reaches
the components and members that need it. It builds on three other notes and does not repeat them:

- [`trading_phases.md`](trading_phases.md) settles the shape: five phases, a halt as a flag alongside the
  phase, a record that states the resulting state and carries the trading day, and two audiences for it.
- [`trading_day_and_reference_data.md`](trading_day_and_reference_data.md) sets out the start of the day in
  detail: the trading date given when the run starts, the request that moves the venue from one phase to the
  next, and the sequencer's rule for checking it.
- [`long_orders.md`](long_orders.md) records how orders that outlive the day would be built, and why they are
  out of scope.

Each part below says what the venue is to do. The choices made between alternatives are collected in the last
section, each with the reason for it.

## 1. What exists today

- **The record.** `TechnicalEvent` (PDU 153) carries the phase, whether trading is halted, the trading day
  and a reason. It is a command in the WAL, which every matching engine applies at its position.
- **Every phase is entered on request (steps 4 and 5).** Each sequencer is started with `--trading-day D`.
  The first record of the WAL is the `TechnicalEvent` entering `sod`, and every record takes its trading
  date from it. An operator, or the procedure that starts the venue, asks for each phase in turn with
  `TradingPhaseRequest` (168), answered with `TradingPhaseAnswer` (169), through `scripts/trading_phase.py`.
  The leading sequencer writes a phase only from the phase before it: `pre_open` once the leading engine
  has acted on the start of day, `open` and `closed` at once, and `eod` once the leading engine has acted
  on the close. The sequencer keeps the phase from its WAL (`TradingPhaseState`); a halt carries the
  current phase, and the phase moves on while trading is halted except while halted because an engine
  cannot account for the orders the venue holds. No schedule exists yet: every change is asked for.
- **The gateways learn of a halt, not a phase.** The leading sequencer sends each gateway `OrderAcceptance`
  (127), whose reason can be `Halted`. The gateways refuse new orders while it is, and pass cancels on.
- **The sequencer refuses members' commands by phase.** Outside `pre_open` and `open` it refuses each
  one with `CommandRefused`, reason `TradingDayNotStarted` (before the start of day), `MarketNotOpen`
  (`sod`), `MarketClosed` (`closed`) or `TradingDayEnded` (`eod`), and both gateways tell the member why.
- **The close cancels every open order (step 6).** Each matching engine, applying the `TechnicalEvent` that
  enters `closed`, cancels every order on its book and sends a report of each to its member, answering the
  close. The sequencer writes `eod` only once the leading engine has acted on the close and every report it
  sent is in the WAL ([BUG-0132](../bug_list.md#bug_0132)).
- **Members are told nothing until they send an order.** Neither gateway sends or understands FIX
  `TradingSessionStatus` (35=h) or a binary equivalent ([BUG-0065](../bug_list.md#bug_0065)).
- **No topic carries the phase.** The matching engine publisher follows the sequencer's WAL and publishes
  two topics from it, `orders` and `execution_reports` (`applications/pubsub.dsl`). A `TechnicalEvent` in
  the WAL is on neither, although the order activity recorder's design follows "the technical events"
  ([oar_external_stream.md](../pubsub/oar_external_stream.md), sections 4.4 and 13.6).
- **There is no start-of-day or end-of-day procedure** ([BUG-0137](../bug_list.md#bug_0137)): `devenv.py`, the
  test harness and the performance scripts start the day themselves once the venue is up, and nothing
  closes it.
- **The matching engine does no matching**, deliberately: the venue is an example. Orders rest on the book
  and are cancelled; nothing trades. So the difference between `pre_open` and `open` is, in this venue, a
  difference in name only, and the place where a real venue runs its opening auction is the move from one to
  the other.

## 2. The day, phase by phase

The phases follow one another in one order, and each is entered by a `TechnicalEvent` the leading sequencer
writes when the phase's time in the day's schedule has come (section 3) or an operator asks, and when the
conditions for it hold
([trading_day_and_reference_data.md](trading_day_and_reference_data.md), "Moving from one phase to the
next"). The table gives what is proposed for each phase; the reasons follow it.

| | `sod` | `pre_open` | `open` | `closed` | `eod` |
|---|---|---|---|---|---|
| Entered when | The run starts, and a request for the day's date and schedule arrives | Its time has come, and the matching engine has applied everything required before orders | Its time has come | Its time has come | Its time has come, and every report caused by the close is in the WAL |
| A member may log on | yes | yes | yes | yes | yes |
| A member may enter an order | no | yes | yes | no | no |
| A member may cancel an order | no | yes | yes | no | no |
| The matching engine acts on orders | no | yes, without matching | yes | no | no |
| What the venue itself does on entry | Establishes the trading date | Nothing | Nothing (where an opening auction would go) | Cancels every open order, telling each member | Nothing further is sequenced; the recorder confirms the day |

### 2.1 Logging on is allowed in every phase

A member that can log on is told what the venue is doing (section 4.2) and why its orders would be refused.
A member refused at logon learns nothing except that it was refused, and will retry until it is let in.
Every real venue lets members connect well before trading. The gateways start during `sod`, so a member may
log on as soon as the gateway is up, and that includes `eod`.

### 2.2 `sod`: start of day

Set out in [trading_day_and_reference_data.md](trading_day_and_reference_data.md). The start-of-day event is
the first record in the WAL; it fixes the trading date for every later record. Orders and cancels are
refused. There is nothing to cancel, because the day starts with an empty book.

### 2.3 `pre_open` and `open`

**When the venue is ready for `pre_open`.** During `sod` no member's command is accepted, so every record in the
WAL before `pre_open` is one the leading sequencer wrote itself as part of the start of the day. The venue is
ready for `pre_open` when the leading matching engine reports, through `EnginePosition` (119), a highest applied
sequence number at or above the last of those records that is a command for the engine. Today that record is the
start-of-day `TechnicalEvent`; the `TradingSchedule` record is for the sequencers and is not sent to the engine.
When reference data or long orders exist, their records are written during `sod`, are commands for the engine,
and so are covered by the same rule with no change to it. The sequencer asks for the engine's position with
`EnginePositionQuery` (118) when the time for `pre_open` has come, and asks again at each check until the
condition holds. The follower engine is not waited for: it applies the same records before it can be promoted
([trading_day_and_reference_data.md](trading_day_and_reference_data.md), "The condition for entering pre-open").

In a real venue, orders entered in `pre_open` rest without matching and the move to `open` runs an auction
that matches them. This venue does no matching, so its engine treats the two phases alike: it accepts orders
onto the book and cancels them. The phases are still kept apart, so that the members, the gateways and the
recorder see the shape a real venue has, and so that an auction can be added at the move to `open` without
changing anything else.

### 2.4 `closed`: the venue cancels what is left

**The `TechnicalEvent` that enters `closed` is the command that cancels every open order.** Each matching
engine, applying it at its position in the WAL, cancels every order on its book and produces an execution
report for each, to its member. Because it is one command at one position, both engines cancel the same
orders, a replay cancels them again in the same way, and the follower engine catches up through it like any
other command. This is [BUG-0132](../bug_list.md#bug_0132)'s fix. The engine already does the same thing when it
halts after a long absence (R-0117), and the sequencer already records reports that no member's command
produced, with nothing in `answers_seq_no`.

From then on the sequencer refuses orders and cancels from members. The engine's reports for the close
are to carry the close's sequence number as `answers_seq_no`, so that a reader of the WAL, and a replay, can
tell which cancels the close produced.

**Commands and reports already in flight when the close is written.** The close is one command at one
position in the WAL, and each engine applies commands strictly in sequence, so every case is decided by
position:

- **A member's command sequenced before the close** is applied before it. An order entered just before the
  close rests on the book and is then cancelled by the close; a cancel sequenced before the close takes effect
  before it. The member receives the report for its own command first, then the close's cancel if the order
  was still open.
- **A report the engine has not yet returned** for a command sequenced before the close is still outstanding
  when the close is written. It arrives later and is recorded in the WAL as now, answering that command, not
  the close.
- **A member's command reaching the sequencer after the close is written** is refused by the sequencer, by
  phase, whatever the gateway believed when it passed it on (R-0170). A cancel for an order the close has
  cancelled is refused in the same way, and the member already has the close's report for that order.
- **The matching engine being absent at the close** delays nothing in the WAL: the close is written at its
  time, and the engine applies it, with every command before it, when it catches up.
- **`eod` waits for all of it.** End of day is entered only once no report is outstanding for any command
  sequenced up to and including the close (section 2.5 gives the check).

### 2.5 `eod`: end of day

The venue enters `eod` when its time has come or an operator asks, and only once the WAL holds a report for every order the close cancelled.
The sequencer has a similar check already, but it does not cover the close as it stands. To lift a halt, the
sequencer requires the WAL to hold a report answering the last order or cancel written before the halt
(`OperatorHaltOutcome::ReportsOutstanding`). That works because every member's command produces at least one
report and the engine sends every report of a command before it applies the next. The close can produce no report
at all, when no order is open, or many. So the check for `eod` is different: the leading engine's position
(`EnginePosition`) must show the close applied, and every report the engine sent before that answer must be in
the WAL.

The engine does not send its position on the same connection as its reports. It answers on the connection the
question came by, which is the sequencer's connection for orders; its reports go on its own connections to each
sequencer, and it may hold some back while it has no connection or does not yet know it leads. So the answer can
arrive before the close's reports. The answer therefore also says how far the engine has sent its reports (the
engine's epoch and its number for the last report sent) and how many it holds unsent. The sequencer writes `eod`
only when the answer shows the close applied and no report held, and the latest report in its WAL is at or after
the last one the engine says it sent. Each report record in the WAL carries the engine's epoch and number, so every
sequencer knows the latest from its own WAL, including one that takes the lead after the close. The
check is what makes the recorder's rule hold:
**no order record follows the `eod` event**, because nothing that can produce one is accepted after the close
and everything the close produced is in the WAL before `eod` is written.

During `eod` nothing further is sequenced. End of day is complete when the order activity recorder reports
that it has every event up to the `eod` event confirmed by the external messaging system
([oar_external_stream.md](../pubsub/oar_external_stream.md), section 13.6). The end-of-day procedure then
stops the components and archives every file the venue wrote during the day
([BUG-0137](../bug_list.md#bug_0137)). The recorder is not built yet, so until it is, the procedure goes
straight from `eod` to stopping the components. How the recorder's report reaches the procedure is
designed with the recorder.

Work belonging to the end of the day, such as receiving the next day's instrument prices, is left with the
reference data until something uses it.

### 2.6 A halt in any phase

A halt is a flag alongside the phase and keeps its meaning in every phase: new orders are refused, cancels are
passed on, and a person lifts it. Two changes follow from the phases:

- **A halt carries the phase the venue is in**, not always `Open` as it does now, because every
  `TechnicalEvent` states the whole of the venue's state.
- **The phase can move on while trading is halted**, with one exception. A halt at 16:20 must not stop the
  close at 16:30 from cancelling the open orders and the day from ending. The exception is a halt because a
  matching engine cannot account for the orders the venue holds (R-0123): cancelling every open order is then
  impossible, because nobody knows which orders are open, and a person must resolve the halt before the venue
  is closed.

## 3. How a phase change is caused: a schedule held in the sequencer

The venue moves from one phase to the next by itself, at the times in a schedule that is recorded in the WAL.
An operator can still move it on at once, or change the remaining times, by request. The schedule is handled
in the same way as the grace period that the venue already allows a member before cancelling its orders
after its session ends, so this section first describes that, and then the schedule.

### 3.1 The pattern the sequencer already uses

When a member's session ends, the leading sequencer writes `SessionEnded` (PDU 166) to the WAL. The record
carries `grace_period_seconds`, and its envelope carries the time the leader wrote it, `wall_time_ns`, so the
moment at which the member's orders are to be cancelled is in the WAL, not only in the leader's memory.

1. Every sequencer notes that deadline when it applies the record (`VenueStateFromTheWal::note_session_cancel`):
   the leader that wrote it, the follower that received it, and a sequencer reading its own WAL as it starts.
2. Once a second, on the recurring timer it already runs for other checks (`engine_absence_check_interval`),
   the leading sequencer compares each pending deadline with its wall clock
   (`SequencerThread::carry_out_due_session_cancels`, through `SessionCancelsPending::for_each_due`).
3. For each deadline that has passed, it writes `CancelSessionOrders` as an ordinary command. The matching
   engines apply that command at its position in the WAL and never consult a clock themselves.
4. Writing the command is itself a record that every sequencer notes, and it removes the deadline from those
   pending. A check repeated just before or just after a change of leader therefore never acts twice, and a
   newly promoted leader that finds a deadline already passed acts on it at once.

Aeron Cluster, which the venue takes as its reference for a replicated WAL, works the same way: a service
registers a deadline while handling a message, the leader writes a `TimerEvent` into the cluster's log when
cluster time passes it, and every member acts on that record, not on its own clock
(the file `external_documentation/README.md`, under "How the trading day moves from one phase to the next",
lists the sources). A clock on one machine never directly changes the venue's state; it only causes the
leader to write a record, and the record changes the state everywhere.

### 3.2 The schedule is a record in the WAL

A new DSL record, written by the leading sequencer as a command, as `TechnicalEvent` is:

```text
message TradingScheduleEntry (id=0)            # an inner record, as TopicRecord is
    TradingPhase phase                         # the phase to enter
    i64 earliest_utc_ns                        # the earliest moment to enter it, in nanoseconds since 1970 UTC;
                                               # 0 means as soon as its conditions hold
end

message TradingSchedule (id=<new>)
    i32 trading_day                            # YYYYMMDD; the trading day the WAL is for
    list<TradingScheduleEntry> entries         # in phase order, from PreOpen onwards
    string operator_name                       # who supplied it
end
```

The DSL already supports a list of messages as a field (`list<SeqNumRange>`, for example), so nothing new is
needed from the compiler.

- **Times are moments in UTC, not local times.** The sequencer never deals with time zones, daylight saving or
  holidays. The run procedure turns the venue's local times for day `D` into moments in UTC, and the WAL
  records the moments actually used.
- **A phase may be left out.** The phases follow one another in one order, so the schedule stops at the first
  phase it leaves out: that phase, and every phase after it, is entered only when an operator asks. A schedule
  with no entries is therefore a day run entirely by hand.
- **A time is the earliest moment, not the moment.** Entering a phase also needs that phase's conditions
  (section 2): `pre_open` needs the matching engine to have applied everything required before orders, and
  `eod` needs every report the close produced to be in the WAL. If the time has come and a condition has not,
  nothing is written, and the phase is entered as soon as the condition holds. Giving `eod` the time 0 is
  how "end the day as soon as the close is complete" is written.

**How the schedule reaches the sequencer.** The run procedure already supplies the trading date `D`. It also
reads the day's times for each phase from a small file, turns them into moments in UTC, and sends both in the
start-of-day request, which is the `TradingPhaseRequest` of
[trading_day_and_reference_data.md](trading_day_and_reference_data.md) with a schedule attached. The leading
sequencer checks that `D` is the run manifest's date, that the entries are in phase order, and that their times
never decrease. It then writes two records, one after the other:

```text
seq 1  TechnicalEvent(StartOfDay, D, reason Scheduled)
seq 2  TradingSchedule(D, [PreOpen 07:50Z, Open 08:00Z, Closed 16:30Z, EndOfDay 0])
```

If the leader fails between the two, the request sent again finds the venue in `sod` with no schedule recorded,
and the sequencer writes only the schedule, so the request is still harmless to repeat. A schedule whose times
have already passed when it is written is valid: the phases are entered at once, one after another, each as
its conditions hold.

**Changing the schedule during the day**, for an early close for example, is a new request on the same
listener, `TradingScheduleRequest`, carrying the trading day, a complete new list of entries and the operator's
name. The sequencer refuses it if it names a phase the venue has already entered or passed, and otherwise writes
a new `TradingSchedule` record, which replaces the previous one from its position in the WAL onwards. Every
change is therefore recorded, with who asked for it, at a definite position among the orders.

### 3.3 How a scheduled time causes a phase change

- **Every sequencer holds the phase and the schedule.** `VenueStateFromTheWal` learns two more record types:
  from `TechnicalEvent` it keeps the current phase as well as whether trading is halted, and from
  `TradingSchedule` it keeps the latest schedule. Like the grace periods, this happens for every record,
  whether written as leader, received as follower, or read from the WAL at startup.
- **The leader checks once a second.** A new check, `carry_out_due_phase_change`, runs beside
  `carry_out_due_session_cancels` on the same timer. It looks up the entry for the phase after the current
  one. If there is one, its time has passed by the leader's wall clock, and the phase's conditions hold, it
  writes the `TechnicalEvent` for that phase with the reason `Scheduled`, through the path the halts already use
  (`write_technical_event`). A phase change is then up to a second later than its time, which is enough for
  an example venue. If more exact timing is ever wanted, the leader also starts a one-off timer
  (`start_one_off_timer`) for the time remaining, and starts it again whenever the phase or the schedule
  changes and whenever it takes the lead; the check once a second stays as a backstop.
- **A halt is obeyed.** The check moves the phase on while trading is halted only as section 2.6 allows: not while halted because a matching engine cannot account for the orders the venue holds.
- **A change is never written twice.** The check writes a phase change only while the venue is in that phase's
  predecessor, and writing it moves the venue on. A repeated check, an operator's request arriving in the same
  second, or a new leader therefore all leave exactly one record.

### 3.4 What every other component does: nothing new

The follower sequencer, both matching engines, the publisher and any replay receive a `TechnicalEvent` at a
position in the WAL and apply it, exactly as they apply a halt. None of them reads the schedule's times or
consults a clock. The `TradingSchedule` record is in the WAL so that a new leader knows the times, so that the
WAL shows why each change happened, and so that a check of the day can confirm that each change with the reason
`Scheduled` was written at or after its time.

### 3.5 A change of leader

The new leader has already rebuilt the phase and the schedule from its WAL, so on taking the lead it runs the
check at once.

- If a time passed while no sequencer led, the change is written at once, as an overdue grace period is now.
- If the old leader wrote the change and the follower received it, the new leader finds the venue already in
  that phase and writes nothing.
- If the old leader wrote the change but the follower never received it, the record is not part of the
  follower's WAL and is discarded when the old leader rejoins. The new leader finds the venue still in the
  predecessor phase and writes the change itself. Either way exactly one change survives.

**Timing, stated exactly.** The times are compared with the clock of whichever sequencer leads, as the grace
period's are.

- A scheduled phase change is never written before its time by the clock of the leader that writes it.
- While a leader exists and the phase's conditions hold, it is written no later than one check interval (one
  second) after its time.
- While no sequencer leads, nothing is written. A leader that takes over after the time has passed writes the
  change at its first check, so the change is then late by the time the venue took to choose a new leader.
- The venue assumes the sequencers' machines keep their clocks synchronised to an external source, to well
  within the one-second check interval. Nothing in the venue checks this. A leader whose clock is fast writes a
  change early by the amount it is fast; one whose clock is slow writes it late.
- Each record carries the time it was written (`wall_time_ns`), and the `TradingSchedule` record carries the
  times asked for, so how late each change was, and which leader wrote it, can be read from the WAL.

### 3.6 What operators still do

- **Before the day:** the run procedure sends the start-of-day request, with `D` and the schedule. After that,
  nothing outside the venue needs to run for the day to proceed.
- **During the day:** an operator can move the venue to the next phase at once with a `TradingPhaseRequest`,
  answered as [trading_day_and_reference_data.md](trading_day_and_reference_data.md) sets out, or change the
  remaining times with a `TradingScheduleRequest`. The London Stock Exchange describes the same arrangement: a
  published schedule, and "market control actions invoked by London Stock Exchange that override the normal
  schedule" (MIT201, section 4.4).

The cost is two new DSL messages (`TradingSchedule` and `TradingScheduleRequest`, with its answer) beside the
phase request and answer, two more record types noted by `VenueStateFromTheWal`, and one more check on a timer
the leader already runs. Nothing is added to the path an order takes.

## 4. How a phase change reaches those that need it

`trading_phases.md` names two audiences: components that must **act** on the phase, and components that
only need to **know** it. Its book section requires both to be served, and the two paths named.

### 4.1 Components that act: the sequencer and the gateways

- **The sequencer** holds the phase from its WAL, as it already holds whether trading is halted, and refuses
  every member command the phase does not allow, with a new `CommandRefusedReason` for each, so that no
  gateway's view of the venue, however out of date, can let a command through.
- **The gateways** take the phase from the sequencer's control connection, as they take a halt now.
  `OrderAcceptance` gains the trading day and the phase, and a reason for each phase that refuses orders. The
  leading sequencer sends it when a gateway connects and whenever it changes, and a newly promoted leader sends
  the state it derived from its WAL. A gateway refuses what the phase does not allow before it reaches the
  sequencer, and says why.

### 4.2 Members

What members are told:

- **The FIX gateway sends `TradingSessionStatus` (35=h)** to every logged-on member each time the phase or
  the halt changes, and to each member as it logs on. It carries `TradSesStatus` (340): pre-open, open,
  closed or halted. `sod` and `eod` are the venue's own housekeeping, and are reported as closed, as real
  venues do.
- **It answers `TradingSessionStatusRequest` (35=g) itself**, from the state it holds, so that a member asking
  never becomes traffic on the sequencer.
- **Every member receives every change, with no list of which members asked**, whatever the request's
  `SubscriptionRequestType` (263) says. Keeping a subscription for each member adds state to the gateway for
  nothing a member needs, since every member wants to know when the market closes.
- **The binary gateway gains an equivalent message and request**, defined in the DSL from the same phase
  enum, so the two gateways cannot drift.

This also closes the part of [BUG-0065](../bug_list.md#bug_0065) about a member that is not trading never
being told of a halt.

### 4.3 Components that only need to know: a topic on the matching engine publisher

The order activity recorder, `topic_probe`, and in future a market data feed or a loader of reference data,
need the phase changes in order among the orders and execution reports, but act on no member's command.
They receive them on a third topic of the matching engine publisher:

- **A `technical_events` topic.** The publisher already follows the sequencer's WAL,
   keeps its own copy, and publishes `orders` and `execution_reports` from it by choosing records by message
   type. A `technical_events` topic, holding the `TechnicalEvent` records, is a declaration in
   `applications/pubsub.dsl` and the same choosing. Its records carry the sequencer's sequence numbers, so a
   subscriber to two topics can tell exactly where each phase change falls among the reports, which is what
   the recorder needs. It is tiny, and a subscriber that has just started takes it from the beginning and
   so learns the whole day's state at once, which is the book's requirement that a component that has just
   started can ask rather than wait. The cost: `TechnicalEvent` is defined in the framework's
   `leader_follower.dsl`, which `pubsub.dsl` does not include today.
Two other ways were considered and are not used:

- **Each such component following the sequencer's WAL itself**, with `WalFollowRequest`, as the publisher does.
  No new topic would be needed, but each such component would become another follower of the sequencers, the
  components the venue can least afford to load, and would have to handle changes of leader itself.
- **Carrying the phase changes on the existing `execution_reports` topic.** The order would be kept without
  effort, but the topic would no longer hold only execution reports, and every subscriber to it would have to
  recognise and skip a second kind of message.

The topic is what the publisher was built for, it leaves the sequencers' load unchanged, and it keeps each
topic to one kind of record.

### 4.4 Per instrument, later

A halt or a phase for one instrument is a different message, not an optional field on this one: FIX already
separates `TradingSessionStatus` (35=h) for the session from `SecurityStatus` (35=f) for one instrument. So the
venue-wide `TechnicalEvent` stays as it is, and a status for one instrument, if it is ever wanted, is an
addition. This settles the question [trading_phases.md](trading_phases.md) leaves open, and needs no work now.

## 5. Order of work

Each step leaves the venue working and is checked by scenarios before the next begins. Steps 1 to 3 are
documents and decisions; code starts at step 4.

1. **Agree this plan.** Done: the decisions are in section 6.
2. **Write the requirements** in the book's high availability chapter, in the section "The venue says what
   it is doing", and fill in the table in [trading_phases.md](trading_phases.md). Done: R-0164 to R-0175.
3. **Write the end-of-day and start-of-day procedures, and the list of every file the venue writes during a
   day** ([BUG-0137](../bug_list.md#bug_0137)), as documents. Done: [start_and_end_of_day.md](start_and_end_of_day.md).
4. **The trading date from the start-of-day event.** The run is given its date; the first record of the WAL is
   the start-of-day `TechnicalEvent`; every record takes its date from it; the sequencer refuses a start of day
   for any other date. The phase request and its answer are built here, for `StartOfDay` and `PreOpen` only.
   The venue then moves to `open` as soon as it is in `pre_open`, so that everything else works as now. Done.
5. **Every phase in the sequencer**: the full cycle of requests, the sequencer's checks, the refusals by phase
   with their new `CommandRefusedReason` values, and a halt that carries the phase. Pre-open is no longer
   followed at once by open: each phase is asked for, and the procedure that starts the venue asks for the
   start of day, pre-open and open in turn. Done.
6. **The close**: the matching engine cancels every open order on applying the close, and the sequencer allows
   `eod` only once every report the close produced is in the WAL ([BUG-0132](../bug_list.md#bug_0132)). Done:
   `EnginePosition` carries how far the engine has sent its reports, as section 2.5 explains.
7. **The gateways**: `OrderAcceptance` with the trading day and the phase, refusals by phase, and
   `TradingSessionStatus` and its binary equivalent to members ([BUG-0065](../bug_list.md#bug_0065)).
8. **The `technical_events` topic** on the matching engine publisher, for the components that only need to know.
9. **The schedule and the end-of-day procedure**: the `TradingSchedule` record, the start-of-day request
   that carries it, `TradingScheduleRequest`, the leader's check once a second, and the run procedure that reads
   the day's times from a file; and a component refusing to start on a file from another trading day
   ([BUG-0137](../bug_list.md#bug_0137)).

Steps 6 to 8 do not depend on one another and can be done in any order after step 5.

## 6. Decisions

1. **Components that only need to know the phase receive it on a `technical_events` topic** on the matching
   engine publisher (section 4.3).
2. **The table in section 2 stands as written.** In particular, cancels are refused in `closed` and `eod`,
   because the close has already cancelled everything and the recorder needs no order record after `eod`; and
   the matching engine treats `pre_open` like `open`, because it does no matching.
3. **Members may log on during `eod`**, as in every other phase (section 2.1).
4. **How the recorder's report that the day is confirmed reaches the end-of-day procedure is designed with the
   recorder.** Until the recorder exists, the procedure goes from `eod` straight to stopping the components
   (section 2.5).
5. **The phase moves on while trading is halted, except while halted because a matching engine cannot account
   for the orders the venue holds** (section 2.6).
6. **The schedule is held in the sequencer**: it is a record in the WAL, sent with the start-of-day request, and
   the leading sequencer writes each phase change when its time has come (section 3).
7. **Members are told as section 4.2 sets out**: `TradingSessionStatus` (35=h) to every logged-on member on
   every change and at logon, `TradingSessionStatusRequest` (35=g) answered by the gateway, no list of who
   asked, and a binary equivalent defined in the DSL.
