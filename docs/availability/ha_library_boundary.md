# High availability in the library: where the line falls {#ha_library_boundary}

> **Status: draft for review, not agreed.** This is the design asked for in section 8 of
> [ha_in_the_library.md](ha_in_the_library.md), which sets out why most of the high availability
> machinery should move from the applications into the library and the direction agreed for it. This
> page says, one design area at a time, what the library would provide, what an application would
> supply, and how the pieces built today map onto it. Nothing in the code is to change on the strength
> of this page until it is agreed. Section 13 lists the decisions it needs.

## 1. The shape in one paragraph

The library provides a **replicated, sequenced log**: a pair of instances, one leading, that put
commands in one order, hold them on both machines, and hand them, in that order, to whatever follows
the log. An application that keeps state is written as a **deterministic state machine** fed by that
log: given the same commands in the same order it reaches the same state and produces the same output,
so every instance that applies the log holds the same state without being sent anyone else's. A
component that only sends commands and receives answers, such as a gateway, is a **client** of the log,
and the library gives it the client's side. Leadership, replication, repair, catch-up and duplicate
detection are the library's; what a command means is the application's.

In the venue, the sequencer pair is the replicated log, the matching engine is the state machine that
follows it, and the gateways are its clients.

## 2. Design area: deciding who leads

**What it is for.** At most one instance of a pair acts as leader at any moment (principle V3 in
[design_principles.md](../design_principles.md)), and an instance missing commands never takes over
(V4).

**How it is done now.** The rules live in classes that already know nothing of the component using
them: `LeaseVoter`, `LeaseHolder`, `LeaseParticipant`, `PairLeaseAgent`, with `LeasePromiseStore`,
`BackgroundPromiseRecorder`, `EpochStore` and `LeaderEpoch` keeping what must survive a restart. They
are in `applications/fix_common`. A component supplies the connections they send on, through
`LeaseLinksInterface`, and calls the agent every tick. The rules are in
[majority_leases.md](majority_leases.md).

**What the library would provide.** The same classes, moved into the library unchanged in behaviour,
with the messages they exchange (`LeaseRequest`, `LeaseGrant`, `LeaseRefusal`, and the statement that a
peer may not lead) defined among the framework's own messages instead of an application's. The voters
with no log, the arbiters and the witness, become a voting service the library provides and any pair
can use, configured rather than written.

**What an application would supply.** Which pair it belongs to, its instance's identity, and where
its voters are: configuration, not code.

**One thing to settle.** The lease classes currently use the disk-sync recorder that principle F4 and
BUG-0126 say must change (its lock and its occasional wait on the order-handling thread). That change
is easier made once, in the library, than in each component.

## 3. Design area: the replicated log

**What it is for.** Every command is held on both machines before anything acts on it (V5), the log
is the record of the day (V2), numbers only go forward (V9), and a rejoining follower's log becomes a
copy of its leader's (V10).

**How it is done now.** Inside `SequencerThread`, mixed with the sequencer's business: writing the log,
sending each record to the follower, holding commands back from the matching engine until the follower
confirms them, trading without a follower, the statement that the follower may not take over, the
table of epochs in the log, and the repair of a rejoining follower. On a follower two threads write the
log, which breaks F1 and is kept safe by locks (BUG-0124).

**What the library would provide.** A log component owned by one thread, which:

- numbers each command and writes it, on the leader;
- sends each record to the follower, and counts a command as committed once the follower has
  confirmed it;
- when no follower is connected or it has fallen behind, carries on alone, and first records through
  the leadership component that the follower may not take over (V4, V11);
- repairs a rejoining follower's log, by the epoch of each record (V10);
- on the follower, writes the leader's records **on its own thread only**, so the second writer and
  its locks do not exist by construction;
- reports, as metrics and log lines, whether it is running without a standby (BUG-0134).

**What an application would supply.** Nothing for replication itself. It hands the log commands to
sequence and is told when each is committed.

## 4. Design area: following the log

**What it is for.** A component that follows the log, such as the matching engine or a publisher,
applies every committed command in order, and one that starts or restarts catches up before it acts.

**How it is done now.** Separately in each follower of the log: the matching engine's catch-up
(`MePositionRequest`, `MePositionAck`, the R-0101 count check), the publishers' subscription
(`handle_wal_subscribe_request`), each served by reading the log on the sequencer's thread
(BUG-0133).

**What the library would provide.** One way to follow the log: a follower says the position it has
reached, the library sends every committed command after it, in order, and says when it has sent all
it holds, so the follower knows it has caught up; then it goes on sending new commands as they are
committed. The reading is done on a thread of its own, never on the thread that sequences, and the
hand-over from the catch-up to live commands is made at an exact position, so nothing is missed or
sent twice.

**What an application would supply.** A handler for each committed command, and the position it has
reached when it starts.

## 5. Design area: snapshots and restarting

**What it is for.** An instance that restarts must not replay the whole day, which takes about 40
seconds at 40 million orders.

**How it is done now.** The matching engine keeps its open orders in a memory-mapped region, written as
it goes, with the position in the log it reflects
([open_order_checkpoint.md](../durability/open_order_checkpoint.md)), and on restart reads it back and
catches up from that position. The region is built on `MappedSlotStore`, which is already in the
library.

**What the library would provide.** The rule that a snapshot and a position in the log are always
taken and restored together, and the catch-up from that position (section 4). Whether a snapshot is
written continuously, as the engine's region is, or at intervals, is the application's choice.

**What an application would supply.** How to write its state and the position together, and how to
read them back.

## 6. Design area: recognising a command twice

**What it is for.** A command is placed in the log at most once, judged by its identity, however many
times and by whatever path it arrives (V7).

**How it is done now.** Three separate mechanisms in the sequencer, because only commands marked as
sent again are checked (BUG-0122), and a reused `ClOrdID` is refused only while its first order is still
on the book (BUG-0114).

**What the library would provide.** One check on every command against the identities the log already
holds, before it is sequenced.

**What an application would supply.** How to read a command's identity (for the venue: comp id,
protocol and `ClOrdID`), and what to send back when a command is already in the log: for the venue, the
outcome of the original if the command was marked as sent again, and a refusal if it was not.

## 7. Design area: the client's side

**What it is for.** Every command a client has sent is answered (V6), across a change of leader or a
lost connection.

**How it is done now.** In each gateway: `UnansweredCommandStore` keeps every command until its first
answer, and the gateway sends all it holds again when a new leader announces itself or the connection
to the leader is made again.

**What the library would provide.** The same, for any client: a fixed-size store of unanswered
commands, sending them again on a change of leader or reconnection, and refusing new commands when the
store is full.

**What an application would supply.** How to recognise the answer to a command, and what to tell its
own users when the store is full.

## 8. Design area: time and decisions only one instance may make

**What it is for.** For every instance that applies the log to reach the same state, nothing that
changes the state may depend on anything but the log.

**How it is done now.** Partly. The matching engine stamps its reports with the time the sequencer
recorded, carried in the log, and uses its own clock only when that is missing. But three things that
change the engine's book are decided by the leading engine alone, from its own clock or its own
circumstances:

- cancelling every order and halting when it has been unable to match for longer than allowed;
- cancelling every inherited order on promotion, under `open_orders_on_promotion = "cancel"`;
- refusing every order while halted.

**What the design requires.** A decision that changes the state of a replicated application is made
once and enters the log as a command, so that every instance applies it at the same point. Timers whose
ringing changes the state (good-till-date expiry, a change of trading phase, a halt) are of the same
kind: the leader of the log issues them as commands. Timers for a process's own housekeeping (leases,
connection retries, metrics) are not, and stay local. This is where the timer design of BUG-0128 and
the trading phases of [trading_phases.md](../venue/trading_phases.md) meet this one.

## 9. Design area: identifiers

**What it is for.** Every identifier the venue issues, such as `OrderID` and `ExecID`, is unique for
the day.

**How it is done now.** From counters in the matching engine. After a restart the order counter is set
above the highest order number in the book, but the report counter, `exec_id_counter_`, starts again at
zero, so an engine that restarts or is promoted issues `ME-EXEC-1`, `ME-EXEC-2` and so on a second time
that day. Recorded as [BUG-0139](../bug_list.md#bug_0139).

**What the design requires.** An identifier derived from the position in the log: for example, a
report's identifier made from the sequence number of the command that caused it and the report's place
among that command's reports. It is the same on every instance, so replicas agree, and unique across
restarts by construction, so no counter has to survive anything.

## 10. Design area: the matching engine pair

**What it is for.** Losing the leading engine costs a short pause, not the orders on the book.

**How it is done now.** The follower engine is sent the leading engine's book (`BookUpdate`) and
discards the commands the sequencer sends it. On promotion it catches up from the log and then cancels
or keeps the inherited orders. The design notes considered running both engines in lockstep and left it
"not built", because it needs every decision the engine makes to be deterministic
([wal_and_ha.md](wal_and_ha.md), "What happens to resting orders on promotion").

**What is now known.** The engine does not match: orders rest until cancelled, and nothing is filled,
so its book changes only by accepting, refusing and cancelling. Its report times already come from the
log. The obstacles to determinism are the ones in sections 8 and 9: decisions taken by the leader alone,
and the report counter. Both are needed anyway. So lockstep is within reach, and the three ways to run
the pair are:

- **A. As now:** book replication, catch-up on promotion, cancel or keep. No determinism needed, but
  the replication, the reconciliation and the cancel-on-promotion policy all stay.
- **B. Lockstep, one engine's output used:** both engines apply every committed command; only the
  leading engine's reports are forwarded. Book replication goes. On promotion the follower's book is
  already exact, so there is nothing to reconcile and no reason to cancel inherited orders.
- **C. Lockstep, both engines' output used:** both engines apply every command and both send their
  reports; the sequencer forwards the first copy of each report, recognised by the identifier of
  section 9, and ignores the second. Engine leadership is not needed at all, and losing an engine costs
  nothing. It doubles the reports sent to the sequencer, and every report must be identical on both,
  which is a stronger condition to test than B's.

**Recommendation: B, with C kept as a later step.** B removes book replication and the reasons for
cancelling on promotion while keeping one path for reports, which is close to today's. C removes engine
leadership as well, and becomes possible once B has shown in practice that the two engines' outputs
agree.

## 11. Design area: using the library without high availability

Not every use of the library wants a replicated log. An application chooses it; one that runs a single
instance gets the same interfaces (commands in, committed commands out, a handler and a snapshot) with
no peer, no voters and no replication, so moving to a pair later changes configuration, not code.

## 12. What moves where, and in what order

| Built today | Where it goes | What stays in the application |
|---|---|---|
| Lease classes in `applications/fix_common` | The library's leadership component (section 2) | Configuration |
| Arbiter and witness voting | A voting service the library provides | Configuration |
| Replication, holding back, trading alone, follower repair, epochs table, in `SequencerThread` | The library's replicated log (section 3) | Nothing |
| Catch-ups for the engine and the publishers | The library's way of following the log (section 4) | The handler and the starting position |
| The engine's open-order region | Stays the engine's snapshot (section 5) | How it writes and reads its state |
| The three checks against sequencing twice | One check in the library's log (section 6) | Reading a command's identity; the reply to a duplicate |
| `UnansweredCommandStore` and resending in each gateway | The library's client side (section 7) | Recognising an answer |
| Decisions taken by the leading engine alone | Commands in the log (section 8) | What each decision does |
| Counters for identifiers | Identifiers from the position in the log (section 9) | The identifiers' format |
| `BookUpdate` replication | Removed, under option B (section 10) | Nothing |

**A proposed order, lowest risk first.** This would replace phases 3 to 7 of
[the plan](../recovery_plan.md): one writer for the follower's log, the remaining locks that belong to
high availability, dividing the sequencer and the matching engine, and the one check against
sequencing twice. The framework locks of phase 4 that are not about high availability (the timers,
BUG-0128; the thread registry, BUG-0129; the log's helper, BUG-0130; the sequencer's allocations,
BUG-0131) are separate and can be done at any point:

1. **Move the lease classes and their messages into the library**, unchanged in behaviour: mostly a
   move, with every high availability scenario as the check.
2. **Build the library's replicated log with one writing thread**, and move the sequencer onto it. This
   takes in what phase 3 was for: the follower's second writer and its locks go.
3. **Give the log its way of being followed**, on a thread of its own, and move the engine's and the
   publishers' catch-ups onto it. This takes in BUG-0133.
4. **Make the engine deterministic**: identifiers from the log position (BUG-0139), and the engine's own
   decisions turned into commands. Test it by running both engines on the same log and comparing every
   report.
5. **Switch the engine pair to option B**, and remove book replication.
6. **Move the duplicate check into the log** (what phase 7 was for), and the gateways onto the library's
   client side.
7. **Divide what is left of the sequencer** (what phase 5 was for), which by then is the venue's own
   business: session binding and where reports go.

## 13. Decisions needed

These are separate questions.

1. **Is section 1 the right shape?** The sequencer pair as the library's replicated log, the matching
   engine as a deterministic state machine following it, the gateways as clients. (Yes or no.)
2. **Should every decision that changes a replicated application's state enter the log as a command**
   (section 8), including the engine's halt after an absence and any action on promotion? (Yes or no.)
3. **Should identifiers come from the position in the log** (section 9)? (Yes or no.)
4. **Which way should the engine pair run: A, B or C** (section 10)? The recommendation is B, with C
   later.
5. **Should the order in section 12 replace phases 3 to 7 of the plan**, with the framework fixes of
   phase 4 that are not about high availability done separately? (Yes or no.)
