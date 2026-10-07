# Deciding leadership by majority, with leases {#majority_leases}

This document describes how the venue decides which instance of a pair leads. It applies in two places:

- **A pair of component instances**: the two sequencers, the two matching engines, and the two matching engine publishers.
- **The two arbiters**, which decide between themselves which one is active.

The design has been specified in TLA+ and model checked, and [tla/findings.md](tla/findings.md) section 11 reports what
that checking found. Section 10 says where each part is implemented and what checks it.

---

## 1. The problem it solves

When a follower stops hearing from its leader, it cannot tell which of two things has happened:

- the leader has died, and someone must take over; or
- the follower has been cut off, and the leader is still running and trading.

From where the follower sits, the two look exactly the same. If it takes over in the first case, the venue keeps trading.
If it takes over in the second case, the venue has two leaders. Every design has to decide what to do given that the
follower cannot tell the difference.

An instance that promotes itself whenever it can reach no arbiter keeps the venue trading when the arbiters are down,
but when the real cause is a cut-off follower it produces two leaders. Model checking shows this happening (findings 1, 2
and 4 in [tla/findings.md](tla/findings.md)), so no instance of this venue promotes itself.

The venue uses instead a rule taken from the standard literature on consensus: **an instance may lead only while a
majority of three voters agrees that it should, and each voter's agreement lasts for a fixed period and must be
renewed.** The agreement that lasts for a fixed period is called a lease.

## 2. The voters

Each decision has three voters. Two of them are the instances that may lead; the third never leads.

| Where the design is used | The two instances that may lead | The third voter |
|---|---|---|
| A component pair | The two instances of the component | The arbiter that is active at the time |
| The arbiters | The two arbiters | The witness |

A majority is any two of the three. An instance always votes for itself while it leads or asks to lead, so it needs the
vote of just one other voter: either its peer or the third voter.

## 3. The rules

1. **A voter grants a lease to at most one instance at a time.** When it grants one, it promises not to grant a lease
   to anyone else until the lease period has passed, counted from the moment it granted.

   This undertaking is what the rest of this document calls a **promise**. The word is the one used for the same idea
   in Paxos, the best known consensus algorithm. Granting a lease and making a promise are two sides of one act: the
   instance that receives the grant holds a lease, and the voter that gave it holds a promise. The lease tells the
   instance that it may lead until a certain time. The promise tells the voter that it must not let anyone else lead
   until that time. Two leaders could only exist if some voter broke a promise, so everything that makes the design
   safe comes down to voters keeping them. The hard case is a voter whose process restarts, because a promise held
   only in memory is lost with the process. Rule 6 says what each kind of voter does about that: an arbiter waits
   until any promise it might have made has run out, and a sequencer or matching engine instance writes each promise
   to disk before it grants, and reads it back when it restarts.
2. **An instance leads only while it holds an unexpired lease from its peer or from the third voter.** With its own
   vote, that is a majority. The instance counts each lease's period from the moment it *sent* its request, which is
   earlier than the moment the voter granted it. So the instance always believes its lease ends no later than the voter
   believes its promise ends. When its last lease runs out, the instance stops acting as leader: it sends nothing more
   on the order path until it holds a lease again.
3. **A leader keeps its lease by asking both other voters again, repeatedly.** It asks well within the lease period.
   Either voter granting is enough. These renewal requests also serve as the leader's heartbeats to its follower.
4. **An instance that is leading, or asking to lead, votes for itself**, so it grants nothing to its peer.
5. **An instance that has granted its peer a lease does not ask to lead until that lease has run out.** In practice
   this is the follower's heartbeat timeout: it is at least the lease period.
6. **A voter that restarts has forgotten what it promised, so it grants nothing for one lease period after it
   starts.** The same applies to an arbiter that becomes the active arbiter: it knows nothing of what the previously
   active arbiter promised, so it grants no component lease for one component lease period.

   A component instance does not forget, and so does not wait. It writes each promise to disk before it sends the
   grant, and while it leads it writes that it is leading. When its process restarts, it reads the record back and
   carries on from it: it votes at once, and if it was leading it asks to lead again at once, without the
   secondary's head start. This is what lets a supervisor restart a leading process without its peer taking over
   (section 5).

   The vote a leader gave itself is not carried across the restart. It was a vote for a process that has died, and
   the lease that process held died with it, so nothing can be acting on it. Keeping it would only make the restarted
   instance refuse a peer that took over while it was down; with the arbiters also down, that would leave the peer
   with only its own vote, and the group with no leader, for no reason.

   The record holds each promise's expiry as a time on the steady clock. On Linux that clock counts from when the
   machine booted and is the same for every process on it, so a time recorded by one process means the same moment to
   the next. After a reboot the time means nothing, so the record also holds the kernel's boot id
   (`/proc/sys/kernel/random/boot_id`), and a record from a different boot is ignored: the instance has then
   forgotten, and waits. A missing or damaged record is treated the same way. The expiry written is ten seconds later
   than the true one, so that a follower granting a renewal every second rewrites the file only now and then. A later
   expiry only makes the restarted instance stricter. If a promise cannot be written, it is not made: the instance
   refuses the request.

   The file is written and synced on a thread of its own (`BackgroundPromiseRecorder`), not on the thread that
   handles leases. When fewer than five seconds remain between the promise's expiry and the record's, a fresh record
   is written in the background, so in normal operation it is on disk seconds before it is needed and the lease
   thread never waits for the disk. The lease thread writes a record itself, and waits for it, only when the record
   it holds does not cover the promise it is about to make: when it first promises its vote to an instance, or when
   a background write has not finished in those five seconds. A sync that takes seconds therefore no longer stops an
   instance answering lease requests ([BUG-0107](../bug_list.md#bug_0107)). Any write that takes 50 milliseconds or
   more is logged with how long it took.

   Each arbiter keeps such a record for its own lease, the one deciding which arbiter is active, so a supervised
   restart of the active arbiter does not make the other one active. An arbiter keeps no record of its votes for the
   components, so an arbiter that becomes active still waits before granting any component a lease. The witness and
   the matching engine publisher keep no record, and always wait.

   An instance resuming a lead it recorded may find its epoch refused as behind: an arbiter keeps no epoch on disk, so
   it first asks below the epoch it led in. It then asks again above that epoch at once, without the random wait of
   rule 10. That wait keeps two candidates from colliding, and there is no second candidate, because both other voters
   promised their votes to the instance resuming.
7. **Each voter remembers the highest epoch it has granted, and grants no lower one.** A new leader's epoch records
   which instance leads in it, as `fix_common/LeaderEpoch.hpp` already does, so no two instances ever lead at the same
   epoch. A voter that refuses a request says in its refusal what the highest epoch it has granted is.
8. **An instance that learns of an epoch higher than its own stops leading and asks again above it.** It can learn of
   one from a voter's refusal, or from a receiver on the order path that holds a higher epoch. Section 6 explains why
   this rule is needed.
9. **An instance asking to lead that receives its peer's request at a higher epoch gives up and grants it.** Without
   this, when both instances ask at the same moment and the third voter is down, each refuses the other, both give up,
   and the same thing can repeat for ever. Epochs record which instance leads in them, so the two requests never carry
   the same epoch, and exactly one instance gives way.
10. **An instance whose request to lead fails waits before asking again.** In the implementation this wait should vary
    from one attempt to the next, which is the usual way of making repeated collisions unlikely; rule 9 is what
    guarantees they end.
11. **In the sequencer pair, a leader acts on a command its follower does not hold only once a voter other than itself
    has recorded that the follower may not lead.** The leader says on every lease request whether its peer may lead,
    as a numbered statement so that a late message cannot undo a newer one, and records it on its own disk first.
    Every leadership starts by saying the peer may not lead, and says it may only once the peer has acknowledged every
    record and the leader waits for its acknowledgements again. A voter that grants the request records the
    statement, and its grant echoes it; a voter refuses a lease to an instance a statement it holds says may not lead,
    and an instance holding one about itself does not ask to lead. Any majority that could elect the follower then
    includes a voter that knows it lacks commands the matching engine acted on. The design, its decisions and its
    tests are in [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md), and its model check in
    section 12 of [tla/findings.md](tla/findings.md).

## 4. Why two instances never act as leader at the same moment

For both instances to act as leader at the same moment, each would need two of the three votes at that moment. There
are only three voters, so at least one voter would have to be giving its vote to both instances at once. Rule 1 forbids
that for a voter that remembers its promises. Rule 6 covers a voter that has forgotten them by restarting. Rules 4 and 5
cover the instances in their role as voters.

Rule 2 is what makes this hold even when messages are delayed and clocks are only approximately in step. The instance
holding a lease always thinks the lease ends no later than the voter thinks its promise ends. So by the time a voter
feels free to grant a lease to someone else, the instance that held its previous lease has already stopped relying on
it.

None of this depends on how long a message takes to arrive. A message that is delayed, or never arrives, can make the
venue slower to choose a leader. It cannot make two leaders act at once. The only timing assumption is that the clocks
on the machines run at nearly the same rate. The instance holding a lease shortens it by the largest difference in
rate allowed, so a small difference cannot break rule 2.

The epoch fencing that receivers already perform stays in place as a second defence. It no longer carries the whole
burden of keeping two leaders apart.

## 5. What happens in each failure

The table describes a component pair. The arbiters behave the same way, with the witness as their third voter.

| What fails | What happens |
|---|---|
| Nothing | The leader renews its lease with both its peer and the arbiter. |
| The arbiter pool, entirely | **Trading continues.** The leader renews its lease with its peer alone. |
| The follower | Trading continues. The leader renews its lease with the arbiter alone. |
| The leader's process, restarted by its supervisor within the lease period | The restarted leader reads its record, asks to lead again at once, and its peer and the arbiter grant it, because both promised their votes to it. It keeps the lead, and the follower does not take over (rule 6). |
| The leader, with the arbiters running | The follower's promise to the leader runs out, and so does the arbiter's. The follower asks to lead, the arbiter grants it, and the follower takes over. |
| The link between the two instances | The leader renews with the arbiter. The follower asks the arbiter to let it lead, and the arbiter refuses, because it has promised its vote to the leader. |
| The link between the leader and the arbiter | The leader renews with its peer. Nothing changes for members. |
| The leader is cut off from both its peer and the arbiter | The leader's leases run out and it stops acting. The follower takes over with the arbiter's vote. No moment exists at which both act. |
| **The arbiter pool and the leader, together** | **Trading halts** until an arbiter is back. See section 7. |
| The two instances are cut off from each other, and the arbiters are down | The leader's lease runs out, and it stops. Neither instance can gather a majority, so nobody leads until a link or an arbiter returns. |
| Both instances start, and the arbiters are down | One of them leads with the other's vote. No arbiter is needed to start. If both ask at the same moment, rule 9 decides between them. |

**Losing the arbiter pool alone never stops trading.** The purpose behind allowing promotion without an arbiter was
exactly this: the venue should not stop trading because of the failure of a component that holds no venue state and is
not on the order path. That purpose is kept. What is given up is the ability of a follower to take over when it can
reach nobody at all.

## 6. An epoch can go backwards, and how the venue recovers

Model checking found one way in which a new leader can begin at an epoch lower than one the other instance has led in.
It needs the third voter to forget the highest epoch it has granted, which happens when it restarts or when a different
arbiter becomes active:

1. Instance 2 is elected with the arbiter's vote at epoch 6, before instance 1 has heard anything from it. Instance 1
   still holds epoch 1 on disk.
2. The arbiter restarts, forgetting epoch 6, and waits out its lease period as rule 6 requires.
3. Instance 2's lease runs out and it stops acting.
4. Instance 1 asks to lead at epoch 5, the next epoch above its own in which it leads. The arbiter, which no longer knows
   of epoch 6, grants it.

This does not break the rule that two instances never act at once: instance 2 had already stopped. But receivers that
saw epoch 6 discard everything instance 1 sends at epoch 5. Rule 8 is the recovery: instance 1 learns of epoch 6 from
its peer's refusal to renew, or from a receiver, stops, and asks again at epoch 9. When its peer is running, instance 1
learns of epoch 6 at its first renewal. When its peer is down, only a receiver can tell it, which is why rule 8 names
receivers as well as voters.

Two things narrow this further. The active arbiter tells the passive one the highest epoch granted in each group, so a
change of active arbiter does not forget it; only both arbiters restarting does. Rule 8's recovery from a voter's refusal
is implemented. Its recovery from a receiver is not yet: receivers refuse the lower epoch but do not tell the leader, so
a leader regressed while its peer is down stays refused until the peer returns. The functional specification records
this as a gap under R-0064.

## 7. The one case in which trading halts

Trading halts when **the whole arbiter pool and the leader fail together**. The follower then hears from nobody. It
cannot tell whether:

- the leader has died along with the arbiters, in which case taking over would be safe; or
- the follower itself has been cut off from everything, and the leader is still running and trading, perhaps with
  the arbiters still running beside it.

If it took over in the second case, two instances would accept and match orders at the same time, and the venue's
record of what traded would split in two. So the follower waits. Trading resumes when any arbiter returns, or when the
follower can reach its peer again.

A note for members, in words that can be used as they stand:

> The venue runs every critical component as a pair, and a separate set of arbiter processes on other machines decides
> which member of each pair is in charge. If a component in charge fails, its partner takes over, which requires the
> arbiters' agreement. If the arbiters are also unavailable at that moment, the partner cannot tell whether the
> component it would replace has really stopped or has only become unreachable. Taking over in that situation could
> leave two components each believing it is in charge, each accepting and matching orders, with no way afterwards to
> say which trades are valid. The venue therefore stops trading until the arbiters are available again, rather than
> risk that.

This needs two failures at once: every arbiter machine, and the machine running the leading instance. The arbiter pool
is itself a majority of three machines (section 8), so losing it entirely already means losing two of those three.

## 8. The arbiters themselves

The arbiters use the same rules among themselves, with the witness as their third voter. An arbiter is active only while
it holds a lease from the other arbiter or from the witness. An active arbiter that can reach neither stops being active
when its lease runs out, and grants no component leases after that. So at most one arbiter is ever active, and that is
what allows a component pair to treat "the arbiter" as a single voter.

The component lease and the arbiter lease can have different periods. An arbiter that becomes active waits one component
lease period before granting any component lease (rule 6). During that wait, component leaders renew with their peers
alone, so a change of active arbiter costs nothing when every component pair has both instances running.

## 9. Timing

The lease timings are configuration, set once in each environment file's `[shared]` section so that every voter uses the
same values: `lease_period_milliseconds` (3000), `lease_drift_allowance_milliseconds` (250) and
`lease_renewal_interval_milliseconds` (1000). The configuration check refuses a drift allowance below two ticks of the
lease rules, which run every 100 milliseconds, and a renewal interval longer than half of the period less the allowance.

A follower takes over roughly one lease period plus one renewal interval after its leader dies: its own promise to the
old leader must run out, and so must the arbiter's. With these values, `ha_test.py` scenario 1 measures a sequencer
failover of about five and a half seconds, including the new leader's first orders.

The drift allowance covers clocks running at slightly different rates, which PTP keeps to microseconds, and the moment
between a leader checking its lease and what it then sends leaving the machine. Each machine measures elapsed time on its
own steady clock, so a difference between two clocks' readings does not matter; only a difference in their rates does.

A shorter lease period gives faster failover. The cost is that a leader stalled for longer than the lease period, by a
long garbage collection pause in a Java component or a machine under heavy load, loses its lease and stops even though
nothing has failed.

## 10. Where it is implemented, and what checks it

**Code.**

- The rules are in `applications/fix_common/`: `LeaseVoter.hpp` (rules 1, 6 and 7), `LeaseHolder.hpp` (rule 2),
  `LeaseParticipant.hpp` (rules 4, 5, 9 and 10), and `PairLeaseAgent.hpp`, which drives them for an instance: asking,
  renewing, giving up a request that goes unanswered, answering the peer, and reporting what changed. `LeaseTiming.hpp`
  reads and checks the timings.
- The sequencer, the matching engine, the matching engine publisher and the arbiter each own a `PairLeaseAgent`, and
  supply only the connections it sends on. An instance that stops leading stops acting at once.
- The arbiter votes for the components through `applications/arbiter/ComponentLeaseVoters.hpp`, which applies rule 6 to
  an arbiter that has just become active, and carries the highest epoch in each group across a change of active arbiter.
- The witness is a `LeaseVoter`.
- A component instance keeps its promises in `applications/fix_common/LeasePromiseStore.hpp`. The sequencer's file is
  `lease_promise.state` in its write-ahead log directory; the matching engine's is its epoch file's name with
  `.lease_promise` added; each arbiter's is `[lease] promise_file` in its configuration. The matching engine
  publisher keeps no record, so it waits after every restart.
- The messages are `LeaseRequest`, `LeaseGrant` and `LeaseRefusal` in `leader_follower.dsl`.

**Tests.**

- `applications/sequencer/tests/LeaseRulesTest.cpp`: one or more tests per rule.
- `applications/sequencer/tests/LeaseSimulationTest.cpp`: a randomised simulation of a pair and its third voter, with
  messages delayed beyond a lease period or lost, links failing, and every party crashing and restarting, checking after
  every step that two instances never act as leader at once. A run in which the third voter's clock gains more than the
  drift allowance must find two leaders, which shows the check can fail.
- `applications/sequencer/tests/PairLeaseAgentTest.cpp` and `applications/arbiter/tests/ComponentLeaseVotersTest.cpp`.
  The agent tests restart a leader after its process has been down for 1.5 seconds, with and without its record, and
  show that it keeps the lead only with the record.
- `applications/sequencer/tests/LeasePromiseStoreTest.cpp`: the record is read back during the same boot, and ignored
  when it comes from another boot, is missing, or is damaged.
- `scripts/ha_test.py`, which runs the whole venue. Scenario 8 checks that an arbiter left with only its own vote does
  not become active, scenario 9 the halt of section 7, and scenario 15 each step of a follower being granted the lead.

**Requirements.** R-0146 states the majority rule, R-0147 the halt in section 7, and R-0059 the wait of an arbiter that
does not know what was promised. R-0093 is qualified: when the surviving instance is the follower, it needs an arbiter to
take over.

**How it was checked before it was built.** [tla/findings.md](tla/findings.md) section 11 gives the results. In summary:
two instances acting as leader at once was not found in any of seven exhaustive runs covering crashes, restarts of the
third voter and link failures, with between 4 and 85 million states each. Removing any one of rules 2, 5 and 6, or adding
promotion without a majority, produces two leaders acting at once within a few steps. With the third voter down for good,
the two instances always elect a leader between themselves, and they fail to without rule 9 or without the peer's vote.
The install step reruns those counterexamples with the TLC model checker
(`scripts/tla_trace_pages.py`, the `tla_trace_pages` target), and fails if any of them no longer
breaks the property it is listed against; without Java or `tla2tools.jar` the step is skipped with a
message.
