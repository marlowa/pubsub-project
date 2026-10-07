# The venue

The deployable components. Each is a process you can start, with its own configuration and its own place in
the order flow. The clickable [architecture map](../orientation/architecture.md) is the fastest way in.

- [fix_order_gateway.md](fix_order_gateway.md) — The FIX session layer: logon, sequence numbers, execution reports
- [binary_order_gateway.md](binary_order_gateway.md) — The binary protocol gateway and its open-order pool
- [gateway_throttles.md](gateway_throttles.md) — Per-session limits on placing and cancelling, configured per comp id. The amend limit is provisioned too, but neither gateway supports amends yet
- [matching_engine.md](matching_engine.md) — The book of open orders, which survives the process and is copied to the standby; leadership and catch-up. It does not match orders
- [sequencer.md](sequencer.md) — The sequencing design: ordering, the WAL commit, and fanout
- [sequencer_app.md](sequencer_app.md) — The sequencer as a deployed component
- [arbiter.md](arbiter.md) — The third party that settles leadership when the peers cannot
- [witness.md](witness.md) — The quorum witness
- [authentication_service.md](authentication_service.md) — Credential checking for member logons
- [admin_service.md](admin_service.md) — The Java administration service
- [fix_test_client.md](fix_test_client.md) — The web client used to drive the venue by hand or by script, through either gateway
- [trading_phases.md](trading_phases.md) — What the venue is doing, how it says so, and why a halt cannot leave the process that declares it

---

Back to the [documentation contents](../README.md).
