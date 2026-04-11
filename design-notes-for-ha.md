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
