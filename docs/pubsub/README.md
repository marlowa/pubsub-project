# Pub/sub

The project's namesake: topic-based publish and subscribe over the WAL. The MEP is built and running; **TAP is
not yet built**, and the documents describing it are the design of record rather than history.

- [pubsub.md](pubsub.md) — The current pub/sub design, with publishing and subscribing worked examples
- [flow_control.md](flow_control.md) — Flow control, backpressure and the control channel. **Not yet implemented**
- [mep_tap.md](mep_tap.md) — The MEP and TAP design sketch. The MEP half is built; the TAP half is not
- [mep_rewire_and_tap.md](mep_rewire_and_tap.md) — Rewiring the MEP onto TopicPublisher (done) and building TAP (planned)
- [tap_bus_deduplication.md](tap_bus_deduplication.md) — Evidence gathered on broker-side deduplication for TAP's bus publisher

---

Back to the [documentation contents](../README.md).
