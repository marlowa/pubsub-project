# Pub/Sub — Requirements (working notes)

**Status:** Requirements-gathering. Started 2026-07-05; topic-naming dimension decided
2026-07-11; architecture principles and a first pass over dimensions 2–5 discussed
2026-07-11 (this update). A **first-cut MEP already exists** (committed `9ff45c6`,
"Added first version of MEP": `applications/topics.dsl` +
`applications/matching_engine_publisher/`, wired into the build) — it is treated here as a
draft to challenge, not a settled baseline.

> **Note for the reader:** R1–R3 are decided. The architecture principles (P1–P6) below are
> decided constraints. Dimensions 2–5 carry **provisional positions and open sub-questions**,
> not final decisions — they are written up here precisely so they can be read and thought
> about away from the keyboard.

## Purpose of this document

`docs/design/mep_tap.md` is a *solution* design (PDUs, ports, class layouts). It is thin on
stated **requirements**. This document captures the requirements first, and is deliberately
kept separate. The solution doc is treated as a **draft to challenge, not a settled
baseline** — where these requirements contradict it, the requirements win and the solution
doc gets revised.

Requirements are grounded in a **reference system** — a production pub/sub system the author
has operated. It has known problems and sub-optimal solutions, but it holds together and
works, so it is a real source of requirements guidance. The warts are often where the real
requirements hide.

## Terminology (settled 2026-07-11)

Plain pub/sub nouns, no "stream" (which drags in Kafka / stream-processing connotations that
do not fit this model):

- a **topic**;
- a **publisher** that **publishes messages** to a topic;
- **subscribers** that **subscribe** to a topic and **receive** every message published to it.

The domain unit is a **message** (e.g. an `addOrder`). At the wire/WAL level the carried
payload is a **record** (`TopicRecord`). Never "stream".

## The reference system (what "pub/sub" means here)

- The reference system is **reliable UDP multicast plus a broker**. "Reliable" is a *transport*
  guarantee: no reordering, no drops, no duplicates — the application treats the multicast
  fabric as being as trustworthy as TCP; it is just configured with multicast group
  addresses instead of unicast peers.
- The **broker** provides the *decoupling*: publishers and subscribers do not rendezvous
  directly — they meet at the broker, which also retains messages so a late subscriber gets
  everything. There is **one broker and several publishers**.
- Why we care: this is the system whose *semantics* we want (multiple consumers, the
  rendezvous problem, late subscribers, replay), but whose *mechanism* we deliberately do
  **not** copy (see P1).

## Architecture principles (decided in discussion 2026-07-11)

- **P1 — No central broker process. Ever.** This is a hard constraint. The design is
  *brokerless* by intent. No single process may act as a broker. (Reliable UDP was also
  discounted: implementing reliable UDP ourselves is too hard, so the same semantics are
  delivered over unicast TCP.)
- **P2 — The per-publisher WAL is how we stay brokerless.** Instead of a central store that
  every publisher routes through, each publisher keeps its **own** WAL (its own retained
  history) and serves its own late joiners and replay directly. Distributed retention, no
  central point. That is *why* the MEP has a WAL — not because it is a broker.
- **P3 — MEP is a publisher, not a broker.** There can be several publishers (MEP now; an
  instrument-prices publisher is likely later). A future prices component is just another
  publisher; it would not be a broker either.
- **P4 — Static topic→publisher binding.** Because a topic's history lives in exactly one
  publisher's WAL, each topic has **exactly one owning publisher**, fixed and not
  reassignable at runtime. You cannot make publisher P2 start publishing topic T1 when T1's
  history is in P1's WAL — that would fragment the record and break replay/ordering. A
  publisher may own several topics; a topic has one owner. A subscriber must know the owner
  to connect to the right WAL for replay ⇒ **topic ownership is a stable, known catalog
  fact** (fits the DSL catalog, R2).
- **P5 — Globally authoritative seq_no.** Verified in code: when the MEP receives a record
  from the sequencer it does `wal_.append(view.seq_no, …)` — it stores the record under the
  **sequencer's** seq_no, not a locally-assigned one. Both MEP HA instances follow the
  sequencer independently and key their WALs by the *same* seq_no. So a cursor of "seq_no N"
  means the same thing on either instance. This total order is the linchpin that makes
  cursor-based replay and failover coherent.
- **P6 — Subscriber owns its cursor.** The subscriber presents its resume position
  (`TopicSubscribeRequest.from_seq_no`); the publisher does not remember it across a failover
  (see D5). This is deliberate — replicating per-subscriber cursor state between publisher
  instances would be exactly the centralised coordination P1 forbids.

## Current state of the MEP (as built, 2026-07-11)

- **It is explicit unicast-TCP fanout, and is *not* "pubsub" yet.** Publishing to subscribers
  is a `for`-loop that sends a `TopicPage` PDU over each subscriber's unicast socket
  (`publish_wal_record_to_topic_subscribers`). Conceptually every send is still a unicast PDU.
  The MEP was always *intended* to be the first component to use pubsub; the current fanout
  is scaffolding standing in until the pubsub mechanism exists, at which point the MEP is
  extended to use it.
- **Today it publishes to nobody.** `applications/` has no TAP and no market-data program, so
  the two topic-subscriber listen ports (`orders`, `execution_reports`) have zero connected
  consumers. The MEP's only *live* traffic is: following the sequencer's WAL
  (`WalSubscribeRequest`/`WalAck`, receiving `WalRecord`s) and HA (arbiter/peer heartbeats and
  arbitration). The publish-side fanout runs against an empty subscriber set.

## Near-term scope (settled 2026-07-11)

- **In scope now:** the pubsub mechanism with **MEP as the first publisher** of two topics
  (`orders`, `execution_reports`) and **TAP as the subscriber** to both.
- **Out of scope now:** market-data applications will also subscribe to the same topics, but
  that is much later. Design and validate against TAP — but do **not** bake in a
  *single*-subscriber assumption, since market-data joins the same topics later.

## The six dimensions

Dimensions 1 and 6 are settled toward *registered catalog* (R1–R3, P4, P6). Dimensions 2–5
have provisional positions below.

1. **What's published, and at what granularity.** Per instrument? Per message type? — SETTLED
   toward a recognised catalog (R1–R2). Current topics: `orders` (NOS+OCR) and
   `execution_reports` (ER).
2. **Delivery & ordering guarantees.** (provisional — see D2)
3. **Durability & replay.** (provisional — see D3)
4. **Fanout & backpressure.** (provisional — see D4)
5. **Failover semantics.** (OPEN — see D5)
6. **Subscriber lifecycle & identity.** Registered, and the **subscriber** tracks its own
   cursor (P6). SETTLED.

## Dimensions 2–5 — analysis & provisional positions

### D2 — Delivery & ordering (provisional: effective exactly-once, strict order)

- Within a single TCP connection we **already** get no-loss / no-duplicate / in-order
  delivery — that is just TCP. So per-connection, exactly-once-in-order is free.
- The **only** place a duplicate or gap can appear is the **seam**: a subscriber resuming
  from its cursor after a reconnect or a leader→follower failover (see D5).
- Provisional position: **at-least-once transport + receiver-side dedup by seq_no = effective
  exactly-once**. Because seq_no is a global total order (P5), a subscriber dedups trivially
  (track last-applied seq_no; discard anything ≤ it). True "no duplicate ever on the wire"
  would need the publisher pair to share each subscriber's exact progress — the centralised
  coordination P1 forbids — so we push dedup to the receiver. This is idiomatic to reliable
  multicast, where receivers recover/dedup by sequence number as a matter of course.
- **Open:** does the reference system genuinely hand consumers a no-duplicates guarantee, or do
  its receivers dedup by sequence number and *treat* that as exactly-once? That answer decides
  whether we owe machinery or just a documented "dedup by seq_no" contract.

### D3 — Durability & replay (provisional: full retained WAL, bounded by a retention window)

- A late or reconnecting subscriber can replay history from a publisher's WAL:
  `from_seq_no = 0` (oldest retained), `-1` (current head), or `N` (resume at N).
- The retained log is owned by the **publisher** (P2), per-publisher, not central.
- **Open:** how much history is retained — a time window, a size cap, both? This is a
  per-topic **policy** value (R3), hand-written TOML validated against the generated
  registry. Its value feeds D4 (a subscriber that falls outside the window cannot replay).

### D4 — Fanout & backpressure (DECIDED 2026-07-11 — see pubsub_flow_control.md)

Superseding the earlier ack-paced sketch. Full design in
[pubsub_flow_control.md](pubsub_flow_control.md); summary:

- One publisher fans a topic out to **multiple** subscribers (TAP now; market-data later),
  each with its own cursor.
- The publisher must **never block** on a slow subscriber. **Delivery is streamed and paced by
  the TCP socket, not by acks** (F1): send until `EAGAIN`, resume on `EPOLLOUT`, next record read
  straight from the WAL. The WAL is the backlog, so a lagging subscriber costs a cursor, not RAM.
  Per-page acks are rejected (a round-trip of latency per page, double the volume, no benefit).
- `TopicAck` is **demoted to a periodic truncation cursor** (F2), not a per-page pacing signal.
- A subscriber that lags past a **lag threshold** (before its next record is truncated) is
  **terminated** with explicit, recoverable notice (F3): a best-effort `TopicLagged` plus the
  reliable **gap-on-resubscribe** signal — never silent loss.
- Prompt signalling uses a **separate control channel** — a second TCP connection to the *same*
  port, classified by a new `role ∈ {Data, Control}` field on `TopicSubscribeRequest`,
  correlated by `subscriber_id`, **bidirectional** (F4).
- **Still open:** exact lag-threshold semantics; whether to add a soft warning stage;
  control-channel arrival timeout. (Listed in pubsub_flow_control.md.)

### D5 — Failover semantics (OPEN — flagged, not decided)

- The MEP is an HA primary/secondary pair (arbiter-elected leader). Only the leader publishes;
  the follower answers `TopicNotLeader` and holds subscriber connections warm.
- Mechanics (mostly already built): leader dies → subscriber's connection drops (or a stale
  attempt gets `TopicNotLeader`) → the subscriber's client library reconnects to the *other*
  MEP endpoint → the subscriber presents its cursor (P6) → the new leader replays from that
  cursor out of its seq_no-identical WAL (P5). Ordering is preserved (replay is in seq_no
  order).
- Because the old leader's per-subscriber cursor **dies with it** (not replicated — P1/P6),
  the resume point is **whatever the subscriber presents**. So resume *correctness* is owned
  by how accurately the subscriber persists its own consumption progress. Duplicates at the
  seam arise only if the subscriber's persisted cursor lags what it actually processed (it
  crashed between "did the work" and "recorded that it did the work").
- Provisional position (couples to D2): **transparent redirect + resume from cursor**, with
  duplicates at the seam absorbed by **receiver-side seq_no dedup**. "Transparent" means the
  client library retries until the new leader is up — there is a brief election window where
  neither instance is leader and reconnects are refused, so it is not zero-interruption.
- **Open / to think about:** is receiver-side dedup an acceptable contract for TAP, or is a
  stronger guarantee expected? At what granularity does TAP persist its cursor, and is that
  persistence atomic with its side effects (its L3 book, anything it forwards on)? What is
  the acceptable duplicate/interruption behaviour during the election window?

## TopicAck is NOT a TCP ack (clarification, 2026-07-11)

This caused confusion, so stated plainly:

- **TCP ack (transport):** TCP guarantees the bytes you write reach the peer's socket, in
  order, no loss — or the connection breaks. You never manage it. This is *not* what
  `TopicAck` is.
- **TopicAck (application-level):** the subscriber sends `TopicAck { last_seq_no }` after it
  has **processed** a page. That is different information from "the bytes arrived": TCP tells
  you bytes reached the peer's *kernel*; it cannot tell you the subscriber *read, decoded and
  acted on* them. "Processed" is an application fact, above TCP.
- **What it is for — one job, given the D4 decision (F1/F2, pubsub_flow_control.md):**
  **WAL truncation (D3).** The publisher only reclaims a WAL segment once *every* connected
  subscriber has processed past it. "Bytes delivered to the socket" does not tell the publisher
  "no one needs to replay this anymore"; the app-level cursor does. So `TopicAck` is now a
  **periodic truncation cursor**, sent coarsely (every N records / every T), NOT per page.
  - *(Superseded: earlier this doc had `TopicAck` also *pace* delivery per page. That was
    dropped — delivery is paced by the TCP socket + WAL, see D4/F1. No per-page acks.)*
- **At the failover seam** `TopicAck`'s role is only *indirect*: it lets the *publisher* truncate.
  Resume *correctness* is owned by the *subscriber's* own cursor (P6), because the publisher's
  ack-tracked cursor is lost with the old leader.

## Requirements (decided so far)

Each traces back to a reference-system observation or an explicit decision.

- **R1 — Recognised topics.** Subscribing to an unknown topic is rejected with a clear
  error; the set of topics is a known catalog, not an open string API. (From the reference
  system's ASCII topic-name file.)
- **R2 — Catalog generated from the schema.** The topic catalog (name + which pdu_ids each
  topic carries) is **generated from the DSL**, so it cannot drift from the PDU ids. Topic
  *identity* is schema; it lives with the message definitions, not in hand-written config.
- **R3 — Identity vs policy split.** Per-topic *operational policy* (retention window, lag
  threshold, page size, listener port) is hand-written TOML, validated at load against the
  generated registry. Identity is generated; policy is config-but-constrained.

The mechanism for R1–R3 (a `topic` grouping construct in the DSL, a `Topics`→`PduId` rename,
and a transitive `include` mechanism) is designed in
[dsl_topic_catalog.md](dsl_topic_catalog.md), which also holds the implementation plan.
It is **implemented and live-verified** as of 2026-07-11.
