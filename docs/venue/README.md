# The venue

The deployable components. Each is a process you can start, with its own configuration and its own place in
the order flow. The clickable [architecture map](../orientation/architecture.md) is the fastest way in.

- [fix_order_gateway.md](fix_order_gateway.md) — The FIX session layer: logon, sequence numbers, execution reports
- [binary_order_gateway.md](binary_order_gateway.md) — The binary protocol gateway and its open-order pool
- [matching_engine.md](matching_engine.md) — The order book, replication to the peer, and the HA role machine
- [sequencer.md](sequencer.md) — The sequencing design: ordering, the WAL commit, and fanout
- [sequencer_app.md](sequencer_app.md) — The sequencer as a deployed component
- [arbiter.md](arbiter.md) — The third party that settles leadership when the peers cannot
- [witness.md](witness.md) — The quorum witness
- [authentication_service.md](authentication_service.md) — Credential checking for member logons
- [admin_service.md](admin_service.md) — The Java administration service
- [fix_test_client.md](fix_test_client.md) — The FIX test client used to drive the venue

---

Back to the [documentation contents](../README.md).
