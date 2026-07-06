# Pub/Sub — Requirements (working notes)

**Status:** Draft, requirements-gathering. Started 2026-07-05. **No code yet — a long way from it.**

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

_(To fill in next session: how the work system's publisher works — what it consumes, what it
publishes, how consumers get their data — and specifically the "sub-optimal but it holds
together" parts, which is where the real requirements live.)_

## Requirements (to be derived from the above)

_(To fill in as we converge. Each requirement should trace back to a work-system observation
or an explicit decision.)_
