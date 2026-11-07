# Checking the high availability design with TLA+: findings {#tla_findings}

This document reports what model checking finds when the venue's high availability design is specified in TLA+ and
checked with its model checker, TLC.

TLA+ is a language for describing a system's design precisely enough for a tool to check it. A specification states
what the system's state can be, which steps can change that state, and which properties must always hold. TLC then
explores every reachable state of a small version of the system and, if any sequence of steps breaks a property,
reports that sequence, which is called a counterexample. It checks the design as specified, not the running code, so
each result below holds in the code only as far as the specification matches the code; section 4 says where it does.

---

## 1. Summary

Two specifications are checked, both in this directory:

- **`MajorityLeaseHA.tla`** (section 2): the rules by which an instance of a pair leads only while a majority of three
  voters grants it a lease. It covers a component pair, whose third voter is the active arbiter, and the two arbiters,
  whose third voter is the witness. Two instances never act as leader at once, and no epoch is led by two instances,
  with crashes and restarts of every voter, link failures, and messages delayed or lost. Each rule of the design is
  needed: with any one removed, TLC finds two instances acting as leader at once, or no instance ever leading.
  Promotion of an instance on its own vote when no arbiter answers, which the design leaves out, lets two instances
  lead at once during a partition (`traces/lease-4-degraded-promotion.txt`).
- **`FollowerBehindHA.tla`** (section 3): rule 11 of the same design, which keeps an instance whose log lacks commands
  the matching engine has acted on from leading, with the arbiter tier as two arbiters of which one is active at a
  time. The instance leading always holds every command the engine has acted on, and each part of the rule is needed.

Section 4 sets the two against the venue's design principles, and says which principles no model checks.

**What remains open.** An epoch can go backwards after both arbiters restart, as section 2.5 shows. Two instances
still never act at once, and a leader learns of the higher epoch from its peer. Learning it from a receiver is not
built.

Every counterexample recorded here is rerun on every install by `scripts/tla_trace_pages.py`, and the install fails
if one stops breaking its property or no longer matches its committed trace.

---

## 2. The design the venue uses: leadership by majority, with leases

This section checks the design the venue uses to decide which instance of a pair leads, described in [../majority_leases.md](../majority_leases.md) and implemented
in the library's `PairLeaseAgent`. It is specified in `MajorityLeaseHA.tla`.

In this design, an instance may lead only while a majority of three voters has granted it a
lease that has not run out. For a component pair the voters are the two instances and the active
arbiter. For the arbiters they are the two arbiters and the witness. One specification covers both
places: voters 1 and 2 may lead, and voter 3 never does. A change of active arbiter is modelled as
voter 3 restarting, because the new active arbiter knows nothing of what the previous one promised
and must wait as a restarted voter does.

### 2.1 What the specification assumes

- **It models time.** Every promise, lease and restart wait is a count of clock ticks remaining,
  and one step of the model advances the clock by a tick. A lease design cannot be checked without
  this, because its safety argument is an argument about time.
- **Messages may take any number of ticks, or be lost.** The safety results below therefore do not
  depend on how quickly messages arrive.
- **Liveness is checked with `Prompt` on.** Whether a leader is eventually chosen does depend on
  timing, so those checks assume that every message arrives within the tick it was sent in, and that
  a leader asks for renewal in every tick. Weak fairness is assumed for every step the design takes,
  and for restarts and link recoveries.
- **Clock drift is not modelled.** The design shortens each lease by the largest drift allowed.
- **A candidate sends its request again** (`CandidateResends`). An instance asking to lead that has
  not yet been granted a second vote sends its request again to each voter, at the same epoch, at
  most once in each tick, until a voter grants it or it gives up. The code does the same every
  renewal interval, so that a voter that refused because its vote was promised to a leader that has
  since died can grant the request as soon as that promise runs out, rather than the candidate
  waiting out a whole lease period ([../voided_commands.md](../voided_commands.md), decision 4).

### 2.2 The properties

| Property | Kind | What it says |
|----------|------|--------------|
| `AtMostOneActing` | Safety | Two instances never act as leader at the same moment. An instance acts only while it holds an unexpired grant from a voter other than itself. |
| `EpochNamesOneLeader` | Safety | No epoch is ever led by two different instances. |
| `NoRegression` | Safety | No instance begins leading at an epoch below one the other instance has led in. It does not hold; see 2.5. |
| `EventuallyLeaderForGood` | Liveness | From some point on, an instance acts as leader for good. |

### 2.3 What held

Every run below is exhaustive: TLC visited every reachable state within the budgets given. `MaxEpoch`
bounds how many generations of epoch can be issued. With `MaxEpoch = 0` each instance can lead in one
epoch only; with `MaxEpoch = 1`, in two. The lease is 2 ticks. Every rule is on, including rule 13, which writes the
epoch to disk before an instance asks to lead (section 2.9).

| Failures allowed | `MaxEpoch` | Distinct states | Result |
|------------------|-----------:|----------------:|--------|
| One instance crash, one restart of voter 3, one link failure | 0 | 43,040,118 | Both safety properties hold |
| Two instance crashes | 0 | 9,310,974 | Both hold |
| Two link failures | 0 | 4,323,990 | Both hold |
| Voter 3 down at the start; one instance crash; one link failure | 0 | 16,235,575 | Both hold |
| One instance crash | 1 | 94,718,162 | Both hold |
| One restart of voter 3 | 1 | 72,310,837 | Both hold |
| One link failure | 1 | 47,402,987 | Both hold |

Each extra generation of epochs multiplies the number of states by about twenty, which is why the
larger failure budgets are run with `MaxEpoch = 0`.

The table above is with `CandidateResends = FALSE`, and with each instance waiting out a lease period after a
restart. The first row is also checked with `CandidateResends = TRUE`, the setting the code uses: both
safety properties hold over 423,144,337 distinct states, which takes TLC about 51 minutes on 32
cores, against about 4 minutes for the 43,040,118 states without it. At that size TLC estimates the chance that two different
states shared a fingerprint, so that one of them was never checked, at between 0.2% and 0.5% from the
fingerprints it actually saw, depending on the fingerprint function it chose for the run, and at 8%
by its more cautious calculation. The other
rows are not yet checked with `CandidateResends = TRUE`. The code also has each instance keep its promises
on disk, so that it need not wait after a restart; section 4.2 gives the results with that setting, which is the
one `MajorityLeaseHA.cfg` holds.

The liveness property, with `Prompt` on and `MaxEpoch = 1`, each checked with `CandidateResends` both
`TRUE` and `FALSE`:

| Situation | Result |
|-----------|--------|
| Voter 3 down from the start and never restarted | Holds: the two instances elect a leader between themselves |
| Voter 3 crashes at some point and never restarts | Holds |
| One instance crash, which is followed by a restart | Holds |
| One link failure, which later recovers | Holds |
| One restart of voter 3 | Holds |

The first two rows show that losing the arbiter tier alone does not stop the pair, so the design needs no
promotion of an instance on its own vote.

### 2.4 Each rule is needed

Each rule of the design was removed in turn, to confirm that the checks can fail and that the rule is
doing work. Every one of these runs fails, with every other rule in place, including
`CandidateResends = TRUE`: sending a request again rescues neither livelock in the table.

| Setting | Property broken | What happens | Trace |
|---------|-----------------|--------------|-------|
| `HolderCountsFromSend = FALSE` | `AtMostOneActing` | Instance 1 counts its lease from when the grant arrived, which was a tick after instance 2 granted it. Instance 2's promise runs out first, it asks to lead, voter 3 grants it, and for a moment both act. | `traces/lease-1-holder-counts-from-arrival.txt` |
| `WaitOutOwnGrant = FALSE` | `AtMostOneActing` | Instance 2 grants instance 1 a lease and then at once asks to lead itself. Voter 3 grants instance 2, and both act. | `traces/lease-2-candidate-ignores-own-grant.txt` |
| `RestartWaits = FALSE` | `AtMostOneActing` | Instance 2 grants instance 1 a lease, crashes, restarts having forgotten the grant, and asks to lead. Voter 3 grants it while instance 1 still acts. | `traces/lease-3-restart-does-not-wait.txt` |
| `DegradedPromotion = TRUE` | `AtMostOneActing` | Each instance asks to lead and, with no second vote, promotes itself on its own vote alone. | `traces/lease-4-degraded-promotion.txt` |
| `CandidateYields = FALSE`, voter 3 down | `EventuallyLeaderForGood` | Both instances ask to lead at the same moment, each refuses the other, both give up, and the same thing repeats for ever at the same epochs. | `traces/lease-6-no-yield-livelock.txt` |
| `PeerVotes = FALSE`, voter 3 down | `EventuallyLeaderForGood` | Without the peer's vote, no instance can gather two of the three votes while voter 3 is down, so nobody ever leads. | `traces/lease-7-no-peer-vote.txt` |

### 2.5 An epoch can go backwards

`NoRegression` fails within two ticks, with one restart of voter 3 (`traces/lease-5-epoch-regresses.txt`):

1. Instance 2 asks to lead at epoch 2. Voter 3 grants it, and instance 2 leads. Instance 1 has not yet
   received instance 2's request, so it still holds epoch 0.
2. Voter 3 crashes and restarts. It has forgotten that it granted epoch 2, and waits out one lease
   period, as it must.
3. Instance 2 has no grant left from anyone, so its lease runs out and it stops acting.
4. Instance 1 asks to lead at epoch 1. Voter 3 grants it, and instance 1 leads at epoch 1, below
   epoch 2.

`AtMostOneActing` is not broken: instance 2 had stopped acting before instance 1 began. The cost is
availability. A receiver that saw epoch 2 discards what instance 1 sends at epoch 1. Rule 8 of the
design recovers from this: instance 1 learns of epoch 2 when its peer refuses to renew at epoch 1, or
from a receiver, and it stops and asks again above epoch 2. The specification models the peer's
refusal. It does not model receivers.

### 2.6 Limits

- The larger failure budgets were checked only with one generation of epochs, so behaviours that
  need three or more elections of the same instance under those budgets were not explored.
- Liveness was checked only with `Prompt` on, and only with two generations of epochs.
- Clock drift, receivers on the order path, and the catch-up a new leader performs are not modelled.
- With `CandidateResends = TRUE` only the configuration in `MajorityLeaseHA.cfg` and the liveness checks
  have been run; the other failure budgets in 2.3 are checked without it.

### 2.7 Files

| File | What it is |
|------|------------|
| `MajorityLeaseHA.tla` | The design |
| `MajorityLeaseHA.cfg` | The settings the code uses: every rule on, each instance keeping its promises on disk, one failure of each kind. Change the constants as the tables above say to reproduce each run. |
| `traces/lease-*.txt` | The counterexamples in 2.4 and 2.5, generated by `scripts/tla_trace_pages.py` |

A liveness run uses `SPECIFICATION LiveSpec` and `PROPERTY EventuallyLeaderForGood` in place of
`SPECIFICATION Spec` and the invariants, with `Prompt = TRUE`.

The counterexamples in 2.4 and 2.5 are reproduced on every install by `scripts/tla_trace_pages.py`,
which also writes a web page that steps through them. The install fails if any of them stops breaking
its property. Each is rerun with the smallest failure budget that produces it, which is smaller than the
budgets in the tables above. The script's list of counterexamples gives the exact settings of each. It
runs TLC with a fixed fingerprint function, `-fp 0`, so that each liveness counterexample is the same loop on every
build; section 2.8 explains why, and why the script needs TLC release 1.8.0 or later.
`docs/orientation/building.md` describes the step and what it needs.

### 2.8 TLC releases before 1.8.0 can fail while printing a liveness counterexample

**What happens.** With `CandidateResends = TRUE`, TLC 2.19 finds `lease-6` and `lease-7` broken, as it should, and then
sometimes stops while printing the counterexample. It reports `java.io.EOFException`, thrown in `GraphNode.read`,
`java.io.IOException: Read past end of file`, or a `java.util.concurrent.ExecutionException` wrapping one of these. The
property is still reported broken; only the trace is lost. Without the trace, `scripts/tla_trace_pages.py` cannot write
the counterexample's page, so the install fails.

**The cause.** This is a race between two threads inside TLC, not a property of the model. It was found by reading the
compiled classes of TLC 2.19 in `tla2tools.jar` with `javap`; TLC was not instrumented, so the explanation below is read
from its code and agrees with the measurements, rather than being observed directly.

- `LiveWorker.printTrace` prints a liveness counterexample in two parts at the same time. It gives the first part, the
  path from an initial state to the start of the cycle, to a second thread, which calls `AbstractDiskGraph.getPath`.
  Its own thread meanwhile finds the cycle, in `dfsPostFix` and `bfsPostFix`. The second thread exists whatever
  `-workers` says, so running TLC with one worker does not prevent it.
- Both parts fetch nodes of the behaviour graph with `AbstractDiskGraph.getNode`. It looks first in an array of 65,536
  nodes held in memory, at the slot given by the node's fingerprint plus its tableau index, modulo 65,536. Only when
  that slot holds a different node does it read the node from the graph's file, in `getNodeFromDisk`, which moves the
  file's position to the node and then reads it.
- Both threads read through the same `BufferedRandomAccessFile`, and neither takes a lock. When both read the file at
  once, one thread can move the position the other is about to read from, so the other reads from the wrong place or
  past the end of the file. That is exactly where the exceptions above are thrown.

**Why the fingerprint function decides how often it happens.** These graphs have a few hundred nodes, so most fit in
the 65,536 slots without sharing one, and the file is read only for the few that do. Which nodes share a slot depends
on their fingerprints, and so on the fingerprint function, which TLC chooses at random on each run unless `-fp` names
one. Whether the two threads then read the file at the same moment depends on timing, so the same function, and even
the same function with the same `-seed`, fails on some runs and not others. Measured on 1 November 2026, each run the
same counterexample as the script runs:

| Counterexample | Fingerprint function | Runs | Failed |
|---|---|---|---|
| `lease-7-no-peer-vote` | 0 | 100 | 0 |
| `lease-6-no-yield-livelock` | 0 | 100 | 0 |
| `lease-7-no-peer-vote` | 129 | 40 | 27 |
| `lease-7-no-peer-vote` | 13 | 4 | 3 |
| `lease-7-no-peer-vote` | 13, with the seed held fixed (two seeds, five runs each) | 10 | 8 |
| `lease-7-no-peer-vote` | chosen at random by TLC | 40 | 9 |
| `lease-6-no-yield-livelock` | chosen at random by TLC | 40 | 2 |

With `CandidateResends = FALSE` no failure was seen, in 44 runs of each counterexample with 44 different functions; the
model then has fewer states, and so a different graph.

**It is a known defect of TLC, fixed in release 1.8.0, which this project uses.** It is
[tlaplus issue 597](https://github.com/tlaplus/tlaplus/issues/597), and commit `ba14c21a6c` of 26 March 2021 fixes it
by making `AbstractDiskGraph.getNode` and `getNodeFromDisk` synchronized. Its message names the same failure, "Read
past end of file", and the same cause: the reconstruction of a liveness counterexample was made parallel in TLC 2.08,
and the two methods are not safe to call from two threads. Release 1.7.4, which reports itself as TLC 2.19 of 8 August
2024 (revision `5a47802`), was not built from the main line: its branch left the main line on 25 April 2020, before the
fix, and in its compiled classes neither method is synchronized. Release 1.8.0, of 30 October 2026, is built from the
main line (revision `090e85c`), and in its compiled classes both methods are synchronized. So nothing needs reporting
upstream.

**Release 1.8.0 does not fail.** Measured on 2 November 2026, each counterexample run as the script runs it but with
the fingerprint function chosen at random by TLC on each run:

| Counterexample | Release | Runs | Failed |
|---|---|---|---|
| `lease-6-no-yield-livelock` | 1.7.4 | 200 | 24 |
| `lease-7-no-peer-vote` | 1.7.4 | 200 | 29 |
| `lease-6-no-yield-livelock` | 1.8.0 | 1,000 | 0 |
| `lease-7-no-peer-vote` | 1.8.0 | 1,000 | 0 |

`scripts/tla_trace_pages.py` still runs TLC with `-fp 0`, for a different reason: a liveness property is broken by
several loops, and which one TLC reports depends on the fingerprint function. With one chosen at random, six runs of
the script gave six different loops for each of `lease-6` and `lease-7`, each of them breaking the property; with
`-fp 0`, three runs gave the same two loops each time. Fixing the function keeps the page the same from one build to
the next. If the script is given an older release and TLC fails while printing a counterexample, the script stops with
a message naming this section and asking for release 1.8.0 or later.

The two threads and the shared file are in TLC's liveness checker, which prints only liveness counterexamples. No
safety counterexample has failed to print, and the safety counterexamples the script compares with the committed copies
are the same under any fingerprint function.

### 2.9 Rule 13: the epoch is written to disk before the instance asks to lead

Rule 13 of [../majority_leases.md](../majority_leases.md) says that an instance writes the epoch it is about to ask to
lead in to disk, and waits for the write, before it sends the request. The lease is counted from the moment the request
is sent, so the wait for the disk does not come out of it ([BUG-0228](../../bug_list.md#bug_0228)). The epoch on disk
can then be one the instance asked to lead in and was never granted, higher than any epoch it granted or led in, and
after a restart the instance starts from that higher epoch: it votes as a voter whose highest epoch is that one, and
asks to lead above it.

The constant `RecordBeforeAsking` turns the rule on. The variable `askedOnDisk` holds the highest epoch an instance has
written before asking, and `Crash` gives the instance back the higher of that and the highest epoch it granted or led
in. With the constant `FALSE`, `askedOnDisk` stays at zero and the specification behaves exactly as without the rule.

**The rule is reached.** `NeverRestartsHigher` says that no instance crashes with an epoch on disk above any it granted
or led in. With the rule on it is broken within a few steps, which shows the model reaches the state the rule creates,
so the checks below test it.

**The rule changes nothing when it is off.** With `RecordBeforeAsking = FALSE`, the rows of 2.3 with one instance
crash, one restart of voter 3 and one link failure; with two instance crashes; and with voter 3 down at the start
visit 42,179,802, 9,008,358 and 15,881,941 distinct states, as the specification without the variable did.

**What held with the rule on.** Every row of the table in 2.3 is the run with the rule on, and both safety properties
hold in each. Only the rows that allow an instance to crash have more states than with the rule off, which is as
expected: the epoch on disk is read back only when an instance crashes. Every liveness check of 2.3 holds with the
rule on, with `CandidateResends` both `TRUE` and `FALSE`. Every counterexample of 2.4 and 2.5 is still produced, and
the committed traces in `traces/` are unchanged.

The one behaviour the rule adds is that a restarted instance refuses its peer's request at an epoch between the
highest it granted or led in and the higher one on its disk. The refusal names the higher epoch, and the peer asks
again above it (rule 8). That costs a round of requests, and the liveness checks show it does not stop a leader being
chosen.

## 3. Rule 11: a follower that is behind must not lead

Rule 11 keeps an instance whose log lacks commands the matching engine has acted on from taking the
lead. It is described in [../a_follower_behind_does_not_lead.md](../a_follower_behind_does_not_lead.md)
and specified in `FollowerBehindHA.tla`.

A leader normally has the engine act on a command only once its follower holds it. When it acts on a
command the follower does not hold, because the follower has disconnected or fallen behind, rule 11
requires it first to have a voter record that the follower may not lead.

### 3.1 How this specification differs from MajorityLeaseHA.tla

- **Leadership is one step.** `MajorityLeaseHA.tla` shows that two instances never act as leader at
  the same moment. This specification takes that as given: when no instance is acting, an instance
  becomes leader if one other running voter grants it, and a leader may stop acting at any moment,
  which stands for its lease running out. Leaving out the timing of leases is what makes it small
  enough to check. Adding rule 11 to `MajorityLeaseHA.tla` instead gives a state space that more than
  450 million states of exploration did not exhaust.
- **A voter may refuse or ignore any request.** So every behaviour the lease rules allow is included,
  and some they do not.
- **Commands are modelled.** Each instance's log is the set of commands it holds, kept on disk, and
  the specification records which commands the engine has acted on. Writing a command, copying the
  log to the peer, and having the engine act are separate steps, so a leader can stop or die between
  any two of them.
- **Statements may be delayed, reordered or lost.** A grant that echoes a statement reaches the leader
  at once or is lost. A later echo would only let the leader act later, which it may always choose to
  do, so this allows every behaviour that delayed echoes allow.

### 3.2 The properties

| Property | Kind | What it says |
|----------|------|--------------|
| `LeaderHoldsActed` | Safety | The instance acting as leader holds every command the engine has acted on. |
| `NoChangeOfLeaderAfterActing` | Safety, expected to be broken | After one instance has had the engine act on a command, the other never leads. It is checked to show the specification is not vacuous: if rule 11 stopped every change of leader, it would hold. |

### 3.3 What held

With every part of rule 11 in place, and an instance writing a statement it receives to disk before it
grants, `LeaderHoldsActed` holds. The run is exhaustive, with two commands, three statements in each
leadership, three leaderships, two crashes of the instances, one restart of the arbiter, and at most two
statements in flight at once: 115,843,292 distinct states, which takes under six minutes on 32 workers. With at most three statements in flight and
the same budgets otherwise, a run found no error in 744,094,524 distinct states, and was stopped after 50
minutes on 32 workers with 124 million states still to explore, so it is not exhaustive.

`NoChangeOfLeaderAfterActing` is broken within eight steps: instance 1 has the engine act on a command
that instance 2 holds, its lease runs out, and instance 2 is elected. Rule 11 does not stop a change of
leader when the follower holds what was acted on.

### 3.4 Each part of the rule is needed

Each part of rule 11 was removed in turn. Every one of these runs breaks `LeaderHoldsActed`.

| Setting | What happens | Trace |
|---------|--------------|-------|
| `RecordStatement = FALSE` | Instance 1 has the engine act on command 1 before instance 2 holds it, with nothing recorded. Its lease runs out, and the arbiter elects instance 2. | `traces/behind-1-no-statement.txt` |
| `LeaderRecordsOwn = FALSE` | The arbiter records that instance 2 may not lead, and instance 1 acts on command 1. Instance 1's lease runs out. It has not crashed, but it holds no record of its own statement, so it votes for instance 2. | `traces/behind-2-leader-keeps-no-record.txt` |
| `LeaderStartsBarred = FALSE` | Instance 1 says instance 2 may not lead and acts on command 1. It is elected again, and its new leadership starts by saying instance 2 may lead, which replaces its own record while instance 2 still lacks command 1. Its lease runs out and it votes for instance 2. | `traces/behind-3-leader-starts-saying-may-lead.txt` |
| `ThirdKeepsRecord = FALSE` | The arbiter records that instance 2 may not lead, then restarts and forgets, and elects instance 2. | `traces/behind-4-arbiter-forgets.txt` |

The second row is why the leader keeps a record of its own statement. A leader whose lease runs out is
still running, and its peer's request to lead can reach it before it asks to lead again.

### 3.5 The risk accepted

In the design as decided, an instance that receives a statement records it in memory, grants at once,
and writes it to disk in the background, so that a follower whose disk has stopped answering does not
pause trading. With `InstancesRecordFirst = FALSE`, `LeaderHoldsActed` is broken
(`traces/behind-5-follower-records-in-memory.txt`):

1. Instance 1 sends its statement that instance 2 may not lead to both voters.
2. Instance 2 records it in memory and grants. The echo reaches instance 1, which has the engine act on
   command 1. The statement to the arbiter has not yet arrived.
3. Instance 1's lease runs out.
4. Instance 2 crashes before its record reaches the disk, and restarts knowing nothing.
5. The arbiter, which never recorded the statement, elects instance 2.

This needs the follower's process to restart within the moments between granting and its background
write completing, and the arbiter not to have recorded the statement in that time, and the leader to
stop acting in that time. That is the risk accepted in section 4.5 of the design.

### 3.6 Two arbiters: a statement the leader relies on must survive a change of active arbiter

The arbiter tier is two arbiters, of which one is active at a time. Sections 3.3 to 3.5 treat it as one
voter that keeps its record on disk. In the venue every instance sends each lease request, and so each
statement, to both arbiters; the active arbiter grants and refuses, and the passive arbiter grants and
refuses nothing. The active arbiter also sends each statement it records to the passive arbiter, without
acknowledgement. When the passive arbiter becomes active, what it holds is all it knows.

The specification models this with these variables and steps:

- `pBar` and `pKey` are the passive arbiter's record, and `pLinked` is whether the link between the two
  arbiters is up.
- A statement sent to the arbiter tier travels to the active arbiter (voter 3) and, with
  `BothArbitersConfirm`, to the passive arbiter too. `ReceiveAtArbiter` delivers it to one of them, which
  records it, writes it to disk and confirms it to the leader; `LoseAtArbiter` loses it on the way to one
  of them. A leader keeps the highest statement number confirmed by each of its peer, the active arbiter
  and the passive arbiter separately.
- `CopyToPassive`: the active arbiter's copy of its record reaches the passive arbiter, at any later moment
  or never.
- `ArbiterLinkFails` and `ArbiterLinkRecovers`: when the link comes back up, each arbiter sends the other its
  whole state, and both keep the newer statement.
- `ChangeActiveArbiter`: the passive arbiter becomes active. Voter 3's record is now the passive arbiter's,
  and the confirmations a leader holds from each arbiter go with that arbiter.

`MaxArbiterChanges` and `MaxArbiterLinkFailures` bound how many of each one behaviour may have.

**One arbiter's confirmation is not enough.** With one change of active arbiter allowed and
`BothArbitersConfirm = FALSE` and `ActiveConfirmsAlone = FALSE`, so that a leader acts on the active arbiter's
confirmation alone even with the link between the arbiters up,
`LeaderHoldsActed` is broken in nine steps (`traces/behind-6-passive-arbiter-lacks-statement.txt`):

1. Instance 1 is elected and says instance 2 may not lead.
2. The active arbiter records the statement and confirms it, and instance 1 has the engine act on command
   1, which instance 2 does not hold.
3. Instance 1 stops leading.
4. The passive arbiter becomes active before the statement has reached it, and elects instance 2.

This is [BUG-0230](../../bug_list.md#bug_0230).

**One machine must not stop trading.** Requiring both arbiters is safe, but one machine runs an arbiter, a
sequencer and a matching engine, so the death of one machine would take the follower and an arbiter at
once, and the new leader could then never act. So, with `ActiveConfirmsAlone`, the active arbiter's
confirmation is enough on its own once the link to the passive arbiter has been down for a set time, D.
The arbiters exchange heartbeats that echo each other's send times, and D is longer than the passive
arbiter takes to become active after the active one dies, plus the time a heartbeat takes to arrive and
the drift between the two clocks over that time. An arbiter that becomes active within D of the last echo
of its own heartbeat therefore knows the other confirmed nothing alone; one that becomes active later knows
it may lack a statement, and grants nothing until it has heard from the other again. An arbiter that has
confirmed alone sends the other its whole state before its next echo, on the same connection, so an echo
never arrives ahead of a statement confirmed alone. The code and its reasoning are in `ArbiterPeerContact`.

Time is not modelled, so the specification represents the timing by what it guarantees: the step that lets
the active arbiter confirm alone, `AloneWindowOpens`, also marks the passive arbiter as possibly behind
(`pBehind`). `ChangeActiveArbiter` hands that mark to the arbiter that becomes active (`thirdBehind`), which
then grants nothing, and `ArbiterLinkRecovers` clears both. A confirmation sent while the window is open is
kept by the leader separately, and is enough on its own.

**Knowing it may be behind is needed.** With `PassiveKnowsBehind = FALSE`, so that the mark is not made,
`LeaderHoldsActed` is broken in eleven steps (`traces/behind-7-arbiter-does-not-know-it-is-behind.txt`):

1. Instance 1 is elected and says instance 2 may not lead.
2. The link between the arbiters fails, and stays down long enough for the active arbiter to confirm alone.
3. The active arbiter confirms the statement alone, and instance 1 has the engine act on command 1.
4. Instance 1 stops leading, and the passive arbiter, which never received the statement, becomes active
   and elects instance 2.

**The fix holds.** With both arbiters needed, the active arbiter's confirmation being enough on its own once the
link has been down for D, and an arbiter that may be behind granting nothing, `LeaderHoldsActed` holds over
301,843,316 distinct states, which took 22.6 minutes on 32 workers. That run is exhaustive with one command,
two statements in each leadership, two leaderships, one crash of an instance, no restart of the arbiter tier,
at most two statements in flight, one change of active arbiter and one failure of the link between the
arbiters. With the same budgets and the mark left out, the property is broken, as above, so those budgets
reach the case the mark exists for.

The budgets of 3.3 do not finish here. Each statement can reach or miss each arbiter separately, and the
number of states grows far faster than with one arbiter: a run with those budgets, requiring both arbiters
but without the active arbiter confirming alone, found no error in 2,685,532,867 distinct states before it
was stopped after three hours, when it had nearly filled the disk.

The cost is availability in two places. A leader that has lost its follower and one arbiter waits until the
other arbiter has heard nothing from the lost one for D, 10 seconds with the default timings, before it acts
on commands its follower lacks. An arbiter that becomes active after being cut off from the other for longer
than D grants nothing until it hears from it again; meanwhile each pair can elect a leader only with both of
its instances' votes.

With `MaxArbiterChanges` and `MaxArbiterLinkFailures` both at zero, `CopyToPassive` and `AloneWindowOpens`
cannot happen, and the passive arbiter's record plays no part.

### 3.7 Limits

- Leadership is abstracted, so this specification depends on `MajorityLeaseHA.tla` for the guarantee
  that two instances never act at once.
- The order of commands within a log is not modelled, nor is the repair of a follower's log when it
  rejoins a leader whose log differs.
- The operator's tool that lets a follower lead after the old leader's machine is lost is not modelled.
  Using it breaks `LeaderHoldsActed` by definition.

### 3.8 Files

| File | What it is |
|------|------------|
| `FollowerBehindHA.tla` | Rule 11, with leadership abstracted |
| `FollowerBehindHA.cfg` | Every part of rule 11 on, with an instance writing to disk before it grants, one change of active arbiter, one failure of the link between the arbiters, a leader needing its peer's or both arbiters' confirmation of a statement, and the active arbiter confirming alone once the link between the arbiters has been down long enough (section 3.6) |
| `traces/behind-*.txt` | The counterexamples in 3.4, 3.5 and 3.6 |

The counterexamples in 3.4, 3.5 and 3.6 are reproduced on every install by `scripts/tla_trace_pages.py`,
in the same way as those of section 2, and the install fails if any of them stops breaking
`LeaderHoldsActed` or no longer matches its committed trace. They appear as text traces only, not on
the web page.

## 4. The models against the design principles

[../../design_principles.md](../../design_principles.md) lists the principles the venue's design rests on. This
section says, for each venue principle, whether a model checks it, which property does, whether the model's
assumptions match the code, and what the model leaves out.

### 4.1 Principle by principle

| Principle | Model and property | Does the model match the code? | Not modelled |
|-----------|--------------------|--------------------------------|--------------|
| V3. At most one instance of a pair acts as leader | `MajorityLeaseHA.tla`: `AtMostOneActing` and `EpochNamesOneLeader` | Yes, for rules 1 to 10 and 13, with the instances keeping their promises on disk as the code does (4.2). An arbiter that becomes active grants nothing for one lease period, which the model treats as voter 3 restarting; in the code it also keeps the highest epoch granted in each pair, which only removes grants the model allows. An arbiter that may lack a statement grants nothing until it hears from the other arbiter (section 3.6), which also only removes grants. | Clock drift between machines, which the code allows for by shortening each lease by the largest drift allowed. |
| V4. A follower that is missing commands never takes over | `FollowerBehindHA.tla`: `LeaderHoldsActed`, with two arbiters | Yes, except where section 3.5 says: the code records a statement it receives in memory, grants, and writes it to disk in the background, which is the risk accepted. | The times on which the two arbiters rely, 7 and 10 seconds (`ArbiterPeerContact.hpp`): the model replaces them with one step that opens the active arbiter's window to confirm alone and marks the passive arbiter as possibly behind together. That the times guarantee this is argued in `ArbiterPeerContact.hpp`, not checked. The operator's tool that lets a follower lead after its leader's machine is lost. |
| V5. The matching engine acts only on commands the logs hold | In part, by `LeaderHoldsActed`: the instance leading holds every command the engine has acted on | Yes, for the part modelled. | The engine catching up from the leading sequencer's log, and checking that it accounted for every record (R-0101), before it acts. |
| V9. Within a log, sequence numbers only go forward | None | | Sequence numbers are not modelled; `FollowerBehindHA.tla` treats a log as a set of commands. Section 2.5 is about epochs, which may go backwards, not sequence numbers. |
| V10. A follower's log becomes a copy of its leader's | None | | Log repair is not modelled. It is checked by `ha_test.py` scenario 62 and the scenarios of the repair design, each made to fail with the repair disabled. |
| V11. A fault in the standby is a loss of resilience, not a loss of service | `MajorityLeaseHA.tla`: `EventuallyLeaderForGood`, which holds with voter 3 down for good | Yes. | `FollowerBehindHA.tla` has no liveness property, so nothing checks that a leader whose follower has died goes on having the engine act on commands. Two costs of rule 11 bear on this principle and are argued, not checked: a leader that has lost its follower and the passive arbiter waits 10 seconds before acting on a command the follower lacks, and an arbiter that becomes active after the link to the other arbiter has been down for 7 seconds or more grants nothing until it hears from the other again (section 3.6). |
| V1, V2, V6, V7, V8, V12, V13, V14 | None | | None of these is about deciding which instance leads, which is what the models check. |

### 4.2 The instances keep their promises on disk

In the code, an instance of a pair, and an arbiter in deciding which arbiter is active, keeps the promise its vote
makes in a record on disk (`LeasePromiseStore.hpp`), and grants nothing the record does not cover until the write has
finished (`PairLeaseAgent::promise_recorded`). After a restart during the same boot of the machine it reads the record
back and votes at once, rather than waiting out a lease period. The vote it gave itself is not kept. This is
`InstancesKeepPromises = TRUE` in `MajorityLeaseHA.tla`, under which rule 6 applies to voter 3 only.

Two runs check it, with the budgets of the first row of section 2.3: one instance crash, one restart of voter 3
and one link failure, with `MaxEpoch = 0`.

| `CandidateResends` | Distinct states | Time on 30 workers | Result |
|--------------------|----------------:|-------------------:|--------|
| `FALSE` | 43,454,511 | 4 minutes 35 seconds | Both safety properties hold |
| `TRUE`, as in the code | 413,573,651 | 47 minutes | Both safety properties hold |

TLC estimates the chance that two states of the second run shared a fingerprint, so that one was never checked, at
0.4% from the fingerprints it saw, and at 8% by its more cautious calculation.

The check was made to fail on purpose. In a copy of the specification in which a crash loses the kept promise, while
`InstancesKeepPromises = TRUE` still lets the restarted instance vote at once, `AtMostOneActing` is broken within
20,689 distinct states. So the result depends on the promise surviving the crash, which in the code it does only
because the instance grants nothing the record on disk does not yet cover.

`MajorityLeaseHA.cfg` holds this setting. The counterexample `traces/lease-3-restart-does-not-wait.txt` is of an
instance that forgets its promise, so `scripts/tla_trace_pages.py` runs it with `InstancesKeepPromises = FALSE`; every
counterexample of sections 2 and 3 still breaks its property and matches its committed trace.

### 4.3 What the check found

- Every result reported in section 2 was with each instance forgetting its promises when it crashes and waiting
  out a lease period after it restarts, which is not what the code does. The code's setting holds (4.2).
- The header of `MajorityLeaseHA.tla` said that rules 11 and 12 are both checked in `FollowerBehindHA.tla`. Rule 12,
  under which the matching engine pair uses the same statements to keep an engine configured differently from its
  peer from leading, is not modelled, as rule 12 in [../majority_leases.md](../majority_leases.md) says. The header
  now says so.
- Section 1.1 of this document held a second, older copy of its closing paragraphs, part of a table and a list that
  named BUG-0097 as open. It is removed.
- The design principles page does not yet describe the two arbiters' part in rule 11. Under V4 it says the leader
  records its statement "with at least one other voter, the arbiter or the follower itself"; the leader needs its
  follower, or both arbiters, or the active arbiter confirming alone. Under V3 it says only that an arbiter which has
  just become active waits one lease period; one that may lack a statement also grants nothing until it hears from
  the other arbiter. The principles hold. Their descriptions are to be agreed before they are changed, as the
  principles page requires.
