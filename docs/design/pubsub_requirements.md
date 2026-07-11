# Pub/Sub — Requirements (working notes)

**Status:** Requirements-gathering. Started 2026-07-05; topic-naming dimension decided
2026-07-11. A **first-cut MEP already exists** (committed `9ff45c6`, "Added first version
of MEP": `applications/topics.dsl` + `applications/matching_engine_publisher/`, wired into
the build) — it is treated here as a draft to challenge, not a settled baseline.

## Purpose of this document

`docs/design/mep_tap.md` is a *solution* design (PDUs, ports, class layouts). It is thin on
stated **requirements**. This document captures the requirements first, and is deliberately
kept separate. The solution doc is treated as a **draft to challenge, not a settled
baseline** — where these requirements contradict it, the requirements win and the solution
doc gets revised.

Requirements are grounded in the **work system** — the production pub/sub in use at work.
It has known problems and sub-optimal solutions, but it holds together and works, so it is a
real source of requirements guidance. The warts are often where the real requirements hide.

MEP (Matching Engine Publisher) is the component that plays the publisher role.

## Open questions to resolve (the six dimensions)

These are the requirement dimensions to pin down. They are questions, not decisions.

1. **What's published, and at what granularity.** One topic per stream? Per instrument?
   Per message type? What does the work system actually slice topics on?
2. **Delivery & ordering guarantees.** At-least-once / exactly-once / best-effort? Strict
   per-topic ordering? Are gaps ever acceptable?
3. **Durability & replay.** Can a subscriber replay from arbitrary history, or only "from
   now"? Who owns the retained log, and for how long?
4. **Fanout & backpressure.** What happens to a slow consumer? Can a slow consumer ever
   stall the publisher or other consumers?
5. **Failover semantics.** Is failover transparent to consumers, or do they re-subscribe?
   Are duplicates on failover acceptable?
6. **Subscriber lifecycle & identity.** Open API or registered subscribers? Who tracks
   cursors — the publisher or the subscriber?

## Work system observations

- **Topic names are a recognised catalog.** The work system keeps an **ASCII file listing
  all recognised topic names**. Topics are not free-form strings; a name is either in the
  catalog or it is not. (This settles dimensions 1 and 6 toward *registered*, not *open*.)

_(Still to fill in: what the work publisher consumes/publishes, how consumers get their data,
and the "sub-optimal but it holds together" parts — dimensions 2–5.)_

## Requirements (decided so far)

Each traces back to a work-system observation or an explicit decision.

- **R1 — Recognised topics.** Subscribing to an unknown topic is rejected with a clear
  error; the set of topics is a known catalog, not an open string API. (From the work
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
