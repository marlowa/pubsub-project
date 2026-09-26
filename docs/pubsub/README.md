# Pub/sub

The project's namesake: topic-based publish and subscribe over the WAL. The MEP is built and running; **OAR is
not yet built**, and the documents describing it are the design of record rather than history.

- [pubsub.md](pubsub.md) — The current pub/sub design, with publishing and subscribing worked examples
- [flow_control.md](flow_control.md) — Flow control, backpressure and the control channel. **Not yet implemented**
- [mep_oar.md](mep_oar.md) — The MEP and OAR design sketch. The MEP half is built; the OAR half is not
- [mep_rewire_and_oar.md](mep_rewire_and_oar.md) — Rewiring the MEP onto TopicPublisher (done) and building OAR (planned)
- [oar_bus_deduplication.md](oar_bus_deduplication.md) — Evidence gathered on broker-side deduplication for OAR's bus publisher
- [oar_external_stream.md](oar_external_stream.md) — How OAR publishes to the external messaging system, Pulsar or Kafka: the interface, the Avro schema, schema evolution and registration, ordering, and configuration. **Design, not implemented**

---

Back to the [documentation contents](../README.md).
