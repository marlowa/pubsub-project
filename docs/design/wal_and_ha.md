# WAL and High Availability

## Design Philosophy

The design follows the convergent pattern that Aeron Cluster, Kafka, Raft, and database
checkpointing all arrive at: **separate the irreversible decision (WAL commit) from its
replayable effects (ME state, ERs, FIX output).** The WAL is authoritative; everything
downstream is reconstructable from it. Followers observe commits, never infer them.
Leadership decides who may append; the WAL decides what already happened. These two concerns
must never leak into each other.

---

## Glossary

Two pairs of terms with strict, non-overlapping meanings. Confusing them is a recognised
source of bugs in HA systems.

| Term | Meaning |
|------|---------|
| **primary** | Configured identity; the instance with the lower `instance_id`. Set at deploy time; never changes for the life of the instance. Used only for deterministic tiebreaking on cold start. |
| **secondary** | Configured identity; the instance with the higher `instance_id`. |
| **leader** | Runtime role; holds the current lease grant from the arbiter. Either configured instance can be leader at any moment. All commit and forward actions check the lease, not the configured identity. |
| **follower** | Runtime role; the other instance. Tails the leader's WAL; does not send to ME or gateway. |
| **active / standby** | **NOT USED.** These terms ambiguously refer to either configured identity or runtime role and are a permanent source of confusion. Always use one of the two more specific terms above. |

In the happy path, primary is leader and secondary is follower. After a primary failure and
failover, secondary becomes leader (still configured as secondary). A graceful failback is
an operational choice, not automatic.

---

## Write-Ahead Log (WAL)

### Format

Each WAL entry is:
```
[ magic | length | seqNo | payload | checksum ]
```

Replay scans from offset 0; on any checksum or bounds failure it stops. Entries past the
failure point are treated as "did not happen". Tail corruption is therefore identical in
effect to a clean crash before commit of that entry. Mid-segment corruption halts the
sequencer; the replica with a clean prefix is promoted.

### Segmentation

The WAL is segmented into fixed-size files (`wal_000001.log`, `wal_000002.log`, …). Each
segment is independently checksummable and archivable. Segmentation simplifies truncation,
localises corruption damage, and makes the disk-full scenario predictable.

The WAL is **single-writer**: only the leader sequencer appends. No locks are needed.

### Two-Tier Commit

"Commit" has two distinct levels, both required:

| Level | Mechanism | Gates |
|-------|-----------|-------|
| **Locally durable** | `store-release` on the commit offset | Leader's send to the ME |
| **Replicated** | Follower acks the record over the replication channel | Leader's emission of the ER to the gateway |

The ER is held back from the gateway until replication has acked, so the gateway never
observes an order whose existence is held by only one machine. This gives two-machine
durability without the overhead of Raft's quorum vote; there is only one follower and
replication is point-to-point streaming.

**No `fsync` per commit.** Disk durability is out-of-band (segment rotation, snapshot
writes, periodic flusher). Cross-machine durability comes from replication, not from disk.
`fsync` per commit would cost 10–100+ µs and is unnecessary.

### Replication Channel

The leader streams WAL records to the follower over a dedicated TCP connection — separate
from the order/ER data channels and from the arbiter control channel. The follower writes
records to its own local WAL (own disk, own machine) and sends per-record acks back. The
leader uses the highest acked seqNo to gate ER emission.

Followers do not infer commits from heartbeat or timing; they observe records arriving on
the replication channel. The follower's role is strictly passive: it tails, it acks, it
does not send to the ME, it does not send to the gateway. Connections from gateway and ME
to the follower exist (so they are pre-warmed for promotion) but carry no data while the
follower is passive.

**The sequencer-to-follower replication channel is not droppable.** If the follower lags,
the leader cannot drop it without violating the two-machine durability rule. Instead, the
leader stops emitting ERs (the gateway sees order receipt but no fills until replication
catches up). If the follower fails entirely, leadership decisions become
single-machine-durable until a new follower is paired in, which is itself an
arbiter-mediated event.

### Epoch Propagation on Every PDU

Every cross-component PDU carries the sender's view of the relevant component pair's
current leader-epoch. This is fencing applied to every message, not just commit decisions.
The receiver has three responses:

| Received epoch | Response |
|----------------|----------|
| Same/expected | Accept and process normally |
| Lower than receiver's | Sender is stale; discard with warning. Sender will detect its staleness on its next arbiter heartbeat. |
| Higher than receiver's | Receiver might be stale. Trigger immediate re-validation with the arbiter; hold the PDU until the answer is known. If receiver's view is stale, update and process. If the claim is from a rogue sender, discard. |

The case that motivates silent-discard being insufficient: ME-secondary believes it has been
promoted and emits cancel ERs with its claimed epoch. If the sequencer silently discards
(because it still thinks ME-primary is leader), the cancels are lost without anyone learning
of the split-brain. Silent discard and silent accept are both wrong; re-validation is the
only correct response to a higher-than-expected epoch.

### Per-Connection Isolation and Backpressure

Each TCP connection has its own outbound queue and non-blocking send semantics. A stalled
peer cannot block sends to a fast peer:
- If ME-secondary stalls, its send cannot block the sequencer's send to ME-primary.
- If one gateway stalls, the others continue to receive ERs.

A peer that exceeds a configured lag threshold is dropped (TCP reset), forcing
reconnect-and-replay. The sequencer-to-follower replication channel is the only exception
(not droppable; see above).

---

## Leader-Follower Protocol

### PDU Summary

| Message | ID | Purpose |
|---------|-----|---------|
| `StatusQuery` | 100 | Identity + epoch announced on TCP connect |
| `StatusResponse` | 101 | Identity confirmation + peer echo + current role |
| `Heartbeat` | 102 | Liveness detection + epoch propagation |
| `ArbitrationReport` | 200 | Sent to arbiter when arbitration is needed |
| `ArbitrationDecision` | 201 | Authoritative tie-break + epoch assignment from arbiter |

### Epoch Semantics

The epoch is a generation counter identifying which leadership generation the cluster is
currently in. A stale node has missed one or more leadership transitions and has a lower
epoch than the current generation.

Rules:
1. A node that has never participated starts with epoch 0.
2. The arbiter assigns the epoch in `ArbitrationDecision`. Both nodes adopt this value. The
   arbiter is itself HA (PSA+witness, see below) so the epoch is durable across arbiter
   restarts.
3. A follower detecting leader death does **not** promote unilaterally. It contacts the
   arbiter via `ArbitrationReport`; the arbiter checks lease expiry and issues an
   `ArbitrationDecision` granting the new epoch. Unilateral promotion permits split-brain
   when the arbiter is reachable from both partition halves.
4. A restarting node that receives a `StatusResponse` with a higher epoch immediately adopts
   follower role — no arbiter contact needed.
5. A heartbeat carrying a lower epoch than the receiver's is from a stale sender; logged as
   a warning and ignored.

### Startup Election Flow

1. Each node attempts TCP connection to its peer (A→B, B→A).
2. Both sides immediately send `StatusQuery` (identity + epoch).
3. Each side replies with `StatusResponse` including `current_role`.
4. **Peer is already leader:** connecting node adopts follower immediately. No arbiter needed.
5. **Both sides unknown:** both send `ArbitrationReport` to the arbiter (primary address first, secondary as fallback).
6. Arbiter issues `ArbitrationDecision` assigning leader and follower by lowest `instance_id`, and sets the epoch.
7. Both nodes adopt assigned roles; arbiter connection closed.

### Post-Election Steady State

- Peer-to-peer TCP connection remains open with periodic `Heartbeat` messages.
- Heartbeats carry `instance_id` and `epoch` for liveness and stale-node detection.
- **Follower dies:** leader logs a warning; no other action.
- **Leader dies:** follower initiates promotion (see below).

### Leader Death and Follower Promotion

On heartbeat loss:
1. Surviving node first attempts reconnection to the peer.
2. Reconnect succeeds: exchange `StatusQuery`/`StatusResponse`; epoch resolves roles normally.
3. Reconnect fails: surviving node sends `ArbitrationReport`. Arbiter checks whether the
   previous leader's lease has expired; if so, issues `ArbitrationDecision` granting
   leadership at the next epoch. The surviving node adopts leader role only after receiving
   this decision.
4. Arbiter unreachable: surviving node cannot promote. Enters degraded waiting state,
   retries the arbiter. System is unavailable for new orders. This is correct: without
   arbiter confirmation the previous leader is gone, promoting risks split-brain.

### Split-Brain Protection

| Scenario | Outcome |
|----------|---------|
| Normal startup, arbiter reachable | Arbiter is sole authority; assigns exactly one leader |
| One node already established | Epoch difference resolves it; restarting node adopts follower |
| Network partition, both nodes alive | Neither promotes unilaterally; whichever can reach arbiter requests promotion; arbiter grants to one only |
| Neither node can reach arbiter | Both enter degraded waiting; system unavailable until arbiter contact restored; split-brain prevented |

---

## Sequencer HA

The sequencer has the richest HA state: order log, FIX session map, per-gateway delivery
cursors, sequence-number authority.

### State Location

The FIX `(SenderCompID, TargetCompID)` → `ClOrdID` routing map lives in the sequencer's
WAL, not in the gateway. This makes the gateway near-stateless:
- On restart, the gateway does not lose ER routing — the state is in the sequencer's WAL.
- After sequencer failover, the new leader has the routing map by WAL replay.
- A FIX client reconnecting to a different gateway is naturally addressable; the new gateway
  registers `(CompA, CompB)` with the sequencer and the map updates.

`ConnectionID` is not stable across reconnects; routing is on the FIX-level comp-id pair,
not on `ConnectionID`.

### Gateway↔Sequencer Connectivity

Gateway and ME each open TCP connections to **both** sequencer instances at startup and keep
both open. Sends go only to whichever is currently leader. The non-leader rejects send
commands at the application layer, so clients know which is leader without a separate
discovery mechanism.

On leader change, the old leader's connection drops or starts rejecting; the new leader's
connection becomes live. The gateway buffers outbound order PDUs locally during the cutover
window.

### Snapshots

Snapshots capture sequencer state only. The ME is never snapshotted — its book is rebuilt
from WAL replay on every restart. Snapshot contents:
- `lastCommittedSeqNo`
- FIX session routing tables
- Per-gateway delivery cursors
- Everything else needed to assign seqNo `S+1` after restart

Anything that can be recomputed from the WAL is not in the snapshot.

**Snapshotting is non-blocking.** The leader briefly gates new seqNo assignment (tens of
microseconds), drains in-flight WAL appends to a clean cut at seqNo `S`, captures snapshot
state in memory, releases the gate, and serialises the file asynchronously. The ME never
sees a pause.

**Dual rolling snapshots.** Two snapshots are kept: `snapshot_A` (older, trusted, used as
the WAL truncation anchor) and `snapshot_B` (newer, candidate, validated before promotion).
WAL truncation uses the older trusted snapshot, not the newest one just taken. Invariant:
**never delete WAL history unless at least one older verified snapshot can reproduce the
same state.**

### WAL Truncation

After a snapshot at seqNo `S` is durable and validated:
- Delete WAL segments fully behind `S`.
- Keep any segment containing seqNos `> S`.
- The follower must have replayed at least `S` (or have its own snapshot ≥ `S`) before the
  leader truncates — otherwise truncation could delete history the follower still needs.

### Cold-Start MTTR and mmap Warm-Up

Three components contribute to recovery time on a cold start:

| Component | Typical cost |
|-----------|-------------|
| Snapshot load | Single-digit ms (small) to low hundreds of ms (large) |
| WAL tail replay | Proportional to tail size since last snapshot |
| mmap page-in | ~1 GB / disk-read-rate for a warm SSD — commonly the dominant factor |

The mmap page-in cost does not appear in microbenchmarks (warm cache). It appears in
production cold starts and can easily dominate.

Mitigation: issue `madvise(MADV_WILLNEED)` on the mmap region immediately after opening the
WAL. The kernel pages the file in parallel with snapshot deserialisation and WAL replay
setup. A 1–2 second disk I/O cost becomes a parallel overlap rather than a serial bottleneck.

In a normal failover the secondary is already running, its WAL is already paged in (from
tailing the leader), and the recovery time is only the time to apply any unapplied records.
Cold-start MTTR matters mainly when bringing a previously-down instance back online.

---

## Matching Engine HA

### ME Failover Policy: Cancel-on-Failover

Four options were considered:

| Option | Description | Decision |
|--------|-------------|----------|
| (a) Slow seamless | ME-secondary cold-starts and replays WAL | Rejected: too slow |
| (b) Fast lockstep | Both MEs process sequencer input in parallel with deterministic logic; failover is switching to secondary's outputs | Future aspiration; requires full determinism discipline |
| (c) Halt-on-failure | ME dies, market halts, operator recovers | Preserved as fallback for unrecoverable failure modes |
| (d) Cancel-on-failover | ME-secondary promoted; issues cancel ERs for all outstanding orders | **Chosen baseline** |

Cancel-on-failover avoids the long downtime of (c) and the determinism investment of (b),
while giving FIX clients an explicit "your order has been cancelled" message rather than
silence.

### ME Failover Correctness Rule

A naive cancel-on-failover implementation has a race condition: ME-primary matches a trade
and emits an ER, the sequencer commits the ER to its WAL, then ME-primary crashes before
replicating the book update to ME-secondary. ME-secondary is promoted and sees the order as
still outstanding — it issues a cancel for an order that already executed. A legally-executed
trade is wrongly cancelled.

The correct order of operations on ME-secondary promotion:

1. Detect ME-primary failure (heartbeat loss).
2. Request promotion via the arbiter; receive `ArbitrationDecision` with new epoch.
3. Stop tailing the dead or failed-over sequencer stream.
4. Connect to the new sequencer leader; exchange position cursors: "my book reflects events
   up to seqNo M; what does your WAL contain?"
5. New sequencer leader replays events `M+1..N` from its WAL. ME-secondary applies them.
6. **Now** the book is consistent with the sequencer's authoritative state. Any order still
   on the reconciled book is genuinely outstanding.
7. Issue cancel ERs for the genuinely-outstanding orders, via the new sequencer leader.

This adds latency to the cancel path (cancels cannot fire until the new sequencer leader is
up and reconciliation is complete) but eliminates the race. A delayed cancel is acceptable;
a wrongly-cancelled executed trade is not.

If the failure event takes out both ME-primary and a sequencer simultaneously, ME failover
waits for sequencer failover to complete first. The cancel-on-failover latency is bounded
by the sum of both failover times.

---

## Gateway Pool

Gateway HA differs from sequencer HA. A FIX session is a TCP connection bound to one
gateway machine. When a gateway dies, the connection is gone and the FIX client must
re-establish.

The gateway pool is **N-way pooled redundancy**, not primary/secondary HA. Failure mode:
"FIX client reconnects to a different gateway in the pool." This works because:
- FIX has built-in resend semantics (sequence number negotiation on reconnect).
- ER routing uses the sequencer's comp-id map, not the gateway's connection state.
- The new gateway registers the FIX session with the sequencer on reconnect; the sequencer's
  map updates and ERs are routed to the new connection.

The user-visible interruption is the FIX reconnect latency — typically seconds, not
transparent.

---

## Arbiter

### Role

The arbiter is **off the critical data path.** It holds leadership state for each component
pair: `(component_id, leader_instance_id, epoch, lease_expiry)`. Leaders heartbeat to
renew their lease. The arbiter never participates in order processing.

On failover, the surviving instance contacts the arbiter with `ArbitrationReport`. The
arbiter performs an atomic compare-and-swap: if the old leader's lease has expired, it
grants promotion, bumps the epoch, and records the new leader. The old leader on revival
sees its epoch is stale and steps down.

### PSA Topology

The arbiter is itself HA, using a Primary-Secondary-Arbiter (PSA, or PSW) topology:

- **Arbiter primary** — full arbiter instance; holds the leadership-state map.
- **Arbiter secondary** — full arbiter instance; holds a replicated copy.
- **Witness** — small process; holds no state; votes on which full arbiter is active.

Three votes total; majority is two; any single failure is tolerated. This is the same shape
MongoDB uses for two-data-bearing-member replica sets.

**Three machines only.** Adding a second witness makes it four votes, requiring three for
majority, meaning any single failure leaves the cluster below majority — worse, not better.
The correct way to add redundancy is to upgrade to five machines (full Raft-style). For this
framework, three is the chosen and final count.

**Failure-independence requirement:** the witness must be on different power, a different
network switch, and ideally a different network segment from the two full arbiter machines.
If a witness shares infrastructure with one full arbiter, a single failure can reduce the
effective vote to 1-of-2, making failover impossible precisely when it is most needed.

**Component contact protocol:** components have a configured list of two arbiter addresses
(primary, secondary — never the witness). The responding arbiter either grants the decision
(if active) or replies "I am not active; contact X" (if passive). Components never contact
the witness directly.

**Internal protocol:**
- Active heartbeats to passive (with state replication) and to witness (liveness only).
- Passive heartbeats to witness (liveness only).
- On active-arbiter heartbeat loss as seen by passive: passive asks the witness "have you
  also lost the active?". If yes, passive promotes; if no, passive does not promote (the
  active is still alive, just unreachable from passive's network position).
- If witness is unreachable from passive: passive cannot promote. System continues with the
  currently-active arbiter for the lease window, then becomes unavailable for new failover
  decisions.

**Operational limitation:** witness outage during arbiter failover prevents arbiter-pool
failover. The witness is a SPOF for the *arbiter failover decision* (not for steady-state
operation). Operational discipline: monitor the witness aggressively and repair outages
quickly.

### Consensus Libraries vs. Lease+Epoch

Raft and Paxos libraries (NuRaft, braft) were considered and rejected for the arbiter's
internal HA. The reasons:
- The arbiter's replicated cell is small (one record per component pair); a full consensus
  library is overkill in code-size terms.
- C++ Raft implementations are less mature than their Java/Go counterparts; operational
  risk of inheriting subtle bugs in code the project did not write.
- The hand-rolled lease+epoch approach is tractable for this cell size and the protocol is
  fully understood by the author.

The trade-off is accepted: the PSA+witness design is simpler to reason about for this
specific use case, and correctness is verifiable from first principles.

---

## Failover Targets

| Component | Target | Notes |
|-----------|--------|-------|
| Sequencer leader → follower | Sub-second; tens of milliseconds aspirational | Drives lease length (~200–500 ms) and heartbeat interval (~50–100 ms) |
| ME crash | Cancel-on-failover; latency = sequencer failover + book reconciliation | Seamless (lockstep) failover is a future aspiration |
| Gateway machine failure | Seconds (FIX reconnect to another pool member) | Inherent to gateway-pool design; FIX resend covers the gap |
| Arbiter primary failure | Tolerated for the lease window | Secondary takes over via internal arbiter HA if witness is reachable |
| WAL disk full | Immediate halt | No "best effort" continuation |

---

## Failure-Handling Boundaries

| Situation | Correct behaviour |
|-----------|------------------|
| Leader crash before WAL commit | Order disappears (never existed); gateway resends or FIX client retries |
| Leader crash after WAL commit, before ME send | Follower promotes, replays WAL, sends order to ME, emits ER |
| Leader crash after ME send, before ER emission | Same — new leader replays from the WAL; FIX client eventually receives ER |
| ME crash | ME restarts empty; new leader replays from ME's `lastAppliedSeqNo + 1` |
| Gateway crash | Gateway reconnects, exchanges cursors, replays any gap |
| WAL disk full | Sequencer immediately gates ingress; halt cleanly or fail to replica |
| WAL tail corruption | Truncate at last good entry; treat as crash before commit |
| WAL mid-segment corruption | Halt; promote replica with clean prefix |
| Snapshot corrupt during validation | Snapshot discarded; system continues with the older trusted snapshot |
| Snapshot format incompatible after upgrade | Roll back binary; the older snapshot in the dual-snapshot pair still works |
| Both arbiters + witness unreachable | No failover decision can be made; leader continues for lease window only |

---

## Time Synchronisation

Several mechanisms depend on clocks across machines agreeing closely:
- **Lease expiry checks** — clock skew between leader and arbiter can cause premature
  step-down (benign) or continued operation past expiry (split-brain risk).
- **TransactTime on ERs** — auditors expect timestamps from different machines to be in a
  sensible total order.
- **Heartbeat liveness detection** — "N milliseconds of silence" must mean the same thing
  on both endpoints.

The intended solution is **PTP (IEEE 1588)**, not NTP. PTP delivers sub-microsecond
synchronisation across a correctly configured local network. NTP's millisecond-range
accuracy is insufficient for tight lease checks.

PTP requires hardware-timestamped NICs, a GPS-disciplined grandmaster, and boundary/
transparent clocks on switches. This is standard infrastructure at real exchanges; the
framework relies on it being present but does not implement PTP itself.

**Framework clock discipline:**
- `CLOCK_MONOTONIC` for hot-path interval measurement (heartbeat timers, timeout checks).
  It never goes backwards and is gently slewed by NTP/PTP to track real wall time.
  `CLOCK_MONOTONIC_RAW` is explicitly **not** used — it is unaffected by slewing, so
  intervals can drift from real-world expectations on long-running processes.
- `CLOCK_REALTIME` (PTP-disciplined) for cross-machine timestamps: lease expiry, WAL entry
  timestamps, `TransactTime` fields.
- Lease grants should ideally carry both an absolute expiry time and a TTL; the leader can
  use whichever frame is safer locally. Open design question.

---

## Open Design Questions

| Question | Status |
|----------|--------|
| DR (Disaster Recovery) topology | Not designed; main-site only for now |
| Sub-second sequencer failover tuning | Lease/heartbeat intervals should be configurable via `ReactorConfiguration` |
| Multi-instrument scaling (sharded sequencer?) | Not in scope; deferred |
| Sequencer-to-gateway connection direction (who initiates?) | Currently sequencer initiates; may need reversing for multi-gateway deployments |
| Market data integration mechanism | Pending conversation with maintainer of the existing market data system |
| Arbiter PSA+witness internal protocol detail | **Done** — implemented in `applications/arbiter/` and `applications/witness/` |

---

## Implementation Status

The WAL and HA design is staged into vertical slices. All slices are now implemented:

| Slice | Description | Status |
|-------|-------------|--------|
| 1 | seqNo on wire and in `EventMessage` | Done |
| 2 | In-memory WAL | Done |
| 3 | mmap'd WAL on disk, segmented, no fsync | Done |
| 4 | Snapshot (single, not yet rolling) | Done |
| 5 | `cl_ord_id → SenderCompID` routing map in sequencer | Done |
| 6 | Leader-follower HA state machine | Done |
| 7 | WAL replication to follower + two-tier commit | Done |
| 8 | Arbiter PSA+witness topology | Done |

---

## See Also

- [Architecture](../architecture.md) — component topology and order flow
- [Arbiter](../applications/arbiter.md) — the arbiter application (PSA active/passive + replication)
- [Witness](../applications/witness.md) — the witness application (tiebreaker)
- [Reactor](reactor.md) — the event loop that drives the replication and heartbeat paths
- [Sequencer](../applications/sequencer_app.md) — the sequencer application
