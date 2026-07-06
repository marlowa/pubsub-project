# Witness {#witness}

## Role

The witness is a small stateless process whose sole purpose is to break ties in the
arbiter's own election. It holds no leadership-state and never interacts with sequencer
or ME instances directly.

When both arbiter instances lose contact with each other and both are undecided, each
contacts the witness asking "may I become active?". The witness grants the vote to the
arbiter with the lower `instance_id`, ensuring exactly one arbiter promotes itself. If
only one arbiter is connected to the witness, that arbiter's vote is automatically
granted (its peer cannot see the witness either, so there is no risk of split-brain).

The witness must be deployed on **failure-independent infrastructure** — different power
supply, different network switch, ideally a different rack — from both arbiter machines.
If the witness shares a failure domain with one arbiter, a single event can isolate that
arbiter and the witness simultaneously, leaving the other arbiter unable to reach a
majority. The witness's value depends entirely on its independence.

---

## PDU Protocol

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `ArbiterHeartbeat` | 300 | Active arbiter → Witness | Liveness; witness tracks which arbiter instance is connected |
| `ArbiterVoteRequest` | 301 | Passive arbiter → Witness | Request permission to promote to active |
| `ArbiterVoteResponse` | 302 | Witness → Passive arbiter | Grant (with assigned epoch) or deny |

The witness identifies each arbiter by the `instance_id` carried in `ArbiterHeartbeat`.
It tracks which instance is connected via `conn_to_instance_id_` and
`instance_to_conn_id_` maps.

**Vote grant rule:** the witness grants the vote to the arbiter with the lower
`instance_id` (deterministic tiebreak). If the peer arbiter is not currently connected
to the witness, the requester's vote is automatically granted — its peer cannot be active
since it cannot reach the witness either.

**Epoch:** the witness tracks `max_observed_epoch_` from received heartbeats and assigns
`max_observed_epoch_ + 1` in `ArbiterVoteResponse`, so the newly-promoted active arbiter
starts with a fresh epoch that any stale components will recognise as newer.

---

## What the Witness Does NOT Do

- It does not store any leadership state.
- It does not contact sequencer or ME instances.
- It does not initiate connections — it only accepts inbound connections from the
  two arbiters.
- It does not participate in sequencer or ME elections directly; those are handled by
  the arbiter pair.

---

## Port Allocation

| Port | Usage |
|------|-------|
| 7100 | Inbound connections from arbiters (heartbeats and vote requests) |

---

## Configuration

`witness.toml` is minimal — the witness needs only a listen port and logging settings.

| Key | Purpose |
|-----|---------|
| `[network] listen_port` | Inbound arbiter connections (default 7100) |

---

## See Also

- [Arbiter](arbiter.md) — the two full arbiter instances that use the witness for tiebreaking
- [WAL and High Availability](../design/wal_and_ha.md) — PSA topology, failure-independence requirement, why exactly three machines
