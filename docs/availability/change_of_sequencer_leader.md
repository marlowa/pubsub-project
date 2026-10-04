# Keeping every order and report across a change of sequencer leader {#change_of_sequencer_leader}

## 1. What this document covers

The venue runs two sequencer instances. At any moment one of them leads, holding a lease granted by
a majority of three voters, and the other follows. When the leader dies, the follower takes the lead
once the old leader's lease has run out. With the three-second lease the venue uses, that took 2.6,
2.9 and 4.2 seconds in the runs recorded in the bug list. This document calls that interval, from the leader's death to the follower
taking the lead, **the change of leader**.

Four defects lose or corrupt what the venue holds across a change of leader. Each is recorded in the
bug list, and three of them have been measured:

| Bug | What goes wrong | How it is known |
|---|---|---|
| [BUG-0103](../bug_list.md#bug_0103) | Orders a gateway sends during the change of leader are never placed and never answered | Measured: `ha_test.py` scenario 1, 20,000 orders, none answered |
| [BUG-0103](../bug_list.md#bug_0103) | Reports the matching engine sends during the change of leader never reach the member | Read in the code |
| [BUG-0103](../bug_list.md#bug_0103) | The matching engine holds orders that no surviving log holds | Measured: scenario 59 |
| [BUG-0105](../bug_list.md#bug_0105) | The new leader numbers new records with numbers its log already holds | Measured: the follower's log after scenario 59 |

A fifth, [BUG-0097](../bug_list.md#bug_0097), is the same problem seen from the instance that stops
leading: the records it holds that its new leader does not are never reconciled.

This document states what the venue must guarantee, sets out the ways each part could be met with
their costs, recommends one way for each, and lists the tests that must fail on today's code and
pass once the work is done. It proposes no code until the recommendation is agreed.

Sections 2 and 3 describe today's behaviour and the requirements. Section 4 takes the five parts in
turn. Section 5 walks through a change of leader under the recommended design. Sections 6 to 9 give
the costs, the decisions and open questions, the tests and the order of the work.

## 2. What happens today

### 2.1 The path of an order

1. A gateway checks a member's order, wraps it in a `WalRecord` envelope that names the member's
   session, and sends it to **both** sequencers. It keeps no copy.
2. The leading sequencer gives the order the next sequence number, appends it to its write-ahead log,
   sends it to the matching engine, and then sends the record to the follower.
3. The follower discards the copy it received from the gateway. It writes its log only from the
   records its leader sends it, so that the two logs stay identical, and it acknowledges each record
   (`WalAck`).
4. The matching engine applies the order and sends its execution report to **both** sequencers.
5. The leading sequencer gives the report a sequence number of its own, logs it and replicates it,
   and forwards the report to the member's gateway once the follower has acknowledged the
   **order's** record. The follower discards the report it received from the engine.

Two parts of this run at the same time: the order's trip through the matching engine (steps 2 to 4)
and its record's trip to the follower and back (steps 2 and 3). The member's report waits for
whichever is slower.

### 2.2 What goes wrong at a change of leader

**Orders sent during the change of leader are lost.** Between the leader's death and the follower
taking the lead, every order a gateway sends reaches only the follower, which discards it because it
is still following. Nothing else holds the order. The member is not told it was refused, so it cannot
tell it from an order still on its way.

**Reports sent during the change of leader are lost.** A report the engine sends after the leader has
died, or that the leader received and had not yet forwarded, reaches the follower, which discards it.
A sequencer that takes the lead does not ask the engine for reports it may have missed
(`SequencerThread::adopt_role`).

**The engine holds orders no surviving log holds.** Because the leader sends an order to the engine
before it sends the record to the follower, a leader that dies between the two leaves the engine
holding an order the follower never received. The engine does not check that the sequence numbers
it is sent only go forward, so it goes on accepting orders from the new leader. Scenario 59 makes the
gap between the two sends as long as it needs with a test library that stops the leader's sends to
its follower from arriving: the engine accepted three orders, the leader was killed, and the new
leader's log held none of them.

**The new leader reuses sequence numbers.** The leader numbers orders and the reports it logs from one
counter. The follower writes replicated records under the leader's numbers but moves its own counter
only for the gateway copies it discards, so it counts orders and not reports. When it takes the lead
it numbers from a point below records it already holds: in the measured case, 1,002 below. A reader
that assumes the numbers only go forward then misreads the log. The matching engine's catch-up skips
every record at or below the position it gives, and the publishers and the order activity recorder
identify a record by its number.

**The old leader's extra records are never reconciled.** When an instance that led rejoins as a
follower, it appends whatever its new leader sends after the records it already holds, with no
comparison. Records it wrote that the new leader never had stay in its log, and the new leader's
records can carry the same numbers with different contents.

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

## 4. The five parts, the ways each could be met, and a recommendation

### 4.1 Numbering: the new leader numbers from the highest record it holds (G4)

A follower moves its counter past every record it writes, whether that record came from its leader
in the ordinary stream or arrived any other way. A sequencer that takes the lead numbers from the
highest record it holds plus one.

There is no real alternative to weigh here; this part is a correction. It is independent of the rest
and is the first thing to implement.

**A guard in the matching engine belongs with 4.2, not here.** As a second line of defence, the
matching engine could refuse a live command whose sequence number is not greater than the last it
applied, and log the refusal as an error naming both numbers, so that a numbering fault showed as a
refusal rather than as a book that silently disagrees with the record. But until 4.2 is built, the
race it describes can leave the engine holding commands numbered above the new leader's last record,
and the new leader's next commands would then be numbered at or below what the engine applied. The
guard would refuse those: genuine orders, dropped with nothing sent to the member, which is worse than
the fault it guards against. Under 4.2 option A the engine never applies a command the follower does
not hold, so the case cannot arise, and the guard is added then.

### 4.2 The matching engine acts only on what the logs hold (G2)

**Option A, recommended: the leader sends a command to the engine once the follower has acknowledged
it.** The leader appends the command to its own log and sends the record to the follower as now, and
sends the command to the matching engine when the follower's acknowledgement arrives, in sequence
order. While no follower is connected, the leader sends to the engine at once, as it does now when it
is running alone.

- *What it guarantees.* The engine never acts on a command the follower does not hold, so G2 holds by
  construction. Everything the old leader holds that the follower does not was never acted on and
  never answered, which is what makes 4.5 safe.
- *What it costs.* The order's trip to the follower and its trip through the engine stop running at
  the same time and run one after the other. The member's report already waits for the follower's
  acknowledgement, so the cost to the member is the length of the shorter of the two trips. On this
  machine an order's round trip is about 98 microseconds, of which about 84 is four crossings between
  processes, about 21 each. The record's trip to the follower and back is two crossings, so the
  estimate is about 40 microseconds more per order at the median, on one machine. Across two machines
  the network adds to it. This is an estimate from measured parts, not a measurement, and the first
  step of the work is to measure it (section 8).
- *What it simplifies.* A report can be forwarded as soon as it arrives, because the order it answers
  is already on both machines. The table of reports waiting for an acknowledgement (`pending_er_`)
  is then needed only for the reports that already wait on their own record.

**Option B: the new leader learns from the engine what it holds.** The leader goes on sending to the
engine first. The engine keeps the last few thousand commands it applied. On taking the lead, the new
leader tells the engine the highest record it holds, the engine sends back every command it applied
after that, and the new leader appends them under their original numbers before it sequences
anything. To keep the engine's store bounded, the leader stops sending to the engine while more
commands than the store holds are waiting for the follower's acknowledgement.

- *What it guarantees.* G2 holds after the new leader has caught up with the engine, and there is no
  cost on the ordinary path.
- *What it costs.* Two sources of truth during recovery, where today there is one: the log, and the
  engine's store. The difficult case is the leading sequencer and the leading matching engine failing
  together. The promoted engine must then answer from what its replicated book shows, and a command
  that was applied but did not change the book, such as a rejected one, leaves no trace there. It
  also makes 4.5 harder: the old leader's extra records can no longer simply be discarded, because
  the engine may have acted on them.

**Option C, rejected: leave the order of sends as it is and make the engine check numbers only.** The
check in 4.1 makes the fault visible, but the engine would still hold orders no log holds, so G2
would not hold.

**Recommendation: option A**, subject to the measurement. If the measured cost is unacceptable, option
B is the fallback, and the double failure it raises has to be designed before it is built.

### 4.3 Commands sent during the change of leader (G1)

With option A in place, a command is in one of three states when the leader dies: held by both
sequencers and acted on; held only by the old leader and not acted on; or held by neither. The last
two must reach the new leader.

**Option A, recommended: the gateway keeps each command until it is answered, and sends the
unanswered ones again to the new leader.**

- The gateway keeps a copy of every command it has sent to the sequencers until the report that
  answers it arrives. The copies live in a store of fixed size, set in the gateway's configuration and
  allocated when the gateway starts, so nothing on the order path allocates memory. If the store is
  full, the gateway refuses the command with a reply saying the venue is busy.
- The gateway learns that a new instance leads from the `OrderAcceptance` message every new leader
  already sends to every gateway on taking the lead. The message gains the leader's epoch. A gateway
  that sees a higher epoch than the last one it saw sends every command it is still holding to the new
  leader, in the order they were first sent, each marked as sent again.
- The new leader checks each command marked as sent again against the record of identifiers used that
  day, per session, that R-0119 and R-0010 require the sequencer to keep. A command it does not hold
  is sequenced as a new one. A command it already holds is not sequenced again: the new leader asks the
  matching engine for the order's current state, and the engine answers with a report marked as a
  possible repeat (R-0122). That report also covers the case where the original report was lost at the
  change of leader.
- Cancels are handled the same way, by the cancel's own `ClOrdID`.

Costs: memory in each gateway for the commands in flight, which is normally a handful per session;
one new field on `OrderAcceptance` and one on the envelope; the day's identifier record in the
sequencer, which does not exist today (section 7); and a request from the sequencer to the engine for
an order's state, which is also what R-0002, the member's order status enquiry, needs from the engine
([BUG-0089](../bug_list.md#bug_0089)).

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

**Recommendation: option A.**

### 4.4 Reports sent during the change of leader (G3)

**Option A, recommended: the follower keeps the reports the engine sends it, and the new leader
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

**Option C: rely on 4.3 alone.** The state request in 4.3 recovers the report for every command a
gateway sends again, but not reports that answer no such command.

**Recommendation: option A, with 4.3's state request as the backstop** for a command whose report the
follower never received, for example because its connection to the engine was down.

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

## 5. A change of leader under the recommended design

1. The leading sequencer dies. Some commands are in its log and acknowledged by the follower; the
   engine has acted on those. Some are in its log only; the engine has not seen them. Some reports
   have reached the follower from the engine and not been forwarded to members.
2. During the change of leader, gateways go on sending commands to both sequencers. The follower
   discards them, as now, and the gateways keep them because nothing has answered them. The follower
   keeps every report the engine sends it.
3. The old leader's lease runs out and the follower takes the lead. It numbers from the highest record
   it holds plus one (4.1).
4. It forwards every report it kept, marked as possible repeats (4.4).
5. It sends `OrderAcceptance`, now carrying its epoch, to every gateway.
6. Each gateway sends again every command it still holds unanswered, in order, marked as sent again.
7. For each command sent again, the new leader checks the day's identifier record. A command it does
   not hold is sequenced, replicated, and sent to the engine once the new follower acknowledges it, or
   at once if no follower is connected. A command it holds is not sequenced again; the leader asks the
   engine for the order's state, and the engine reports it marked as a possible repeat.
8. Every command is now answered once, or answered and repeated with the repeat marked.
9. When the old leader restarts and rejoins as a follower, it discards the records its new leader does
   not hold and takes the new leader's records from the point where the two logs agree (4.5).

## 6. What it costs

| Part | Cost on the ordinary path | Other cost |
|---|---|---|
| 4.1 numbering | None | None |
| 4.2 option A | About 40 microseconds per order at the median on one machine, estimated; to be measured | Simpler report handling |
| 4.3 option A | A copy of each command in flight, in the gateway | A fixed store per gateway; one field on `OrderAcceptance` and one on the envelope; the day's identifier record in the sequencer; a state request from sequencer to engine |
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
  4. **Loud reporting.** A Warning when the leader starts running alone, saying why and how far behind
     the follower is; one line when it stops, saying for how long; and a metric an operator can alert
     on. Not a line per order.
  5. **A follower that is behind must not take the lead.** Running alone happens more often under this
     decision, so open question 2, a stale instance taking the lead, is the next work after 4.2.
- **The venue plans for 50 million orders a day, and is tested with 100 million.** That sizes the
  day's identifier record that 4.3 depends on (R-0119). Held as a 128-bit hash of the comp id and the
  `ClOrdID`, 50 million identifiers are 800 MB of hashes, and roughly 1.6 GB once a hash table's own
  overhead is included; the 100 million test is roughly 3.2 GB. That is acceptable only if it is
  planned for, so the form of the record is designed when 4.3 is built. The ways to make it smaller
  are a table per session keyed on a 64-bit hash of the `ClOrdID` alone, which halves it at the cost
  of a small chance of refusing a genuine order as a repeat, which must then be stated; and holding it
  in a table that grows without stalling the thread that owns it, such as `IncrementalRehashMap`. The
  record must also be rebuilt from the log at start, and how long that takes at 50 million records
  is to be measured. The order book's own growth at this volume is
  [BUG-0028](../bug_list.md#bug_0028).

### Open

1. **The leading sequencer and the leading matching engine failing together.** Under the
   recommendation, the promoted engine catches up from the new leader's log, as it does now. A command
   the old leader held alone was never applied by either engine, and the gateway sends it again. This
   needs a scenario before it is relied on.
2. **A leader running alone.** The next work after 4.2 (see the decision above). While no follower is
   connected, or the follower is too far behind, the leader sends commands to the engine at once, so
   its log alone holds them. If it then dies, the instance that takes over has an older
   log. A voter granting a lease does not compare the two instances' logs, so nothing stops the stale
   instance leading. Raft prevents this by refusing to vote for a candidate whose log is behind. That is
   a change to the lease rules (`majority_leases.md`). The design, for review, is
   [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md).
3. **A gateway that dies during the change of leader.** Its store dies with it. Its members recover by
   resubmitting (R-0003), which depends on the duplicate check, as now.

## 8. Tests

Each test must fail on today's code. That is shown, not assumed, before it is used to judge a fix.

| Test | What it requires | Today |
|---|---|---|
| Scenario 59 | The engine holds no order the new leader's log does not hold | Fails, measured; marked as expected to fail |
| Numbering, added to scenarios 1 and 59 | After the change of leader, the numbers in the new leader's log only go forward | Fails, measured by reading the log files |
| Scenario 1, strengthened | Every order sent during the change of leader is answered, accepted or refused, and none is placed twice | Fails: 20,000 sent, none answered |
| Reports, new | With the leader's sends to the gateways blocked by `libblock_sends_to_ports.so` and then the leader killed, every report reaches its member, repeats marked | Expected to fail; not yet written |
| Rejoin, new | The old leader, holding records the new leader does not, is restarted, and its log ends identical to the new leader's | Expected to fail; not yet written |
| Latency | The order round trip before and after 4.2, measured by the method in `docs/operations/latency_findings.md` | A measurement, not a pass or fail |

## 9. The order of the work

1. **4.1, numbering,** with the numbering check in scenarios 1 and 59. Small and independent, and it
   corrects a defect present at every change of leader. **Done:** [BUG-0105](../bug_list.md#bug_0105).
2. **Measure 4.2 option A** in a build that changes only that, before deciding between options A
   and B. The change is kept only if option A is chosen. **Done:** see section 7.
3. **4.2,** the chosen option, with the matching engine's guard described under 4.1. Scenario 59 then
   passes and its expected failure is removed. **Done:** option A, running as if alone when the follower
   falls behind (scenario 60), and the guard.
4. **Open question 2,** so that a follower that has fallen behind cannot take the lead. A change to the
   lease rules, designed in [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md).
   The rule itself is built into the lease classes, the lease agent and the arbiter; the sequencer does
   not yet use it.
5. **4.5,** the rejoin, designed in [follower_log_repair.md](follower_log_repair.md), with its
   scenarios. This closes BUG-0097. It comes before the sequencer uses open question 2's rule, because
   that rule lets the leader say its follower may lead again only once the follower holds every record
   the leader holds, and only 4.5 makes that knowable for a follower that has reconnected.
6. **The sequencer uses open question 2's rule:** step 4 of
   [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md) section 8.
7. **4.4,** the follower keeping reports, with the new reports scenario.
8. **4.3,** the gateway keeping commands, the day's identifier record and the state request, with
   scenario 1 strengthened. This is the largest part, and it closes the gap the specification records
   under R-0119.

Related: [wal_and_ha.md](wal_and_ha.md), [majority_leases.md](majority_leases.md),
[order_acceptance.md](order_acceptance.md), [tla/findings.md](tla/findings.md), and the requirements
R-0002, R-0003, R-0006, R-0010, R-0119 and R-0122 in the functional specification.
