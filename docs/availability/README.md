# High availability

How the venue survives a process dying, and how it survives a machine dying. These are two different problems with
different time budgets, and conflating them is the subject of a bug entry.

- [design_notes.md](design_notes.md) — The decision record: primary/secondary against leader/follower, why no STONITH, the two loops
- [wal_and_ha.md](wal_and_ha.md) — How the log and the HA model fit together
- [gateway_ha.md](gateway_ha.md) — Session identity, cancel-on-disconnect, and what a member sees across a failover
- [session_binding.md](session_binding.md) — how a session outlives its connection: the gateway/sequencer protocol, and what each message is for
- [resend_provenance.md](resend_provenance.md) — which outbound number carried each execution report, and how a resend uses that to send reports again and gap-fill the rest
- [order_acceptance.md](order_acceptance.md) — refusing orders the venue cannot process, and telling the member so
- [matching_engine_presence.md](matching_engine_presence.md) — asking the arbiter whether any matching engine exists, rather than waiting on a timer to guess
- [process_death.md](process_death.md) — the inner loop: what is settled, what `launch.py` already does, and the measurement that ruled out a shared-memory journal
- [majority_leases.md](majority_leases.md) — how the venue decides which instance leads: only while a majority of three voters grants it a lease; the rules, each failure, and where it is implemented
- [change_of_sequencer_leader.md](change_of_sequencer_leader.md) — keeping every order and report across a change of sequencer leader: four defects, what the venue must guarantee, the options for each part with their costs, and a recommendation (a design for review)
- [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md) — how a leader that sends to the matching engine without waiting for its follower stops that follower taking the lead: the problem, why Raft's rule does not fit, the design and its two decisions (for review)
- [follower_log_repair.md](follower_log_repair.md) — repairing a sequencer follower's log when it rejoins: today it skips what it missed and keeps records its leader does not have; the epoch on each record, finding the last record the two logs agree on, discarding after it and sending what it missed (a design for review)
- [tla/findings.md](tla/findings.md) — the HA design specified in TLA+ and model checked: what held, eight findings with counterexamples, and directions for fixing

---

Back to the [documentation contents](../README.md).
