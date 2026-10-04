# A follower that is behind must not take the lead {#a_follower_behind_does_not_lead}

## 1. What this document covers

The sequencer's leader sends a command to the matching engine only after its follower has acknowledged
the command's record, so that the follower always holds every command the engine has acted on
([change_of_sequencer_leader.md](change_of_sequencer_leader.md), section 4.2, option A). There are two
situations in which the leader does not wait, and sends commands to the engine at once:

- **No follower is connected.** The follower has died, has not started yet, or the link to it is down.
- **The follower has fallen behind.** The leader's storage for commands waiting on the follower is full,
  or no acknowledgement has arrived for 100 milliseconds while commands were waiting. The leader then
  runs as if it had no follower, and the gauge `sequencer_running_alone` is 1.

In both situations the leader's log holds commands that the engine has acted on, that members may have
been sent reports for, and that the follower's log does not hold. If the leader dies at that moment, the
follower can still be granted the lead today. A voter granting a lease does not know whether the
instance asking holds everything the engine has acted on. The new leader would then number new commands
from a point below commands the engine has already applied. The engine's guard refuses commands whose
sequence numbers do not go forward, so the venue would stop placing orders. And the commands only the
dead leader held would be missing from the surviving record of the day, even though members have
reports for them.

This document states what must hold, explains why the usual rule from the literature does not fit this
venue, sets out the design and the decisions taken on it, and gives the tests. It proposes no
code until the design is agreed.

## 2. What must hold

**G6. An instance leads only if its log holds every command the matching engine has acted on.**

G6 holds whenever the leader is waiting for its follower's acknowledgements, because the engine then
acts only on what the follower holds. G6 is at risk only while the leader is sending to the engine
without waiting. This design is about that interval.

## 3. Why the usual rule does not fit

Raft, the consensus algorithm most often used for replicated logs, prevents a stale instance from leading
with one rule: **a voter refuses to vote for a candidate whose log is less up to date than its own.** It
works because in Raft every voter holds a log, and a command counts as committed only once a majority
of voters hold it. Any majority that elects a new leader then includes at least one voter that holds
every committed command, and that voter refuses a candidate lacking one.

That rule cannot be used here as it stands, for two reasons.

**The arbiter holds no log.** Of the three voters for a sequencer pair, only the two sequencer instances
hold a log. The arbiter has nothing to compare a candidate's log against. If the leader dies, the
follower asks to lead and needs one vote besides its own. The only other voter alive is the arbiter.
So the voter that must refuse is exactly the one that cannot compare.

**A longer log is not the test that matters here.** When the leader is waiting for acknowledgements, it
writes each command to its own log before the follower has it. A leader that dies at that moment holds a
few records more than the follower, and the engine acted on none of them. The follower holds every
command that was acted on, so it is perfectly fit to lead. When the old leader restarts and is asked for
its vote, Raft's rule would have it refuse the follower because the follower's log is shorter. That would
stop the venue for no reason. What matters is not which log is longer, but whether the candidate holds
everything the engine acted on, and only the leader knows when that stopped being true.

## 4. The design

### 4.1 The idea in one sentence

**Before the leader acts on a command its follower does not hold, it gets a second voter to record that
the follower may not lead, and that record stays until the follower holds everything again.**

This keeps the reasoning that makes Raft safe: any majority that could elect the follower must contain a
voter that knows the follower may not lead. The majorities that could elect the follower are the
follower with the arbiter, and the follower with the old leader:

- **The follower with the old leader.** While the old leader is leading, it does not vote for its peer
  (rule 4 of [majority_leases.md](majority_leases.md)). But once its lease has run out, whether its
  process is still running or has died and restarted, its peer's request to lead can reach it before it
  has asked to lead again, and an instance that knew nothing would grant it. So the leader is a voter
  that knows, like the other two: it writes each statement to its own record on disk before it sends
  it, and while its record says its peer may not lead, it refuses that peer's requests to lead. The
  model check in section 12 of [tla/findings.md](tla/findings.md) finds the follower elected without
  this, even with no crash.
- **The follower with the arbiter.** Either the follower knows it may not lead and does not ask, or the
  arbiter knows and refuses. One of the two knowing is enough.

So the leader needs its own record on disk and one confirmation, from either the follower or the
arbiter, before it acts on a command the follower lacks.

### 4.2 How the statement travels: on the lease requests the leader already sends

The leader already asks both other voters to renew its lease every second (rule 3), and it holds a lease
only while at least one of them grants. **Whichever voter is granting the leader's lease is reachable,**
so it is the natural voter to record the statement. This design adds the statement to the lease
request rather than adding a new exchange.

Each `LeaseRequest` from a leader gains two fields:

- **Whether its peer may lead.** "No" from the moment the leader takes the lead, and for as long as it
  sends to the engine without waiting. "Yes" only once the follower has acknowledged every record the
  leader holds and the leader has gone back to waiting for acknowledgements (4.3).
- **A number for that statement,** which goes up by one every time the leader changes it. A voter keeps
  only the statement with the highest number it has seen from that group at that epoch or a later one,
  so a delayed message cannot undo a newer statement. Without it, an old "yes" arriving late, after a
  newer "no", would wrongly let the follower lead again.

Each `LeaseGrant` gains one field: the number of the statement the voter recorded before granting. The
leader treats a statement as recorded only when a grant echoes its number.

A voter records a "no" before it sends the grant, exactly as it records a promise before granting, so
that the statement survives the voter's own restart:

- **The leader** writes its own statement to its record on disk before it sends it, in the same file
  as its lease promise record. An instance whose record says its peer may not lead refuses that peer's
  requests to lead, whether it has restarted or its lease has simply run out.
- **The follower** records it in memory at once and grants, and writes it to disk and syncs it on its
  background thread, in the same way as its lease promise record (`BackgroundPromiseRecorder` and
  `LeasePromiseStore`). It does not wait for the disk before granting, for the reason given in 4.5.
  While it holds the statement, in memory or on disk, it does not ask to lead, and it grants its peer's
  requests to lead.
- **The arbiter** writes it to disk, and sends it to the passive arbiter in the same way it already
  sends the highest epoch granted in each group (`ArbiterStateRecord`). It refuses a lease to an
  instance its record says may not lead, with a refusal reason that says so, and the instance logs
  that reason.

### 4.3 What the leader does

**Starting to act without the follower.** When the leader would start sending to the engine without
waiting, because the follower has disconnected or has fallen behind, it first sends a lease request
saying its peer may not lead, to both voters, at once rather than at the next renewal. It goes on holding
commands until a grant echoes that statement. Only then does it send the held commands to the engine and
stop waiting for acknowledgements. The delay is one round trip to the nearer voter plus that voter's
synced write: normally a few milliseconds.

While it waits for the confirmation, the leader goes on logging commands and holding them. If its
storage for held commands fills before the confirmation arrives, it stops reading new commands from its
gateway connections until the confirmation arrives. The commands wait in the connections, and nothing is
lost or refused. This is a rare fallback: the storage holds 16,384 commands, which lasts about a third of
a second at 50,000 orders a second, and a confirmation normally takes a few milliseconds.

Stopping and restarting reads needs two things the framework does not yet have:

- **A way for an application thread to ask for it.** The reactor can already stop watching a socket for
  incoming data and start again (`InboundConnectionManager`), but only a raw-bytes connection uses
  this, when its own buffer fills. Two new reactor commands are needed, one to stop reading a connection
  and one to start again, and the sequencer's gateway connections, which carry framework messages, must
  honour them.
- **Room for what is already on its way.** Stopping reads does not stop messages already read, which are
  already queued for the sequencer's thread, and that thread cannot stop taking from its queue, because
  lease messages arrive on the same queue. So the leader stops reading when its storage passes a high
  mark, leaving room for those, and starts again when it falls below a lower mark.

A long stop has a cost beyond the sequencer. A gateway whose sends to the sequencer cannot complete stops
sending anything at all, to members as well as to both sequencers, once the kernel's buffers for that
connection are full ([BUG-0112](../bug_list.md#bug_0112)). A stop of a few milliseconds is absorbed by
those buffers; a long one freezes every gateway.

**Every leadership starts with "no".** A leader does not know, when it takes the lead, whether its
follower holds every record it holds: its own log may hold records it wrote in an earlier leadership
that the follower never received. So every leadership starts by saying that the peer may not lead, and
says "yes" only once the follower has acknowledged everything. A leadership that started with "yes"
would replace the leader's own record of an earlier "no" while the follower still lacked commands the
engine had acted on, and the model check finds the follower elected that way. At start of day this
means a secondary that starts later cannot lead until it has caught up.

**Going back to waiting.** When the follower has acknowledged every record the leader has written, the
leader first goes back to waiting for acknowledgements, and only then sends a request saying its peer may
lead. The order matters. From the moment the leader goes back to waiting, the follower holds everything
the engine has acted on, so a "yes" sent after that moment is true when it arrives.

**While running without the follower.** Every renewal goes on saying the peer may not lead. A voter that
restarted and lost what it had recorded learns it again within one renewal interval. A voter that
recorded it on disk loses nothing.

### 4.4 When the leader dies while acting without the follower

The follower may not lead, and the arbiter refuses it if it asks. **Trading stops until the old leader
returns.** This is the same outcome as a leader with no follower at all dying, which the venue already
accepts, and it is the honest outcome: the only complete record of the day is on the dead leader's disk.

The old leader returns in one of two ways:

- **Its supervisor restarts its process.** It reads its log and its promise record, asks to lead, and is
  granted: the follower grants it because the follower may not lead, and the arbiter grants it because
  it is not the instance the arbiter's record names. The follower then catches up from it.
- **Its machine is lost.** Nothing in the venue can recover the commands only that machine held. The
  operator, who knows the machine has died, decides to let the follower lead, accepting that those
  commands, which members have reports for, are missing from the record. An operator's tool clears the
  statement at the arbiter and at the follower, and logs at Warning what it has done and why. The tool,
  and the procedure for reconciling what members were told with what the log holds, are designed
  separately.

### 4.5 When the follower's disk has stopped answering

The follower's statement reaches its disk in the background, so a follower whose disk has stopped
answering still records the statement in memory and grants at once. The leader goes on trading without
a pause. The statement is then held only by the follower's running process until the disk write
completes.

The commands only the leader holds could be lost only if all of these happened while the statement was
held in the follower's memory alone, and before the arbiter had recorded it: the leader acts on the
follower's grant; the leader stops acting, because it dies or its lease runs out; the follower's process
restarts, losing what it held in memory; and the arbiter, which never recorded the statement, elects the
follower. Section 12.5 of [tla/findings.md](tla/findings.md) gives this sequence as found by the model
check. When the arbiter is reachable it records the statement within milliseconds, so the window is that
short. When the arbiter pool is unreachable, the window lasts until the follower's disk write completes,
and that is short too, because the lease rules already end this situation within seconds. A follower must write each promise to disk before it gives the grant that
carries it (rule 6 of [majority_leases.md](majority_leases.md)), and its promise record covers promises
for only about ten seconds ahead. When that runs out, a follower whose disk has stopped answering can no
longer grant renewals. With the arbiter pool also unreachable, the leader then holds no lease, and it
stops leading under rule 2. That halt is caused by the lease rules, not by this design, and it needs two
faults at once.

The alternative, the leader holding every command until the follower's disk confirms the statement, was
considered and rejected. It turns a fault in the backup into a pause in trading, and it guards only
against the combination of four events above. A fault in the backup costs resilience, not service.

The leader waits for a confirmation only when no voter answers at all. In that case it holds no lease
from anyone and has already stopped leading, so the wait costs nothing more.

### 4.6 Where this does not apply

**The matching engine pair and the publisher pair** do not need it. Their state is derived from the
sequencer's log, and an engine that takes the lead catches up from the sequencer before it acts.

**The two arbiters** do not need it. They hold no log.

## 5. Alternatives considered

**Raft's rule alone, comparing logs at the peer's vote.** Rejected for the two reasons in section 3: the
arbiter cannot compare, and a longer log is the wrong test once commands are held until acknowledged.

**The arbiter learns each instance's log position from the lease requests, and refuses a candidate whose
position is below the leader's.** Rejected. Renewals go every second, so the arbiter's knowledge is up to
a second old, and a leader that dies just after it starts acting without the follower leaves the arbiter
thinking the follower is fit. Closing that gap means confirming before acting, which is this design with
extra comparisons in it.

**Record the statement at the arbiter only.** Simpler: the follower keeps no record and needs no new
behaviour. The cost is that a leader whose lease comes from its follower alone, because the arbiter pool
is down, can never act without the follower, even when the follower could have recorded the statement.
Recording at whichever voter grants gives the leader the best chance of carrying on, for little more
code, because both voters are already `LeaseVoter`s and the change sits in one place.

**Do nothing, and accept the risk.** Rejected. The risk is not small: running without the follower is
what the leader does every time its follower restarts, and at every start of day until the secondary
has caught up.

## 6. Decisions

1. **The follower records the statement in memory and grants at once, and the leader does not wait for
   the follower's disk** (4.2 and 4.5). Holding commands until the disk confirms was considered and
   rejected, because it turns a fault in the backup into a pause in trading.
2. **An operator's tool lets the follower lead after the old leader's machine is lost** (4.4), accepting
   the loss of the commands only that machine held. It is designed separately, with the procedure for
   reconciling members' reports.

## 7. Tests

Each test must fail on today's code. That is shown, not assumed, before it is used to judge the change.

| Test | What it requires | Today |
|---|---|---|
| Scenario, new: follower stopped | The follower is stopped with SIGSTOP. The leader runs without it and has orders placed. The leader is killed and the follower resumed. The follower must not lead. The old leader is restarted, leads, and its log holds every order that was placed | Expected to fail: the follower takes the lead |
| Scenario, new: arbiters down | As above, but with the arbiters stopped, and the follower made to fall behind by blocking its acknowledgements with `libblock_sends_to_ports.so`, so that its lease handling still answers. The follower records the statement, and must not lead | Expected to fail |
| Scenario, new: caught up again | The follower falls behind, catches up, and the leader goes back to waiting. The leader is then killed, and the follower must take the lead normally | Passes today; it guards against the change blocking a healthy failover |
| Scenario, new: arbiter restarted | As the first, but the active arbiter is restarted after the statement is recorded and before the leader is killed. The follower must still not lead | Expected to fail |
| Unit tests | In `LeaseRulesTest.cpp`: a voter records a "no" before it grants, keeps the highest numbered statement, refuses the named instance, and a follower that may not lead does not ask and grants its peer | Not yet written |
| Simulation | `LeaseSimulationTest.cpp` gains logs and the engine's set of acted-on commands, and checks after every step that no instance leads without every acted-on command. A run with the recording step removed must find a violation, which shows the check can fail | Not yet written |
| TLA+ | `FollowerBehindHA.tla` checks that the instance acting as leader holds every command the engine has acted on, and each part of the rule is removed in turn to show it is needed | Done: section 12 of [tla/findings.md](tla/findings.md). Each counterexample is rerun on every install |

## 8. Order of the work

1. Agreement to the design.
2. The TLA+ model, done in section 12 of [tla/findings.md](tla/findings.md), and the simulation, each
   shown to find the fault when a part of the rule is removed.
3. The new fields on `LeaseRequest` and `LeaseGrant`, the follower's and the arbiter's records, and the
   arbiter's copy to the passive arbiter, with the unit tests.
4. The sequencer's switch: confirm before acting, the order of going back to waiting, and stopping reading
   from gateway connections while waiting.
5. The new scenarios.
6. Rules added to [majority_leases.md](majority_leases.md), and open question 2 of
   [change_of_sequencer_leader.md](change_of_sequencer_leader.md) marked as answered here.

Related: [majority_leases.md](majority_leases.md), [change_of_sequencer_leader.md](change_of_sequencer_leader.md),
[tla/findings.md](tla/findings.md).
