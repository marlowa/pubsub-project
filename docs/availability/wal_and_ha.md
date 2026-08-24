# WAL and High Availability {#wal_and_ha}

## Design Philosophy

The design follows the convergent pattern that Aeron Cluster, Kafka, Raft, and database
checkpointing all arrive at: **separate the irreversible decision (WAL commit) from its
replayable effects (ME state, ERs, FIX output).** The WAL is authoritative; everything
downstream is reconstructable from it. Followers observe commits, never infer them.
Leadership decides who may append; the WAL decides what already happened. These two concerns
must never leak into each other.

---

## HA Is Component-Specific

There is **no single, generic HA mechanism** in this framework. Each component has a
different failure model, so each composes HA differently. The framework provides
reusable *primitives* — the WAL data structure, the point-to-point replication
channel, the arbiter-client protocol, and epoch/fencing discipline — but a component
uses only the primitives its failure model needs. In particular, **the WAL is
central to the sequencer and matching engine but is not used by the gateway or the
authentication service at all.**

| Component | HA model | Elected leader? | Uses the WAL? | Failover trigger | Client-visible impact |
|-----------|----------|:---------------:|:-------------:|------------------|----------------------|
| **Sequencer** | Arbiter-elected leader/follower | Yes (arbiter grant) | Yes — owns the authoritative WAL | Peer-heartbeat loss → arbiter grant | Brief cutover; gateway buffers orders across it |
| **Matching engine** | Arbiter-elected leader/follower | Yes (arbiter grant) | No WAL of its own; **reconciles from the sequencer's WAL** on promotion | Replication-channel TCP EOF → promotion timer → arbiter grant | Cancel-on-failover: outstanding orders cancelled; client resubmits |
| **Arbiter** | Primary/secondary + witness (PSA quorum) | Yes (witness vote) | No (small replicated leadership cell) | Active-arbiter heartbeat loss + witness confirm | None — off the data path |
| **Order gateway** | Single instance today; session-pinned primary/backup planned (see [Gateway HA](gateway_ha.md)) | No | No | FIX client reconnects to the session's backup instance | FIX reconnect (seconds) |
| **Authentication service** | Active/active, caller-selected | No | No — the database is the source of truth | Caller (gateway) falls to the other endpoint | None — the surviving instance is already current |

Reading the table by column makes the point precise:

- **Only three components elect a leader** (sequencer, ME, arbiter). They are the
  ones with single-writer state whose hand-off must be atomic, so they connect to
  the arbiter pool. The gateway and auth service elect nothing.
- **Only the sequencer and ME are on the WAL path.** The sequencer owns it; the ME
  has no WAL and reconciles from the sequencer's. The gateway is near-stateless (its
  routing state lives in the sequencer's WAL); the auth service's state lives in the
  database, synced by the admin service, not by a WAL.
- **The failover *trigger* and the *client impact* both differ per component.** A
  sequencer failover is a brief invisible cutover; an ME failover cancels outstanding
  orders; a gateway failover is a client-driven FIX reconnect; an auth failover is
  invisible.

The rest of this document covers each component's model in turn. The
`primary`/`secondary` and `leader`/`follower` glossary below applies **only** to the
arbiter-elected components. The gateway (a singleton per FIX endpoint, named
`fix_order_gateway` with no suffix) and the auth service (active/active instances named
`a`/`b`) deliberately do not use those terms — see their sections.

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

The WAL data structure — its on-disk format, segmentation, single-writer rule, the
scan-from-zero / stop-on-damage replay model, and the cursor abstraction — is described in
its own document, **[Write-Ahead Log](../durability/wal.md)**. It is a shared primitive: the topic
publisher builds pub/sub fan-out on the same structure (see [Pub/Sub](../pubsub/pubsub.md)).

This section covers only what the *sequencer's* high availability adds on top of that
primitive: how a commit is made durable across two machines, how records replicate to the
follower, and how epochs fence stale writers. The sequencer is the WAL's single writer while
it holds leadership; on mid-log corruption it halts and the replica with a clean prefix is
promoted.

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

### Fencing

*Fencing* means stopping a deposed or partitioned old leader from continuing to act after a new
leader is elected. This system fences **cooperatively, using the epoch as a fencing token**. It does
**not** do power fencing (STONITH), and there is deliberately no fence file.

**How the epoch fences.** Every promotion increments a monotonic `epoch` granted by the arbiter (see
*Epoch Semantics* above). A node stands down the moment it observes a higher epoch, and a leader
ignores any heartbeat carrying an epoch lower than its own. Combined with the single-writer sequencer
and monotonic `seq_no`, a stale leader cannot corrupt the record stream: followers and WAL consumers
skip anything at or below what they have already applied.

**The honest limitation.** This is *cooperative* fencing: a deposed leader stops only once it
**observes** the newer epoch — via a heartbeat, an arbitration decision, or a reconnect. A leader
that is hung, or partitioned away from both its peer and the arbiter, may keep serving its existing
clients until it regains contact, at which point the higher epoch forces it to follower. The design
bounds the damage rather than making it impossible: the arbiter grants the next epoch to exactly one
node (so there is never a second *authoritative* writer), and the single-writer WAL + `seq_no`
ordering mean a stale leader's late writes are rejected downstream, not merged. For the scale this
framework targets, that trade-off is deliberate and sufficient.

**Why not STONITH / power fencing.** True power fencing forcibly removes a suspect node —
independently of its OS — via managed PDUs, a BMC/IPMI channel (iLO, DRAC), or SCSI-3 persistent
reservations on shared storage. It guarantees the old leader is gone, but it needs specific hardware
and an out-of-band control path, adds significant operational complexity, and is a production-cluster
concern (Pacemaker/Corosync territory). It is out of scope for this project, whose goal is framework
validation and which runs its HA pairs cooperatively rather than over managed power hardware.

**If stronger fencing were ever wanted** without STONITH hardware, the natural step is a
**self-fencing watchdog**: a leader that cannot renew its arbiter lease within the takeover timeout
stops serving (or restarts) itself, closing the hung/partitioned-old-leader gap above. It would be
built from a watchdog timer plus the existing epoch/lease — not from a marker file. (An earlier
write-only `fence_file`, written by every HA component on promotion but never read by anything,
contributed nothing to correctness and was removed to avoid implying a guarantee the system does not
make.)

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

**Superseded.** ER routing is now on `gateway_session_conn_id` — the originating connection —
carried on the `WalRecord` envelope, paired with `origin_gateway_id` to say which gateway that
connection belongs to. The comp-id pair was dropped because two sessions sharing a comp id
could reuse a ClOrdID and collide; a connection identifies exactly one session and cannot.

The trade-off that buys is real and is not yet resolved: a connection id is gateway-local and
does not survive a reconnect, so a client that reconnects to a *different* instance cannot
be handed reports still in flight for its old connection. Resolving it is the substantial half
of [Gateway High Availability](gateway_ha.md), which re-keys the routing entry on the session's
provisioned identity and keeps the connection triple as a mutable destination.

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

### Orders In Flight During ME Failover

A `kill -9` of the leader ME gives no clean shutdown; the only signal is the replication TCP
connection dropping. Between that moment and the promoted ME going live there is a
**promotion-timeout window** (`heartbeat_timeout_seconds`, ~15 s by default) during which no
ME is processing orders. What happens to orders the gateway is still sending during that
window is a common question, and the answer is that **none are lost**:

1. The leader sequencer **WAL-commits every order before it checks the ME connection.**
   Committing is unconditional for the leader; forwarding is what depends on a live ME.
2. With no ME connected, the sequencer logs *"no matching engine connected — order seq=N
   WAL-committed, forward deferred …"* and skips the forward. This is a **deferred forward,
   not a dropped order** — the record is already durable in the WAL. (The message was
   historically worded "dropping order PDU", which wrongly read as data loss; it was
   reworded for exactly this reason.)
3. On promotion, the ME's WAL reconciliation replays the whole gap — from the ME's
   last-applied seqNo to the current WAL head — so every order sequenced during the window is
   applied to the promoted book.
4. Cancel-on-failover then cancels whatever is genuinely outstanding on the reconciled book,
   the gap orders included.

**Client experience.** For an order sent during the gap the client gets **no immediate ER**
(no live ME), then — after promotion and reconciliation — a **Cancel ER with no preceding New
ER**, and resubmits. This is the cancel-on-failover contract: a FIX client must tolerate a
cancel for an order it never saw acknowledged. Nothing is silently lost; the cost is the
~15 s window of order-processing unavailability plus the cancel-and-resubmit.

*Verified directly:* a failover under a continuous ~1000 orders/sec stream WAL-committed
15,000 orders during the 15 s gap and replayed all 15,000 to the promoted ME — none dropped.

---

## Gateway Pool

Gateway HA differs from sequencer HA. A FIX session is a TCP connection bound to one
gateway machine. When a gateway dies, the connection is gone and the FIX client must
re-establish.

**Superseded on 2026-07-30. See [Gateway High Availability](gateway_ha.md), which replaces
this section.**

This section previously described **N-way pooled redundancy**: a client reconnecting to any
gateway in the pool, with ER routing on the sequencer's comp-id map. That claim is withdrawn on
two counts. It was never implemented -- only one gateway instance is ever run -- and it no longer
matched the code, because ER routing moved to `gateway_session_conn_id`, which is gateway-local
and so cannot be inherited by a different pool member. The three bullets that used to justify
pooling rested on the comp-id map that routing no longer uses.

The agreed direction is **session-pinned primary/backup**: a session is provisioned against two
named gateway instances and may log on to either, which is how venues actually provision order
entry. The reasoning, the gaps between here and there, and the implementation order are all in
[Gateway High Availability](gateway_ha.md). It is planned for 0.3.0 and is not built as of 0.2.0;
the gateway remains a single point of failure until it is.

The user-visible interruption on failover is the FIX reconnect latency -- typically seconds, not
transparent.

---

## Authentication Service HA

The authentication service is **active/active, caller-selected** — not an arbiter-elected
pair. Two instances, named `a` and `b`, both run and both serve the gateway; neither is a
leader and neither is promoted. This is the correct model because the auth instances are
*not* the writer of the state they hold — they are reflectors of an upstream single writer.

### What state auth holds, and who writes it

Auth validates FIX logons against a compID credential set using SCRAM. That set is mutable at
runtime (compIDs are added and removed), so it **is** shared state — but the **admin service
is its single writer.** The admin service writes the database (the durable source of truth)
and then fans the credential change out to **both** auth instances
(`SetCredential`/`RemoveCredential` PDUs) so each keeps its in-memory copy current. Two
reflectors fed the same changes never diverge, so there is no write-contention to arbitrate
and therefore no election.

Consequently auth uses **none** of the leader-follower machinery: no arbiter, no WAL, no
epoch/fencing, no promotion. It also has no failover *window* — because both instances are
always live and current, there is no leader to lose.

### Failover

Failover is **caller-driven.** The gateway holds a connection to both auth instances and has
a try-first/backup preference. That preference is a *caller* concept — like a DNS
primary/secondary resolver — and does **not** imply an election among the auth instances.
When the preferred instance dies, the gateway detects the dropped connection and
authenticates the next logon against the surviving instance. An already-established FIX
session is unaffected (it does not re-authenticate); only new logons exercise the failover.

### Invariants the model depends on

1. **The admin fan-out reaches both instances** on every credential change, so a later
   failover target is current. Implemented in the admin service's `AuthServiceClient`,
   best-effort: the operation succeeds if at least one endpoint applies it, and an endpoint
   that missed a change reconciles from the database on its next start.
2. **Each instance loads the credential set from the database export on startup**, so a
   restarted instance is current as of that export.

*Verified:* a live compID create and delete each reach both instances (both log
`SetCredential`/`RemoveCredential … Success`), and killing instance `a` leaves a fresh FIX
logon authenticated by instance `b` (`ha_test.py` scenario 17).

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
| Auth instance failure | Transparent for established sessions; next logon uses the surviving instance | Active/active; caller-selected; no promotion |
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
| Market data integration mechanism | Pending requirements for the downstream market data consumer |
| Arbiter PSA+witness internal protocol detail | **Done** — implemented in `applications/arbiter/` and `applications/witness/` |

---

## Implementation Status

The WAL and HA design is staged into vertical slices.

**Sequencer / arbiter WAL+HA:**

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

**Matching-engine HA (slices A–D):**

| Slice | Description | Status |
|-------|-------------|--------|
| A | Role config + second ME instance | Done |
| B | Book-replication channel (ME-leader → ME-follower) | Done |
| C | Arbiter-mediated promotion | Done |
| D | WAL reconciliation + cancel-on-failover; sequencer re-routes to the promoted ME | Done |

**Cross-cutting HA fixes:**

| Item | Description | Status |
|------|-------------|--------|
| Arbiter component-group keying | Arbiter keys leadership state by `(component-group, instance_id)` so the sequencer/ME/MEP pairs don't alias onto shared `{1,2}` slots | Done |
| Auth active/active fan-out | Admin service fans credential updates to **both** auth instances so a failover target is current | Done |

---

## See Also

- [Write-Ahead Log](../durability/wal.md) — the WAL data structure this HA design builds on
- [Pub/Sub](../pubsub/pubsub.md) — the topic publisher, which reuses the WAL as a fan-out backlog
- [Architecture](../orientation/architecture.md) — component topology and order flow
- [Arbiter](../venue/arbiter.md) — the arbiter application (PSA active/passive + replication)
- [Witness](../venue/witness.md) — the witness application (tiebreaker)
- [Reactor](../framework/reactor.md) — the event loop that drives the replication and heartbeat paths
- [Sequencer](../venue/sequencer_app.md) — the sequencer application
- [Secure Comms](../operations/secure_comms.md) — the authentication service (active/active) and TLS
