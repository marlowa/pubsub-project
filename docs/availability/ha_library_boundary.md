# High availability in the library: where the line falls {#ha_library_boundary}

> **Status: draft for review, not agreed.** This is the design asked for in section 8 of
> [ha_in_the_library.md](ha_in_the_library.md), which sets out why most of the high availability
> machinery should move from the applications into the library and the direction agreed for it. This
> page says, one design area at a time, what the library would provide, what an application would
> supply, and how the pieces built today map onto it. Nothing in the code is to change on the strength
> of this page until it is agreed. Section 13 lists the decisions it needs.

## 1. The shape in one paragraph

The library provides a **replicated, sequenced write-ahead log (WAL)**: a pair of instances, one leading, that put
commands in one order, hold them on both machines, and hand them, in that order, to whatever follows
the WAL. An application that keeps state is written as a **deterministic state machine** fed by that
WAL: given the same commands in the same order it reaches the same state and produces the same output,
so every instance that applies the WAL holds the same state without being sent anyone else's. A
component that only sends commands and receives answers, such as a gateway, is a **client** of the WAL,
and the library gives it the client's side. Leadership, replication, repair, catch-up and duplicate
detection are the library's; what a command means is the application's.

In the venue, the sequencer pair is the replicated WAL, the matching engine is the state machine that
follows it, and the gateways are its clients.

Throughout this page, WAL means the write-ahead log, the file of sequenced commands on disk. The text
log each process writes about what it is doing is called the Quill log, and is never meant by WAL.

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
with no WAL, the arbiters and the witness, become a voting service the library provides and any pair
can use, configured rather than written.

**What an application would supply.** Which pair it belongs to, its instance's identity, and where
its voters are: configuration, not code.

**One thing to settle.** The lease classes currently use the disk-sync recorder that principle F4 and
BUG-0126 say must change (its lock and its occasional wait on the order-handling thread). That change
is easier made once, in the library, than in each component.

## 3. Design area: the replicated WAL

**What it is for.** Every command is held on both machines before anything acts on it (V5), the WAL
is the record of the day (V2), numbers only go forward (V9), and a rejoining follower's WAL becomes a
copy of its leader's (V10).

**How it is done now.** Inside `SequencerThread`, mixed with the sequencer's business: writing the WAL,
sending each record to the follower, holding commands back from the matching engine until the follower
confirms them, trading without a follower, the statement that the follower may not take over, the
table of epochs in the WAL, and the repair of a rejoining follower. On a follower two threads write the
WAL, which breaks F1 and is kept safe by locks (BUG-0124).

**What the library would provide.** A WAL component owned by one thread, which:

- numbers each command and writes it, on the leader;
- sends each record to the follower, and counts a command as committed once the follower has
  confirmed it;
- when no follower is connected or it has fallen behind, carries on alone, and first records through
  the leadership component that the follower may not take over (V4, V11);
- repairs a rejoining follower's WAL, by the epoch of each record (V10);
- on the follower, writes the leader's records **on its own thread only**, so the second writer and
  its locks do not exist by construction;
- reports, as metrics and Quill log lines, whether it is running without a standby (BUG-0134).

**What an application would supply.** Nothing for replication itself. It hands the WAL commands to
sequence and is told when each is committed.

## 4. Design area: following the WAL

**What it is for.** A component that follows the WAL, such as the matching engine or a publisher,
applies every committed command in order, and one that starts or restarts catches up before it acts.

**How it is done now.** Separately in each follower of the WAL: the matching engine's catch-up
(`MePositionRequest`, `MePositionAck`, the R-0101 count check), the publishers' subscription
(`handle_wal_subscribe_request`), each served by reading the WAL on the sequencer's thread
(BUG-0133).

**What the library would provide.** One way to follow the WAL: a follower says the position it has
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
it goes, with the position in the WAL it reflects
([open_order_checkpoint.md](../durability/open_order_checkpoint.md)), and on restart reads it back and
catches up from that position. The region is built on `MappedSlotStore`, which is already in the
library.

**What the library would provide.** The rule that a snapshot and a position in the WAL are always
taken and restored together, and the catch-up from that position (section 4). Whether a snapshot is
written continuously, as the engine's region is, or at intervals, is the application's choice.

**What an application would supply.** How to write its state and the position together, and how to
read them back.

## 6. Design area: recognising a command twice

**What it is for.** A command is placed in the WAL at most once, judged by its identity, however many
times and by whatever path it arrives (V7).

**How it is done now.** Three separate mechanisms in the sequencer, because only commands marked as
sent again are checked (BUG-0122), and a reused `ClOrdID` is refused only while its first order is still
on the book (BUG-0114).

**What the library would provide.** One check on every command against the identities the WAL already
holds, before it is sequenced.

**What an application would supply.** How to read a command's identity (for the venue: comp id,
protocol and `ClOrdID`), and what to send back when a command is already in the WAL: for the venue, the
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

**What it is for.** For every instance that applies the WAL to reach the same state, nothing that
changes the state may depend on anything but the WAL.

**How it is done now.** Partly. The matching engine stamps its reports with the time the sequencer
recorded, carried in the WAL, and uses its own clock only when that is missing. But three things that
change the engine's book are decided by the leading engine alone, from its own clock or its own
circumstances:

- cancelling every order and halting when it has been unable to match for longer than allowed;
- cancelling every inherited order on promotion, under `open_orders_on_promotion = "cancel"`;
- refusing every order while halted.

**What the design requires.** A decision that changes the state of a replicated application is made
once and enters the WAL as a command, so that every instance applies it at the same point. Timers whose
ringing changes the state (good-till-date expiry, a change of trading phase, a halt) are of the same
kind: the leader of the WAL issues them as commands. Timers for a process's own housekeeping (leases,
connection retries, metrics) are not, and stay local. This is where the timer design of BUG-0128 and
the trading phases of [trading_phases.md](../venue/trading_phases.md) meet this one.

## 9. Design area: identifiers

**What it is for.** Every identifier the venue issues, such as `OrderID` and `ExecID`, is unique for
the day.

**How it is done now.** From counters in the matching engine. After a restart the order counter is set
above the highest order number in the book, but the report counter, `exec_id_counter_`, starts again at
zero, so an engine that restarts or is promoted issues `ME-EXEC-1`, `ME-EXEC-2` and so on a second time
that day. Recorded as [BUG-0139](../bug_list.md#bug_0139).

**What the design requires.** An identifier derived from the position in the WAL: for example, a
report's identifier made from the sequence number of the command that caused it and the report's place
among that command's reports. It is the same on every instance, so replicas agree, and unique across
restarts by construction, so no counter has to survive anything.

The identifier members see is scrambled, so that it does not reveal the sequence number: the
sequence number and the report's place are passed through a reversible cipher with a key fixed for the
trading day and shared by both engines. Question 3 of section 13 gives the reason.

## 10. Design area: the matching engine pair

**What it is for.** Losing the leading engine costs a short pause, not the orders on the book.

**How it is done now.** The follower engine is sent the leading engine's book (`BookUpdate`) and
discards the commands the sequencer sends it. On promotion it catches up from the WAL and then cancels
or keeps the inherited orders. The design notes considered running both engines in lockstep and left it
"not built", because it needs every decision the engine makes to be deterministic
([wal_and_ha.md](wal_and_ha.md), "What happens to resting orders on promotion").

**What is now known.** The engine does not match: orders rest until cancelled, and nothing is filled,
so its book changes only by accepting, refusing and cancelling. Its report times already come from the
WAL. The obstacles to determinism are the ones in sections 8 and 9: decisions taken by the leader alone,
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

## 11. Design area: which applications use the replicated WAL

**The replicated WAL is something an application chooses, never something it is given.** The
library's ordinary application thread, the one every component is built on today, knows nothing of a
WAL, a pair or a vote, and an application built only on it has none of them. Nothing in this design
changes that.

**What decides it: whether the application creates the library's replicated WAL object.** The
library would offer two new types. The names here are proposals; no code exists yet.

- `ReplicatedStateMachine`, an interface with three functions an application implements:
  `apply_committed_command`, which the library calls once for each command after it is held in the WAL
  on both machines; `save_state`, which writes the application's state together with the position in
  the WAL that state reflects; and `restore_state`, which reads both back after a restart.
- `ReplicatedWal`, the object that does everything else in sections 2 to 6: deciding who leads,
  writing and replicating the WAL, repairing a rejoining follower, catching the application up after a
  restart, and refusing a command already in the WAL. It is constructed with a reference to the
  application's `ReplicatedStateMachine` and with the application's configuration.

An application that wants the replicated WAL implements the interface and holds a `ReplicatedWal` as
a member of its application thread:

```cpp
class MatchingEngineThread : public ApplicationThread, public ReplicatedStateMachine {
    void apply_committed_command(const WalRecord& record) override;
    void save_state(StateWriter& writer) override;
    void restore_state(StateReader& reader) override;

    ReplicatedWal replicated_wal_;  // constructed with *this and the [replicated_wal] configuration
};
```

An application that does not do this, such as the authentication service, never constructs a
`ReplicatedWal`, so none of its machinery exists in that process: no WAL, no peer connection, no lease,
no voters. `ApplicationThread` itself gains nothing and knows nothing of any of it. There is no switch
to turn off, because nothing is there unless the application asks for it.

**What the configuration decides: one instance or a pair.** A `ReplicatedWal` reads a
`[replicated_wal]` section of the application's configuration, which says where its WAL lives and,
for a pair, which instance is its peer and where its voters are. With no peer named, it runs as one
instance: it still writes the WAL and still calls `apply_committed_command` for each command, but it
replicates nothing and needs no lease. The application's code is the same in both cases, so moving
from one instance to a pair changes the configuration, not the code.

The client's side (section 7) and the voting service (section 2) are chosen in the same way: an
application uses them by creating them, and one that does not create them has neither.

**What the WAL is for, and so which applications want it.** It is for state that must be the same on
two machines, built from commands whose order matters, where losing a command that was answered is not
allowed. If any of those three is missing, the WAL costs more than it gives.

**How the venue's components divide.**

| Component | Uses | Why |
|---|---|---|
| Sequencer pair | The replicated WAL | It is the WAL (section 1). |
| Matching engine | Follows the WAL, as a state machine | Its book is built from the commands in the order the WAL gives them. |
| Matching engine publisher pair | Follows the WAL, and the leadership component | It reads committed records in order and sends them to subscribers. Only the leading instance publishes, so it needs to know who leads, but it keeps no state of its own that the other instance must agree with. |
| Order activity recorder (designed, not built) | Follows the WAL | It reads committed records in order and sends them outside the venue. |
| FIX and binary order gateways | The client's side | They send commands and must see every one answered, but the state they hold, each member's session, belongs to the connection, not to the WAL. |
| Arbiters and witness | The voting service | They vote on who leads and keep no WAL. |
| Authentication service | None of it | See below. |
| Admin service | None of it | It is a Java web application outside the framework. |

**Why the authentication service does not use it.** It holds one piece of state, the map from comp id
to stored credential. That map is loaded at startup from an export of the database, and each change to
it is sent by the admin service to both instances, so each instance gets its state from outside and
neither needs the other's. A logon exchange is complete in itself, nothing about it is kept, and the
order of two logons from different members does not matter. The two instances both serve at once and
neither is elected: a gateway uses one and turns to the other if the first dies, and a session already
logged on is not affected. Putting every logon through a replicated WAL would add a disk write on two
machines and a round trip to a second machine to each logon, and would protect nothing that is at risk.

## 12. What moves where, and in what order

| Built today | Where it goes | What stays in the application |
|---|---|---|
| Lease classes in `applications/fix_common` | The library's leadership component (section 2) | Configuration |
| Arbiter and witness voting | A voting service the library provides | Configuration |
| Replication, holding back, trading alone, follower repair, epochs table, in `SequencerThread` | The library's replicated WAL (section 3) | Nothing |
| Catch-ups for the engine and the publishers | The library's way of following the WAL (section 4) | The handler and the starting position |
| The engine's open-order region | Stays the engine's snapshot (section 5) | How it writes and reads its state |
| The three checks against sequencing twice | One check in the library's WAL (section 6) | Reading a command's identity; the reply to a duplicate |
| `UnansweredCommandStore` and resending in each gateway | The library's client side (section 7) | Recognising an answer |
| Decisions taken by the leading engine alone | Commands in the WAL (section 8) | What each decision does |
| Counters for identifiers | Identifiers from the position in the WAL (section 9) | The identifiers' format |
| `BookUpdate` replication | Removed, under option B (section 10) | Nothing |

**A proposed order, lowest risk first.** This would replace phases 3 to 7 of
[the plan](../recovery_plan.md): one writer for the follower's WAL, the remaining locks that belong to
high availability, dividing the sequencer and the matching engine, and the one check against
sequencing twice. The framework locks of phase 4 that are not about high availability (the timers,
BUG-0128; the thread registry, BUG-0129; the WAL's helper, BUG-0130; the sequencer's allocations,
BUG-0131) are separate and can be done at any point:

1. **Move the lease classes and their messages into the library**, unchanged in behaviour: mostly a
   move, with every high availability scenario as the check.
2. **Build the library's replicated WAL with one writing thread**, and move the sequencer onto it. This
   takes in what phase 3 was for: the follower's second writer and its locks go.
3. **Give the WAL its way of being followed**, on a thread of its own, and move the engine's and the
   publishers' catch-ups onto it. This takes in BUG-0133.
4. **Make the engine deterministic**: identifiers from the WAL position (BUG-0139), and the engine's own
   decisions turned into commands. Test it by running both engines on the same WAL and comparing every
   report.
5. **Switch the engine pair to option B**, and remove book replication.
6. **Move the duplicate check into the WAL** (what phase 7 was for), and the gateways onto the library's
   client side.
7. **Divide what is left of the sequencer** (what phase 5 was for), which by then is the venue's own
   business: session binding and where reports go.

## 13. Decisions needed

There are five questions. Questions 1, 2, 3 and 5 are each answered yes or no. Question 4 is a choice
of one among three ways, A, B and C. The questions are not independent: ways B and C of question 4
are possible only if the answers to questions 2 and 3 are both yes. If either answer is no, the only
way left in question 4 is A.

Each question below carries its own background, so that it can be answered without reading the rest
of this page first.

### Question 1. Is the overall shape right?

The library provides a replicated write-ahead log (WAL). The sequencer pair is that WAL. The matching
engine is an application that follows the WAL as a deterministic state machine. The gateways are
clients of the WAL.

**Agreed 12 October 2026**, on the understanding that an application uses the replicated WAL only if
it chooses to, as section 11 describes. The authentication service, for example, does not use it.

### Question 2. Should every decision that changes the matching engine's book enter the WAL as a command? (Yes or no.)

**What happens now.** Three things change the engine's book without any command in the WAL telling
it to:

- **Halting after an absence.** If no engine could trade for longer than the configured limit
  (`absence_limit_seconds`, 300 seconds by default), the engine cancels every order and halts.
- **Acting on promotion.** When the follower engine takes over as leader, it cancels every order it
  inherited, if `open_orders_on_promotion = "cancel"` is configured.
- **Refusing orders while halted.**

The engine that leads makes each of these decisions alone, from its own clock and its own view of
its circumstances. The follower engine does not apply the commands in the WAL at all; it is sent a
copy of the leading engine's book.

**What "yes" means.** Each engine no longer decides these things for itself. Something still makes
each decision, but only once, and the decision is then written into the WAL as a command, in the same
way as an order or a cancel, at a definite position among the other commands. Every engine applies
it at that same position. For halts and changes of trading phase, the leading sequencer, which is the
leader of the WAL, issues the command. Timers that serve only a process's own upkeep, such as leases,
connection retries and metrics, are not affected; they stay local to the process.

**For "yes":**

1. **Both engines end up with exactly the same book.** An order that arrives just before a halt falls
   on the same side of the halt in both engines, because the WAL puts every command, the halt
   included, into one order. This is what makes ways B and C of question 4 possible.
2. **An engine can no longer change its book because of something only it saw**, such as how long it
   was itself down. That is the cause of
   [BUG-0146](../bug_list.md#bug_0146), where a restarted follower engine cancels its book after five
   minutes down although the other engine traded throughout. An engine replaying the WAL after a
   restart meets the halt at the position where it happened, and nowhere else.
3. **The decision is recorded.** The WAL shows when the venue halted and which orders came before and
   after the halt. Replaying the WAL gives the same result every time, so these behaviours can be
   tested by replaying a WAL.
4. **Members receive one set of cancel reports for each decision**, not one from each engine.

**Against "yes":**

1. **A decision takes slightly longer to take effect**, because it has to be sequenced and written to
   the WAL before anything applies it. The extra time is the usual round trip of an order through the
   sequencer, well under a millisecond.
2. **More machinery.** The WAL needs new kinds of command, and each kind needs a defined answer to the
   question of which process issues it. If the sequencer issues them, it has to know rules such as
   the absence limit, which ties it more closely to the venue's rules than it is now.
3. **The absence rule needs designing again.** "No engine could trade for longer than the limit" has
   to be measured by a process that was running throughout, or, if the whole venue was down, by the
   first leader of the WAL to start. This does not follow automatically from the change.
4. **It is work.** It touches the timer design recorded in
   [BUG-0128](../bug_list.md#bug_0128) and the trading phases in
   [trading_phases.md](../venue/trading_phases.md).

**Recommendation: yes.** Without it, the two engines can only be kept in step by copying the book from
one to the other, which is how it is done now and is the source of several open defects.

**Agreed 12 October 2026: yes.** How the absence rule is measured, the third point against, is still to
be designed.

### Question 3. Should identifiers be made from the position in the WAL? (Yes or no.)

**What happens now.** The matching engine issues identifiers such as `OrderID` and `ExecID` from
counters it keeps in memory. After a restart, the order counter is set above the highest order number
in the book, but the report counter, `exec_id_counter_`, starts again at zero. So an engine that
restarts or is promoted issues `ME-EXEC-1`, `ME-EXEC-2` and so on a second time in the same day, and
members can receive two different reports with the same identifier. This is recorded as
[BUG-0139](../bug_list.md#bug_0139).

**What "yes" means.** Each identifier is built from the sequence number of the command that caused it
and its place among that command's reports. For example, the second report caused by command 81,234
gets an identifier made from 81,234 and 2.

**For "yes":**

1. **The identifier is unique for the day by construction**, because no two commands have the same
   sequence number. No counter has to survive a restart.
2. **It is the same on both engines**, because both apply the same commands in the same order. Ways B
   and C of question 4 need this.

**Against "yes":**

1. **The identifiers look different from the current ones**, so anything that parses them or expects
   their current form, such as tests and tools downstream, has to change.
2. **The identifiers reveal sequence numbers to members.** This is probably harmless, but it is a
   choice to make knowingly.

**The other way to fix BUG-0139** is to restore the report counter after a restart. That works only
for way A of question 4, because under ways B and C two engines would also have to agree on the
counter's value.

**Recommendation: yes.**

**Agreed 12 October 2026: yes, with the identifiers scrambled.** Members are not shown the sequence
number. The sequence number and the report's place are passed through a reversible cipher whose key is
fixed for the trading day and is the same on both engines. The result is still unique for the day and
still the same on both engines, but the difference between two identifiers no longer tells a member how
many commands the venue sequenced between them, which would reveal the venue's total message rate.
Support staff turn an identifier back into its sequence number with a small tool that holds the day's
key. Still to be designed: the cipher, and how the day's key is created and given to both engines.

### Question 4. Which way should the two matching engines run: A, B or C? (Choose one.)

**Background.** The venue runs two matching engines so that losing one costs a short pause, not the
orders on the book. The engine never fills anything: orders rest until they are cancelled. So its book
changes only when it accepts, refuses or cancels an order.

**A. As now.**

- The follower engine ignores the commands the sequencer sends it, and is sent copies of the leading
  engine's book (`BookUpdate` messages) instead.
- On promotion, it catches up from the WAL, then cancels or keeps the inherited orders according to
  `open_orders_on_promotion`.
- It needs neither question 2 nor question 3 to be answered yes.
- The copying of the book, the reconciliation on promotion and the cancel-on-promotion setting all
  stay, and so do their open defects.

**B. Both engines apply every command; only the leading engine's reports are sent on.**

- Both engines build the same book from the WAL, so the copying of the book is removed.
- On promotion the follower's book is already exact, so there is nothing to reconcile and no reason to
  cancel the inherited orders.
- Reports still flow along one path, as they do now.
- It needs yes to both question 2 and question 3.

**C. Both engines apply every command, and both send their reports.**

- The sequencer passes on the first copy of each report, recognised by the identifier of question 3,
  and discards the second copy.
- There is no engine leadership at all, and losing an engine costs no pause.
- It doubles the number of reports sent to the sequencer.
- Every report must be identical on both engines, which is harder to test than way B.
- It needs yes to both question 2 and question 3.

**Recommendation: B now, and C later**, once B has shown in practice that the two engines always agree.

**Agreed 12 October 2026: B.** Way C stays possible as a later step, once B has shown in practice that
the two engines always produce the same reports.

### Question 5. Should the order of work in section 12 replace phases 3 to 7 of the recovery plan? (Yes or no.)

**Background.** [The recovery plan](../recovery_plan.md) puts the work that follows phase 2 in this
order:

- phase 3: one writing thread for the follower's WAL;
- phase 4: removing the remaining locks;
- phase 5: dividing the sequencer into parts;
- phase 6: dividing the matching engine into parts;
- phase 7: one check against sequencing a command twice.

Section 12 proposes a different order, in which the high availability machinery moves into the library
step by step. Each step of section 12 takes in the purpose of one or more of those phases. For example,
building the library's replicated WAL with one writing thread does what phase 3 was for, and moving the
duplicate check into the WAL does what phase 7 was for. Doing phases 3 to 7 as written, and then moving
everything into the library, would change the same code twice.

**What "yes" means.** Section 12's order replaces phases 3 to 7. The locks of phase 4 that have nothing
to do with high availability are done separately, at any point: the timers
([BUG-0128](../bug_list.md#bug_0128)), the thread registry ([BUG-0129](../bug_list.md#bug_0129)), the
WAL's helper thread ([BUG-0130](../bug_list.md#bug_0130)) and the sequencer's allocations
([BUG-0131](../bug_list.md#bug_0131)).

**What "no" means.** Phases 3 to 7 are done as written, and moving high availability into the library
is planned afterwards.

**Recommendation: yes.** Section 12's order assumes way B in question 4. If the answer to question 4 is
A or C, steps 4 and 5 of section 12 change, and the order is revised to match before it replaces the
phases.
