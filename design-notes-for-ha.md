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
