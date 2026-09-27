# Witness {#witness}

## Role

The witness is the third voter in deciding which of the two arbiters is active. An arbiter is active
only while a majority of three voters -- the two arbiters and the witness -- has granted it a lease
that has not run out. The witness is the voter that is never a candidate: it only answers the
arbiters' requests. It never interacts with sequencer, matching engine or publisher instances.

It answers each request by the rules every voter follows (`applications/fix_common/LeaseVoter.hpp`):

- It grants a lease to at most one arbiter at a time. Granting one is a promise not to grant a lease
  to the other arbiter until the lease period has passed, counted from the moment of granting.
- It grants nothing for one lease period after it starts.
- It never grants an epoch below the highest it has granted.

It keeps nothing on disk. Waiting out one lease period at startup is what makes that safe: any
promise it made before it stopped has run out by the time it grants again.

The witness must be deployed on **failure-independent infrastructure** -- different power supply,
different network switch, ideally a different rack -- from both arbiter machines. If the witness
shares a failure domain with one arbiter, a single event can take out that arbiter and the witness
together, and the surviving arbiter then holds only its own vote and cannot be active. The witness's
value depends entirely on its independence.

---

## PDU Protocol

| PDU | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `LeaseRequest` | 130 | Arbiter → witness | Ask to be the active arbiter, or renew |
| `LeaseGrant` | 131 | Witness → arbiter | Grant it, as voter 3 |
| `LeaseRefusal` | 132 | Witness → arbiter | Refuse it, with the highest epoch the witness has granted and the reason |

The witness logs when the arbiter it grants to changes, not at every renewal.

---

## What the Witness Does NOT Do

- It does not store anything on disk.
- It does not contact sequencer, matching engine or publisher instances.
- It does not initiate connections -- it only accepts inbound connections from the two arbiters.
- It does not vote on which instance of a component pair leads; the arbiter pool does that.

---

## Port Allocation

| Port | Usage |
|------|-------|
| 7100 | Inbound connections from arbiters (lease requests) |

---

## Configuration

| Key | Purpose |
|-----|---------|
| `[network] listen_port` | Inbound arbiter connections (default 7100) |
| `[lease] period_milliseconds` | How long a grant lasts, and how long the witness grants nothing after starting |
| `[lease] drift_allowance_milliseconds`, `renewal_interval_milliseconds` | Read so the configuration can be checked for consistency; the witness holds no lease itself |

The `[lease]` values are expanded from the environment's `[shared]` section, because every voter
must use the same values.

---

## See Also

- [Arbiter](arbiter.md) -- the two arbiters the witness votes on
- [Deciding leadership by majority, with leases](../availability/majority_leases.md) -- the rules and what happens in each failure
