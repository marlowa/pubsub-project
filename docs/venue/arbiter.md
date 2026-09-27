# Arbiter {#arbiter}

## Role

The arbiter pool is the third voter in deciding which instance of each component pair leads: the
two sequencers, the two matching engines, and the two matching engine publishers. An instance leads
only while a majority of three voters -- itself, its peer and the arbiter pool -- has granted it a
lease that has not run out. Its own vote is one of the three, so one grant from either its peer or
the arbiter pool is enough. The rules, why each is needed, and what happens in each failure are in
[Deciding leadership by majority, with leases](../availability/majority_leases.md).

Two arbiters form the pool, and it votes through whichever of them is **active**. The other is
**passive** and votes on nothing. An arbiter is active only while a majority of three voters -- the
two arbiters and the [witness](witness.md) -- has granted it a lease in turn, by the same rules. An
active arbiter that can reach neither its peer nor the witness stops being active when its lease
runs out, so at most one arbiter is ever active.

The arbiter is **off the order path**. It never takes part in order processing. It answers lease
requests from component instances, which a leader sends about once a second.

---

## What the arbiter holds

On disk, one small file: the promise of its vote in deciding which arbiter is active, or that it
is the active arbiter (`[lease] promise_file`). An arbiter restarted by its supervisor
reads it back and carries on, so a quick restart of the active arbiter does not make the other one
active. The file is written with the machine's boot id and ignored after a reboot. See
[Deciding leadership by majority, with leases](../availability/majority_leases.md), rule 6.

In memory it holds:

- its own lease, and whether it is active;
- while active, one voter per component group: the promise it has made in that group, if any, and
  the highest epoch it has granted. See `applications/arbiter/ComponentLeaseVoters.hpp`;
- the highest epoch granted in each group, whether by this arbiter or, as its peer reports, by the
  other one.

An arbiter that becomes active does not know what the previously active arbiter promised, so it
grants no component a lease for one lease period. During that period each component leader renews
with its peer alone, so a change of active arbiter costs nothing while every pair has both instances
running.

---

## PDU Protocol

### Component to arbiter

Components connect to **both** arbiters and send each lease request to both, because which arbiter
is active can change. The active arbiter answers; the passive one stays silent. A refusal from the
passive one would cancel the request the active one is answering, because both carry the same id.

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `LeaseRequest` | 130 | Component → both arbiters | Ask to lead, or renew a lease already held |
| `LeaseGrant` | 131 | Active arbiter → component | Grant it; the arbiter promises its vote to no other instance of the pair for one lease period |
| `LeaseRefusal` | 132 | Active arbiter → component | Refuse it, with the highest epoch granted in the group and the reason |

The arbiter answers as voter 3. The instances of a pair are 1 and 2.

### Between the arbiters

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `LeaseRequest` / `LeaseGrant` / `LeaseRefusal` | 130-132 | Arbiter ↔ arbiter | Deciding which arbiter is active, in group `arbiter` |
| `ArbiterStateRecord` | 400 | Arbiter → peer arbiter | The highest epoch granted in one component group, sent when it rises and for every group when the link comes up |

### Arbiter and witness

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `LeaseRequest` | 130 | Arbiter → witness | Ask to be active, or renew |
| `LeaseGrant` / `LeaseRefusal` | 131, 132 | Witness → arbiter | The witness's vote, as voter 3 |

---

## Deciding which arbiter is active

1. For one lease period after starting, an arbiter neither asks nor grants, because it has forgotten
   anything it promised before it stopped.
2. Then the primary asks its peer and the witness to let it be active. The secondary waits one
   renewal interval longer, so that when both start together the primary asks first and is preferred.
3. The first grant it receives, with its own vote, is a majority: it becomes active. The other
   arbiter, having granted the request, is passive.
4. While active, it asks both again every renewal interval. Either granting is enough.
5. If the active arbiter dies, the passive one's promise to it runs out, and so does the witness's.
   The passive arbiter then asks, the witness grants, and it becomes active.
6. If only one arbiter and no witness remain, that arbiter holds only its own vote. It stops being
   active when its lease runs out, and the pool has no active arbiter until another voter returns.

Component leaders are unaffected by the pool having no active arbiter while their peers are
running, because each renews with its peer.

---

## Fencing

Every epoch records which instance leads in it: its remainder on division by 4 is that instance's
id. Voters never grant an epoch below the highest they have granted, and receivers discard anything
from an older generation. Leases keep two instances from acting as leader at once; epochs are the
second defence. This system does **not** do power fencing (STONITH).

---

## Port Allocation

| Port | Usage |
|------|-------|
| 7200 | Inbound component connections (lease requests) |
| 7203 | Arbiter primary peer listener (arbiter-to-arbiter PDUs) |
| 7204 | Arbiter secondary peer listener |
| 7100 | Witness inbound (arbiter → witness lease requests) |

---

## Configuration

Key `arbiter_primary.toml` / `arbiter_secondary.toml` sections:

| Key | Purpose |
|-----|---------|
| `[network] listen_port` | Component connection listener (default 7200) |
| `[ha] instance_id` | 1 for the primary, 2 for the secondary; the primary is preferred when both start together |
| `[peer] instance_id` | The peer arbiter's `instance_id`, which identifies its vote |
| `[peer] listen_port` | Arbiter-to-arbiter listener port |
| `[peer] host / port` | Peer arbiter's peer listener endpoint |
| `[witness] host / port` | Witness endpoint |
| `[lease] period_milliseconds` | How long a grant lasts, and how long a voter that has just started grants nothing |
| `[lease] drift_allowance_milliseconds` | How much shorter than the period an instance takes a lease it holds to be |
| `[lease] renewal_interval_milliseconds` | How often a leader renews |
| `[lease] promise_file` | Where this arbiter records its promise in deciding which arbiter is active; see above |

The three `[lease]` values are expanded from the environment's `[shared]` section, because every
voter and every instance holding a lease must use the same values.

---

## See Also

- [Witness](witness.md) — the third voter in deciding which arbiter is active
- [Deciding leadership by majority, with leases](../availability/majority_leases.md) — the rules and what happens in each failure
- [Sequencer Application](sequencer_app.md) — the sequencer pair, which asks the arbiter pool for leases
