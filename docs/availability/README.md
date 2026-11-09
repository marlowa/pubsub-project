# High availability

How the venue survives a process dying, and how it survives a machine dying. These are two different problems with
different time budgets, and conflating them is the subject of a bug entry ([BUG-0029](../bug_list.md#bug_0029)).

- [design_notes.md](design_notes.md) — The decision record: primary/secondary against leader/follower, why no STONITH, the two loops
- [wal_and_ha.md](wal_and_ha.md) — How the log and the HA model fit together
- [gateway_ha.md](gateway_ha.md) — Session identity, cancel-on-disconnect, and what a member sees across a failover
- [session_binding.md](session_binding.md) — how a session outlives its connection: the gateway/sequencer protocol, and what each message is for
- [resend_provenance.md](resend_provenance.md) — which outbound number carried each execution report, and how a resend uses that to send reports again and gap-fill the rest
- [order_acceptance.md](order_acceptance.md) — refusing orders the venue cannot process, and telling the member so
- [matching_engine_presence.md](matching_engine_presence.md) — a design, not built: asking the arbiter whether any matching engine exists, rather than waiting on a timer to guess
- [process_death.md](process_death.md) — the inner loop: what is settled, what `launch.py` already does, and the measurement that ruled out a shared-memory journal
- [majority_leases.md](majority_leases.md) — how the venue decides which instance leads: only while a majority of three voters grants it a lease; the rules, each failure, and where it is implemented
- [change_of_sequencer_leader.md](change_of_sequencer_leader.md) — keeping every order and report across a change of sequencer leader: four defects, the guarantees G1 to G5 the venue must keep, the options for each part with their costs, and which was chosen; most parts are built
- [a_follower_behind_does_not_lead.md](a_follower_behind_does_not_lead.md) — how a leader that sends to the matching engine without waiting for its follower stops that follower taking the lead: the problem, why Raft's rule does not fit, the design and its decisions; built, apart from the operator's tool for a lost machine
- [follower_log_repair.md](follower_log_repair.md) — how a sequencer follower's log is made a copy of its leader's again when it rejoins: the epoch on each record, finding the last record the two logs agree on, discarding after it and sending what it missed
- [ha_in_the_library.md](ha_in_the_library.md) — why most of the high availability machinery is in the applications rather than the library, and the agreed direction for moving it: a replicated, sequenced log in the library, and applications written as deterministic state machines
- [ha_library_boundary.md](ha_library_boundary.md) — the design, area by area, of what the library provides for high availability and what each application supplies, with the order of work that phases 3 to 5 and 7 to 10 of the recovery plan follow (built)
- [replicated_wal_in_the_library.md](replicated_wal_in_the_library.md) — the design for phase 4 of the recovery plan: the library's replicated WAL, written by one thread on the leader and on the follower (built)
- [following_the_wal.md](following_the_wal.md) — the design for phase 5: following the WAL through the library, on a reading thread of its own, so that the thread that sequences orders does not stop to read (built)
- [deterministic_matching_engine.md](deterministic_matching_engine.md) — the design for phase 7 of the recovery plan: a matching engine that, given the same commands, produces the same book and the same reports, with identifiers made from the position in the WAL, and every decision the engine takes on its own written into the WAL as a command (built)
- [lease_classes_in_the_library.md](lease_classes_in_the_library.md) — the design for phase 3: moving the lease classes and their messages into the library, then the arbiter and the witness as a voting service, then removing the lock and the disk waits from the promise recorder (built)
- [both_engines_apply_the_wal.md](both_engines_apply_the_wal.md) — the design for phase 8: both matching engines apply every committed command from the WAL, only the leading engine's reports are sent on, and a promoted engine keeps every order (built)
- [voided_commands.md](voided_commands.md) — the second design of phase 8: voiding a command that crashes both matching engines, halting when no matching engine leads for too long, and an operator lifting the halt (built)
- [far_back_catch_up.md](far_back_catch_up.md) — what a matching engine catching up from far back in the WAL must do so that the venue keeps working meanwhile, and the limit on how long the venue recovers on its own (built)
- [recording_statements_in_the_background.md](recording_statements_in_the_background.md) — the design for BUG-0142: the arbiter writes leaders' statements to disk on a thread of its own, so that the thread answering lease requests never waits for the disk (designed and agreed, not built)
- [pacing_the_engines_reports.md](pacing_the_engines_reports.md) — the design for BUG-0234: a matching engine catching up sends its reports no faster than the sequencer reads them, so its connection for reports is never closed and no report is lost (built)
- [one_check_against_sequencing_twice.md](one_check_against_sequencing_twice.md) — the design for phase 9: one check, on every command, against sequencing it twice, and the gateways' store of unanswered commands in the library (built)
- [publishers_sent_from_memory.md](publishers_sent_from_memory.md) — the design for BUG-0171 (built): sending the matching engine publishers each committed record from a copy kept in memory instead of reading it back from the WAL
- [reports_held_until_sessions_are_announced.md](reports_held_until_sessions_are_announced.md) — the design for BUG-0190: a sequencer that leads before every gateway has announced its sessions holds the reports for sessions it does not know of, and sends them once the session is announced (built)
- [test_events.md](test_events.md) — the design for phase 6: the high availability tests check what a member receives, and test events defined in the DSL, instead of the wording of Quill log messages; with a new generator that writes plain Python from the DSL (built)
- [test_events_inventory.md](test_events_inventory.md) — every check in `ha_test.py` that reads a Quill log, grouped by the kind of fact it proves
- [state_between_pair_instances.md](state_between_pair_instances.md) — every message sent between the two instances of a pair outside the WAL, what it carries, and whether a lost or repeated copy can leave the receiver wrong without its knowing
- [tla/findings.md](tla/findings.md) — the majority-lease design and its rule 11 specified in TLA+ and model checked: what holds, the counterexamples that show each rule is needed, and how the models compare with the design principles

---

Back to the [documentation contents](../README.md).
