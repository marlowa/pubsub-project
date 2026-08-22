# HA Design Discussion Summary: 2-Node System, STONITH, and Cloud Portability

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
* **Tooling:** Systemd or local supervisors.

### Layer 2: Cluster Failure (The Outer Loop)
Handles node-level uncertainty: network partitions, OS hangs, or kernel
lockups.
* **Behaviour:** Use STONITH to resolve ambiguity.
* **Philosophy:** If we cannot prove the node is safe, we make it safe by
    removing it entirely.

---

## 7. High Availability Design: Process vs. Machine Recovery

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

## 10. Decision Taken: no STONITH

**STONITH was evaluated and is not implemented. It is not planned.** Sections 4, 6 and 7
above are the discussion that led here, not a description of what runs. Where section 7 says
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

## 11. Restart of a failed process: what role does it come back as?

Agreed 2026-08-22, while designing process supervision.

**A process that dies on a machine that is still alive should be restarted, and it should come
back as a follower unless it discovers there is no leader.** Coming back believing it leads is
a second leader, which is the split-brain the arbiter exists to prevent, so the restart can be
made as fast as we like without ever racing a promotion.

**The lowest-instance-id preference is a cold-start tie-break, not a leadership policy.** It
exists because at startup the two instances can come up in either order with a delay between
them, and something has to make that deterministic. It is the wrong rule for a restart, where
one instance may already be leading. The arbiter currently applies it to both -- see
`docs/bug_list.md`, "Rejoin after a promotion re-runs the cold-start tie-break".

The rule that distinguishes them:

* **If a leader is recorded for the group and that instance is still connected, it keeps
  leadership.** A restarted primary becomes the follower.
* **Otherwise the lowest instance id wins.** This covers a genuine cold start, and it covers
  the primary restarting to find the secondary's machine gone -- it cannot reach the peer, the
  arbiter sees no peer connection, and the primary takes leadership.
* **The arbiter decides in every case.** A restarting node never promotes itself, so no
  sequence of restarts can produce two leaders.

**Why the pair must be able to swap repeatedly.** If the secondary is promoted and later dies,
the primary -- by then a follower -- must be able to take over again. Roles are therefore
symmetric at runtime and only the identities are fixed: primary and secondary are permanent
names tied to instance ids, leader and follower are positions either can hold.

**Two consequences that are easy to miss.**

* **Resuming leadership must wait for reconciliation, not for the decision.** A restarted
  process has lost its state; that is why it restarted. A leader with an empty order book does
  not know what is resting, so a member's cancel for a live order is rejected and the venue has
  quietly lost state it still holds.
* **The grace period is sized by the restart, not chosen.** The follower waits before promoting
  so that a quick local restart makes promotion unnecessary. That wait has to exceed a
  supervised restart comfortably, or the venue fails over across machines for a failure that
  did not need it -- and it has to stay short enough that a genuinely dead machine is not
  tolerated for long. It cannot be picked before restart times are measured.

---

## 12. A supervisor starts processes; it does not decide leadership

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
comes back as a follower unless it finds there is no leader -- section 11. So automating the
restart does not invent a policy, it removes a human from a judgement that already has a right
answer.

### Which is exactly why the crash-loop guard is load-bearing

A person deciding whether to restart is doing something a loop does not: they are asking *why*
it died. If the cause was deterministic, restarting achieves another crash. Remove the person
and something must take that job:

* **A minimum runtime.** A process that dies sooner than this did not run, it failed to start.
* **A backoff before retrying**, so a deterministic fault produces a slow retry rather than a
  spin.
* **Restart as follower**, so a flapping instance cannot repeatedly take leadership from a
  healthy peer and then lose it.

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
* **A restart count per component**, published by whatever performs the restart. Deaths that are
  being papered over show up as a rate; a flap is a rising count against a flat `up`.

A third, once leadership state is exposed: **a gauge that reads 1 while a component group is
running without a standby**, so the window of no resilience is a query and an alert rather than
something known by whoever happened to be watching.
