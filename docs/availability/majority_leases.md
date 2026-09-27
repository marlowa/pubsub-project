# Deciding leadership by majority, with leases: a design proposal {#majority_leases}

This document proposes how the venue decides which instance of a pair leads. It applies in two places:

- **A pair of component instances**, such as the two sequencers or the two matching engines.
- **The two arbiters**, which decide between themselves which one is active.

It is a proposal. Nothing in the code works this way yet. The design has been specified in TLA+ and model checked, and
[tla/findings.md](tla/findings.md) section 11 reports what that checking found. Section 10 below lists what would have to
change in the requirements, the tests and the code.

---

## 1. The problem it solves

When a follower stops hearing from its leader, it cannot tell which of two things has happened:

- the leader has died, and someone must take over; or
- the follower has been cut off, and the leader is still running and trading.

From where the follower sits, the two look exactly the same. If it takes over in the first case, the venue keeps trading.
If it takes over in the second case, the venue has two leaders. Every design has to decide what to do given that the
follower cannot tell the difference.

The design this proposal replaces lets an instance that can reach no arbiter promote itself. That keeps the venue
trading when the arbiters are down, but when the real cause is a cut-off follower it produces two leaders. Model checking
confirmed that this happens (findings 1, 2 and 4 in [tla/findings.md](tla/findings.md)). The same checking showed that
the arbiter pool, which the design documents describe as a majority of three, does not behave as one (findings 7 and 8).

This design removes promotion without an arbiter, and in its place uses a rule taken from the standard literature on
consensus: **an instance may lead only while a majority of three voters agrees that it should, and each voter's agreement
lasts for a fixed period and must be renewed.** The agreement that lasts for a fixed period is called a lease.

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

A voter that keeps its highest epoch on disk would narrow this, but would not close it. When a different arbiter becomes
active, the new one does not know the highest epoch the previous one granted. Rule 8 covers both cases, so this proposal
relies on it and keeps the arbiters free of stored state.

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

The lease period is a configuration setting. A follower takes over roughly one lease period plus one renewal interval
after its leader dies: its own promise to the old leader must run out, and so must the arbiter's. With a lease period of
three seconds and renewals every second, a failover takes about four seconds, plus the time the new leader needs to catch
up. The value to use has not been decided.

A shorter lease period gives faster failover. The cost is that a leader stalled for longer than the lease period, by a
long garbage collection pause in a Java component or a machine under heavy load, loses its lease and stops even though
nothing has failed.

## 10. What would change

**Requirements.**

- R-0095, "An instance may act with no arbiter reachable, and says that it did", would be replaced by a requirement
  that an instance leads only while it holds leases from a majority of its voters.
- R-0093, "The venue trades while a group is reduced to one instance", stays true with one qualification: when the
  surviving instance is the follower, it needs an arbiter to take over.
- The statement in the arbiter chapter that the venue goes on trading with both arbiters and the witness gone stays true
  for a leader whose peer is running.
- A new requirement would state the halt in section 7, with its rationale, so that it is a documented decision rather
  than a surprise.

**Test scenarios.** `ha_test` scenario 9, "degraded sequencer election", would expect the venue to halt and report
why, instead of expecting the secondary to promote itself. Scenarios 8, 35 and 39 also cite R-0095 and would need to be
checked against the new requirement. Scenario 6, in which both arbiters die and the leader continues, stays as it is.

**Code.**

- The degraded promotion paths in the sequencer and the matching engine would be removed.
- A leader would stop acting when its leases run out, which it does not do now.
- A follower would vote for its leader by answering its renewal requests, and would not ask to lead until its promise
  had run out.
- An instance asking to lead would give way to its peer's request at a higher epoch (rule 9), and would wait a varying
  time before asking again after a failed attempt (rule 10).
- The arbiter would grant component leases as a voter under rules 1, 6 and 7, instead of issuing decisions.
- The arbiters and the witness would follow the same rules among themselves. The witness would grant leases instead of
  answering one-off vote requests.
- The unique epochs of `fix_common/LeaderEpoch.hpp` stay. The rule that a leader stands down on hearing a peer leading
  at a higher epoch stays as well: under this design it should never be needed, and it costs nothing to keep.

**How it was checked.** [tla/findings.md](tla/findings.md) section 11 gives the results. In summary: two instances
acting as leader at once was not found in any of seven exhaustive runs covering crashes, restarts of the third voter and
link failures, with between 4 and 85 million states each. Removing any one of rules 2, 5 and 6, or adding promotion
without a majority, produces two leaders acting at once within a few steps. With the third voter down for good, the two
instances always elect a leader between themselves, and they fail to without rule 9 or without the peer's vote.

**The documents that disagree.** [wal_and_ha.md](wal_and_ha.md) says an instance that cannot reach the arbiter must not
promote, while the functional specification says it may. Under this design the two would agree.
