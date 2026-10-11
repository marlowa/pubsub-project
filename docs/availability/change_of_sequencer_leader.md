# Keeping every order and report across a change of sequencer leader {#change_of_sequencer_leader}

## 1. What this document covers

> **Status: built.** Every part described here is in the code, with the option chosen for each, and
> the `ha_test.py` scenarios in section 8 check them.

The venue runs two sequencer instances. At any moment one of them leads, holding a lease granted by
a majority of three voters, and the other follows. When the leader dies, the follower takes the lead
once the old leader's lease has run out. With the three-second lease the venue uses, that took 2.6,
2.9 and 4.2 seconds in the runs recorded in the bug list. This document calls that interval, from the leader's death to the
follower taking the lead, **the change of leader**.

A change of leader can lose or corrupt what the venue holds in five ways unless each is guarded
against. Each was recorded in the bug list, and each is prevented by a part of this design:

| What would go wrong | Bug | Prevented by |
|---|---|---|
| Orders a gateway sends during the change of leader are never placed and never answered | [BUG-0103](../bug_list.md#bug_0103) | 4.3 |
| Reports the matching engine sends during the change of leader never reach the member | [BUG-0103](../bug_list.md#bug_0103) | 4.4 |
| The matching engine holds orders that no surviving log holds | [BUG-0103](../bug_list.md#bug_0103) | 4.2 |
| The new leader numbers new records with numbers its log already holds | [BUG-0105](../bug_list.md#bug_0105) | 4.1 |
| An instance that stops leading keeps records its new leader does not hold | [BUG-0097](../bug_list.md#bug_0097) | 4.5 |

This document states what the venue must guarantee (section 3), the ways each part could be met with
their costs and which was chosen (section 4), a change of leader as it now happens (section 5), the
costs (section 6), the decisions and open questions (section 7), the tests (section 8), and where each
part is built (section 9).

## 2. The path of an order, and what each part guards against

### 2.1 The path of an order

1. A gateway checks a member's order, wraps it in a `WalRecord` envelope that names the member's
   session, sends it to **both** sequencers, and keeps a copy until the order is answered (4.3).
2. The leading sequencer gives the order the next sequence number, appends it to its write-ahead log,
   and sends the record to the follower. It holds the order back from the matching engine (4.2).
3. The follower discards the copy it received from the gateway. It writes its log only from the
   records its leader sends it, so that the two logs stay identical, and it acknowledges each record
   (`WalAck`).
4. When the follower's acknowledgement arrives, the leader sends the order to the matching engine.
5. The matching engine applies the order and sends its execution report to **both** sequencers.
6. The leading sequencer gives the report a sequence number of its own, logs it and replicates it, and
   forwards it to the member's gateway. The follower keeps a copy of the report it received from the
   engine, for as long as a change of leader could need it (4.4).

### 2.2 What each part guards against

**Orders sent during the change of leader would be lost.** Between the leader's death and the follower
taking the lead, every order a gateway sends reaches only the follower, which discards it because it
is still following. The gateway's copy is what saves it: the gateway sends it again to the new leader
(4.3).

**Reports sent during the change of leader would be lost.** A report the engine sends after the leader
has died, or that the leader received and had not yet forwarded, reaches only the follower. The
follower's kept copy is what saves it: the new leader forwards it (4.4).

**The engine would hold orders no surviving log holds.** If the leader sent an order to the engine
before its follower held it, a leader that died between the two sends would leave the engine holding an
order the new leader never received. Holding each order back until the follower has acknowledged it
prevents that (4.2). Scenario 59 makes the gap as long as it needs, with a test library that stops the
leader's sends to its follower from arriving.

**The new leader would reuse sequence numbers.** If a follower moved its counter only for some of the
records it wrote, it would number from a point below records it already holds when it took the lead,
and a reader that assumes the numbers only go forward would misread the log: the matching engine's
catch-up skips every record at or below the position it gives, and the publishers and the order
activity recorder identify a record by its number. Numbering from the highest record held prevents it
(4.1).

**The old leader's extra records would disagree with the new leader's log.** An instance that led and
rejoins as a follower may hold records its new leader never had, and the new leader's records can carry
the same numbers with different contents. Repairing the follower's log when it rejoins prevents it
(4.5).

## 3. What the venue must guarantee

These are stated for the change of leader, but each holds at every other moment too.

- **G1. Every command a gateway accepted from a member is answered.** It is either placed and
  reported, or refused with a reply. It is never left unanswered, and it is never placed twice.
- **G2. The matching engine acts only on commands the logs hold.** While both sequencers are running,
  that means both logs. The engine never holds an order that no surviving log holds.
- **G3. Every report the matching engine produces reaches the member.** A member may be sent a report
  more than once, provided every repeat is marked as one, as R-0122 already requires.
- **G4. Within a log, sequence numbers only go forward.** No number is used twice.
- **G5. When an instance rejoins as a follower, its log ends up identical to its leader's.**

The functional specification already contains the requirements a member relies on when it does not
know whether the venue took an order: it may ask (R-0002), it may resubmit under the original
`ClOrdID` (R-0003), and the venue refuses the repeat of an order it has committed (R-0006), whatever
became of the first order (R-0010), for the rest of the trading day (R-0119). G1 asks more of the
venue than those do: it requires the venue to answer without the member having to notice and act.
The duplicate check those requirements describe is still needed, and the design below depends on it.

## 4. The five parts, the ways each could be met, and which was chosen

### 4.1 Numbering: the new leader numbers from the highest record it holds (G4)

A follower moves its counter past every record it writes, whether that record came from its leader
in the ordinary stream or arrived any other way. A sequencer that takes the lead numbers from the
highest record it holds plus one. There was no real alternative to weigh. Built:
[BUG-0105](../bug_list.md#bug_0105).

**A guard in the matching engine, as a second line of defence.** The matching engine refuses a live
command whose sequence number is not greater than the last it applied, logs the refusal as an error
naming both numbers, and sends the member a rejection, so that a numbering fault shows as a refusal
rather than as a book that silently disagrees with the record
(`MatchingEngineThread::sequence_goes_backwards`). The guard is safe only with option A of 4.2: if
the engine could hold commands numbered above the new leader's last record, the new leader's next
genuine orders would be numbered at or below them, and the guard would refuse them. Under option A the
engine never applies a command the follower does not hold, so that case cannot arise.

### 4.2 The matching engine acts only on what the logs hold (G2)

**Option A, chosen and built: the leader sends a command to the engine once the follower has
acknowledged it.** The leader appends the command to its own log and sends the record to the follower,
and sends the command to the matching engine when the follower's acknowledgement arrives, in sequence
order. While no follower is connected, or the follower has fallen behind, the leader sends to the
engine at once, but only once a voter has recorded that its follower may not take over (section 7,
open question 2).

- *What it guarantees.* The engine never acts on a command the follower does not hold, so G2 holds by
  construction. Everything the old leader holds that the follower does not was never acted on and
  never answered, which is what makes 4.5 safe.
- *What it costs.* The order's trip to the follower and its trip through the engine stop running at
  the same time and run one after the other. The member's report already waits for the follower's
  acknowledgement, so the cost to the member is the length of the shorter of the two trips. On this
  machine an order's round trip was about 98 microseconds, of which about 84 was four crossings between
  processes, about 21 each; the record's trip to the follower and back is two crossings, which gave an
  estimate of about 40 microseconds more per order at the median. Measured, it is about 19.5
  microseconds at the median on one machine (section 7). Across two machines the network adds to it.
- *What it simplifies.* A report can be forwarded as soon as it arrives, because the order it answers
  is already on both machines. The table of reports waiting for an acknowledgement (`pending_er_`)
  is then needed only for the reports that already wait on their own record.

**Option B, not chosen: the new leader learns from the engine what it holds.** The leader goes on
sending to the engine first. The engine keeps the last few thousand commands it applied. On taking the lead, the new
leader tells the engine the highest record it holds, the engine sends back every command it applied
after that, and the new leader appends them under their original numbers before it sequences
anything. To keep the engine's store bounded, the leader stops sending to the engine while more
commands than the store holds are waiting for the follower's acknowledgement.

- *What it guarantees.* G2 holds after the new leader has caught up with the engine, and there is no
  cost on the ordinary path.
- *What it costs.* Two sources of truth during recovery: the log, and the engine's store, where option
  A has one. The difficult case is the leading sequencer and the leading matching engine failing
  together. The promoted engine must then answer from what its replicated book shows, and a command
  that was applied but did not change the book, such as a rejected one, leaves no trace there. It
  also makes 4.5 harder: the old leader's extra records can no longer simply be discarded, because
  the engine may have acted on them.

**Option C, rejected: leave the order of sends as it is and make the engine check numbers only.** The
check in 4.1 makes the fault visible, but the engine would still hold orders no log holds, so G2
would not hold.

**Chosen: option A**, at its measured cost (section 7).

### 4.3 Commands sent during the change of leader (G1)

With option A in place, a command is in one of three states when the leader dies: held by both
sequencers and acted on; held only by the old leader and not acted on; or held by neither. The last
two must reach the new leader.

**Option A, chosen and built: the gateway keeps each command until it is answered, and sends the
unanswered ones again to the new leader.**

- The gateway keeps a copy of every command it has sent to the sequencers until the report that
  answers it arrives. The copies live in a store of fixed size, set in the gateway's configuration and
  allocated when the gateway starts, so nothing on the order path allocates memory. If the store is
  full, the gateway refuses the command with a reply saying the venue is busy.
- The gateway learns that a new instance leads from the `OrderAcceptance` message every new leader
  sends to every gateway on taking the lead, which carries the leader's epoch. A gateway
  that sees a higher epoch than the last one it saw sends every command it is still holding to the new
  leader, in the order they were first sent, each marked as sent again.
- The new leader checks each command marked as sent again against a record of the identifiers in its
  log. A command it does not hold is sequenced as a new one. A command it already holds is not
  sequenced again; its answer comes from the engine by one of the routes in section 3.6 of
  [commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md).
- Cancels are handled the same way, by the cancel's own `ClOrdID`.

Costs: memory in each gateway for the commands in flight, which is normally a handful per session;
one field on `OrderAcceptance` and one on the envelope; and the day's identifier record in the
sequencer (section 7).

**Option B: the follower keeps the copies it discards, and appends those its log does not hold when
it takes the lead.** It is cheaper, but it loses every command the follower never received, for
example while the gateway's connection to it was down, and it needs the same duplicate check. It does
nothing for reports. It is not enough on its own, and with option A in place it adds nothing.

**Option C, rejected: the gateway refuses, with a reply, every command it sent while it knew of no
leader.** The gateway cannot know whether the old leader logged and replicated a command before it
died. If it did, the command is live, and the member would have been told it was refused.

**Option D: rely on the member.** A member that has had no answer can resubmit under the original
`ClOrdID` (R-0003), and the venue refuses the repeat of an order it has committed (R-0006). This is
already what the specification describes, and it remains the path after a gateway dies, because a
gateway's store dies with it. As the answer to a change of sequencer leader it leaves the member to
notice the silence and act on its own timeout, for a failure the venue knows about and the member
does not.

**Chosen: option A.** Its detailed design, and what was decided, is
[commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md); `ha_test.py` scenarios
66, 67 and 68 check it.

### 4.4 Reports sent during the change of leader (G3)

**Option A, chosen and built: the follower keeps the reports the engine sends it, and the new leader
forwards them.** The follower keeps each report it receives from the engine, in a store of fixed size,
for at least one lease period plus the drift allowance. On taking the lead, it forwards every report it
is keeping to the member's gateway, each marked as a possible repeat, because it cannot tell which of
them the old leader forwarded before it died. It then empties the store. A member may receive a few
reports twice across a change of leader, each repeat marked, which R-0122 allows.

This covers every report, including those not prompted by a command sent during the change of
leader, such as a fill caused by another member's order once the venue matches. It adds nothing to
the ordinary path.

**Option B: forward a report only once the follower holds the report's own record.** The follower's
log would then show exactly which reports were forwarded, and the new leader would know which ones
were not. That is precise, but it adds another trip to the follower and back to every report, about
40 microseconds more on top of 4.2.

**Option C: rely on 4.3 alone.** Sending commands again under 4.3 recovers a command that no log
holds, but not the report of a command the engine has already acted on, nor reports that answer no
such command.

**Chosen: option A.** A report that reached neither sequencer, for example because the engine's
connection to the follower was down when the leader died, is recovered only by an order status
enquiry (R-0002), which the venue does not answer ([BUG-0089](../bug_list.md#bug_0089)).

How it works:

- **What is kept.** A sequencer that is not leading keeps a copy of every report the matching engine
  sends it, exactly as the engine sent it, in `KeptReportStore`. The copies are written into a block of
  128 MiB, and a list of where each copy lies holds up to 262,144 of them. Both are allocated when the
  sequencer starts, so keeping a report allocates nothing. When a leader dies, its follower holds the
  reports of up to one renewal interval plus one lease period plus the drift allowance, 4.25 seconds
  with the usual timings. At the highest rate measured on this venue, about 34,000 orders a second, and
  taking one and a half reports an order, that is about 212,000 reports, which the store holds. The
  block allows an average of 512 bytes a report; the acceptances scenario 65 forwards are 182 bytes
  each, envelope included.
- **How long a report is kept.** A report is discarded once it arrived more than one lease period plus
  the drift allowance before the follower last granted its leader's request to lead. A leader that has
  just been granted a lease leads for at least a lease period, and forwards each report within a small
  fraction of a second, so a report that old has been forwarded. The age is measured against the last
  grant and not against the clock, and that is what makes it safe. When the leader dies, it stops
  asking, so nothing more ages, however long the change of leader then takes: a voter that is slow to
  answer, or an arbiter that is down, cannot make the follower discard a report the old leader never
  forwarded.
- **Which reports are sent again.** Discarding by age alone keeps every report of the last few
  seconds, almost all of which the old leader had forwarded, so a member would be sent them all again.
  So the matching engine numbers its reports: each carries the epoch of the engine leadership that sent
  it and its number within that leadership. Every record the leader writes carries the position up to
  which it has forwarded every report, stopping just before any report still waiting for an
  acknowledgement. The follower reads those positions from its log, with the time the leader wrote each
  record, and discards the kept reports covered by the position on records written at least
  100 milliseconds before the latest record it holds; both times are by the leader's clock. The delay
  is there because "forwarded" means handed to the connection to the gateway, not received there: a
  report still in the old leader's send buffers when its machine or network failed would be lost if
  the follower discarded it at once. `ha_test.py` scenario 69 checks it: after 1,000 orders and one
  more half a second later, the new leader discarded 1,999 kept reports and forwarded 2 again, where
  it forwarded 2,001 before ([BUG-0116](../bug_list.md#bug_0116)).
- **When the store is full,** the oldest copies are overwritten. A copy overwritten while it may still
  have been needed is a report that may never reach its member, so the sequencer logs a Warning when
  that happens and keeps the total in the metric `sequencer_kept_reports_lost`, which is expected to
  be 0.
- **On taking the lead,** the sequencer forwards every copy it holds, oldest first, through the same
  code that forwards a report the engine has just sent, each marked as a possible repeat. Each is given
  its own record in the new leader's log, as any report is, so the log and the `execution_reports`
  topic may also hold a report twice across a change of leader, the repeat marked. A forwarded report
  is routed by the member's identity, which the matching engine puts on the envelope of every report
  it sends, so the new leader does not need the old leader's record of which session sent which
  order. It then empties the store.

`ha_test.py` scenario 65 checks it.

### 4.5 An instance rejoining as a follower (G5, BUG-0097)

The design in full, which also covers a follower that has merely restarted, is
[follower_log_repair.md](follower_log_repair.md). The outline:

Each record gains the epoch of the leader that wrote it. When an instance connects to a leader as a
follower, it sends the sequence number and epoch of its last record. The leader replies with the
highest sequence number at which the two logs agree: the last record they both hold under the same
number and epoch. The follower discards everything after that point and receives the leader's records
from there. This is how Raft repairs a follower's log, and it is not new here.

Discarding is safe only with option A of 4.2. Under that option, a record the old leader holds and the
new leader does not was never acted on by the engine and never answered to a member, and the gateway
sent the command again to the new leader under 4.3. Nothing is lost by discarding it.

### 4.6 What is committed and what is applied, in Raft's terms

Raft, the consensus algorithm most of this design follows, keeps two numbers for a replicated log. The
**commit index** is the last record known to be held by a majority, and so safe to act on. The
**applied index** is the last record the state machine has acted on. The sequencer keeps neither under
those names, and stores neither on disk. This is what stands in for each.

**When an order may be acted on (Raft's commit).** There are three voters for the sequencer pair, the
two instances and the arbiter, so a majority is the leader and one other. The leader has the matching
engine act on an order in one of two cases:

- **The follower holds it.** The follower has acknowledged the order's record: it is at or below
  `peer_acked_through_` (`SequencerThread.hpp`), the highest record the follower has acknowledged. The
  follower's log is repaired when it rejoins ([follower_log_repair.md](follower_log_repair.md)), so
  "acknowledged through N" means the follower holds exactly the leader's records 1 to N.
  `release_held_orders_through()` sends the orders an acknowledgement covers. This is option A of 4.2.
- **The follower does not, and a voter has recorded that it may not lead.** When no follower keeps up,
  the leader acts on an order its follower lacks only once a voter other than itself has echoed its
  statement that the follower may not lead
  ([a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md), rule 11). The majority
  behind the decision is then the leader, which holds the record, and a voter that holds the statement
  instead of the record. What Raft guarantees with its commit index, that a new leader holds every
  committed record, this guarantees by keeping any instance that lacks one from being elected.

A report is forwarded to its member on the same terms: once the follower holds the record it waits on,
or once acting without the follower has been confirmed.

**What has been acted on (Raft's applied).** `released_through()` (`SequencerThread.cpp`) is the last
order the leader has sent to the matching engine: every order at or below it has been sent, or deferred
because no engine was connected, and none above it. Orders above it are waiting, for the follower's
acknowledgement or a voter's confirmation, in the storage for held orders or, once that is full, in the
log. The matching engine knows its own position, the last record it applied: an engine that is starting
reads it from the open-order region it recovers, and one taking the lead from the replica it maintained
while following. It gives that position when it asks to catch up. The leader's catch-up sends it records up to
`released_through()` and no further, so that nothing waiting is acted on early.

**After a change of leader.** A new leader does not work out either number from its peer. It treats its
whole log as safe to act on, and numbers new records from the highest record it holds plus one (4.1).
That is sound because of the two guarantees above: the engine acted only on records the follower held,
or with the follower barred from leading, so the instance that takes the lead holds every record the
engine acted on. Its log may hold more: records the old leader had replicated but not yet sent to the
engine. Both instances hold those, which is a majority, so they are safe to act on, and the engine acts
on them when it catches up from the new leader's log, from its own position. Records an old leader held
that its new leader never had were acted on by nobody, and the old leader discards them when it rejoins
([follower_log_repair.md](follower_log_repair.md)).

## 5. A change of leader under this design

1. The leading sequencer dies. Some commands are in its log and acknowledged by the follower; the
   engine has acted on those. Some are in its log only; the engine has not seen them. Some reports
   have reached the follower from the engine and not been forwarded to members.
2. During the change of leader, gateways go on sending commands to both sequencers. The follower
   discards them, and the gateways keep them because nothing has answered them. The follower keeps
   every report the engine sends it.
3. The old leader's lease runs out and the follower takes the lead. It numbers from the highest record
   it holds plus one (4.1).
4. It forwards every report it kept, marked as possible repeats (4.4).
5. It sends `OrderAcceptance`, carrying its epoch, to every gateway.
6. Each gateway sends again every command it still holds unanswered, in order, marked as sent again.
7. For each command sent again, the new leader checks the day's identifier record. A command it does
   not hold is sequenced, replicated, and sent to the engine once the new follower acknowledges it, or
   at once, under the rule for a leader without a follower, if no follower is connected. A command it
   holds is not sequenced again; its answer comes by one of the routes in section 3.6 of
   [commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md).
8. Every command is now answered once, or answered and repeated with the repeat marked.
9. When the old leader restarts and rejoins as a follower, it discards the records its new leader does
   not hold and takes the new leader's records from the point where the two logs agree (4.5).

## 6. What it costs

| Part | Cost on the ordinary path | Other cost |
|---|---|---|
| 4.1 numbering | None | None |
| 4.2 option A | About 19.5 microseconds per order at the median on one machine, measured (section 7) | Simpler report handling |
| 4.3 option A | A copy of each command in flight, in the gateway; an insert into the record of identifiers in the sequencer | A fixed store per gateway; a field on `OrderAcceptance` and two on the envelope; the record of identifiers, 4 GiB reserved in each sequencer; a position query from a new leader to the engine |
| 4.4 option A | None | A fixed store of reports in the follower |
| 4.5 | None | One field on each record; a short exchange when a follower connects |

The venue has no wire compatibility to keep before release 1.0.0, so each new field is added as a new
field.

## 7. Decisions, and open questions

### Decided

- **Option A of 4.2 is chosen, at its measured cost.** Measured by the method in
  `docs/operations/latency_findings.md`, three runs of each after one discarded, with no change of
  leader in any run, on one machine:

  | | Median | 90th percentile | 99th percentile |
  |---|---|---|---|
  | Sending to the engine at once | 105.0 us | 115.0 us | 141.9 us |
  | Option A | 124.4 us | 144.0 us | 174.8 us |
  | Added | about 19.5 us | about 29 us | about 33 us |

  The estimate in 4.2 was about 40 microseconds at the median; the measured cost is half that, and
  acceptable. Across two machines the network adds to it. The orders a leader holds until the follower
  acknowledges them are kept in storage of fixed size, allocated when the sequencer starts, so holding
  an order allocates no memory.
- **A follower that falls too far behind is treated as gone, and the leader runs as if alone.** A
  fault in the follower is a loss of resilience, not a loss of service: while the leader is working,
  it goes on offering the service. *Considered and rejected: refusing new orders until the follower
  catches up,* which keeps the guarantee of 4.2 unconditional but turns a fault in the backup into an
  outage, and would make a follower that is alive but stalled more dangerous than one that has died,
  since a dead follower's connection drops and the leader already runs alone then. Running alone with
  a follower that is behind carries no more risk than running with no follower, which the venue
  already accepts. Five things go with it:
  1. **The trigger.** The leader declares the follower behind when its storage for held orders is
     full, or when no acknowledgement has arrived for 100 milliseconds while it holds orders, so that a
     follower that stops answering is noticed quickly rather than after thousands of orders. Both
     limits are constants in the code, with their reasoning beside them.
  2. **A clean switch.** The orders it holds are sent to the matching engine in sequence order, new
     orders go to the engine at once, and reports stop waiting for the follower's acknowledgement, as
     when the follower disconnects.
  3. **A clean return.** When the follower has acknowledged every record the leader has written, the
     leader goes back to waiting for its acknowledgements.
  4. **Reporting.** A Warning when the leader starts running alone, saying why and how far behind the
     follower is; one line when it stops, saying for how long; and a metric an operator can alert on.
     Not a line per order. That this is not yet loud enough, and misses the case of no follower being
     connected, is [BUG-0134](../bug_list.md#bug_0134).
  5. **A follower that is behind must not take the lead.** Running alone happens more often under this
     decision; open question 2 below is how it is prevented.
- **The venue plans for 50 million orders a day, and is tested with 100 million.** That sizes the
  day's identifier record that 4.3 depends on (R-0119). Held as a 128-bit hash of the comp id and the
  `ClOrdID`, 50 million identifiers are 800 MB of hashes, and roughly 1.6 GB once a hash table's own
  overhead is included; the 100 million test is roughly 3.2 GB. That is acceptable only if it is
  planned for. The form the record takes is described in
  [commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md). The ways to make it smaller
  are a table per session keyed on a 64-bit hash of the `ClOrdID` alone, which halves it at the cost
  of a small chance of refusing a genuine order as a repeat, which must then be stated; and holding it
  in a table that grows without stalling the thread that owns it, such as `IncrementalRehashMap`. The
  record must also be rebuilt from the log at start, and how long that takes at 50 million records
  is to be measured. The order book's own growth at this volume is
  [BUG-0028](../bug_list.md#bug_0028).

### Open

1. **The leading sequencer and the leading matching engine failing together.** The promoted engine
   catches up from the new leader's log. A command
   the old leader held alone was never applied by either engine, and the gateway sends it again. This
   needs a scenario before it is relied on.
2. **A leader running alone. Answered and built.** While no follower is connected, or the follower is
   too far behind, the leader acts on commands its follower lacks, so if it dies the instance that
   takes over has an older log. Rule 11 of [majority_leases.md](majority_leases.md) keeps that
   instance from leading: the leader acts on such commands only once a voter has recorded that its
   follower may not lead. The design is [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md),
   and `ha_test.py` scenarios 63 and 64 check it.
3. **A gateway that dies during the change of leader.** Its store dies with it. Its members recover by
   resubmitting (R-0003), which depends on the duplicate check.

## 8. Tests

| Test | What it requires |
|---|---|
| Scenario 59 | The engine holds no order the new leader's log does not hold |
| The numbering checks in scenarios 1 and 59 | After the change of leader, the numbers in the new leader's log only go forward |
| Scenario 60 | A follower that stalls does not stop trading: the leader runs as if alone |
| Scenarios 61 and 62 | A follower that restarts is sent the records it missed; an old leader holding records the new leader does not ends with a log identical to the new leader's |
| Scenarios 63 and 64 | A follower that is behind does not take the lead, also with the active arbiter restarted before the leader dies |
| Scenario 65 | With the leader's sends to the gateways blocked by `libblock_sends_to_ports.so` and then the leader killed, every report reaches its member, repeats marked; it fails with the new leader's forwarding switched off |
| Scenarios 66, 67 and 68 | A command the new leader's log holds, which the engine never received, is applied; a command sent again is sequenced only if the log does not already hold it; every order sent during the change of leader is answered, and none twice |
| Latency | The order round trip with option A of 4.2, measured by the method in `docs/operations/latency_findings.md` (section 7) |

## 9. Where each part is built

| Part | Where | Bug |
|---|---|---|
| 4.1 numbering, and the engine's guard | `SequencerThread` on taking the lead; `MatchingEngineThread::sequence_goes_backwards` | [BUG-0105](../bug_list.md#bug_0105) |
| 4.2 option A, and running as if alone | `SequencerThread::hold_until_acknowledged`, `release_if_confirmed`, `start_running_alone` | [BUG-0103](../bug_list.md#bug_0103) |
| Open question 2, a follower that is behind does not lead | The lease classes, the lease agent, the arbiter, and `SequencerThread::may_act_without_follower` ([a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md)) | |
| 4.3 the gateway keeping commands, and the record of identifiers | `UnansweredCommandStore` in each gateway; `LoggedCommandIdentifiers` in the sequencer ([commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md)) | [BUG-0103](../bug_list.md#bug_0103), [BUG-0115](../bug_list.md#bug_0115) |
| 4.4 the follower keeping reports | `KeptReportStore` | [BUG-0103](../bug_list.md#bug_0103) |
| 4.5 the rejoin | [follower_log_repair.md](follower_log_repair.md) | [BUG-0097](../bug_list.md#bug_0097) |

No request from the sequencer to the engine for an order's state is needed: section 3.6 of
[commands_during_a_change_of_leader.md](commands_during_a_change_of_leader.md) says why. The gap the
specification records under R-0119, a `ClOrdID` reused once its first order has ended, is
[BUG-0114](../bug_list.md#bug_0114), to be designed with [BUG-0122](../bug_list.md#bug_0122).

Related: [wal_and_ha.md](wal_and_ha.md), [majority_leases.md](majority_leases.md),
[order_acceptance.md](order_acceptance.md), [tla/findings.md](tla/findings.md), and the requirements
R-0002, R-0003, R-0006, R-0010, R-0119 and R-0122 in the functional specification.
