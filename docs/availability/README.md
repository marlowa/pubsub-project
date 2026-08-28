# High availability

How the venue survives a process dying, and how it survives a machine dying. These are two different problems with
different time budgets, and conflating them is the subject of a bug entry.

- [design_notes.md](design_notes.md) — The decision record: primary/secondary against leader/follower, why no STONITH, the two loops
- [wal_and_ha.md](wal_and_ha.md) — How the log and the HA model fit together
- [gateway_ha.md](gateway_ha.md) — Session identity, cancel-on-disconnect, and what a member sees across a failover
- [session_binding.md](session_binding.md) — how a session outlives its connection: the gateway/sequencer protocol, and what each message is for
- [resend_provenance.md](resend_provenance.md) — which outbound number carried what, and why a resend cannot currently say
- [order_acceptance.md](order_acceptance.md) — refusing orders the venue cannot process, and telling the member so
- [matching_engine_presence.md](matching_engine_presence.md) — asking the arbiter whether any matching engine exists, rather than waiting on a timer to guess
- [process_death.md](process_death.md) — the inner loop: what is settled, what `launch.py` already does, and the measurement that ruled out a shared-memory journal

---

Back to the [documentation contents](../README.md).
