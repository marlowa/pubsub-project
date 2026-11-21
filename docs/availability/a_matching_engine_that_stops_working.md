# A matching engine that stops working while it stays connected {#a_matching_engine_that_stops_working}

> **Designed, not built.** This is the design for [BUG-0194](../bug_list.md#bug_0194). Its decisions, in section 9, are
> Andrew's to make. Nothing here is built until they are made.

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
2. **The time since the engine last sent a report, while it held reports.** While `reports_held` is above zero and
   `reports_sent_through` has not moved since the previous answer, the time runs. It starts again from zero whenever
   `reports_sent_through` moves or `reports_held` is zero.

A missing answer counts as no movement: an engine that does not answer the question has not shown that it moved.

**The engine is judged by time without progress, not by the size of the gap.** Under overload the gap between commands
sent and commands applied is large while the engine is healthy: about 60,000 commands in `ha_test.py` scenario 116.
What a healthy engine never does is stop moving for seconds while it has work.

**The engine has stopped working** when either time reaches the stall limit, `[recovery] engine_stall_limit_seconds`.
The recommended value is 3 seconds (decision 2).

### 3.1 Not blaming the engine for the sequencer's own delay

The engine paces its reports to the rate the leading sequencer reads them. If the sequencer's own thread falls behind,
for example while its disk is slow ([BUG-0175](../bug_list.md#bug_0175)), it stops reading the engine's reports. The
engine then pauses reading commands, and its `highest_applied` stops moving. The engine is working correctly; it is the
sequencer that is slow.

So neither time runs while the sequencer has messages from that engine that it has not yet taken: while it has paused
reading either of the engine's connections, or while messages from them wait in its own thread's queue
([bounding_messages_waiting.md](../framework/bounding_messages_waiting.md)). Both are facts the sequencer's own
framework already holds. This is decision 4.

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

1. **Writes the statement to its own disk**, in the record it keeps of its statements, so that the statement outlives
   the process.
2. **Sends it to both arbiters and to the other engine.** The statement names the engine, the epoch the engine leads
   in, the sequencer's own epoch, and a number that goes up by one each time the sequencer changes what it says, so that
   a late message cannot undo a newer one. This is how every statement is numbered now.
3. **Stops counting the engine as taking orders**, so that it holds the orders that arrive from then on, exactly as it
   does when an engine disconnects. The orders already sent to the stalled engine are in the WAL.
4. **Logs at Error** which engine, which time ran out, and the last positions it was told.

Each voter that receives the statement writes it to disk and from then on refuses that engine's lease requests:

- **The arbiters** refuse the stalled engine's renewals. They record a statement another component made; they judge
  nothing.
- **The other engine** refuses the stalled engine's renewals, and, since it no longer grants its vote to the stalled
  engine, asks to lead.

The stalled engine then has only its own vote. Its lease runs out within one lease period. If its thread is still
running, it notices this by the lease rules it already follows and stops acting as leader; if it is not running, it
does nothing at all, which is equally safe. The other engine is granted the lead by the arbiter once the arbiter's
promise to the stalled engine has run out, catches up from the WAL, and is sent the held orders, as after any change of
engine leader.

### 4.3 Why this cannot produce two leaders

The statement only makes voters refuse. A refusal can make the venue slower to choose a leader; it cannot give any
instance a lease. Section 4 of [majority_leases.md](majority_leases.md) shows that two instances could only lead at
once if some voter granted to both, and nothing in this design makes a voter grant. So the safety argument, and the
TLA+ model check of it ([tla/findings.md](tla/findings.md)), are unchanged. Only a sequencer that leads may make the
statement: an arbiter accepts it only from the sequencer it currently grants the sequencer pair's lease to, and the
other engine only from the sequencer whose `RoleAnnouncement` says it leads, under the higher epoch if both say so.

### 4.4 The time it takes

With the timings in `environments/dev.toml`, from the moment the engine stops:

| Step | Time |
|---|---|
| The stall limit runs out | 3 seconds, plus up to 1 second for the next answer |
| The stalled engine's lease, and the arbiter's promise to it, run out | up to 3 seconds |
| The other engine asks, is granted, and catches up | as after any change of engine leader: 2.4 seconds measured when the leader dies |

About 8 to 10 seconds in all. Orders accepted in the first 3 or 4 seconds were sent to the stalled engine; they are in
the WAL, the new leader applies them when it catches up, and their reports reach the members. The orders accepted
after the sequencer judged the engine stalled are held, and refused once they have been held for 5 seconds. If no
engine takes orders 10 seconds after the sequencer stops counting the stalled one, the existing recovery limit halts
trading and an operator decides.

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
following engine does not answer `EnginePositionQuery`; in this design it answers with its `highest_applied` and says
that it follows. A following engine applies every committed command from the WAL
([both_engines_apply_the_wal.md](both_engines_apply_the_wal.md)), so a working one keeps up, and the usual way for a
stalled one to start working again is to be restarted, catch up as a follower, and so show that it applies commands.

Without this, an engine judged stalled once could never lead again that day, and the venue would be left without a
standby for the rest of it even after the engine was restarted. This is decision 5.

## 6. Both engines, the follower, and the end of the day

**Both engines stall.** The sequencer says the first may not lead, the second leads and stalls in turn, and the
sequencer says the second may not lead either. No engine can then take orders, and the existing recovery limit halts
trading and tells the gateways why. That is the right outcome: the venue cannot process orders, and a person must
decide. Nothing new is needed. This is decision 6.

**The following engine stops applying.** That is a loss of resilience, not of service. The sequencer logs it at
Warning once its own time without progress runs past the stall limit, and repeats it at Error while it lasts, as the
leading sequencer does while it trades without a follower sequencer. It makes no statement about it. If that
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
   reports while holding them likewise; neither time runs while the sequencer has unread messages from the engine; a
   missing answer counts as no movement.
2. **Each voter's refusal** is tested where the existing statements are tested: an arbiter and an engine holding the
   sequencer's statement refuse the named engine's renewals, accept a later withdrawal, ignore a statement from a
   sequencer that does not lead, and ignore a late statement with a lower number.
3. **An `ha_test.py` scenario** stops the leading engine applying while it goes on renewing its lease, through a
   setting that only a test build accepts, and requires that every order the members sent is answered, that the other
   engine leads, and that no order is answered twice. Its result is decided from test events and members' messages,
   never from a Quill log.
4. **A second scenario** stops both engines and requires the halt and the gateways' refusals.
5. **Scenarios 115 and 116**, the load scenarios, must still pass, showing that overload is not mistaken for a stall.
6. Each test is made to fail on purpose before it is trusted.

## 9. Decisions

Each decision is a separate yes-or-no question.

### Decision 1. Is the evidence that the leading engine works its progress, asked for once a second with `EnginePositionQuery`, measured as the two times in section 3? (Yes or no.)

The alternative would be to watch only the reports arriving at the sequencer. That cannot tell an engine that has
nothing to report from one that has stopped.

### Decision 2. Is the stall limit 3 seconds, set as `[recovery] engine_stall_limit_seconds`? (Yes or no.)

It must be longer than the longest pause a healthy engine makes while it has work, which in normal running is a few
milliseconds, and short enough that the whole change of engine leader stays near the venue's other limits (section
4.4). The configuration check would refuse a value of the recovery limit or more.

### Decision 3. Does the leading sequencer decide, and act by a statement that the engine may not lead, recorded by both arbiters and the other engine, which then refuse its lease requests (section 4.2)? (Yes or no.)

### Decision 4. Do the times stop while the sequencer has unread messages from that engine, so that the engine is not blamed for the sequencer's own delay (section 3.1)? (Yes or no.)

### Decision 5. Is the statement withdrawn once the engine, following, has applied every command committed up to a point after the statement, with a following engine answering `EnginePositionQuery` for this (section 5)? (Yes or no.)

### Decision 6. When both engines stall, is the existing halt by the recovery limit the outcome, with nothing new added (section 6)? (Yes or no.)

### Decision 7. Is a following engine that stops applying logged, as a loss of resilience, with no statement made about it (section 6)? (Yes or no.)

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

Back to [High availability](README.md).
