# Broker-Side Deduplication for OAR's Bus Publisher — Evidence

> The order activity recorder was called TAP, the Trade Activity Publisher, in earlier
> documents. Sessions in `docs/history/` and anything under `docs/superseded/` keep that
> name, because they are records of what was said at the time.


**Status: evidence gathered 2026-08-16, no decision taken.** OAR does not exist yet. This
records what was checked, with sources, so the reasoning behind `BusPublisher`'s interface is
traceable rather than remembered.

---

## The problem this is evidence for

OAR will publish records downstream over an abstract `BusPublisher`. Because OAR resumes from
its own persisted cursor, and because that cursor must be written *after* a publish is
confirmed (a crash between the two must re-publish rather than skip — see below), OAR is
inherently **at-least-once**. Duplicates are routine, not exceptional: every restart and every
promotion re-publishes some records.

The default answer — "downstream consumers deduplicate on `seq_no`" — is a contract by
convention. It has to be implemented correctly by every consumer, it needs enough retained
state to cover the largest possible replay burst, and a single consumer that gets it wrong
publishes duplicate orders into whatever it feeds. The question is whether the broker can
absorb duplicates instead, so no consumer has to.

**The ordering rule that makes OAR at-least-once, stated once so the rest follows from it:**
persist the cursor *after* the publish is confirmed. Crash between publish and cursor-write
re-publishes (duplicate, absorbable). Crash between cursor-write and publish skips the record
permanently (unabsorbable). The two failure costs are wildly asymmetric, so the order is
forced, and duplicates are the price.

---

## Apache Pulsar

### The mechanism

Deduplication keys on the pair **`(producerName, sequenceId)`**, both supplied by the
application:

> "To achieve de-deduplication, Pulsar relies on the (producerName, sequenceId) to track the
> last sequence id that was committed on the log for each individual producer."
> — [PIP-6: Guaranteed Message Deduplication](https://github.com/apache/pulsar/wiki/PIP-6:-Guaranteed-Message-Deduplication)

The broker keeps two maps, `last-sequence-pushed` (checked and updated on each publish request)
and `last-sequence-persisted` (updated on write acknowledgement from BookKeeper). Cost on the
publish path is *"only one additional local in-memory hashmap lookup and update"* (PIP-6).

### It survives broker restart and topic ownership change

This was the specific question. PIP-6 describes both the snapshot and the recovery:

> "Every `N` entries persisted (eg: 1000 entries), perform a 'mark-delete' on the dedup cursor,
> attaching the `last-sequence-persisted` map as additional metadata on the cursor position."

and on recovery:

> "Open the dedup cursor and get the recovered metadata properties" … "Replay all the entries
> from the mark-delete position to the end" … "For each entry, deserialize the message metadata,
> extract the `producerName` and `sequenceId` and update the sequence id map"

So the snapshot is an **optimisation, not the correctness mechanism** — the replay from the
mark-delete position to the end reconstructs whatever the snapshot missed. State is held on a
dedicated `ManagedCursor`, which is durable and replicated like any other Pulsar cursor.

### Application-supplied sequence IDs with gaps are explicitly supported

This matters because the intended sequence ID is the MEP's `seq_no`, which is globally
monotonic but has gaps within any one partition or topic:

> "`MessageBuilder.setSequenceId()` allows the application to have custom sequence schemes, also
> with 'holes' in the middle. For example, if the producer is reading data from a file and
> publishing on a Pulsar topic, it might want to use the offset in the file for a particular
> record as the sequence id."

That is the same shape as using a WAL sequence number, and it is the documented intent rather
than a reinterpretation of it.

### Producer requirements

From [Message deduplication](https://pulsar.apache.org/docs/next/cookbooks-deduplication/) and
[Work with producer](https://pulsar.apache.org/docs/next/client-libraries-producers/):

| Requirement | Detail |
|---|---|
| Broker config | `brokerDeduplicationEnabled=true`, requires a broker restart. Default is **disabled**. Overridable per namespace (`pulsar-admin namespaces set-deduplication`) or per topic (`topics set-deduplication`) |
| Producer name | **Mandatory.** "You **must** set an explicit producer name when using message deduplication" |
| Message timeout | Must be `0` (no timeout). The docs recommend the client "retry infinitely the messages until it succeeds" |
| Producer count | `brokerDeduplicationMaxNumberOfProducers` defaults to 10,000 producers tracked per topic |

### `getLastSequenceId()` exists, and is intended for exactly this recovery case

> "`Producer.getLastSequenceId()` gets the sequence id of the last message that was published by
> this producer. This method is useful for recovery scenarios where an application crashes and
> restarts, allowing it to resume publishing from the record next to the last successfully
> published record before the crash."

Note this is a *convenience*, not the safety mechanism. With broker-side dedup enabled OAR does
not need to consult it: a promoted instance may re-publish from a conservative cursor and the
broker discards what it has already seen. Keeping OAR's resume decision local — rather than
querying the bus — keeps a venue correctness property from depending on an external system's
availability.

---

## Apache Kafka, for comparison

Kafka's idempotent producer does **not** span a producer restart, which is precisely OAR's
failover case:

> "If an idempotent producer is stopped and restarted, it gets a new PID assigned, i.e., PIDs
> don't 'survive'."

> "When a producer restarts, a new PID is assigned to this producer instance by Kafka and also
> memory buffer is lost … messages which were sent but not acknowledged before restart are now
> in unknown state."

The mechanism that *does* span restarts is the transactional producer, because the identifier
is application-supplied rather than broker-assigned:

> "A `transactional.id` is a user config and thus on producer restart, the same
> `transactional.id` is used. This allows brokers to identify the same producer across producer
> restarts."

So Kafka can meet the requirement, but via transactions rather than plain idempotence — a
heavier mechanism than Pulsar's `(producerName, sequenceId)`.

---

## What is NOT established, and matters

**No source found tests producer restart or failover deduplication.** The most thorough
independent work available is Jack Vanlightly's chaos testing (50 runs per scenario), and it
covers TCP failures and *broker* failover, not producer restart. With dedup enabled both
systems produced zero duplicates and perfect ordering under those two scenarios. But he
explicitly notes the untested area, and flags a concern about Pulsar's snapshotting:

> "Pulsar's constraint: deduplication state requires the broker periodically snapshots the
> latest sequence number to a cursor, introducing potential gaps if a producer restarts before
> snapshots complete."

That concern reads as being in tension with PIP-6's recovery description, which replays from
the mark-delete position precisely to close such a gap. The article is from 2018 and may
predate or misread the replay path. **It is not resolved here**, and it is the one claim in
this document that should be settled by experiment rather than by reading, because it sits
exactly on OAR's failover path.

He also notes a point that cuts the other way, in Kafka's favour, for *broker* failover:
Kafka followers maintain the sequence map from the messages themselves, so a newly elected
leader has dedup state without a snapshot or replay step.

**Other caveats:**

- **Batching with external sequence IDs was broken.**
  [apache/pulsar#5476](https://github.com/apache/pulsar/issues/5476) — the producer did not
  check sequence IDs when adding to a batch, and computed `lastSequenceIdPublished` as
  `op.sequenceId + op.numMessagesInBatch - 1`, which is wrong for externally-supplied IDs.
  Closed via PR #5491, targeted at 2.5.0. Any Pulsar older than that must not combine batching
  with application sequence IDs.
- **Producer names must be globally unique, and the broker fences on them.** "Only one producer
  with that name can publish on a topic at a time." This composes well with arbiter-mediated
  leadership — it is a second, independent fence — but it introduces a **promotion
  interaction that must be tested**: a promoted OAR secondary claiming the same producer name
  may be refused while the broker still believes the dead primary holds it.
- **Dedup state is per-topic and in-memory before snapshot**, so it scales with producer count.
  OAR is a single producer, so this is not a concern here, but it is the reason the 10,000
  default exists.

---

## Maturity and independent verification (checked 2026-08-16)

The question this section answers: is broker-side dedup something to *rely* on?

**Adoption is not the concern.** 736 companies tracked using Pulsar, Yahoo running it at scale,
724 contributors and 13,000+ commits on the main repository, Pulsar 5.0 GA targeted for
September/October 2026. This is not a thinly-used system.

### What Jepsen is, and why its absence is evidence

[Jepsen](https://jepsen.io/) is the distributed-systems safety research outfit run by Kyle
Kingsbury (formally Jepsen, LLC). In its own words it "aims to improve the safety of distributed
databases, queues, consensus systems, etc." — and in practice it is the nearest thing this field
has to an independent auditor.

The method is empirical rather than analytical. Jepsen drives a real cluster with a concurrent
workload while injecting faults — network partitions, process crashes and pauses, clock skew,
file corruption — and records a history of every operation and its outcome. That history is then
checked against a stated correctness model (say, "this should behave like a linearizable
register"). The output is either "the history is consistent with the model" or a specific,
reproducible sequence of operations that violated it. The stated aim is to test "whether the
system lives up to its documentation's claims, file new bugs, and suggest recommendations for
operators."

Two things make this the relevant yardstick here. It tests the **implementation**, not the
design, so it catches the gap between what a protocol says and what the code does. And it is
adversarial: reports routinely document data loss in commercial systems, published without
vendor veto, so a clean report means something and an unflattering one still gets published.

The caveats matter too, and cut both ways. Analyses are often **commissioned and paid for** by
the vendor, so the set of systems tested reflects who chose to pay, not who most needed testing.
Absence from the list is therefore *weak* evidence about a system's quality — it may only mean
nobody commissioned a report. What it is *strong* evidence of is that **no independent
adversarial audit exists to consult**, which is a different and, for our purposes, more
important fact: it tells us the assurance has to come from somewhere else.

**With that established, the verification here is uneven — and unevenly in the wrong place.**

- **Jepsen has never published an analysis of Pulsar.** Checked against
  [jepsen.io/analyses](https://jepsen.io/analyses): Kafka (2013), Redpanda (2022), RabbitMQ,
  NATS and Bufstream are all listed; Pulsar is not. Redpanda and Bufstream are both younger and
  smaller systems that commissioned reports.
- **The storage layer has had a different and complementary treatment.** Jack Vanlightly formally
  verified the BookKeeper replication protocol in TLA+ in 2020–21, finding a protocol bug and an
  implementation bug, including a flaw in the writer-fencing mechanism
  ([bookkeeper-tlaplus](https://github.com/Vanlightly/bookkeeper-tlaplus)).

  *TLA+ is a specification language for describing concurrent and distributed algorithms
  precisely enough that a model checker can explore **every** interleaving of a small instance
  exhaustively.* Where Jepsen runs the real code and hopes the faults it injects happen to
  provoke a bug, TLA+ proves a property over all reachable states of a model — but of the
  **design**, not of the code that was actually written. The two are complements, not
  substitutes: TLA+ catches protocol flaws that testing might never hit; Jepsen catches the gap
  between the protocol and its implementation. Durability, in other words, has been modelled but
  not independently attacked.
- **Deduplication has not.** The formal work covers BookKeeper, not the broker's dedup path.
  StreamNative's own HA material on the 2.10 `AutoClusterFailover` / `ControlledClusterFailover`
  strategies covers cluster-to-cluster failover and explicitly does not discuss producer
  failover or dedup across it.

**The bug history is the deciding evidence.** Three deduplication defects, all 2019–2020, all
long fixed:

| Issue | Failure mode | Fixed in |
|---|---|---|
| [#5218](https://github.com/apache/pulsar/issues/5218) | Sequence id recorded *before* persistence. If the BookKeeper write failed, the client's retry was treated as a duplicate and dropped — **the message was never persisted at all** | 2.4.2 |
| [#6273](https://github.com/apache/pulsar/issues/6273) | A batch containing both duplicate and valid messages was rejected whole, so **valid messages were dropped** and consumers never saw them | 2.6.0 |
| [#5476](https://github.com/apache/pulsar/issues/5476) | Batching with externally-supplied sequence ids miscomputed the high-water mark | 2.5.0 |

Every one of them is **loss-shaped**. That is the pattern that matters here: when broker-side
deduplication misbehaves, it does not emit duplicates — it silently drops good messages.

### Position taken, on this evidence

**Do not make OAR's correctness depend on broker-side deduplication.**

OAR's whole design rests on an asymmetry: duplicates are absorbable, loss is catastrophic.
Broker-side dedup asks us to enable a feature whose *historical failure mode is silent loss*, in
order to avoid the failure we can already absorb. That is the wrong way round on our own
asymmetry, and no amount of "those bugs are fixed" changes the shape of the risk — an
unverified component in the loss path is worse than a verified one in the duplicate path.

So:

- **Baseline (load-bearing):** at-least-once from OAR, dedup downstream on `seq_no`.
- **Optimisation (never load-bearing):** broker-side dedup, enabled per deployment, to relieve
  consumers of the burden. If it silently drops something, the baseline is what must already
  have made OAR correct.

This does not weaken the interface conclusions below — stable producer identity and an explicit
sequence id are exactly what downstream dedup needs too. It changes only what we are permitted
to *rely* on.

The unresolved 2018 concern above (dedup state across producer restart) stays unresolved and
becomes less urgent under this position, because nothing safety-critical now hangs on it.

---

## Consequences for the `BusPublisher` interface

Independent of which broker is eventually chosen, and cheap now versus expensive later:

1. **Carry a stable producer identity.** Not derived from hostname, PID or instance role — a
   promoted secondary must present the same identity the primary did, or dedup keys on the
   wrong thing.
2. **Carry an explicit, application-supplied sequence ID**, set to the MEP `seq_no`. Monotonic
   with gaps is supported and is what a WAL sequence number looks like.
3. **Be idempotent by contract**, with the duplicate-absorbing mechanism named per
   implementation rather than assumed of consumers.

An interface shaped `publish(payload)` forecloses all three, and reopening them later means
changing every call site and probably the wire format. `publish(producer_identity, sequence_id,
payload)` costs nothing now and keeps Pulsar, Kafka-with-transactions, and a stub all reachable.

---

## Sources

- [PIP-6: Guaranteed Message Deduplication](https://github.com/apache/pulsar/wiki/PIP-6:-Guaranteed-Message-Deduplication) — the design document; snapshot and recovery mechanism
- [Message deduplication | Apache Pulsar](https://pulsar.apache.org/docs/next/cookbooks-deduplication/) — broker/namespace/topic configuration, producer requirements
- [Work with producer | Apache Pulsar](https://pulsar.apache.org/docs/next/client-libraries-producers/) — producer naming rules, `sequenceId` on the message builder
- [Effectively-Once Semantics in Apache Pulsar (Splunk)](https://www.splunk.com/en_us/blog/it/effectively-once-semantics-in-apache-pulsar.html) — snapshot/recovery narrative, stable-producer-name behaviour
- [Testing Producer Deduplication in Apache Kafka and Apache Pulsar (Jack Vanlightly)](https://jack-vanlightly.com/blog/2018/10/25/testing-producer-deduplication-in-apache-kafka-and-apache-pulsar) — independent chaos testing; the source of the untested-restart caveat
- [apache/pulsar#5476](https://github.com/apache/pulsar/issues/5476) — batching with external sequence IDs, fixed in 2.5.0
- [Idempotent Producer — Apache Kafka wiki](https://cwiki.apache.org/confluence/display/KAFKA/Idempotent+Producer) — PID assignment
- [Kafka Idempotent Producer (Conduktor)](https://www.conduktor.io/kafka/idempotent-kafka-producer) — PID does not survive restart; `transactional.id` as the mechanism that does
- [jepsen.io/analyses](https://jepsen.io/analyses) — the published-analyses list; Pulsar's absence, Kafka 2013, Redpanda 2022
- [bookkeeper-tlaplus](https://github.com/Vanlightly/bookkeeper-tlaplus) — TLA+ specification of the BookKeeper replication protocol
- [Learn about TLA+ and the formal verification of Apache BookKeeper](https://jack-vanlightly.com/blog/2021/10/9/learn-about-tla-and-the-formal-verification-of-apache-bookkeeper) — the protocol and implementation bugs it found
- [apache/pulsar#5218](https://github.com/apache/pulsar/issues/5218) — dedup dropped messages when a BookKeeper write failed; fixed 2.4.2
- [apache/pulsar#6273](https://github.com/apache/pulsar/issues/6273) — batches with mixed duplicate/valid messages dropped the valid ones; fixed 2.6.0
- [Failover strategies for Apache Pulsar (StreamNative)](https://streamnative.io/blog/failure-is-not-an-option-it-is-a-given) — 2.10 cluster failover; does not cover producer failover or dedup across it

## See Also

- [Pub/Sub](pubsub.md) — the upstream half: topics, cursors, retention, D4's slow-consumer drop
- [WAL](../durability/wal.md) — `seq_no` as the total order, and why it is the natural sequence ID
