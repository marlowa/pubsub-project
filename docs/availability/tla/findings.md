# Checking the high availability design with TLA+: findings {#tla_findings}

This document reports what model checking found when two parts of the venue's high availability
design were specified in TLA+ and checked with its model checker, TLC. The specifications are
written from the code at commit `42d29e9`, and from the design documents where they explain it.

TLA+ is a language for describing a system's design precisely enough for a tool to check it. A
specification states what the system's state can be, which steps can change that state, and which
properties must always hold. TLC then explores every reachable state of a small version of the
system and, if any sequence of steps breaks a property, reports that sequence. It checks the design
as specified, not the running code, so each finding below also says where in the code the behaviour
comes from.

---

## 1. Summary

Two specifications were written:

- **`SequencerPairHA.tla`**: the two sequencer instances and the arbiter they report to. It covers
  the instances' roles and persisted epochs, the heartbeat and arbitration timeouts, resolving
  leadership between visible peers, degraded self-promotion, the arbiter's record, its learning
  period after a restart, and leases.
- **`ArbiterPoolHA.tla`**: the two arbiters and the witness, which decide which arbiter is active.

Both include crashes and restarts of every party, link failures, and messages that are still in
flight when the state they describe changes.

**What held.** With nothing failing, every property holds in both specifications. So do these, on
their own:
- a single crash and restart of either sequencer instance;
- a single link failure, of any link, in either specification;
- a single arbiter restart;
- a single crash of the witness.

The first of these is the case the recent work on process death was aimed at, and it checks out.

**What did not.** Every result below is reported under the strict timing rules described in section
3.3. So none of them depends on a failure, restart or timeout landing within a message's flight
time. Where a timing-dependent variant also exists, it is mentioned as supporting evidence.

| # | Finding | Smallest failure found to cause it |
|---|---------|------------------------------------|
| 1 | Two running sequencer leaders can hold **the same epoch**, which defeats epoch fencing | The primary crashes while the secondary takes over, then restarts cut off from both the secondary and the arbiter |
| 2 | A leader never stands down when it learns of a newer generation, so two leaders **persist** after the fault that caused them has gone | Any path to two leaders, of which the simplest is two concurrent failures (section 6.2) |
| 3 | An arbitration round begun at startup is never cancelled, and can later **promote a healthy follower beside its healthy leader** | The primary starting after the secondary, followed by one link failure or one arbiter crash |
| 4 | After a degraded promotion the generations **diverge for good**: the follower ignores its leader from then on. This reproduces BUG-0085 | Two link failures; or a late-starting primary and one link failure |
| 5 | A follower that the arbiter confirms is left with **no timeout armed**, and never takes over if its leader then dies | A failure of the link between the two instances, followed by the leader's death |
| 6 | The arbiter **ignores the epoch in its own record** when that record is unconfirmed, and issues an epoch already used by another instance | Both instances crashing, one after the other |
| 7 | **Two arbiters can both be active**, and nothing ever makes one stand down | An arbiter restart and one link failure; or the active arbiter losing its links to both the other arbiter and the witness |
| 8 | After one blip on the link between the arbiters, the passive arbiter **can never take over**, so the loss of the active arbiter's machine leaves no active arbiter | One link blip at any earlier time, then the active arbiter's death |

Two claims in the design documents do not survive checking:

- **The header of `leader_follower.dsl`** says the arbiter pool has "three votes, majority is two"
  and "tolerates any single-machine failure". The witness keeps no record of the votes it has
  granted, and an active arbiter never stands down. After one transient blip on the link between
  the arbiters (finding 8), the death of the active arbiter's machine leaves the venue with no
  active arbiter at all.
- **`design_notes.md` section 11e** says there is only ever one issuer of a generation, because
  resolution between peers and arbitration by the arbiter "are mutually exclusive by construction".
  Within the strict timing rules and the budgets checked, those two did not collide. But the
  argument does not cover the third issuer, degraded self-promotion, and finding 1 is a collision
  between degraded self-promotion and the arbiter. With the timing rules relaxed, peer resolution
  and the arbiter do collide (section 6.1).

Writing the specifications raised further questions before the checker ran. Section 7 lists those.

Section 11 checks a proposed replacement design, in which an instance leads only while a majority of three voters
grants it a lease. It is specified in a third file, `MajorityLeaseHA.tla`.

### 1.1 Which findings the code now addresses

The code now addresses these findings:

| Finding | What the code does now |
|---------|------------------------|
| 1 | Every new epoch records which instance leads in it: its remainder on division by 4 is that instance's id (`fix_common/LeaderEpoch.hpp`). Every issuer takes the next such epoch above what it knows, so two different instances never lead at the same epoch. This applies to the sequencer, the matching engine, the arbiter's decisions for every group, and the arbiters' own epochs. |
| 2 | Heartbeats say whether the sender leads. A sequencer leader that hears its peer leading at a higher epoch, or the same epoch from a lower instance id, stands down and follows it. An active arbiter does the same. The matching engine does not yet, because its peer protocol has not been modelled. |
| 3 | `SequencerThread::adopt_role` ends any arbitration round in progress whenever a role is adopted, by any route. |
| 4 | For the sequencer group, when the arbiter confirms a connected leader but the report carries a higher epoch, it confirms that leader in a new generation above the report's epoch. The leader moves above the follower, and the follower accepts its heartbeats again. |
| 5 | A sequencer instance told again that it follows re-arms its heartbeat timeout. |
| 6 | The arbiter bounds a new epoch by the epoch on record, whether or not the record is still trusted. The inputs are built by `LeadershipDecision::inputs_for`, which the unit tests exercise directly. |
| 8, liveness | An arbiter that the witness tells to stay passive re-arms its heartbeat timeout. |

The specifications model the code with all of these through their constants. For
`SequencerPairHA.tla`, the constants are `WithFixes`, `UniqueEpochs`, `LeaderStandsDown` and
`ArbiterLiftsIncumbent`. For `ArbiterPoolHA.tla`, they are `WithFixes` and
`UniqueEpochsAndStandDown`. With every constant off, each specification models the code at
`42d29e9`. With them all on, and the strict timing rules, these results hold within the budgets in
section 5:

- two leaders never share an epoch, and an epoch never names two leaders, in any combination;
- two leaders that can hear each other always resolve, and so do two active arbiters;
- every property that held before still holds, and the fair-weather runs still pass.

**What the majority design resolves.** The venue now decides leadership by majority, with leases, as section 11 and
[../majority_leases.md](../majority_leases.md) describe. That removes promotion without an arbiter, which was the cause of
two leaders during a partition, and makes the arbiter pool a genuine majority, which resolves finding 7 and both halves
of finding 8: the witness no longer issues epochs, and an arbiter becomes active only with a grant. The matching engine
decides by the same rules as the sequencer, so it needs no stand-down of its own.

**What remains open:**

- **An epoch can go backwards** after both arbiters restart, as section 11.5 shows. Two instances still never act at
  once, and a leader learns of the higher epoch from its peer. Learning it from a receiver is not built.
- **What the old leader had not replicated when it stopped.** A leader that loses its lease stops at once, but its log
  may hold records its follower never received. That is `docs/bug_list.md`, BUG-0097.

---

## 2. What was modelled

### 2.1 The sequencer pair: `SequencerPairHA.tla`

The specification follows `SequencerThread.cpp`, `ArbiterThread.cpp` and `LeadershipDecision.hpp`
action by action:

| Code | Specification |
|------|---------------|
| `handle_peer_status_response` and `elect_role` | `ReceiveStatus` |
| `resolve_with_visible_peer` | `ResolveRole` and `ResolveTarget`: the lower instance id leads, at one past the higher epoch |
| `handle_peer_heartbeat` | `ReceiveHeartbeat`: a follower follows a newer epoch and re-arms its timeout |
| `on_timer_event`, peer heartbeat timeout | `HeartbeatTimeout`: report to the arbiter, or promote in a new generation if none is connected |
| `on_timer_event`, arbitration timeout | `ArbitrationTimeout`: retry, or promote in a new generation |
| `decide_and_broadcast` with `LeadershipDecision::decide` | `ArbiterDecides`, including declining while learning |
| `handle_arbitration_decision` | `ReceiveDecision` |
| `handle_leadership_lease` | `LeaseArrives` |
| `on_connection_lost` in the arbiter | `Unconfirm`: the record stops being trusted when its leader disconnects |
| `adopt_role` | `Adopt`: the heartbeat timeout is re-armed only on a transition into follower |

There are three links: instance to instance, and each instance to the arbiter. Status responses,
arbitration reports and arbitration decisions are messages in flight.

The code retries arbitration six times, three seconds apart. The specification uses two retries,
and adds a timing rule in their place: a round can run out while the arbiter is still reachable only
if the arbiter restarted during the round. Fifteen seconds of retries outlast its ten-second learning
period, and a reachable arbiter that is not learning answers every report.

### 2.2 The arbiter pool: `ArbiterPoolHA.tla`

The specification follows `ArbiterThread.cpp` (`elect_role`, `adopt_role`,
`promote_if_nothing_else_can_be_active`, `handle_arbiter_vote_response`) and `WitnessThread.cpp`
(`handle_arbiter_vote_request`). It covers:

- each arbiter's role, its epoch held only in memory, and whether it has seen its peer act as
  active;
- the heartbeat or startup timeout, and an outstanding vote request;
- the witness, whose highest-epoch record is also held only in memory;
- three links: between the arbiters, and from each arbiter to the witness.

### 2.3 What is left out

- **The matching engine pair.** Its code has the same degraded self-promotion
  (`MatchingEngineThread.cpp`, around lines 1254 and 1506). But by design it only ever gives
  leadership up to its peer, never takes it from its peer, so some of the collisions below cannot
  happen there in the same way. It was not modelled.
- **Receivers of leader traffic,** such as the gateways and the matching engine receiving from the
  sequencer. The properties are stated about leaders' roles and epochs, which is what receivers judge
  by.
- **Lease expiry by elapsed time.** A leader that stops renewing is modelled by its link or its
  process failing instead.
- **The two specifications together.** What two active arbiters do to a sequencer pair is argued
  from the code in section 6.7, not checked.

---

## 3. How the results were produced

### 3.1 The properties

For the sequencer pair:

| Property | Meaning |
|----------|---------|
| `NoTwoLeadersSharingAnEpoch` | Two running instances are never both leader at the same epoch. Receivers accept a message whose epoch equals the one they hold, so two leaders sharing an epoch are both believed. |
| `EpochNamesOneLeader` | Over a whole run, no epoch is ever held as leader by two different instances. |
| `NoRegression` | A new leader's epoch is never below an epoch another instance has already led in. Receivers discard messages with a lower epoch than the one they hold. |
| `FollowerCanNotice` | A running follower whose peer is dead always has a way to notice: an armed timeout, an outstanding arbitration request, or a message on its way. |
| `ArbiterNeverIssuesBelowItsRecord` | The arbiter never starts a new generation at or below the epoch in its own record. |
| `AtMostOneLeader` | Two instances never lead at once. The design does not claim this, because a partitioned leader keeps running and is meant to be fenced by its epoch. It is checked to show where fencing is all that stands between the venue and two leaders. |

For the arbiter pool:

| Property | Meaning |
|----------|---------|
| `AtMostOneActive` | Two running arbiters are never both active. |
| `NoLastingTwoActive` | Two active arbiters that can see each other, with nothing left in flight, do not exist, because nothing would ever make one stand down. |
| `PassiveNotAheadOfActive` | A passive arbiter's epoch is never above the active arbiter's. |
| `PassiveCanNotice` | A passive arbiter whose active peer has died always has a way to notice. |

### 3.2 Failure budgets

Each run allows a fixed number of each kind of failure: instance or arbiter crashes, witness
crashes, and link failures. Restarts and link recoveries are unlimited. The tables in section 5
give, for each property, the smallest budgets that break it.

### 3.3 The timing rules

TLC lets any enabled step happen next, which includes a failure landing in the millisecond or so
while a message is in flight. That does happen, but rarely. So each specification has a constant,
`Quiet`, and every result in sections 1, 5 and 6 is reported with it on unless it says otherwise.

With `Quiet` on:

- no failure, restart or timeout happens while any message is in flight;
- continuous traffic has caught up before any failure or restart. A follower receiving heartbeats
  has followed its leader's epoch and has its timeout armed, and a leader connected to the arbiter
  has had its lease recorded.

The sequencer specification has two further constants:
- **`FailuresAfterStart`**, on by default. Failures begin only once the pair has first settled, with
  one leader, one follower, and the arbiter holding a confirmed record.
- **`LateStart`**, off by default. When it is on, the primary starts after the secondary, which is
  how finding 3 arises without any race.

A result found with `Quiet` on does not depend on unlucky timing within a message's flight. It can
still depend on the order of events seconds apart, such as a link failing before a retry fires,
because that is exactly what the real timeouts race against.

### 3.4 Checking the checks

- **Every property holds in fair weather.** With all failure budgets at zero, every property holds
  in both specifications. So the counterexamples come from failures, not from a mistake that breaks
  the specification in fair weather.
- **No property holds vacuously.** Every property is shown to fail by at least one counterexample.
- **Each counterexample was checked against the code.** Each was read step by step against the code
  before being reported.
- **The timing rules were introduced one at a time, each for a stated reason.** Several early
  counterexamples depended on timings the real constants rule out. Each rule was added because a
  specific counterexample relied on something the real system does not do, and the counterexamples
  that survive all of them are the ones reported here.

---

## 4. What held

- **Fair weather.** With no failures, every property holds in both specifications, including
  `AtMostOneLeader` and `AtMostOneActive`.
- **A single process death of either sequencer instance, with its restart.** All six sequencer
  properties hold. The restarted instance comes back without taking leadership from a working peer,
  epochs never collide or go backwards, and the survivor always notices a dead peer.
- **Any single link failure, or a single arbiter restart, in the sequencer specification,** when
  both instances start together.
- **A crash and restart combined with an arbiter restart.**
- **In the arbiter pool, a single arbiter crash, a single witness crash, or a single link failure**
  never produces two active arbiters.

---

## 5. Results

"holds" means the property holds within the stated budgets. "fails" means TLC found a counterexample.

### 5.1 The sequencer pair, with `Quiet` on, instances starting together

| Failures allowed | Two leaders, same epoch | Epoch names one leader | No regression | Follower can notice | Arbiter below its record | At most one leader |
|---|---|---|---|---|---|---|
| none | holds | holds | holds | holds | holds | holds |
| 1 instance crash | holds | holds | holds | holds | holds | holds |
| 1 link failure | holds | holds | holds | holds | holds | holds |
| 1 arbiter restart | holds | holds | holds | holds | holds | holds |
| 2 instance crashes | holds | fails | holds | holds | fails | holds |
| 2 link failures | holds | holds | fails | holds | holds | fails |
| 1 crash and 1 link failure | holds | holds | holds | fails | holds | holds |
| 1 crash and 1 arbiter restart | holds | holds | holds | holds | holds | holds |
| 1 link failure and 1 arbiter restart | holds | holds | holds | holds | holds | fails |
| 1 crash and 2 link failures | fails | fails | fails | fails | fails | fails |

### 5.2 The sequencer pair, with `Quiet` on, the primary starting later

| Failures allowed | Two leaders, same epoch | No regression | Follower can notice | At most one leader |
|---|---|---|---|---|
| none | holds | holds | holds | holds |
| 1 link failure | holds | fails | holds | fails |
| 1 arbiter restart | holds | holds | holds | fails |

### 5.3 The arbiter pool, with `Quiet` on

| Failures allowed | At most one active | No lasting two active | Passive not ahead of active | Passive can notice |
|---|---|---|---|---|
| none | holds | holds | holds | holds |
| 1 arbiter crash | holds | holds | not run | holds |
| 1 witness crash | holds | holds | not run | not run |
| 1 link failure | holds | holds | fails | not run |
| 2 link failures | fails | fails | not run | not run |
| 1 arbiter crash and 1 link failure | fails | fails | not run | fails |
| 1 witness crash and 1 link failure | holds | holds | not run | not run |
| 1 arbiter crash and 1 witness crash | holds | holds | not run | not run |

"Not run" means that combination was not checked for that property, because the finding it belongs
to was already established by a smaller combination.

---

## 6. Findings

Each finding gives what happens in plain steps, the counterexample, the code responsible, and how
likely it is. The counterexamples are in `traces/`, one file per counterexample, as a table of
states. In the text, S1 and S2 are the sequencer instances with instance ids 1 (the primary) and 2
(the secondary), and A1 and A2 are the arbiters. "S1 at 2" means instance 1 holding epoch 2.

### 6.1 Two running sequencer leaders can hold the same epoch

**What happens** (`traces/seq-1-same-epoch.txt`):

1. The pair has settled: S1 leads at 1, and S2 follows at 1.
2. S1's process dies. S2 times out and reports, and the arbiter makes S2 leader at 2. S1's epoch
   file still says 1, because S1 was not running when generation 2 began.
3. S1's links to the arbiter and to S2 both fail, and S1 is then restarted.
4. S1's startup timeout fires. It can see neither its peer nor the arbiter, so it promotes itself in
   a new generation: its own epoch plus one, 1 + 1 = 2.

S1 and S2 now both lead at epoch 2. Every receiver accepts a message whose epoch equals its own, so
anything that can reach both believes both, and epoch fencing cannot tell them apart.

**Why it happens.** Degraded self-promotion adds one to the instance's own epoch, which is stale
whenever the instance was not running while its peer moved on. The arbiter's decision and peer
resolution also add one to "the higher known epoch". So any two of the three issuers can arrive at
the same number from the same base.

**Supporting evidence, timing-dependent** (`traces/seq-11-timing-one-crash-same-epoch.txt`, `Quiet`
off). With nothing but S1's crash and restart, the arbiter answers S2's report with "S2 leads at 2"
at the moment S1, restarting, resolves itself to leader at 2. S1 is working from a status reply that
still describes S2 as a follower. This is resolution between peers and the arbiter issuing at once,
which section 11e of the design notes argues cannot happen. It needs the restart to complete while
the arbiter's decision is in flight, which is a window of about a millisecond. It also relies on a
follower ignoring its peer's status: `elect_role` returns at once for an instance that already
holds a role, so S1 resolves on the assumption that S2 is resolving too, and S2 is not.

**Code.** The degraded paths in `SequencerThread::on_timer_event` call `set_epoch(epoch_ + 1)`.
`resolve_with_visible_peer` and `LeadershipDecision::decide` use the higher known epoch plus one.

**How likely.** The robust sequence needs S1 to come back cut off from both S2 and the arbiter while
still being reachable by something that acts on its traffic. If S1 is cut off from everything, it is
harmless. The partial isolation this needs is a network fault rather than a process fault.

### 6.2 A leader never stands down on learning of a newer generation

**What happens.** Once two instances both hold the leader role, nothing in the sequencer's code
makes either give it up, even after every link has recovered:
- A leader ignores its peer's heartbeats: `handle_peer_heartbeat` follows a newer epoch only when
  `role_` is follower.
- A leader ignores status responses: `elect_role` returns at once for an instance with a role.
- A leader never asks the arbiter anything: only a follower's timeout sends a report.

The only thing that demotes a leader is an arbitration decision naming its peer. That is sent only
in answer to a report, and a report comes only from a follower.

The simplest paths to two leaders are two concurrent failures that leave the follower with neither
its peer nor an arbiter:
- the peer link and the follower's arbiter link (`traces/seq-5-two-leaders-two-links.txt`);
- the peer link and an arbiter crash (`traces/seq-4-two-leaders-link-and-arbiter.txt`).

The follower then promotes itself, as the design intends when no arbiter can be reached. What this
finding adds is that the result is permanent. Receivers that see both leaders keep only the higher
epoch's traffic, but the lower-epoch leader goes on believing it leads, and anything that sees only
it keeps acting on it.

The arbiter pool has the same property. An active arbiter never stands down: its heartbeat timeout
handler returns immediately while it is active, and `elect_role` returns at once for an arbiter with
a role. `traces/arb-a-two-link-blips-two-active.txt` ends with two active arbiters that can see each
other and each know the other is active, with nothing left that would change either.

**Relation to the design.** `design_notes.md` section 10 accepts that a partitioned leader "keeps
running and keeps being refused", as the price of having no hardware fencing. The checking adds two
things. It stays that way after the partition ends too, not only during it. And when the two leaders
share an epoch (finding 1), it is not refused at all.

### 6.3 An arbitration round begun at startup can promote a healthy follower

**What happens** (`traces/seq-8-late-start-leftover-round-link.txt`, `LateStart` on):

1. The venue starts, and the arbiter is in its learning period. S2 is running, but S1 has not
   started yet.
2. S2's startup timeout fires. It reports to the arbiter, which declines because it is still
   learning. S2's round of retries begins.
3. S1 starts. The two exchange status and settle leadership between themselves: S1 leads at 1, and
   S2 follows at 1. S2's round of retries is not cancelled.
4. S2's link to the arbiter fails. S2's next retry finds no arbiter connected, so S2 promotes itself
   to leader at 2.

S1 is healthy, and still leading at 1. `traces/seq-10-late-start-leftover-round-arbiter-crash.txt`
reaches the same end with an arbiter crash instead of the link failure.

**Code.** `adopt_role` does not cancel `arbitration_timeout_timer_id_` or clear
`arbitration_outstanding_`. Only `handle_arbitration_decision` does. The arbitration timeout handler
returns early only for a leader, so a follower carries on retrying, and falls back to degraded
promotion when no arbiter is connected.

**How likely.** It needs one instance to start after the other while the arbiter is learning. That
is common at a cold start of the whole venue, where the arbiter has just started too. The leftover
round lasts until the arbiter next answers, a few seconds after its learning period ends. So a link
failure or arbiter crash has to land in that window.

### 6.4 After a degraded promotion the generations diverge for good (BUG-0085)

**What happens** (`traces/seq-6-regression-two-links.txt`):

1. S2 promotes itself to 2 in the degraded path, as in finding 6.2. S1 still leads at 1.
2. S2's link to the arbiter recovers. When S2 registers again, the arbiter sends it the confirmed
   record: "S1 leads at 1". S2 becomes a follower, but `set_epoch` refuses to go backwards, so S2
   stays at 2.
3. S2 is now a follower at 2, and its leader S1 is at 1. S2 discards every heartbeat from S1 as
   stale, so its timeout is never re-armed by them. It times out, reports, is told again that S1
   leads at 1, and repeats this indefinitely.

`traces/seq-9-late-start-leftover-round-regression.txt` reaches the same state from finding 3's
leftover round and a single link failure.

This is BUG-0085's mechanism. The model adds one thing to the entry's "where to start", which
suggests letting the arbiter learn a component's generation from its heartbeat. In this sequence,
that would tell the arbiter S2 is at 2. But S1 would still lead at 1, and every receiver that had
accepted S2's traffic at 2 would discard S1's. Learning the higher epoch heals nothing unless the
leader is also moved to a generation above it.

### 6.5 A confirmed follower is left with no timeout armed

**What happens** (`traces/seq-7-stuck-follower.txt`):

1. The pair has settled. The link between S1 and S2 fails.
2. S2 hears no heartbeats, times out, and reports. S1 is still connected to the arbiter, so the
   arbiter confirms it: "S1 leads at 1, S2 follows".
3. S2 was already a follower, so `adopt_role` changes nothing. In particular it does not re-arm the
   heartbeat timeout, which has already fired. `handle_arbitration_decision` has cancelled the
   arbitration timeout, so S2 now has no timer of either kind.
4. S1 dies.

S2 never notices. Heartbeats from S1 were the only thing that would have re-armed S2's timeout, and
none will come. The arbiter sees S1 disconnect and stops trusting its record, but it tells nobody:
it answers questions and does not volunteer decisions.

The pair has no leader until S1 is restarted. If S1's machine has died, which is the case the
secondary exists for, that is indefinitely.

**Code.** `adopt_role` returns immediately when the new role equals the current one, so the timeout
is re-armed only on a transition into follower. `handle_arbitration_decision` calls
`adopt_role(follower)` and cancels the arbitration timeout.

**How likely.** It needs the peer link to fail, then the leader to die before the link comes back
and a heartbeat gets through. A leader's machine dying often takes the peer link with it, so if the
two happen in the wrong order, this is the case the secondary exists for.

### 6.6 The arbiter ignores the epoch in its own record when that record is unconfirmed

**What happens** (`traces/seq-2-epoch-reused.txt` and `traces/seq-3-arbiter-at-its-record.txt`):

1. The pair has settled at 1. S1 dies, and the arbiter makes S2 leader at 2.
2. S2 dies too. The arbiter's record still says "S2 at 2", but it is no longer confirmed.
3. S1 restarts, with its epoch file still at 1. It cannot see S2, so it reports epoch 1.
4. The arbiter has no confirmed incumbent, so it decides from the reported epoch alone:
   max(0, 1) + 1 = 2. It makes S1 leader at 2, the epoch S2 held.

Epoch 2 has now named two different leaders. Anything still carrying S2's traffic at epoch 2, such
as a message in a queue or a reconnecting receiver, is accepted as current alongside S1's.

**Code.** `LeadershipDecision.hpp` says a new epoch "is taken from the higher of the arbiter's record
and the reporter's, so a component that has been away and comes back with a stale epoch cannot wind
the sequence backwards". But `ArbiterThread::decide_and_broadcast` sets `inputs.incumbent_epoch`
only when the record is confirmed. When the incumbent has disconnected, which is exactly when a
returning component is most likely to report, the decision uses the reporter's epoch alone.

**How likely.** Both instances dying in turn is the pattern of a faulty release, or of both hosts
being restarted.

### 6.7 Two arbiters can both be active

**What happens** (`traces/arb-b-restart-and-link-two-active.txt`):

1. A1 is active. A1 crashes. A2 times out, asks the witness, which cannot see A1, and becomes
   active at 1.
2. The link between the arbiters fails.
3. A1 is restarted. It holds no role, epoch zero, and no memory of having seen A2 active. Its
   startup timeout fires, and it asks the witness.
4. The witness sees both arbiters connected, and grants the lower id, A1, at epoch 2.

A1 and A2 are now both active, and when the link between them recovers, neither stands down
(finding 6.2).

`traces/arb-a-two-link-blips-two-active.txt` reaches the same end without any crash. A1, while
active, loses its links to both A2 and the witness, but stays reachable by the components, and the
witness grants A2. `traces/arb-c-timing-startup-race-and-link-two-active.txt` (`Quiet` off) reaches
it with a single link failure during startup.

**Why it matters.** Components send their arbitration reports to both arbiters
(`SequencerThread::send_arbitration_report` sends to both connections), and only a passive arbiter
drops them. With two active arbiters, each decides from its own connection table and its own record.
If S1 is connected to A1 but not to A2, a report from S2 is answered by A1 with "S1 leads", and by
A2 with "S2 leads at a new epoch". `handle_arbitration_decision` applies whichever arrives, so the
component's role depends on the order of arrival. Leases also go to both arbiters, and their records
can diverge further from there. This consequence is argued from the code, not checked, because the
two specifications are separate.

**Why it happens.** The witness is not a third vote in a majority:
- It grants the role to whichever arbiter asks, preferring the lower id when it can see both.
- It remembers nothing of what it has granted, so it cannot tell a restarted A1 asking for the first
  time from a pool whose active member is already serving. That is the mistake BUG-0031 describes
  for component arbitration, a cold-start preference applied to a rejoin, recurring one level up.
- An active arbiter never checks that it still holds a majority. It stays active with no link to
  either the other arbiter or the witness.

### 6.8 After one blip on the link between the arbiters, the passive arbiter cannot take over

**What happens** (`traces/arb-e-vote-leaves-passive-ahead.txt` and
`traces/arb-f-passive-cannot-notice.txt`):

1. A1 is active at 0, and A2 passive at 0. The link between them blips.
2. A2 hears no heartbeats, times out, and asks the witness. The witness sees A1 connected, grants A1,
   and returns the epoch for the new arbiter generation: 1.
3. A2 takes that epoch and stays passive. A1 never learns of epoch 1.
4. The link recovers. A1's heartbeats carry epoch 0, below A2's 1, so A2 discards them as coming from
   a stale peer.
5. A2's heartbeat timeout has already fired, and `adopt_role(follower)` changes nothing for an
   arbiter that is already passive, so nothing re-arms it. The heartbeats that would re-arm it are
   being discarded.
6. A1 dies. A2 never notices. No arbiter is active.

With no active arbiter, every arbitration report is dropped, and components fall back to degraded
self-promotion. Findings 1, 3 and 4 show where that leads.

The blip in step 1 can have happened at any time before step 6, and leaves no visible sign. So after
any transient fault on that link, the pool no longer survives the loss of the active arbiter's
machine.

**Code.** `handle_arbiter_vote_response` takes `resp.epoch` and calls `adopt_role`. `adopt_role`
returns immediately for the current role. `handle_peer_heartbeat` ignores a heartbeat whose epoch
is below the receiver's, and re-arms the timeout only otherwise.

---

## 7. Questions the specifications raised before checking

Writing a specification forces each piece of state and each step to be stated exactly. These are the
places where the design documents did not say, and the code had to be read to find out:

- **Which facts survive whose restart.** Sequencer epochs persist. Arbiter epochs, the arbiter's
  record, its memory of having seen its peer active, and the witness's highest epoch do not. The
  design notes discuss persistence for the sequencer and for the arbiter's record. They do not
  discuss the arbiter's own epoch or the witness's, and findings 6.7 and 6.8 turn on exactly those.
- **What a vote from the witness means to an arbiter that did not win it.** The code makes it a
  source of a new epoch for that arbiter too. Finding 6.8 follows.
- **Whether an arbitration round belongs to a role or to the instance.** The code ties it to the
  instance, so it outlives the role being settled another way. Finding 6.3 follows.
- **Whether the arbiter's record epoch is a fact or an opinion.** The comment in
  `LeadershipDecision.hpp` treats it as a fact that bounds new epochs. The code treats it as valid
  only while the record is confirmed. Finding 6.6 follows.
- **Whether the arbiter elects on a status query.** The sequencer deliberately elects only on the
  response, because the query carries no role, and `design_notes.md` records that electing on the
  query once produced two leaders. The arbiter still elects on the query: `handle_peer_status_query`
  calls `elect_role` with the peer's role as unknown. No counterexample was traced to this within the
  budgets run, but it is the pattern the design notes identify as a cause of two leaders.

---

## 8. Limits of this checking

- **Bounded.** Each run explores every state reachable within small budgets: epochs up to 3 or 4,
  and up to three failures in all. A property that holds here holds only within those bounds, and is
  not proved for longer runs.
- **Abstracted timing.** Time is replaced by "any enabled step may happen next", with the timing
  rules in sections 2.1 and 3.3. The rules exclude races within a message's flight. They do not
  exclude races between events seconds apart, because the real timeouts race against those.
- **The specification, not the code.** Each counterexample was read against the code, and each
  finding names the code responsible. But the specification is a reading of the code, and where the
  two differ, the code decides.
- **Not modelled:** the matching engine pair, receivers of leader traffic, several component groups
  sharing one arbiter pool, the effect of two active arbiters on components (argued in section 6.7),
  and lease expiry by elapsed time.

---

## 9. Directions for fixing

These are options to decide between, not decisions.

- **Make generations unique to their issuer.** Finding 1 and part of 6.6 come from several issuers
  all computing "an epoch plus one". A standard remedy, used by Paxos for its ballot numbers, is to
  make each issuer's numbers distinct. For example, the epoch can be a counter paired with the
  issuing party's identity, and compared counter first. Two issuers then cannot produce the same
  generation, however their timing falls.
- **Make a leader stand down on seeing a newer generation.** A leader that receives a heartbeat or
  status response carrying a higher epoch than its own should stop acting as leader and ask. That
  gives the pair a way to converge after a fault ends (finding 6.2). The same applies to an active
  arbiter that learns of a peer active at a higher epoch.
- **Tie arbitration rounds to the role, not the instance.** Cancel the arbitration timeout and clear
  the outstanding flag whenever a role is adopted by any path (finding 6.3).
- **Re-arm the heartbeat timeout on every decision, not only on a transition.** In `adopt_role` for
  the sequencer, and in `handle_arbiter_vote_response` for the arbiters (findings 6.5 and 6.8).
- **Let the arbiter bound new epochs by everything it has seen.** Keep, for each group, the highest
  epoch seen from any lease, report, heartbeat or replicated record, confirmed or not, and issue
  above it (finding 6.6). Moving the leader above a degraded generation, not only learning of it, is
  what BUG-0085 needs (finding 6.4).
- **Give the arbiter pool a real majority.** An active arbiter should stay active only while it can
  reach at least one of the other arbiter and the witness, and stand down otherwise. The witness
  should remember whom it last granted, and at what epoch, and should not grant a rejoining arbiter
  over one it knows to be active. The witness's vote should not hand a new epoch to the arbiter that
  did not win it (findings 6.7 and 6.8).
- **Re-run the checks after each change.** A change to the code should be matched by the same change
  to the specification, then run against the budgets in section 5. A fix is confirmed when the
  counterexample it targets disappears and the fair-weather runs still pass.

---

## 10. Files, and how to reproduce

| File | What it is |
|------|------------|
| `SequencerPairHA.tla` | The sequencer pair and its arbiter |
| `SequencerPairHA.cfg` | Default settings: `Quiet` on, `WithFixes` off, one crash and one link failure; it reproduces finding 5, and passes with `WithFixes` on |
| `ArbiterPoolHA.tla` | The two arbiters and the witness |
| `ArbiterPoolHA.cfg` | Default settings: `Quiet` on, `WithFixes` off, one arbiter crash and one link failure |
| `traces/` | One counterexample per file, as a table of states |

TLC is in `tla2tools.jar`, from the TLA+ project's releases at
`https://github.com/tlaplus/tlaplus/releases`. It needs Java 11 or later. From this directory:

```
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config SequencerPairHA.cfg SequencerPairHA.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config ArbiterPoolHA.cfg ArbiterPoolHA.tla
```

`-deadlock` turns off deadlock checking. Once the failure budgets are spent, a run can reach a state
with nothing left to do, and that is not an error.

To reproduce one finding, copy the configuration file, set the budgets given for that finding in
section 5, and list only its property under `INVARIANTS`. TLC stops at the first property it finds
broken. Every run in this document completes in under a minute on an ordinary machine.

---

## 11. The proposed design: leadership by majority, with leases

Sections 1 to 10 check the design the code implements. This section checks a proposed replacement,
described in [../majority_leases.md](../majority_leases.md). It is specified in `MajorityLeaseHA.tla`.

In the proposed design, an instance may lead only while a majority of three voters has granted it a
lease that has not run out. For a component pair the voters are the two instances and the active
arbiter. For the arbiters they are the two arbiters and the witness. One specification covers both
places: voters 1 and 2 may lead, and voter 3 never does. A change of active arbiter is modelled as
voter 3 restarting, because the new active arbiter knows nothing of what the previous one promised
and must wait as a restarted voter does.

### 11.1 How this specification differs from the other two

- **It models time.** Every promise, lease and restart wait is a count of clock ticks remaining,
  and one step of the model advances the clock by a tick. A lease design cannot be checked without
  this, because its safety argument is an argument about time.
- **Messages may take any number of ticks, or be lost.** The safety results below therefore do not
  depend on how quickly messages arrive. There is no `Quiet` setting for safety.
- **Liveness is checked with `Prompt` on.** Whether a leader is eventually chosen does depend on
  timing, so those checks assume that every message arrives within the tick it was sent in, and that
  a leader asks for renewal in every tick. Weak fairness is assumed for every step the design takes,
  and for restarts and link recoveries.
- **Clock drift is not modelled.** The design shortens each lease by the largest drift allowed.

### 11.2 The properties

| Property | Kind | What it says |
|----------|------|--------------|
| `AtMostOneActing` | Safety | Two instances never act as leader at the same moment. An instance acts only while it holds an unexpired grant from a voter other than itself. |
| `EpochNamesOneLeader` | Safety | No epoch is ever led by two different instances. |
| `NoRegression` | Safety | No instance begins leading at an epoch below one the other instance has led in. It does not hold; see 11.5. |
| `EventuallyLeaderForGood` | Liveness | From some point on, an instance acts as leader for good. |

### 11.3 What held

Every run below is exhaustive: TLC visited every reachable state within the budgets given. `MaxEpoch`
bounds how many generations of epoch can be issued. With `MaxEpoch = 0` each instance can lead in one
epoch only; with `MaxEpoch = 1`, in two. The lease is 2 ticks.

| Failures allowed | `MaxEpoch` | Distinct states | Result |
|------------------|-----------:|----------------:|--------|
| One instance crash, one restart of voter 3, one link failure | 0 | 42,179,802 | Both safety properties hold |
| Two instance crashes | 0 | 9,008,358 | Both hold |
| Two link failures | 0 | 4,323,990 | Both hold |
| Voter 3 down at the start; one instance crash; one link failure | 0 | 15,881,941 | Both hold |
| One instance crash | 1 | 84,646,945 | Both hold |
| One restart of voter 3 | 1 | 72,310,837 | Both hold |
| One link failure | 1 | 47,402,987 | Both hold |

Each extra generation of epochs multiplies the number of states by about twenty, which is why the
larger failure budgets are run with `MaxEpoch = 0`.

The liveness property, with `Prompt` on and `MaxEpoch = 1`:

| Situation | Result |
|-----------|--------|
| Voter 3 down from the start and never restarted | Holds: the two instances elect a leader between themselves |
| Voter 3 crashes at some point and never restarts | Holds |
| One instance crash, which is followed by a restart | Holds |
| One link failure, which later recovers | Holds |
| One restart of voter 3 | Holds |

The first two rows are the concern that degraded self-promotion was meant to address: losing the
arbiter tier alone does not stop the pair.

### 11.4 Each rule is needed

Each rule of the design was removed in turn, to confirm that the checks can fail and that the rule is
doing work. Every one of these runs fails, with every other rule in place.

| Setting | Property broken | What happens | Trace |
|---------|-----------------|--------------|-------|
| `HolderCountsFromSend = FALSE` | `AtMostOneActing` | Instance 1 counts its lease from when the grant arrived, which was a tick after instance 2 granted it. Instance 2's promise runs out first, it asks to lead, voter 3 grants it, and for a moment both act. | `traces/lease-1-holder-counts-from-arrival.txt` |
| `WaitOutOwnGrant = FALSE` | `AtMostOneActing` | Instance 2 grants instance 1 a lease and then at once asks to lead itself. Voter 3 grants instance 2, and both act. | `traces/lease-2-candidate-ignores-own-grant.txt` |
| `RestartWaits = FALSE` | `AtMostOneActing` | Instance 2 grants instance 1 a lease, crashes, restarts having forgotten the grant, and asks to lead. Voter 3 grants it while instance 1 still acts. | `traces/lease-3-restart-does-not-wait.txt` |
| `DegradedPromotion = TRUE` | `AtMostOneActing` | Each instance asks to lead and, with no second vote, promotes itself on its own vote alone. | `traces/lease-4-degraded-promotion.txt` |
| `CandidateYields = FALSE`, voter 3 down | `EventuallyLeaderForGood` | Both instances ask to lead at the same moment, each refuses the other, both give up, and the same thing repeats for ever at the same epochs. | `traces/lease-6-no-yield-livelock.txt` |
| `PeerVotes = FALSE`, voter 3 down | `EventuallyLeaderForGood` | Without the peer's vote, no instance can gather two of the three votes while voter 3 is down, so nobody ever leads. | `traces/lease-7-no-peer-vote.txt` |

### 11.5 An epoch can go backwards

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

### 11.6 Limits

- The larger failure budgets were checked only with one generation of epochs, so behaviours that
  need three or more elections of the same instance under those budgets were not explored.
- Liveness was checked only with `Prompt` on, and only with two generations of epochs.
- Clock drift, receivers on the order path, and the catch-up a new leader performs are not modelled.
- The specification checks the design. No code implements it yet.

### 11.7 Files

| File | What it is |
|------|------------|
| `MajorityLeaseHA.tla` | The proposed design |
| `MajorityLeaseHA.cfg` | The design's settings: every rule on, one failure of each kind. Change the constants as the tables above say to reproduce each run. |
| `traces/lease-*.txt` | The counterexamples in 11.4 and 11.5, generated by `scripts/tla_trace_pages.py` |

A liveness run uses `SPECIFICATION LiveSpec` and `PROPERTY EventuallyLeaderForGood` in place of
`SPECIFICATION Spec` and the invariants, with `Prompt = TRUE`.

The counterexamples in 11.4 and 11.5 are reproduced on every install by `scripts/tla_trace_pages.py`,
which also writes a web page that steps through them. The install fails if any of them stops breaking
its property. Each is rerun with the smallest failure budget that produces it, which is smaller than the
budgets in the tables above. The script's list of counterexamples gives the exact settings of each.
`docs/orientation/building.md` describes the step and what it needs.
