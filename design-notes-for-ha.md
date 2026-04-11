HA Design Discussion Summary (2-Node System, STONITH, and Cloud Portability)

1. Initial Problem

Designing a high-availability (HA) system with:

Two instances: primary (leader) and secondary (follower)
Each has an instanceID
Lower instanceID preferred as leader
Automatic failover required
Avoid complexity of full consensus (no cluster)

Core Concern

A 2-node system without a third party risks split-brain.

2. Fundamental Limitation

A 2-node system cannot safely distinguish between:

Node failure
Network partition

This leads to:

Both nodes believing the other is dead
Both promoting themselves to leader
Split-brain condition

This is a fundamental distributed systems limitation (related to Byzantine Generals / CAP theorem).

3. Ways to Solve the Problem

Option A — Add a Third Party (Arbiter)

2 nodes + 1 arbiter (tie-breaker)
Leader requires majority (2/3)
Prevents split-brain cleanly

✔ Safe
✔ Simple
✔ Common industry solution

Option B — STONITH (Fencing)

STONITH = Shoot The Other Node In The Head

Mechanism:

Before promoting, a node must forcibly disable the other node
Typically via:
    IPMI / BMC (power off/reset)
    Smart PDU
    Hypervisor API

Key rule:

No successful fencing → no promotion

✔ Prevents split-brain
✔ Preserves availability (failover proceeds)
✔ No third node required

Option C — Self-Fencing (Watchdog)

Each node kills itself if it cannot prove exclusivity
Uses hardware/kernel watchdog

✔ Prevents split-brain
❌ Both nodes may die → downtime

Rejected due to availability requirements.

Option D — Shared Lock / Lease

External system grants leadership (DB, KV store, etc.)

✔ Works well
✔ Essentially a “third party” in disguise

Option E — Do Nothing (2 nodes only)

❌ Unsafe (split-brain inevitable)

4. STONITH Clarified

Key Insight

STONITH does NOT sacrifice availability (unlike self-fencing).

Instead:

It resolves uncertainty by forcing a known state

Requirements for Valid STONITH

Must be out-of-band
Must be authoritative
Must not depend on the same network being evaluated

Example Flow

Node B detects A is unreachable

B requests BMC to power off A
If successful → B becomes leader
If not → B must NOT promote

Failure Case

If both nodes lose:

connectivity to each other
AND access to BMC network

Then:

neither can fence
neither promotes

✔ Safe
❌ Temporarily unavailable

This is unavoidable in any correct system.

5. Environment Constraints

Current

Bare metal (RHEL8, gcc 8.5)
Solarflare NICs
Onload kernel bypass (low latency)

Implication

Likely no VMs
Tight hardware coupling

6. New Concern: Cloud Portability

Management may push toward cloud.

Problems:

No IPMI/BMC in cloud

Limited hardware control

Kernel bypass may not be available

Risk:

Design becomes unusable in cloud → project rejected

7. Key Architectural Insight

Split system into:

Data Plane
    Low latency
    Solarflare / Onload
    Hardware dependent

Control Plane
    Leadership
    Failover
    Fencing / arbitration

👉 These must be decoupled

8. Solution: Abstract the HA Mechanism

Define an interface:

interface ExclusivityProvider {
    bool acquire_leadership(node_id);
    bool renew_leadership(node_id);
    void release_leadership(node_id);
}

HA logic depends on this abstraction, not implementation.

9. Multiple Implementations

Bare Metal (Current)
    Uses STONITH (IPMI/BMC)
    Acquire leadership = fence peer

Cloud (Future)
    Uses lease-based system
    Examples:
        etcd
        Consul
        simple arbiter service
Acquire leadership = obtain lease

Hybrid

Arbiter even on-prem (optional)

10. Unifying Principle

STONITH and leases both solve:

Ensuring exclusive leadership

Method	Mechanism
STONITH	Destroy other node
Lease	Get permission from third

11. Final Design Direction

Requirements:

No split-brain
Automatic failover
High availability
Future cloud compatibility

Recommended Approach:

Implement HA using abstract exclusivity provider
Provide:
    STONITH backend (bare metal)
    Lease backend (cloud)

12. Key Rules for Correctness

Never promote without exclusivity guarantee
InstanceID is only a tie-breaker, not authority
Loss of exclusivity → immediate step-down
Control plane must be independent of data plane

13. Positioning for Stakeholders

If challenged:

“The HA mechanism is abstracted.
On-prem deployments use hardware fencing for correctness.
Cloud deployments use lease-based coordination.
The application logic is unchanged.”

14. Final Conclusion

2-node HA without fencing or arbiter is unsafe

STONITH is a valid and widely used solution
Self-fencing rejected due to availability loss
Future-proof design requires abstraction
Control plane portability is the key to cloud readiness

15. Next Steps (Optional Future Work)

Define full HA state machine
Specify message protocol (request/renew/release)
Handle race conditions (simultaneous fencing attempts)
Integrate into DSL/codegen system

🧠 Think in Layers (this is the missing piece)

A well-designed HA system has two separate layers:

1. Local failure handling (process-level)

This handles:

core dumps

crashes

bugs

transient failures

Typical behaviour:

restart the process

fail over locally if needed

no fencing involved

👉 This is where most failures happen.

2. Cluster-level failure handling (node-level uncertainty)

This handles:

node unreachable
network partition
OS hang
scheduler stalls
kernel lockups

👉 This is where STONITH is used

⚠️ Why This Distinction Matters

Let’s walk your exact concern:

Scenario: process core dumps on Node A

Correct behaviour:
    Process dies
    Local supervisor (systemd, etc.) restarts it
    Node A remains leader (or briefly unavailable)
    No STONITH occurs

👉 This is normal and frequent.

🚨 When STONITH does trigger

STONITH is used when something like this happens:

Scenario: Node A becomes unresponsive

B cannot contact A
A might still:
    be running
    be sending messages
    be writing state

B must decide:

    “Is A dead?”
    or
    “Is the network broken?”

It cannot know.

So:

B must fence A before taking over

🧠 Key Insight

STONITH is about:

    uncertainty at the node level, not process failure

💡 Why Killing the Whole Machine Is Actually Rational

Because partial failure is the dangerous case:
    process is alive but misbehaving
    OS is wedged but not dead
    network stack is broken one way

These are far worse than a clean crash.

👉 They can cause split-brain or corruption

So the rule becomes:

    If we cannot prove the node is safe, we make it safe by removing it entirely.

🔍 Real-World Perspective

In production HA systems:

    Process crashes → handled locally, no fencing
    Node ambiguity → fencing

STONITH events are:

    relatively rare
    but critical when they occur

⚖️ What Happens If You Don’t Use STONITH

In your system:

Without fencing:
    A is slow / partitioned
    B promotes
    A is still running

👉 both active → split brain

Now compare:

Approach	Outcome
No STONITH	silent corruption / double processing
STONITH	one node forcibly stopped

In most domains (especially finance):

👉 STONITH is the lesser evil

🧩 How Systems Avoid “Over-Fencing”

Good HA systems are careful:
    multiple missed heartbeats required
    timeouts tuned conservatively
    sometimes multiple channels checked
    fencing is last resort, not first reaction

💡 Design Guidance for You

You can explicitly encode this philosophy:

Separate failure domains in your design

Local supervision:
    process restart
    crash recovery
    fast and frequent

Cluster logic:
only triggers after:
    sustained loss of communication
    inability to verify peer state

🧠 Mental Model Upgrade

Instead of:

“STONITH kills machines when processes crash”

Think:

“STONITH resolves uncertainty when a node might still be dangerous”

most failures are process-level; STONITH is not used for those.
It is used only when:
    node state is uncertain
    and correctness is at risk

👉 Properly used, STONITH is:
    rare
    deliberate
    and essential for safety

# High Availability Design: Process vs. Machine Recovery

## 1. The Core Philosophy: Layered Recovery
In high-performance systems, treating a **process crash** (deterministic software failure) the same as a **machine death** (non-deterministic hardware failure) leads to unnecessary complexity and slower recovery. A robust design treats these as two distinct "loops" of availability.

---

## 2. Dealing with Process Death (The "Inner Loop")
Process death occurs when a software bug leads to a core dump or a kernel-initiated termination (e.g., OOM). The hardware and kernel remain healthy, providing a unique opportunity for high-speed, local recovery.

### The Shared Memory (SHM) Journal Approach
Instead of triggering a network failover, the system uses a local **State Journal** to recover in place.

* **Mechanism:** The application writes its state changes and inbound sequence numbers into a pre-allocated `/dev/shm` segment.
* **Mechanical Sympathy:** Like the **Aeron Log Buffer**, this uses lock-free, concurrent ring buffers. Writing to SHM is a memory-to-memory operation, maintaining sub-microsecond latency.
* **Zero-Copy Persistence:** The journal acts as a "Warm Handoff." When the process crashes, the Linux kernel preserves the SHM segment.
* **Local Restart:** A supervisor (e.g., systemd) restarts the process on the same CPU core. The new instance re-attaches to the existing SHM segment and replays the journal.
* **The Poison Pill Filter:** Recovery logic checks if a specific sequence number caused the previous crash. If a restart occurs at the exact same sequence multiple times, the message is skipped to prevent a crash loop.

---

## 3. Dealing with Machine Death (The "Outer Loop")
Machine death (power loss, kernel panic, or motherboard failure) results in total silence from the node. This is a connectivity problem that requires a peer to take over.

### The "Last Resort" Failover
* **Trigger:** Failover is triggered only when the Heartbeat timer expires **and** the primary node fails to reconnect after the local recovery grace period.
* **Safety via STONITH:** If a follower suspects a machine death, it may use "Shoot The Other Node In The Head" (STONITH) to ensure the primary is truly dead before promoting itself, preventing "Split-Brain."
* **Asymmetric Strictness:** Without a third-party arbiter, the system relies on pre-ordained logic (e.g., "Lowest Instance ID Wins") to decide which node is allowed to lead during a network partition.

---

## 4. Why Differentiate the Two?

| Feature | Local (Process) Recovery | Network (Machine) Failover |
| :--- | :--- | :--- |
| **Recovery Target** | < 50ms | 100ms - Seconds |
| **Data Locality** | Remains on the same hardware | Must be replicated/moved |
| **Complexity** | Low (Single-host state) | High (Distributed consensus) |
| **TCP State** | Often preserved by stack retries | Connections must be re-established |
| **Primary Risk** | Poison Pill messages | Split-Brain / Ghost Leaders |

### Conclusion
By using an **Aeron-like SHM Journal**, we solve 99% of failures (software crashes) locally and instantly. This allows the **Leader-Follower network protocol** to remain simple, acting only as the fallback for the rare 1% of hardware disasters.