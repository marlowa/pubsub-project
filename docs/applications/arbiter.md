# Arbiter

## Role

The arbiter manages the leadership-state map for component pairs (sequencer pair, ME pair).
It is the external authority that grants and fences leadership: no component promotes itself
without an `ArbitrationDecision` from the active arbiter.

Two arbiter instances form a primary/secondary HA pair using the same
StatusQuery/StatusResponse/Heartbeat election protocol as the sequencer. One is **active**
(makes decisions, replicates state to passive); the other is **passive** (replicates state,
ready to take over). A separate **witness** process breaks ties in the arbiters' own
election — see [Witness](witness.md).

The arbiter is **off the critical data path**. It never participates in order processing;
it only responds to `ArbitrationReport` requests from components during failover or cold
start.

---

## PDU Protocol

### Component ↔ Arbiter PDUs

Components (sequencer, ME) connect to **both** arbiter instances and send heartbeats and
arbitration requests. The active arbiter processes them; the passive arbiter drops
`ArbitrationReport` with a log warning.

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `StatusQuery` | 100 | Arbiter → Arbiter peer | Identity + epoch announced on peer connect |
| `StatusResponse` | 101 | Arbiter → Arbiter peer | Identity confirmation + current role |
| `Heartbeat` | 102 | Bidirectional | Liveness + epoch propagation between arbiter peers |
| `ArbitrationReport` | 200 | Component → Active arbiter | Component requests a leadership decision |
| `ArbitrationDecision` | 201 | Active arbiter → Component | Authoritative leader/follower assignment + epoch |

### Arbiter Internal (Active ↔ Passive Replication)

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `ArbiterStateRecord` | 400 | Active → Passive | Replicate a leadership-state entry after each decision |
| `ArbiterStateAck` | 401 | Passive → Active | Acknowledge receipt of a state record |

### Arbiter ↔ Witness

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `ArbiterHeartbeat` | 300 | Active arbiter → Witness | Liveness; allows witness to track which arbiter is connected |
| `ArbiterVoteRequest` | 301 | Passive arbiter → Witness | Request a vote before self-promoting to active |
| `ArbiterVoteResponse` | 302 | Witness → Passive arbiter | Grant vote (to lower `instance_id`) or deny |

---

## Election Protocol

The arbiter pair uses the same peer protocol as the sequencer (StatusQuery/StatusResponse/
Heartbeat). On startup:

1. Both arbiters connect to each other and exchange `StatusQuery`.
2. If one is already active (higher epoch), the other adopts passive immediately.
3. If both are undecided, each contacts the witness with `ArbiterVoteRequest`. The witness
   grants the vote to the arbiter with the lower `instance_id` (deterministic tiebreak), or
   to the requester if its peer is not connected to the witness.
4. The winner adopts active role; the loser adopts passive.
5. If the witness is unreachable, each arbiter self-promotes using the instance-id rule
   after `vote_timeout_seconds` (degraded mode — only safe when the two arbiters cannot see
   each other either, ensuring no split-brain).

On active arbiter failure, the passive arbiter detects heartbeat loss, requests a vote from
the witness, and promotes itself if the vote is granted.

---

## State Replication

After each `ArbitrationDecision`, the active arbiter sends an `ArbiterStateRecord` (400)
to the passive, which replies with `ArbiterStateAck` (401). The leadership-state map
(`component_instance_id → ComponentState`) is thereby kept in sync across both arbiter
instances. On active failure, the passive promotes with a current copy of the map and
can immediately serve the next `ArbitrationReport` without data loss.

`ComponentState` per component pair:
- `leader_instance_id` — which instance is currently leader
- `follower_instance_id` — which instance is follower
- `epoch` — generation counter for this component pair's leadership

---

## Fence File

When the active arbiter promotes itself it writes a fence file
(`fence_file_path` in config, e.g. `/dev/shm/arbiter_fence`). This provides an
out-of-band indicator for monitoring and operator tooling. It does not affect protocol
correctness.

---

## Port Allocation

| Port | Usage |
|------|-------|
| 7200 | Inbound component connections (sequencer, ME heartbeats and arbitration requests) |
| 7203 | Arbiter primary peer listener (arbiter-to-arbiter PDUs) |
| 7204 | Arbiter secondary peer listener |
| 7100 | Witness inbound (arbiter → witness heartbeats and vote requests) |

---

## Configuration

Key `arbiter.toml` sections:

| Key | Purpose |
|-----|---------|
| `[network] listen_port` | Component connection listener (default 7200) |
| `[ha] instance_id` | Unique integer; 1 = primary, 2 = secondary; lower wins active role |
| `[peer] listen_port` | Arbiter-to-arbiter listener port |
| `[peer] host / port` | Peer arbiter's peer listener endpoint |
| `[peer] heartbeat_interval_seconds` | How often to send `Heartbeat` to peer (default 2 s) |
| `[peer] heartbeat_timeout_seconds` | Peer silence before promotion attempt (default 6 s) |
| `[peer] startup_election_timeout_seconds` | How long to wait for peer before self-promoting at startup (default 20 s) |
| `[peer] fence_file_path` | Path written on promotion |
| `[witness] host / port` | Witness endpoint |
| `[witness] vote_timeout_seconds` | How long to wait for witness vote before degraded self-promotion (default 3 s) |
| `[witness] heartbeat_interval_seconds` | How often to send `ArbiterHeartbeat` to witness (default 30 s) |

---

## See Also

- [Witness](witness.md) — the tiebreaker process
- [WAL and High Availability](../design/wal_and_ha.md) — arbiter PSA topology, split-brain protection, lease+epoch rationale
- [Sequencer Application](sequencer_app.md) — how the sequencer contacts the arbiter for leader election
