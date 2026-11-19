# Witness {#witness}

## Role

The witness is the third voter in deciding which of the two arbiters is active. An arbiter is active
only while a majority of three voters -- the two arbiters and the witness -- has granted it a lease
that has not run out. The witness is the voter that is never a candidate: it only answers the
arbiters' requests. It never interacts with sequencer, matching engine or publisher instances.

It answers each request by the rules every voter follows (`libraries/pubsub_itc_fw/include/pubsub_itc_fw/LeaseVoter.hpp`):

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
| 11100 | Inbound connections from arbiters (lease requests), in every environment file |

---

## Configuration

| Key | Purpose |
|-----|---------|
| `[network] listen_port` | Inbound arbiter connections. Required; the environment files set 11100 |
| `[voting_pair] name / pair_id` | The pair the witness votes in: the two arbiters, `arbiter` 4, the same number as the arbiters' `[own_pair]`. A request for any other pair is dropped and logged |
| `[ha] enabled` | Must be true. With high availability off there are no arbiters to vote on, so the witness logs an error and refuses to start rather than run idle |
| `[lease] period_milliseconds` | How long a grant lasts, and how long the witness grants nothing after starting |
| `[lease] drift_allowance_milliseconds`, `renewal_interval_milliseconds` | Read so the configuration can be checked for consistency; the witness holds no lease itself |
| `[technical_events] primary_host / primary_port / secondary_host / secondary_port` | Where each matching engine publisher serves the `technical_events` topic. The witness subscribes to it to learn of the end of the trading day, and stops for the end of day when the end-of-day end of its own day arrives ([start_and_end_of_day.md](start_and_end_of_day.md), section 4.4, decision 10c). `deploy.py` fills these in; a publisher the environment does not start has an empty host and is not connected to |
| `[end_of_day] last_state_file` | The file of this instance's last trading state, which outlives the day. It records the end of the day once seen, and is read first at start-up: a witness that finds the end of its own day there exits at once, with status 3 |

The `[lease]` values are expanded from the environment's `[shared]` section, because every voter
must use the same values.

## Code

The witness is a program in `applications/witness`: `Witness`, `WitnessThread`, `WitnessConfiguration` and
`WitnessConfigurationLoader`, in the namespace `witness`, with `main.cpp` reading the configuration file and
running `Witness`, and their tests in `applications/witness/tests`. It is built on the lease classes in the
library, `libraries/pubsub_itc_fw`, which decide each vote it gives.

---

## See Also

- [Arbiter](arbiter.md) -- the two arbiters the witness votes on
- [Deciding leadership by majority, with leases](../availability/majority_leases.md) -- the rules and what happens in each failure
