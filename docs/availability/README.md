# High availability

How the venue survives a process dying, and how it survives a machine dying. These are two different problems with
different time budgets, and conflating them is the subject of a bug entry.

- [design_notes.md](design_notes.md) — The decision record: primary/secondary against leader/follower, why no STONITH, the two loops
- [wal_and_ha.md](wal_and_ha.md) — How the log and the HA model fit together
- [gateway_ha.md](gateway_ha.md) — Session identity, cancel-on-disconnect, and what a member sees across a failover
- [process_death.md](process_death.md) — STUB: evolving thoughts on handling process death. Nothing written yet

---

Back to the [documentation contents](../README.md).
