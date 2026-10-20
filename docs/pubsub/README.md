# Pub/sub

The project's namesake: topic-based publish and subscribe over the WAL. The matching engine publisher is built;
**the order activity recorder (OAR) is not yet built**, and the documents describing it are its design.

- [pubsub.md](pubsub.md) — The current pub/sub design, with publishing and subscribing worked examples
- [flow_control.md](flow_control.md) — The design for socket-paced delivery, lag handling and the control channel; implementation details are in [pubsub.md](pubsub.md)
- [mep_oar.md](mep_oar.md) — The design sketch of the matching engine publisher and the order activity recorder. The publisher half is built; the recorder half is not
- [mep_rewire_and_oar.md](mep_rewire_and_oar.md) — Moving the matching engine publisher onto `TopicPublisher` (done) and building the order activity recorder (planned)
- [oar_bus_deduplication.md](oar_bus_deduplication.md) — Evidence gathered on broker-side deduplication for OAR's bus publisher
- [oar_external_stream.md](oar_external_stream.md) — How OAR publishes to the external messaging system, Pulsar or Kafka: the interface, the Avro schema, schema evolution and registration, ordering, and configuration. **Design, not implemented**

---

Back to the [documentation contents](../README.md).
