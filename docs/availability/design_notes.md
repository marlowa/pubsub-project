# High Availability: the decision record {#ha_design_notes}

## 1. Initial Problem
Designing a high-availability (HA) system with the following constraints:
* **Two instances:** primary (leader) and secondary (follower).
* **Identification:** Each has a unique instanceID.
* **Preference:** Lower instanceID is preferred as the leader.
* **Goal:** Automatic failover without the complexity of a full consensus cluster.

### Core Concern
A 2-node system without a third party risks **split-brain**, where both nodes
assume leadership simultaneously.

---

## 2. Fundamental Limitation
A 2-node system cannot safely distinguish between a **node failure** and a
**network partition**. This leads to both nodes believing the other is dead,
promoting themselves, and causing data corruption. This is a fundamental
limitation related to the Byzantine Generals Problem and the CAP theorem.

---

## 3. Options for Resolution

### Option A: Add a Third Party (Arbiter)
Uses 2 nodes + 1 arbiter as a tie-breaker. Leadership requires a majority (2/3).
* **Status:** Safe, Simple, Common industry solution.

### Option B: STONITH (Fencing)
"Shoot The Other Node In The Head." Before promoting, a node must forcibly
disable the other via IPMI/BMC, Smart PDU, or Hypervisor API.
* **Status:** Prevents split-brain; preserves availability.

### Option C: Self-Fencing (Watchdog)
Each node kills itself if it cannot prove exclusivity.
* **Status:** Rejected; both nodes may die, causing total downtime.

### Option D: Shared Lock / Lease
External system (DB, KV store) grants leadership.
* **Status:** Works well; essentially a "third party" in disguise.

### Option E: Do Nothing
* **Status:** Unsafe; split-brain is inevitable.

---

## 4. STONITH Clarified
STONITH does **not** sacrifice availability. It resolves uncertainty by forcing
a known state.

**Requirements for Valid STONITH:**
* Must be out-of-band (separate management network).
* Must be authoritative (hardware-level power control).
* Must not depend on the data network being evaluated.

---

## 5. Environment and Cloud Portability
**Current Environment:** Bare metal (RHEL8, Solarflare NICs, Onload).
**Future Risk:** Management may push toward the cloud, where IPMI/BMC and
kernel bypass are unavailable.

### Solution: Abstract the HA Mechanism
Decouple the **Data Plane** (Low latency/hardware dependent) from the
**Control Plane** (Leadership/Fencing).

**Multiple Implementations:**
* **Bare Metal:** Uses STONITH (IPMI/BMC).
* **Cloud:** Uses lease-based systems (etcd, Consul).

---

## 6. Layered Recovery: The "Inner" vs "Outer" Loop
A well-designed HA system separates failure domains to avoid "over-fencing."

### Layer 1: Local Failure (The Inner Loop)
Handles process-level issues: core dumps, crashes, and transient bugs.
* **Behaviour:** Restart the process locally; no fencing involved.
* **Tooling:** `scripts/launch.py`, a per-process launcher that restarts what it
    starts and knows nothing else. See [section 12](#ha_supervisor_role) for why it deliberately has no
    say in leadership, and section 13 for why the restart must be automatic.

### Layer 2: Cluster Failure (The Outer Loop)
Handles node-level uncertainty: network partitions, OS hangs, or kernel
lockups.
* **Behaviour:** Resolve the ambiguity rather than tolerating it. This section
    originally said STONITH; **that was evaluated and rejected** and the venue
    does not fence. What stands in its place -- arbiter-mediated leadership and
    epoch checks on every PDU -- is [section 10](#ha_no_stonith), which also records why.
* **Philosophy:** If we cannot prove a node is safe, we must not let it act.
    Fencing achieves that by removing the node; the arbiter and the epoch achieve
    it by refusing everything the node sends.

---

## 7. High Availability Design: Process vs. Machine Recovery {#ha_process_vs_machine}

### The Core Philosophy
Treating a deterministic software failure the same as a non-deterministic
hardware failure leads to unnecessary complexity. We use two distinct loops.

### Dealing with Process Death (Inner Loop)
Hardware and kernel remain healthy. We use a **Shared Memory (SHM) Journal** approach for high-speed local recovery.

* **Mechanism:** State changes are written to a /dev/shm segment.
* **Mechanical Sympathy:** Like **Aeron Log Buffers**, this uses lock-free
    ring buffers for sub-microsecond latency.
* **Zero-Copy Persistence:** The Linux kernel preserves the SHM segment even
    if the process crashes.
* **Poison Pill Filter:** Recovery logic identifies if a specific sequence
    caused a crash loop and skips it.

### Dealing with Machine Death (Outer Loop)
Total silence from a node requires a "Last Resort" failover.

* **Trigger:** Heartbeat timer expires and the primary fails to reconnect
    after the local recovery grace period.
* **Safety:** Follower uses STONITH to ensure the primary is dead before
    promoting, preventing split-brain.
* **Asymmetric Strictness:** In the absence of an arbiter, use "Lowest
    Instance ID Wins" as a tie-breaker.

---

## 8. Summary Comparison Table

| Feature            | Local (Process) Recovery    | Network (Machine) Failover     |
| :----------------- | :-------------------------- | :----------------------------- |
| **Recovery Target**| Less than 50ms              | 100ms - Seconds                |
| **Data Locality** | Remains on same hardware    | Must be replicated/moved       |
| **Complexity** | Low (Single-host state)     | High (Distributed consensus)   |
| **TCP State** | Often preserved by retries  | Must be re-established         |
| **Primary Risk** | Poison Pill messages        | Split-Brain / Ghost Leaders    |

---

## 9. Final Conclusion
A 2-node HA system without fencing or an arbiter is fundamentally unsafe.
By using an **Aeron-like SHM Journal**, we solve 99% of failures (software
crashes) locally and instantly. This allows the **Leader-Follower network
protocol** to remain simple, acting only as the fallback for the rare 1% of
hardware disasters.

---

## 10. Decision Taken: no STONITH {#ha_no_stonith}

**STONITH was evaluated and is not implemented. It is not planned.** Sections 4, 6 and 7
above are the discussion that led here, not a description of what runs. Where [section 7](#ha_process_vs_machine) says
"Follower uses STONITH to ensure the primary is dead before promoting", that step does not
exist in the code and no node has ever been fenced by this venue.

**Why it was rejected.** Section 4's own requirements are the reason: valid STONITH must be
out-of-band, authoritative at the hardware level, and independent of the data network being
judged. Every one of those is a property of a *deployment*, not of this software -- an IPMI
or BMC path, a separate management network, credentials to power-cycle a peer. A developer
machine has none of them, and the venue has to run the same way on a laptop as on a pair of
production hosts. Building a mechanism that can only ever be exercised in one environment
means shipping a promotion path that is never tested where it is written.

**What is relied on instead** is Option A, and section 9's conclusion permits exactly this:
*"a 2-node HA system without fencing **or an arbiter** is fundamentally unsafe"*. The arbiter
is the answer to that "or".

* **Arbiter-mediated leadership.** A follower does not promote itself. It asks, and the
  arbiter decides from live connection state -- if the peer still holds a connection, the
  peer keeps leadership. Two nodes cannot both be told they lead.
* **Epoch fencing on every PDU, not only on commits.** Every cross-component PDU carries the
  sender's view of the relevant pair's leader epoch, and receivers check it before
  processing: equal accepted, lower discarded as stale, higher re-validated with the arbiter.
  A stale leader that believes it still leads is therefore rejected at every interaction it
  attempts, rather than at a commit boundary. This is a narrower guarantee than STONITH --
  the stale node keeps running -- but it is a stronger one than commit-time fencing, and it
  needs no hardware.
* **A fence file written on becoming leader**, covering split-brain between two instances on
  one host, where a network-level mechanism sees nothing to judge.

**What this costs, stated plainly.** STONITH resolves uncertainty by removing a node; this
does not. A node that is partitioned but alive keeps running and keeps being refused. That is
the accepted trade: it cannot corrupt shared state, because nothing it sends is accepted, but
it also does not stop, and it may hold resources until someone intervenes.

---

## 11. Restart of a failed process: what role does it come back as? {#ha_restart_role}

Agreed 2026-08-22, while designing process supervision.

**A process that dies on a machine that is still alive should be restarted, and it should come
back as a follower unless it discovers there is no leader.** If it came back believing it led,
there would be two leaders whenever the peer had already been promoted -- the split-brain the
arbiter exists to prevent.

Coming back as a follower also means the restart can be made as fast as we like. There is no
window in which a hurried restart might collide with a promotion that is already under way,
because whichever of the two happens first, the restarted instance ends up following.

**The lowest-instance-id preference is a cold-start tie-break, not a leadership policy.** It
exists because at startup the two instances can come up in either order with a delay between
them, and something has to make that deterministic.

It is the wrong rule for a restart, because by then one of the two may already be leading and
serving traffic. Applying a preference at that point moves leadership for no reason other than
which id is lower, and the instance it moves leadership *to* is the one that just failed. The
arbiter currently applies it to both cases -- see
`docs/bug_list.md`, BUG-0031.

The rule that distinguishes them:

* **If a leader is recorded for the group and that instance is still connected, it keeps
  leadership.** A restarted primary becomes the follower.
* **Otherwise the lowest instance id wins.** This covers a genuine cold start, and it covers
  the primary restarting to find the secondary's machine gone -- it cannot reach the peer, the
  arbiter sees no peer connection, and the primary takes leadership.
* **The arbiter decides in every case.** A restarting node never promotes itself, so no
  sequence of restarts can produce two leaders.

**Why the pair must be able to swap repeatedly.** If the secondary is promoted and later dies,
the primary -- by then a follower -- must be able to take over again.

So the two words must not be confused. **Primary and secondary are permanent names**, fixed to
instance ids in configuration, and they never change for the life of a deployment. **Leader and
follower are positions**, and either instance can hold either one at any time. A sentence like
"the primary is the follower" is not a contradiction; after one failover it is the normal
state.

**Two consequences that are easy to miss.**

* **Resuming leadership must wait for reconciliation, not for the decision.** A restarted
  process has lost its state; that is why it restarted. A leader with an empty order book does
  not know what is resting, so a member's cancel for a live order is rejected and the venue has
  quietly lost state it still holds.
* **The grace period is not a number to be chosen; it is measured.** The follower waits before
  promoting so that a quick local restart can make promotion unnecessary. Set that wait shorter
  than a restart takes and the venue fails over to another machine for a failure that did not
  need it. Set it far longer and a genuinely dead machine is tolerated for longer than it
  should be. The right value is therefore whatever a supervised restart actually takes, plus a
  margin -- which cannot be known until restarts have been timed, and is a reason to build the
  restart before tuning the timer.

---

## 11a. Where the restart bugs came from: treating two failures as one

Worth recording, because it explains a cluster of defects rather than any one of them.

**The original HA design treated process death and machine death as equivalent.** They are not,
and the difference is not about how the failure is detected -- it is about what the system is
left with afterwards.

When a machine dies, the pair is genuinely reduced to one instance. Nothing can be done about
that until the machine comes back; resilience is lost because a machine is lost.

When a *process* dies on a healthy machine, the pair need not be reduced at all. The machine is
still there, the failed instance can be restarted on it in seconds, and the pair can be whole
again almost immediately. **Treating this as machine death throws that away**: the peer is
promoted, the failed instance is left down, and the venue runs single until a human decides
otherwise -- carrying an unbounded exposure to a second failure for no reason other than the
first having been misclassified.

**That misclassification is what produced the restart defects found on 2026-08-21 and
2026-08-22.** If a restart is not part of the model, then nothing needs to decide what a
restarted instance comes back as -- and so nothing did:

* the follower's grace period existed but nothing filled it, so it was dead time before a
  promotion rather than a window for recovery (`docs/bug_list.md`, BUG-0029);
* the arbiter recomputed leadership from instance ids on every request, because "an instance
  rejoining while another leads" was not a case it had been asked to handle (BUG-0031);
* the matching engine adopted LEADER the moment its arbiter connection came up, because a
  primary starting was only ever imagined as a cold start ("A restarted primary matching engine
  promotes itself, producing two leaders").

Each of those reads as a separate bug and all three are the same omission.

**The requirement this yields, stated positively:** a process death must not cost resilience.
Whether the pair ends up whole again is the measure -- not whether the venue kept trading, which
a promotion achieves on its own while leaving one instance down and nobody watching.

## 11b. The arbiter arbitrates, and does nothing else {#ha_arbiter_only_arbitrates}

Decided 2026-08-22, when the sequencer turned out to be routing orders to whichever socket was
"the primary matching engine" rather than to whichever instance leads.

Three ways to tell the sequencer who leads were considered.

**The engine states its role, unqualified.** Simple, and it trusts a claim from the one party
whose confusion is the problem: an instance that has wrongly promoted itself announces
leadership with exactly the same confidence as one that has been told it leads.

**The arbiter tells everyone who cares.** Rejected, and not on cost. **The arbiter's job is to
arbitrate.** Making it also the distributor of leadership news gives it a second role, a list of
subscribers, and knowledge of who depends on which decision -- and a component that decides
*and* announces is on its way to being the thing [section 12](#ha_supervisor_role) warns about. It should answer the
question it is asked, by the party that asked it, and nothing more.

**The engine states its role, stamped with the epoch it holds it under.** Chosen. The sequencer
accepts a claim only when its epoch is at least as new as the last one it accepted for that
group, so an instance whose leadership has been superseded cannot reclaim routing: its epoch is
behind, and the claim is refused without anyone having to ask the arbiter anything.

This is the same mechanism the venue already uses everywhere else -- epochs travel on every PDU
precisely so a stale sender is detectable by the receiver -- and the same shape as
`StatusResponse`, which already carries `current_role` and `epoch` so that a restarting
sequencer can adopt follower without arbitration. The authority still rests with the arbiter,
because the epoch a claim carries is one the arbiter issued.

## 11c. An arbiter that restarts is told who leads; it does not remember {#ha_arbiter_relearns}

Decided 2026-08-22, after the restart coverage matrix asked what the arbiter's leadership state
depends on.

**The problem.** `leadership_state_` is held in memory and nothing reads it back at startup. It
is what stops a restarted primary taking leadership from a working secondary -- [section 11](#ha_restart_role) --
so an arbiter that has forgotten it applies the cold-start tie-break instead, hands leadership
to the lower instance id, and reproduces exactly the split-brain the rule was added to prevent.
Narrow while the peer survives and answers; live when the surviving arbiter is the one that
restarted, or when both restart.

**Three ways out were considered, and the choice was made on what it does to the arbiter's
job.**

Persisting the map would work and was rejected: it gives a component that deliberately holds no
venue state a persistence story, and a file written before a long outage asserts an incumbent
that may since have changed, so it needs epoch validation regardless.

Asking the peer works and is not sufficient on its own, because the case that matters most --
both arbiters restarted -- is the one with no peer to ask.

**What was chosen: the arbiter rebuilds the picture from what it is told.** Only a leader
heartbeats an arbiter, so a heartbeat is already a claim to lead; it now carries the role
explicitly rather than by implication, and the epoch settles disagreement. A restarted arbiter
learns who leads by listening, needs no persistence, and depends on nothing that might also
have restarted. **It arbitrates over facts it is told rather than facts it stored**, which is
the same boundary as [section 11b](#ha_arbiter_only_arbitrates): the arbiter's job stays narrow.

Asking the peer is kept as an accelerator rather than the mechanism. On a peer link coming up,
each arbiter replays what it knows as `ArbiterStateRecord`s -- a message that already exists and
that the peer already applies -- so in the common case a restarted arbiter is informed
immediately instead of waiting a heartbeat interval.

**The price, which is worth paying.** Between starting and being informed, an arbiter cannot
tell "nobody leads" from "I have not been told yet". During that window it **declines to
arbitrate** for a group it knows nothing about, rather than guessing. The asking component
retries; the venue carries on under whoever currently leads. Declining costs a retry, and
guessing costs the venue its order book.

Two consequences follow and are part of the decision.

* **A component must not read silence as absence.** A matching engine that asked and heard
  nothing used to self-promote on the assumption that no arbiter was listening. It now asks
  again, several times, before degrading -- because a silent arbiter may be one deliberately
  declining, and promoting against that decision produces the second leader the decline exists
  to prevent.
* **The heartbeat interval now sets how long ignorance lasts** when there is no peer to ask. It
  was chosen when a heartbeat meant liveness alone; now that it also carries leadership, thirty
  seconds is longer than it wants to be, and is worth revisiting on its own.

## 11d. A lease that never expires is not a lease {#ha_lease_expiry}

Found 2026-08-22 by the scenario written to check the cold-start tie-break, and worth recording
because two changes made the same day combined to produce it.

**The arbiter recorded who led and then believed it indefinitely.** The record exists to stop
leadership being moved away from an instance that is *serving*; it was being treated as a
statement about an instance that had merely served once. An instance can restart, come back as a
follower, and still be named as leader -- it is connected, and connection says nothing about
leadership.

**And it was made reachable by a fix.** Before, only leaders reached the arbiter, so a restarted
follower was not in its connection table and the incumbent check failed, falling through to the
cold-start rule -- accidentally right. Making both roles report liveness was correct on its own
merits, and turned that accident into a trap.

**Three ways the stale belief survived, all of which had to be closed:**

* **The record outlived the process.** It is now cleared when the recorded leader disconnects.
  Disconnection is the precise signal: a leader that has genuinely blipped re-leases the instant
  it reconnects, so nothing healthy is unseated for longer than a reconnect.
* **The lease never expired.** A time-to-live backstops it, for a leader that holds its socket
  open and stops renewing -- which from the outside is indistinguishable from a healthy one. The
  renewal interval was shortened from 30s to 3s to make a 10s TTL meaningful; at 30s a healthy
  leader would expire between renewals.
* **The invalidated record was handed back out.** This is the one that reasoning alone missed.
  The arbiter tells a reconnecting instance what it last decided, and it was doing so from the
  unconfirmed record -- so the restarting instance was told it led, believed it, leased, and
  resurrected the belief that had just been invalidated. Only a confirmed record is now offered.

**The general rule this leaves:** leadership is a claim with a lifetime, not a fact on file. The
arbiter believes it while it is being renewed and while the claimant is present, and stops
believing it otherwise -- which is what makes it a lease rather than a record.

## 11e. The epoch must outlive the process, and not everything may issue one

The epoch is the venue's generation counter for leadership. Every component checks it on
every PDU and rejects anything from an older generation. That check is the whole of the
fencing, and it works only while the counter never goes backwards.

Held only in memory, it went backwards. It started at zero on every start, so a pair
restarted together both came back at zero, elected a leader between them, and stamped
messages with a generation the venue had used long ago. Downstream could not tell the new
leader from the old one, and a message left over from the earlier generation compared as
current. Restarting one node at a time hid this completely, because the survivor told the
returning node what generation it was in -- which is section 11a's mistake again, in a new
place: process death and machine death are not the same failure.

The epoch is now written to a small file that outlives the process, one per instance, next
to the WAL. Writes go to a temporary file which is flushed and renamed over the real one,
and the directory is flushed too, so a reader sees either the old epoch or the new one
whatever moment the process dies at, and the rename survives loss of power rather than
merely loss of the process. A missing or damaged file reads as zero, which is right for a
new deployment and safe for a damaged one: zero loses to every real epoch, so the node
defers instead of claiming a generation it cannot substantiate.

### Who is allowed to issue a generation

The first answer was that the arbiter should be the only one, on the argument that fencing
with two issuers is not fencing. Two issuers can fail two ways:

* **Collision.** The pair issues epoch 11 naming instance 1 while the arbiter issues epoch
  11 naming instance 2. Downstream sees two leaders in the same generation and has no basis
  to prefer either.
* **Regression.** An issuer produces an epoch that is not greater than one already in
  circulation. This was the live bug: local election never advanced the counter at all.

That answer was wrong, and it was measurement that showed it. Routing every election to the
arbiter produced no decisions at all -- in one run the arbiter received 17 reports and
issued none, and every election fell through to the degraded path. The reason is section
11c: an arbiter that has just started declines to arbitrate rather than guess, and a venue
starting up is precisely when elections happen. The rule and the requirement could not both
be satisfied.

There is a better reason to reject it than the timing. **In the case where neither node
holds a role and the two can see each other, the arbiter is the least-informed party in the
system.** The two peers hold every fact the decision needs: both instance ids, both epochs,
and the knowledge that neither is leading. The arbiter holds none of it first-hand and says
so. Referring the question from the side that has the facts to the side that has not is the
wrong direction.

**The rule that replaced it:**

* **The peer is visible.** The peers settle it themselves. Leadership goes to the lower
  instance id and the new generation is one past the higher of the two epochs. Both sides
  compute both values from the same two inputs, so they reach the same answer without
  needing to agree on anything further. `max` is symmetric, so they still agree when one of
  them has lost its stored epoch and the other has not.
* **The peer is not visible.** The arbiter decides. This is the case a node cannot settle
  alone, and the only one where a second claimant might exist unseen -- which is what the
  arbiter is for.

**Why that is not two issuers after all.** The two are mutually exclusive by construction. A
node only resolves locally when it can see its peer, and a peer that is visible is running
the same local resolution rather than reporting. The arbiter only issues on receiving a
report, and a node only reports when it cannot see its peer. So for any group, at any
moment, exactly one of them is in a position to issue. Regression is closed separately, by
both of them using the same rule -- strictly greater than every epoch known -- which is only
truthful because the epoch now survives a restart.

The residual case is visibility that is briefly asymmetric: A sees B while B does not see A,
so B reports while A resolves locally. The lease closes it. A node that takes leadership
tells the arbiter immediately rather than at the next heartbeat, and an arbiter holding a
confirmed incumbent confirms that incumbent instead of issuing a fresh generation. Measured,
the lease reaches the arbiter around a tenth of a millisecond after leadership is adopted.

**This is weaker than a single issuer and deliberately so.** Sole-issuer was a structural
guarantee; this one depends on visibility being symmetric, with the lease narrowing the
window where it is not. What it buys is a venue that elects a leader in about a second
instead of having no leader for the ten seconds an arbiter spends learning. Anyone tempted
to restore the tidier rule should re-read the measurement above first.

### The sequencer and the matching engine are deliberately different

The sequencer resolves in **both** directions between peers: it may take leadership as well
as give it up. It can, because both sides exchange status and hold both epochs at the same
moment, so both can compute the same answer.

The matching engine only ever **gives leadership up**. Its peer exchange is a one-way
announcement, so there is no moment at which both sides hold both epochs and no basis for a
symmetric rule. It adopts follower on the peer's word and takes the peer's generation rather
than inventing one, so it never issues at all. Deferring cannot create a second leader
whatever the peer is confused about, which is what makes acting on the peer's word alone
safe. It is guarded three ways: the peer must say it is leading, this instance must still
hold no role, and the peer's epoch must not be older than one this instance has seen.

That asymmetry is a consequence of what each protocol carries, not an inconsistency to be
tidied away.

### What persistence broke on the way in

Worth recording, because all three have the same shape and a fourth of the same kind is
likely. Each was a rule that held while the epoch was ephemeral and stopped holding once it
survived a restart -- because a restarted node went from losing every epoch comparison
automatically to being a credible claimant.

* **Two followers and no leader.** A peer with a higher epoch was treated as a newer
  generation to defer to. Once epochs persist, two nodes' stored values drift apart in normal
  operation, and the rule started reading as "whoever stored the bigger number leads". The
  node with the lower epoch deferred by that rule and the other deferred by the instance-id
  rule. Only a peer that *says* it is leading is deferred to now; having seen more
  generations does not put a node in charge.
* **Two leaders.** Election ran on the status *query*, which carries an instance id and an
  epoch but not the sender's role, so a node already leading looked exactly like one holding
  nothing. A node restarting beside a healthy leader made itself a second leader before the
  reply that would have said so arrived. Election now runs only on the *response*, which
  carries the role.
* **Silent drift.** A follower never adopted the leader's epoch -- the heartbeat handler
  rejected stale heartbeats but never followed a newer one -- so promoting a lagging follower
  produced a generation the venue had already used.

## 12. A supervisor starts processes; it does not decide leadership {#ha_supervisor_role}

These are two jobs and they must not be the same component.

* **The supervisor** answers one question: is this process running, and if not, start it. It
  needs to know only about its own machine.
* **The arbiter** answers a different one: which instance leads. Both nodes ask it; neither
  decides for itself.

**Why the separation matters more than it looks.**

* **It keeps the supervisor optional.** Nothing about correctness depends on it, so a developer
  can run every component by hand on one machine -- as `scripts/devenv.py` does today -- and the
  system behaves identically. A supervisor that also assigns roles becomes mandatory, because
  without it nothing knows what it is.
* **It keeps the configuration small.** A component that assigns roles has to know the whole
  topology. A component that only starts processes needs to know its own machine and nothing
  else.
* **It stops an outer layer overruling an inner one.** A supervisor that forms its own opinion
  about whether a thread is alive, and acts on it, does not detect outages so much as cause
  them when the opinion is wrong. Liveness within a process is the reactor's own backstop to
  judge; the supervisor's business is whether the process exists.

**The failure this avoids, in both directions.** A promotion scheme with no third party cannot
distinguish "the peer is dead" from "I cannot see the peer", and both answers are available to
it. Promote on the second and the pair ends up with **two actives**, each certain it is right
and neither able to be told otherwise, diverging state as they go. Decline on the first and it
ends up with **two standbys**, nothing serving at all -- the safer failure but often the later
noticed, because nothing crashes and nothing logs an error; traffic simply stops.

Both are observed failure modes of active/standby schemes that promote without an authority.
Avoiding them is the reason this design has an arbiter, and the reason a supervisor here is
deliberately ignorant of roles.

**Why an arbiter rather than a consensus algorithm.** Raft or Paxos would also prevent both, at
the cost of implementing and then maintaining a consensus protocol inside a trading venue. An
arbiter gets the same guarantee for this topology far more cheaply: one party decides, so only
one leadership assignment exists at a time, and epoch checks on every PDU stop a stale leader
acting on an assignment that has since been replaced. The arbiter is itself made available by
running a pair plus a witness -- a small, bounded amount of the same problem, solved once in a
component that holds no venue state.

---

## 13. Why restart must be automatic, and why that makes monitoring compulsory

### The window of no resilience

When the leader dies and the follower is promoted, the pair is now a single instance. It stays
that way until something restores the second one. If that restoration is a human decision --
someone choosing whether to bring the failed instance back -- then the window is **unbounded**.
It might be minutes; it might be until the next working day. A second failure inside it is an
outage, and the venue has been one failure from an outage for however long nobody acted.

Closing that window is the reason automatic restart exists. It is not a convenience for
operators; it is the difference between a bounded and an unbounded exposure.

**The decision being automated is the one already designed.** Restart the failed instance and it
comes back as a follower unless it finds there is no leader -- [section 11](#ha_restart_role). So automating the
restart does not invent a policy, it removes a human from a judgement that already has a right
answer.

### Removing the human also removes a judgement, which has to be replaced

A person deciding whether to restart is doing something a loop does not: they are asking *why*
it died. If the cause was deterministic, restarting achieves another crash. Remove the person
and something must take that job:

* **A minimum runtime.** A healthy component runs for hours. One that exits a couple of
  seconds after starting has not run and stopped, it has failed to start -- so treat a death
  inside that window as a failed start rather than as a normal exit, and count it.
* **A backoff before retrying**, so a deterministic fault produces a slow retry rather than a
  spin.
* **Restart as follower**, so that an instance which is dying repeatedly cannot take leadership
  from a healthy peer, die again, and hand it back -- disrupting a peer that was working
  perfectly well, once per crash.

Those three together are what make it safe to remove the human, and none is optional.

### Automatic restart makes monitoring compulsory rather than optional

A process that dies and stays dead is eventually noticed, even by a system nobody is watching:
something downstream stops, and someone complains. **A process that dies and is silently
restarted forty times an hour is never noticed at all.** The symptom disappears while the fault
continues. Automating recovery therefore raises the need for monitoring rather than reducing it.

This matters most for components whose failure no user can feel. A gateway dying is noticed
because members cannot trade. A publisher dying may produce no symptom any member can observe,
so under complaint-driven monitoring it can stay dead indefinitely, losing data quietly the
whole time. **Detection must not depend on a component being externally visible.**

Two signals are wanted, and the first needs no new code:

* **Liveness per component.** Prometheus synthesises an `up` series for every scrape target, so
  `up{job="pubsub_venue"} == 0` already names any component that has stopped answering. Fifteen
  targets, no instrumentation required, and it covers the invisible components exactly as well
  as the visible ones.
* **A restart count per component**, published by whatever does the restarting. This is not a
  duplicate of the first signal, because a fast restart can be invisible to it: with a five
  second scrape interval, a process that dies and is back within a second may never miss a
  scrape at all, so `up` stays at 1 throughout and nothing looks wrong. A steadily rising
  restart count beside an unbroken `up` is the only evidence that anything is happening.

A third, once leadership state is exposed: **a gauge that reads 1 while a component group is
running without a standby**, so the window of no resilience is a query and an alert rather than
something known by whoever happened to be watching.

---

## 14. Monitoring is not the control plane

### The rule that decides this

**If something can be switched off, nothing may depend on it to work correctly.**
`MetricsConfiguration::enabled` defaults to `false`, so a venue running with no Prometheus at
all is not merely supported -- it is what you get unless you ask for otherwise. No recovery
decision may therefore depend on it, or the default deployment is the broken one.

The same rule decides two other things in this document. The role is a command-line argument,
so no supervisor is needed for a process to know what it is; and a launcher is optional, so it
cannot decide leadership. Both are [section 12](#ha_supervisor_role).

**Letting the system depend on something means it can never be absent again.** Every
deployment then has to run it, configure it and keep it healthy -- a production pair of
machines, a developer's single laptop, and every automated test run alike. Prometheus is
currently in none of those: `devenv.py` can start a venue without it, and the unit tests never
see it. Make recovery depend on it and all three have to change. That is the price to weigh
whenever something new is about to become a thing the venue cannot run without, and it is why
that list should stay short.

### Three jobs travel under the word "monitoring"

**Detection that drives control.** This is the system noticing that something has failed and
acting on it by itself: restarting a process that has died, or promoting a follower because the
leader has gone. Nobody is asked and nobody is waiting, so it has to happen quickly, and it has
to keep working on a machine where nothing else has been installed.

Two things follow from that. It has to be **local**, because a decision taken on one machine
must not wait on something running elsewhere that may be unreachable for the very reason the
decision is being taken. And it has to be **immediate**, because the point of acting
automatically is to have finished before anyone notices.

The venue already has everything it needs, and in each case the failure is *reported* to it
rather than worked out from something that has stopped happening:

* a launcher calls `waitpid` and it returns -- the operating system is telling it that its
  child has died, and telling it the exit status, at the moment it happens;
* the follower's replication socket closes -- TCP is telling it the peer has gone, within about
  a second, exactly as it did at 19:08:42 during the run on 2026-08-21;
* a component's connection to the arbiter drops, and the arbiter removes it from its connection
  table there and then.

None of those involves polling, waiting for a timeout to expire, or concluding anything from
data that has stopped arriving. The difference matters and it is the reason this job cannot be
given to a metrics system. Being told is both fast and unambiguous. Noticing an absence takes at
least as long as the interval you were expecting something at, and even then it cannot
distinguish a process that has died from one that is merely slow, or from a network path that
has broken between the two of you.

**Notification to a person.** "The publisher has been dead for an hour and nobody noticed."
This is what a metrics and alerting stack is for. Waking someone is inherently slow, so a scrape
interval is a perfectly good granularity for it.

**Reporting an event.** "This process died at 06:12 and I restarted it, for the third time this
hour." The component that acted is the authoritative source, because it knows what happened and
why. A poller can only ever infer it after the fact.

Only the middle one belongs to Prometheus.

### What `up == 0` actually means

It is a **scrape failure**, not a process death, and it fires identically for a dead process, a
wedged metrics thread inside a healthy one, and a broken path to the endpoint. That ambiguity is
fine when paging a human, who will go and look. It is not an acceptable input to a restart
decision. The timing says the same: scrape interval, plus rule evaluation, plus whatever `for:`
duration stops it flapping -- tens of seconds before anything fires, against a `waitpid` that
already knew.

### Who watches the watchers

The regress does not terminate by adding another watcher. It terminates when each watcher's
failure is **covered, loud, or safe**.

* **Covered -- redundancy.** The arbiter is a pair plus a witness. Nothing watches the arbiter;
  one arbiter dying simply does not matter. This is why an arbiter beats a consensus protocol
  for this topology: the small amount of consensus is confined to a component holding no venue
  state.
* **Loud -- a dead man's switch.** The answer to "who watches the monitoring". An alert that
  always fires, routed to something that raises the alarm when it *stops* arriving. Silence
  becomes the signal. Without it a dead monitoring system is indistinguishable from a healthy
  venue, which is the worst failure available to it.
* **Safe -- fail into a harmless state.** A launcher that dies leaves its child running: the
  process is reparented and keeps trading, having lost only its restart capability. A
  degradation rather than an outage.
* **And, where it is cheap, let the watched watch the watcher.** A component can check
  `getppid()`; a return of 1 means its launcher has died and it has been reparented to init.
  Local, exact, free, no infrastructure. The pair watch each other and the regress closes at
  two. Worth doing because it removes the one silent failure the launcher design would otherwise
  introduce -- a component running unsupervised with nothing saying so.


## 15. Automatic recovery ends where loss begins {#ha_recovery_ends_at_loss}

Decided 2026-08-28, after [BUG-0064](../bug_list.md#bug_0064) showed that a venue which has lost
orders resumes trading without saying so.

[Refusing orders the venue cannot process](order_acceptance.md) rejected requiring an operator to
re-enable acceptance, and the argument was good: BUG-0009 is a case where nobody watched for seven
minutes, and a design whose safety rests on the watching that has already failed is not safer. That
still holds. **It holds for a brief outage and not for a long one**, and the distinction is not
duration -- it is whether anything was lost.

**The principle: automate recovery from conditions where nothing was lost; require a person where
something was.**

Resuming automatically is safe when the venue comes back with everything it took. It is wrong when
it does not. Today, after every matching engine has gone and one starts cold, the orders deferred
before refusal began are stranded -- never applied, never answered -- and the venue starts accepting
again and says nothing to anybody. **Automatic recovery there does not merely fail to help; it
conceals the damage.** A venue that reopens quietly having lost orders is worse than one that stays
shut.

### What was missing: the venue could not get louder

Before this, an outage of thirty seconds and one of three hours were indistinguishable. The same
warning every five seconds, the same rejection text, indefinitely. The venue could heal itself and
could not **escalate** -- it had no way to say "this has stopped being a blip", and no state beyond
"not accepting" to move into.

That is also what a member experiences. A stream of per-order rejections is the wrong way to tell
anyone the venue is down: it is repeated for every order, it invites retries, and it never says
*stop asking*. Members will not tolerate it indefinitely, and they should not have to.

### Three states, and only the last needs a person

| State | Entered when | Left by |
|---|---|---|
| **Defer** | An engine is gone and a failover is plausible | Itself. Measured: 27 orders deferred over a 14-second gap, all answered |
| **Refuse** | The outage outlives a failover, or the arbiter reports no engine registered | Itself, when an engine returns |
| **Halt** | No matching engine service exists, or orders are known to have been stranded | **A person** |

The first two are automatic because they are cheap to get wrong in the safe direction: refusing an
order the venue could have taken costs a member one rejection it can retry. The third is not. By the
time it is reached somebody has to answer *what became of the orders we took*, and that is a
judgement rather than a timer.

### A halt is declared, not inferred

The venue has no notion of a trading halt --
[BUG-0065](../bug_list.md#bug_0065). It should, and it should be a first-class state rather than a
side effect of refusing everything.

**A member learns of it from the reply to the order it sent.** That is how it works on a venue
Andrew has operated: the matching engine holds a halted state, and a member placing an order is told
so in the FIX response. It is the mechanism that must exist, because it is the answer to the
question the member actually asked, and it reaches a member that is trading -- which is the member
who needs to know.

**A broadcast should exist as well, and does not replace it.** `TradingSessionStatus` (35=h)
carrying `TradSesStatus=2` (Halted), and `1` (Open) when it lifts. The reply alone leaves a
connected but idle member unaware, leaves a member reconnecting into a halt to discover it by
trial, and gives nothing to a member that has stopped sending. One message with a defined exit is
what stops a halt being something members work out.

**A halt must be rare, and the ladder is built to keep it so.** Observed in practice: halts get
declared at the drop of a hat, and a venue that halts readily trains its members to ignore the
signal. The wish to automate that away is right; the error would be automating it so completely
that no halt is possible. Deferring and refusing are automatic and self-clearing precisely so that
the halt below them is reached rarely -- everything recoverable is recovered before it, and what
remains is the class of condition where a person genuinely must decide.

**Where the state lives matters here in a way it may not elsewhere.** Holding it in the matching
engine is natural when the engine is up and the halt is a trading decision. It cannot be the whole
answer for this venue, because the condition that drove this design is *there is no matching
engine*. A halt entered because no engine exists has to be held by something that outlives the
engine -- the sequencer, or the gateways, or both -- or the state disappears exactly when it is
needed.

It is worth having independently of any of this. A venue needs to halt for reasons that have nothing
to do with a failure -- a scheduled pause, a market-wide event, an instrument suspension -- and
today it cannot express any of them.

**Halting is not the same as refusing**, and conflating them would be a mistake. Refusing is a
statement about capacity: the venue cannot process this order now, and will accept again by itself.
Halting is a statement about the venue's condition: trading has stopped, it will not restart on its
own, and a person is dealing with it.
