# A matching engine that stops working while it stays connected {#a_matching_engine_that_stops_working}

> **Designed, not built.** This is the design for [BUG-0194](../bug_list.md#bug_0194). Six of its seven decisions are
> made (section 9). The stall limit is chosen once the engine's longest pause while healthy has been measured, as
> section 4.4 describes.

## 1. The failure

The leading matching engine can stop applying the commands it is sent, or apply them and stop sending their execution
reports, while everything that watches it still sees a healthy engine:

- its connections to both sequencers stay open;
- it goes on renewing its lease with the arbiter and with the other engine, so it goes on leading;
- the leading sequencer goes on sending it every order and cancel the gateways accept.

Members' orders are accepted, sequenced and written to the WAL, and no member hears anything more about them. Nothing
promotes the other engine, because nothing has failed that any part of the venue can see. This goes on until a person
notices.

Principles V6 (every command a gateway accepted is answered) and V8 (every report the matching engine produces reaches
the member) are broken for as long as it lasts.

### 1.1 What is already caught, and how

**A thread stuck inside one callback.** The engine applies commands and renews its lease on the same thread. A thread
stuck in one callback renews nothing, so its lease runs out within one lease period, 3 seconds. The other engine is
then granted the lead by the arbiter, catches up from the WAL and is sent the orders the sequencer held meanwhile. The
reactor's check for a stuck thread (`Reactor::check_for_stuck_threads`) stops the process later, once the callback has
run for `itc_maximum_inactivity_interval_`, 60 seconds by default.

**An engine that dies or loses its connections.** The sequencer stops routing orders to it, holds them, and sends them
to whichever engine leads next ([order_acceptance.md](order_acceptance.md), [voided_commands.md](voided_commands.md)).

**An engine that stalls while it catches up to lead.** While an engine is being caught up, the sequencer does not count
it as taking orders (`SequencerThread::an_engine_takes_orders`). The sequencer holds orders, refuses new ones once
`[recovery] orders_held_at_most_seconds` (5 seconds) has passed, and halts trading once
`[recovery] automatic_recovery_limit_seconds` (10 seconds) has passed with no engine taking orders
([far_back_catch_up.md](far_back_catch_up.md), section 5). So a stall during a catch-up already ends in a declared
halt, and this design adds nothing for it.

### 1.2 What is not caught

An engine whose thread keeps running, so that it renews its lease and answers on its connections, and which does not
apply the commands it is sent or does not send their reports. Some ways this can happen:

- it has paused reading its connections from the sequencers, as it does to pace its reports
  ([pacing_the_engines_reports.md](pacing_the_engines_reports.md)), and the notification that should end the pause never
  arrives;
- a fault in the order path drops commands or reports without failing;
- it holds its reports because it does not believe it leads, while the arbiter and the other engine believe it does.

This is the failure the design is for.

## 2. The rules this design keeps

- **The arbiter never judges health** ([matching_engine_presence.md](matching_engine_presence.md)). It may record a
  statement another component makes, as it does for lease rules 11 and 12
  ([majority_leases.md](majority_leases.md), section 3). It never decides for itself that an engine is not working.
- **A loss of resilience is not a loss of service** (V11). A following engine that falls behind does not stop the
  leading engine, and is not this bug.
- **No commands are held back while a stall is suspected.** If the engine has stalled, holding commands back gains
  nothing, since they are in the WAL and whichever engine leads next applies them. If the suspicion is wrong, holding
  them back causes the very outage this design is meant to end.
- **No two engines lead at once.** Whatever this design adds may only take a lease away; it may never give one.

## 3. The evidence that an engine is working

A connection, a lease and a heartbeat show that an engine can communicate. They do not show that it is applying
commands. The evidence of work is the engine's progress through the commands it is sent, and through the reports of
those commands.

The leading sequencer already has the means to ask for it. `EnginePositionQuery` (PDU 118) asks the leading engine,
and `EnginePosition` (PDU 119) answers with:

| Field | What it says |
|---|---|
| `highest_applied` | the highest sequence number of a command the engine has applied |
| `reports_sent_epoch`, `reports_sent_through` | the number of the last report it sent, under the epoch it numbered it in |
| `reports_held` | how many reports it holds and has not yet sent to every sequencer |

Today the sequencer asks only when it takes the lead, and for the trading-phase checks. In this design it also asks
the leading engine once a second for as long as the engine leads.

From the answers and what it knows itself, the sequencer keeps two times:

1. **The time since the engine last applied a command, while it had commands it had not applied.** The sequencer knows
   the highest command it has sent the engine. While that is above `highest_applied`, and `highest_applied` has not
   moved since the previous answer, the time runs. It stops, and starts again from zero, whenever `highest_applied`
   moves or the engine has applied everything sent to it.
2. **The time since the engine last sent a report, while it held reports.** While `reports_held` is above zero and the
   last report sent has not changed since the previous answer, the time runs. The last report sent is the pair
   `reports_sent_epoch` and `reports_sent_through`, compared as a pair: the number of a report starts again under each
   new epoch, so a number alone could appear not to move, or to go backwards, across a change of epoch. The time starts
   again from zero whenever the pair changes or `reports_held` is zero.

A missing answer counts as no movement: an engine that does not answer the question has not shown that it moved. An
answer counts only if it carries the `request_id` of the latest query sent to that engine, so a late answer to an
earlier query can neither show progress nor hide its absence.

**The engine is judged by time without progress, not by the size of the gap.** Under overload the gap between commands
sent and commands applied is large while the engine is healthy: about 60,000 commands in `ha_test.py` scenario 116.
What a healthy engine never does is stop moving for seconds while it has work.

**The engine has stopped working** when either time reaches the stall limit, `[recovery] engine_stall_limit_seconds`.
Its value is chosen from measurement (section 4.4).

### 3.1 Not blaming the engine for the sequencer's own delay

The engine paces its reports to the rate the leading sequencer reads them. If the sequencer's own thread falls behind,
for example while its disk is slow ([BUG-0175](../bug_list.md#bug_0175)), it stops reading the engine's reports. The
engine then pauses reading commands, and its `highest_applied` stops moving. The engine is working correctly; it is the
sequencer that is slow.

So neither time runs while the sequencer has messages from that engine that it has not yet taken: while it has paused
reading either of the engine's connections, or while messages from them wait in its own thread's queue
([bounding_messages_waiting.md](../framework/bounding_messages_waiting.md)). Both are facts the sequencer's own
framework already holds. Both of the engine's connections count: the one on which the sequencer sends commands and
receives answers to its queries, and the one on which the engine sends reports. This is decision 4, and section 8 says
how it is tested against a slow disk.

### 3.2 Which engine is judged

Only the engine the sequencer routes orders to, and only once it takes orders (`an_engine_takes_orders`), so never
during its catch-up, which section 1.1 shows is already covered. The following engine is not judged for leading. A
following engine that has stopped applying is a loss of resilience, which section 6 deals with.

## 4. Who decides, and what is done

### 4.1 The leading sequencer decides

The leading sequencer is the only component that sees both sides: what it has sent the engine, and what the engine
says it has done. It is also the component whose orders go unanswered. It decides.

### 4.2 It says the engine may not lead

The sequencer acts through the rule the voters of a pair already follow: **a voter refuses a lease to an instance that a
statement it holds says may not lead** ([majority_leases.md](majority_leases.md), section 3, rules 11 and 12). Today
only the leader of a pair makes such statements, about its peer. This design lets the leading sequencer make one about
a matching engine.

When the sequencer judges that the leading engine has stopped working, it:

1. **Sends the statement to both arbiters and to the other engine, and sends it again every second** for as long as it
   stands. The statement names the engine, the sequencer's own epoch, and a number that goes up by one each time the
   sequencer changes what it says, so that a late message cannot undo a newer one, as every statement is numbered now.
2. **Stops counting the engine as taking orders**, so that it holds the orders that arrive from then on, exactly as it
   does when an engine disconnects. The orders already sent to the stalled engine are in the WAL.
3. **Logs at Error** which engine, which time ran out, and the last positions it was told.

Each voter that receives the statement holds it in memory and from then on refuses that engine's lease requests,
renewals included:

- **The arbiters** refuse the stalled engine's renewals. They record a statement another component made; they judge
  nothing.
- **The other engine** refuses the stalled engine's renewals, and, since it no longer grants its vote to the stalled
  engine, asks to lead.

Refusing a renewal is what ends the stalled engine's lead: a voter that refused only a request to begin leading would
go on renewing the lease it had already granted, and the stalled engine would lead for good. A candidate holding the
statement also does not give way to the stalled engine under lease rule 9, which otherwise makes a candidate grant a
peer's request at a higher epoch. The model check in section 4.3 shows each of these is needed.

The stalled engine then has only its own vote. Its lease runs out within one lease period. If its thread is still
running, it notices this by the lease rules it already follows and stops acting as leader; if it is not running, it
does nothing at all, which is equally safe. The other engine is granted the lead by the arbiter once the arbiter's
promise to the stalled engine has run out, catches up from the WAL, and is sent the held orders, as after any change of
engine leader.

### 4.3 What the statement survives, and the model check

**A statement lasts only as long as the leadership of the sequencer that made it.** An arbiter accepts it only from the
sequencer it currently grants the sequencer pair's lease to, and drops it once it grants that lease at a higher epoch.
The other engine accepts it only from the sequencer whose `RoleAnnouncement` says it leads, under the higher epoch if
both say so, and drops it once a sequencer announces that it leads at a higher epoch. A new leading sequencer, whether
the other one or the same one after a restart, therefore starts with no statement standing, and judges the engines
afresh.

**Nothing about the statement is written to disk**, by the sequencer or by the voters. A statement under lease rule 11
must be on disk because forgetting it could let a follower that lacks commands lead, which loses commands. Forgetting
this statement can only let a stalled engine keep the lead for longer, which is the failure the design exists to end,
not a new one, and the sequencer's repetition every second restores it. Each way it can be lost:

| What happens | What follows |
|---|---|
| The sequencer dies after judging the engine stalled and before any voter receives the statement | No voter refuses the engine. The sequencer that leads next judges it afresh and, if it is still stalled, makes the statement after one stall limit |
| One voter receives the statement and the other does not | The engine keeps the vote of the voter without it and goes on leading, until the next repetition reaches that voter |
| A voter restarts and forgets it | The same: the next repetition restores it within a second |
| A late copy of an older statement arrives | It carries a lower number, or an older sequencer epoch, and is ignored |
| The leading sequencer changes | Every voter drops the old leadership's statements; the new leader judges afresh |

**The model check, and what it covers.** The statement is added to the TLA+ specification of the lease rules,
`MajorityLeaseHA.tla`, as the other engine and the arbiter coming to hold, or dropping, the statement that the stalled
engine may not lead, at any moment and independently of each other. The model has no statement messages, numbers or
sequencer epochs. What it checks is the lease question: whatever pattern of holding and dropping the voters go
through, which includes every pattern the rows of the table above can produce,

- two engines never act as leader at once;
- once both voters have held the statement for a lease period without a break, the stalled engine does not act as
  leader; and
- if from some point both voters hold it for good, the other engine comes to lead for good.

Removing the refusal of renewals, or letting a candidate give way under rule 9, each makes the second property fail.
The configurations are `EngineStallHA.cfg` for the first two and `EngineStallLiveHA.cfg` for the third, and the
results are in section 5 of [tla/findings.md](tla/findings.md).

That a voter ignores a statement with a lower number or from an older sequencer leadership, and drops a leadership's
statements when a newer sequencer leads, is not in the model. It is a property of the code that receives the statement,
and the unit tests of section 8 check it.

### 4.4 The time it takes, and how the stall limit is chosen

The time from the engine stopping to the other engine answering orders falls into two parts, which the venue's limits
treat differently.

**Before the judgement.** From the moment the engine stops until the sequencer judges it stalled: the stall limit, plus
up to one second until the next answer is due. Throughout this time the sequencer still counts the engine as taking
orders, so it sends it every order the gateways accept. Those orders are in the WAL; the engine that leads next applies
them and their reports reach the members, but until then the members hear nothing. No existing limit bounds this part.

**After the judgement.** The sequencer stops counting the engine as taking orders, and from that moment it is exactly
the case of an engine that has died ([order_acceptance.md](order_acceptance.md), "When holding becomes refusing"): the
engine may have renewed its lease just before the statement reached the voters, as a dead engine may have just before
it died, so the other engine can lead after at most one renewal interval, plus the lease period, plus the drift
allowance, which is 4.25 seconds with the timings every environment uses, plus its catch-up. The limit on holding
orders, 5 seconds, is derived from that same gap, and the limit on automatic recovery, 10 seconds, is longer. Both are
counted from the judgement, so a stalled engine is replaced within them as a dead one is.

**How the stall limit is chosen.** It must be longer than the longest a healthy engine goes without progress while it
has work, or a healthy engine is replaced, and short enough that the silence before the judgement stays within what a
member is told to expect at a change of engine leader. The first is a measurement, not an estimate, so the limit is
chosen in two steps:

1. The judging rule is built with the limit configurable, and the sequencer records, as a gauge and in a test event,
   the longest each time has run on a healthy engine. Scenarios 115 and 116, the load scenarios, and the scenarios that
   change the engine leader are run, and the longest time measured is recorded in
   [latency_findings.md](../operations/latency_findings.md).
2. The limit is set to at least three times that measurement, and the scenario of section 8 measures the time from the
   engine stopping to the first order answered by the other engine. Andrew then decides the value (decision 2).

**The configuration check.** The sequencer refuses to start unless:

- the stall limit is at least two query intervals, so that one late answer cannot by itself make an engine look
  stalled; and
- the stall limit plus one query interval is no longer than `[recovery] orders_held_at_most_seconds`, so that a member
  waits no longer for the venue to act on a stalled engine than it may wait for any change of engine leader.

With a query once a second and orders held for at most 5 seconds, the stall limit can be 2, 3 or 4 seconds.

### 4.5 The alternatives considered

**The sequencer closes its connections to the engine.** The failure then becomes one the sequencer already handles, but
the engine still holds its lease, so the other engine cannot lead. The engine reconnects and looks healthy again, or,
if the sequencer refuses it, no engine takes orders until the recovery limit halts trading. It ends the silence and
always costs a halt. Rejected.

**The engine watches its own progress from another thread and stands down.** This is how a thread stuck in a callback
is caught. It catches only the failures the engine can see in itself. An engine that has paused reading does not know
commands are waiting, because they wait in its kernel's receive buffer, and a fault that drops a command leaves the
engine no record that the command existed. It also relies on the engine that has gone wrong to act correctly. It could
be added later as a faster check for the cases it does see, and is not needed for this design.

**The arbiter decides from what it sees.** Ruled out by the rule that the arbiter never judges health.

## 5. When the engine may lead again

A statement that an engine may not lead lasts until the sequencer says it may. The sequencer withdraws it, with a new
statement under a higher number, once the engine shows that it is applying commands again: once, while following, it
has applied every command committed up to a point after the statement was made.

To know that, the sequencer asks the following engine for its position in the same way as the leading one. Today a
following engine does not answer `EnginePositionQuery`; in this design it answers, and `EnginePosition` gains two
fields: whether the engine leads or follows, and the epoch of the engine pair's leadership it knows. The sequencer
withdraws the statement only on an answer that

- carries the `request_id` of the latest query it sent that engine, so a late answer to an earlier query is ignored;
- was asked after the statement was made, so it cannot describe the engine as it was before;
- says the engine follows, under an engine epoch at least as high as the one the stalled engine led in; and
- gives a `highest_applied` at or above the sequencer's committed position at the moment it sent the query.

The withdrawal is a new statement with a higher number, so a delayed copy of the statement it replaces cannot undo it. A following engine applies every committed command from the WAL
([both_engines_apply_the_wal.md](both_engines_apply_the_wal.md)), so a working one keeps up, and the usual way for a
stalled one to start working again is to be restarted, catch up as a follower, and so show that it applies commands.

Without this, an engine judged stalled once could never lead again that day, and the venue would be left without a
standby for the rest of it even after the engine was restarted. This is decision 5.

## 6. Both engines, the follower, and the end of the day

**Both engines stall.** The sequencer says the first may not lead, the second leads and stalls in turn, and the
sequencer says the second may not lead either. No engine can then take orders, and the existing recovery limit halts
trading and tells the gateways why. That is the right outcome: the venue cannot process orders, and a person must
decide. Nothing new is needed. This is decision 6.

**The following engine stops applying.** That is a loss of resilience, not of service, and principle V11 asks that it
be reported loudly for as long as it lasts. The sequencer measures the following engine's time without progress in the
same way, from its answers. Once that passes the stall limit it logs at Error that the venue has no working standby
engine, and repeats the Error every 60 seconds until the engine is applying again, when it logs at Info that the
standby is back. It also sets a gauge, the seconds the following engine has gone without progress, which is zero
while the engine keeps up. It makes no statement about the engine, and the leading engine goes on serving. If that
engine is later promoted while still stalled, it is judged as the leading engine like any other. This is decision 7.

**With high availability turned off** there is one engine and nothing to promote. The sequencer still judges it,
stops counting it as taking orders and logs at Error, so members' orders are held, then refused, and trading is halted
by the recovery limit, instead of being taken in silence. It sends no statement, since there are no voters to send it
to.

**The end of the day.** The judgement runs until the end of day's second event is written. If the engine stalls
before then, the other engine leads, applies the end of day's events and shuts down as usual. What the stalled engine
does at the end of the day is the subject of [process_death.md](process_death.md) and is not changed here.

## 7. The stages a report passes after the engine

[BUG-0194](../bug_list.md#bug_0194) asks that each stage a report passes be named, with the hand-over it can observe
and how a gap there is detected or recovered. This design is about the first stage only. The later stages are listed
so that it is clear what this design does not claim.

| Stage | What can be observed | How a gap is found or recovered |
|---|---|---|
| The engine produces a report and sends it to the leading sequencer | `reports_held` and `reports_sent_through` in `EnginePosition` | This design: the second time in section 3 |
| The sequencer receives it and writes it into the WAL | `latest_engine_report_in_wal`, which the sequencer holds | Within one connection TCP delivers in order or closes the connection. A connection lost while reports are owed is logged at Error naming the commands whose reports may be lost ([pacing_the_engines_reports.md](pacing_the_engines_reports.md), section 4) |
| The sequencer hands it to its connection to the member's gateway | `MemberSessions::report_forwarded` counts hand-overs, not receipt | TCP as above; a member whose gateway reconnects asks for what it missed |
| The gateway hands it to its connection to the member | `GatewayProgressMetrics::count_report_sent`, counted before the socket write | The member sees a gap in its FIX sequence numbers and asks for the reports again, or asks on logon for those it has not received |

A report the engine sent that never reaches the member is a failure of a later stage, not of the engine, and is not
detected by this design.

## 8. How it is known to be right

1. **The rule that judges the engine** is a class of its own in the sequencer, as `RecoveryLimitRule` is, tested by
   unit tests that give it positions and times: a healthy engine under a large gap is never judged stalled; an engine
   whose applied position stops while it has commands is judged stalled at the limit and not before; one that sends no
   reports while holding them likewise; a report number that starts again under a new epoch counts as movement;
   neither time runs while the sequencer has unread messages from the engine; a missing answer, or one to an earlier
   query, counts as no movement; a withdrawal needs an answer asked after the statement.
2. **The sequencer's own delay.** A scenario slows the leading sequencer's writes to its WAL, through a setting that
   only a test build accepts, until the engine pauses for its reports to be read, for longer than the stall limit, and
   requires that no statement is made and that the engine goes on leading.
3. **Each voter's refusal** is tested where the existing statements are tested: an arbiter and an engine holding the
   sequencer's statement refuse the named engine's renewals, accept a later withdrawal, ignore a statement from a
   sequencer that does not lead, ignore a late statement with a lower number or from an older sequencer leadership,
   and drop a leadership's statements when a sequencer leads at a higher epoch.
4. **An `ha_test.py` scenario** stops the leading engine applying while it goes on renewing its lease, through a
   setting that only a test build accepts, and requires that every order the members sent is answered, that the other
   engine leads, and that no order is answered twice. Its result is decided from test events and members' messages,
   never from a Quill log.
5. **A second scenario** stops both engines and requires the halt and the gateways' refusals.
6. **Scenarios 115 and 116**, the load scenarios, must still pass, showing that overload is not mistaken for a stall.
7. **The model check** of section 4.3, with each new rule removed in turn to show it fails.
8. Each test is made to fail on purpose before it is trusted.

## 9. Decisions

Each decision is a separate yes-or-no question. Andrew answered them on 21 November 2026.

### Decision 1. Is the evidence that the leading engine works its progress, asked for once a second with `EnginePositionQuery`, measured as the two times in section 3? (Yes or no.)

**Yes.** The report position is compared as the pair of epoch and number, and a missing answer counts as no progress
only because the times stop while the sequencer has unread messages from the engine (decision 4). The alternative,
watching only the reports arriving at the sequencer, cannot tell an engine that has nothing to report from one that
has stopped.

### Decision 2. Is the stall limit 3 seconds, set as `[recovery] engine_stall_limit_seconds`? (Yes or no.)

**No, not yet.** The value is chosen from measurement, as section 4.4 describes: the longest a healthy engine goes
without progress under load, and the time from an engine stopping to the other engine answering. The configuration
check enforces the budget of section 4.4, not only that the limit is below the recovery limit. This decision is asked
again once the measurements are made.

### Decision 3. Does the leading sequencer decide, and act by a statement that the engine may not lead, recorded by both arbiters and the other engine, which then refuse its lease requests (section 4.2)? (Yes or no.)

**Yes,** with what the statement survives specified, including the sequencer dying after it decides and before any voter
receives the statement, and with the new path model-checked rather than argued from the existing check (section 4.3).

### Decision 4. Do the times stop while the sequencer has unread messages from that engine, so that the engine is not blamed for the sequencer's own delay (section 3.1)? (Yes or no.)

**Yes,** covering both of the engine's connections and the sequencer's queue, and tested against a slow disk
(section 8).

### Decision 5. Is the statement withdrawn once the engine, following, has applied every command committed up to a point after the statement, with a following engine answering `EnginePositionQuery` for this (section 5)? (Yes or no.)

**Yes,** with the answer saying the engine's role and epoch, and a late answer unable to withdraw a newer statement
(section 5).

### Decision 6. When both engines stall, is the existing halt by the recovery limit the outcome, with nothing new added (section 6)? (Yes or no.)

**Yes.**

### Decision 7. Is a following engine that stops applying logged, as a loss of resilience, with no statement made about it (section 6)? (Yes or no.)

**Yes,** provided the signal lasts as long as the engine is stalled, not a single warning: an Error repeated every 60
seconds and a gauge (section 6). The leading engine goes on serving.

## See also

- [BUG-0194](../bug_list.md#bug_0194), which this design is for
- [Deciding leadership by majority, with leases](majority_leases.md), whose rules 11 and 12 are the statements this
  design uses
- [Knowing there is no matching engine](matching_engine_presence.md), the case where the engine is absent rather than
  stalled
- [Refusing orders the venue cannot process](order_acceptance.md), which holds and refuses orders once no engine takes
  them
- [Pacing the matching engine's reports](pacing_the_engines_reports.md), the pause that section 3.1 must not mistake
  for a stall

---

Back to [High availability](../availability/README.md).
