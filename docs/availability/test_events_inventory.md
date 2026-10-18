# Phase 6 inventory: every check that reads a Quill log, and what replaces it {#test_events_inventory}

This is step 1 of phase 6 ([test_events.md](test_events.md)): every place `scripts/ha_test.py` reads a Quill log, and
every phrase a unit test looks for in a captured log, with what each check proves and what replaces it. Andrew reviews it
before step 3, because it fixes the list of test events.

**Where the line falls between member checks and events was agreed with Andrew on 18 October 2026.** A check moves to
what a member receives wherever a member can see the fact. The matching engine's outcome for an order becomes an event
in three cases: the order was sent with `inject_order` and reaches no member by design; the scenario blocks the reports
on purpose, so the member's silence is the expected result; or the check is that an order was applied exactly once,
which a member cannot judge, since a report sent again after a change of leader, marked as a possible repeat, is
legitimate.

It was made on 18 October 2026 by parsing the script with Python's `ast` module and finding every call of a function
that reads a log: the general helpers (`poll_log_for`, `count_log_marker`, `count_lines_with_all`,
`read_complete_lines`) and the helpers that read one particular line (`me_cancel_on_failover_count`,
`me_resumed_book_size`, `me_inherited_book_size`, `_resent_report_count`, `_inherited_report_range_count`,
`_resend_resume_point`, `_resend_gap_fill`, `_held_grace_period_seconds`, `_gateway_progress_lines`,
`_gateway_progress_lines_since`, `gateway_progress_totals`, `find_last_me_ord`, `check_me_seq_monotonic` and
`_client_logon_seq_num`). The counts change as the script changes, so it is made again before each kind of check is
moved. Some calls are inside helpers used in several places, so a full run makes more checks than there are calls.

## Summary

171 calls read a log. What replaces them:

| Replacement | Calls | Meaning |
|---|---|---|
| Event | 125 | a test event the component sends, defined in the DSL |
| Member | 33 | what a member receives, checked at the test client |
| WAL files | 2 | the WAL files themselves, read by the test after the scenario |
| File (decision 5) | 5 | a file the fault-injection library creates (decision 5) |
| Stays | 2 | unchanged: not a Quill log of the venue |
| Goes with its callers | 4 | removed once nothing calls it |

The events these need, named in the library's terms where the library sends them, are listed at the end, with the
checks that need each. The fields given are what the checks need; the DSL definitions are written in step 3.

## A. Who leads: the role changes of sequencers, matching engines and arbiters (47)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 15 scenarios | `do_kill_step` | secondary_log | *markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `do_machine_kill_step` | log_dir / step.ready_log_name | *step.ready_markers | a component started and is ready to work | **Event**: Ready(component) from the framework once its threads are operational |
| general | `do_isolate_step` | log_dir / step.takeover_log_name | *step.takeover_markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `do_isolate_step` | own_log | *step.stopped_markers | an instance woken after being frozen reports that its lease has run out | **Event**: RoleChanged(role=follower or unknown) with the reason: lease ran out |
| general | `do_restart_step` | ready_log | *step.ready_markers | a component started and is ready to work | **Event**: Ready(component) from the framework once its threads are operational |
| general | `run_scenario` | seq_ready_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 43, 44, 45, 46, 47 | `run_scenario` | me_log | *_ME_READY_MARKERS | a component started and is ready to work | **Event**: Ready(component) from the framework once its threads are operational |
| 43, 44, 45, 46, 47 | `run_scenario` | arb_primary_log | _ARB_ROLE \| _TO_LEADER | the primary arbiter is active | **Event**: RoleChanged(group=arbiter, role=active) |
| 22 scenarios | `run_scenario` | path | "MatchingEngineThread:" \| "adopting LEADER role" | a matching engine leads | **Event**: RoleChanged(group=matching engine, role=leader) |
| 27 scenarios | `run_scenario` | me_log | "MatchingEngineThread:" \| "adopting LEADER role" | a matching engine leads | **Event**: RoleChanged(group=matching engine, role=leader) |
| general | `run_scenario` | log_dir / name | *step.markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `run_scenario` | log_dir / name | *step.markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `run_scenario` | log_dir / step.log_name | *step.markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `run_scenario` | log_dir / step.log_name | *step.markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `run_scenario` | log_dir / step.log_name | *step.absent_markers | a scenario step's expected outcome, usually that an instance leads, or that none or only one does | **Event**: RoleChanged, checked at a checkpoint for the steps that require an absence |
| general | `run_scenario` | restarted_log | "MatchingEngineThread:" \| "adopting LEADER role" | a matching engine leads | **Event**: RoleChanged(group=matching engine, role=leader) |
| 46 | `run_scenario` | log_dir / "sequencer_secondary.log" | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 47 | `run_scenario` | log_dir / f"{name}.log" | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 59 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 59 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 76 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 76 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 60 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 75 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 61 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 61 | `run_scenario` | log_dir / "sequencer_secondary.log" | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 62 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 62 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 63, 64 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 63, 64 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 63, 64 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 65 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 65 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 66 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 66 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 67, 74 | `run_scenario` | leader_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 67, 74 | `run_scenario` | follower_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 68 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 68 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 69 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 69 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 70 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 71 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 71 | `run_scenario` | secondary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 72 | `run_scenario` | primary_log | _SEQ_ROLE \| _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 40, 73 | `run_scenario` | primary_log | _SEQ_ROLE | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |
| 40, 73 | `run_scenario` | primary_log | _TO_LEADER | a sequencer leads (or, where the check is that it is absent, that it does not) | **Event**: RoleChanged(group=sequencer, role=leader, epoch) |

## B. What the matching engine did with an order or a cancel (32)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 23 | `run_scenario` | me_log | "accepted NOS OrderID=ME-ORD-" | how many orders the engine accepted in the in-flight scenario | **Member**: the execution reports the member receives, counted at the client |
| 22 scenarios | `run_scenario` | me_log | 'find_last_me_ord' | the highest order number the engine had issued before a kill, so recovery can be checked from it | **Member**: the execution reports the test client received, which carry the OrderID |
| general | `run_scenario` | recovery_log | 'check_me_seq_monotonic' | the engine applied orders in ascending sequence number | **Event**: OrderApplied(seq_no) from the matching engine, for the scenarios that need it; or the order of execution reports at the client |
| 51 | `run_scenario` | me_log | "trading is halted -- refusing NOS" | an order sent while the engine is halted is refused | **Member**: the reject the member receives |
| 58 | `run_scenario` | me_log | "refusing OCR" \| cancel_marker | the engine refused an over-long order or cancel, live or while catching up | **Event**: OrderOutcome(cl_ord_id, refused, during_catch_up) from the matching engine: these orders come from inject_order and reach no member |
| 58 | `run_scenario` | me_log | "refusing NOS" \| order_marker | the engine refused an over-long order or cancel, live or while catching up | **Event**: OrderOutcome(cl_ord_id, refused, during_catch_up) from the matching engine: these orders come from inject_order and reach no member |
| 58 | `run_scenario` | me_log | "refusing OCR" \| cancel_marker | the engine refused an over-long order or cancel, live or while catching up | **Event**: OrderOutcome(cl_ord_id, refused, during_catch_up) from the matching engine: these orders come from inject_order and reach no member |
| 58 | `run_scenario` | me_log | "accepted NOS" \| live_pos | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 58 | `run_scenario` | me_log | "duplicate ClOrdID" \| live_pos | the engine did or did not refuse an order as a duplicate | **Event**: OrderOutcome(cl_ord_id, duplicate) |
| 58 | `run_scenario` | me_log | "refusing NOS" \| order_marker \| "during catch-up" | the engine refused an over-long order or cancel, live or while catching up | **Event**: OrderOutcome(cl_ord_id, refused, during_catch_up) from the matching engine: these orders come from inject_order and reach no member |
| 58 | `run_scenario` | me_log | "refusing OCR" \| cancel_marker \| "during catch-up" | the engine refused an over-long order or cancel, live or while catching up | **Event**: OrderOutcome(cl_ord_id, refused, during_catch_up) from the matching engine: these orders come from inject_order and reach no member |
| 59 | `engine_accepted` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 59 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={after_takeover} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 76 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={while_down}-{number} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 76 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={after_takeover} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 60 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={first} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 60 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={second} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 75 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 61 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 63, 64 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={behind} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 65 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 66 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={c} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 66 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={c} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 67, 74 | `accepted_by_engine` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 67, 74 | `refused_as_duplicate` | me_log | "duplicate ClOrdID" \| f"ClOrdID={cl_ord_id} " | the engine did or did not refuse an order as a duplicate | **Event**: OrderOutcome(cl_ord_id, duplicate) |
| 67, 74 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={held} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 67, 74 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={never_sent} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 68 | `run_scenario` | me_log | "accepted NOS" \| f"ClOrdID={c} " | the engine accepted a given order, once (or not at all, or not twice) | **Event**: OrderOutcome(cl_ord_id, accepted) for orders sent with inject_order; the member's New report where the order went through a gateway (decision 2) |
| 70 | `run_scenario` | me_log | "sent cancel ER" \| f"ClOrdID={cancel} " | the engine cancelled the order while the member was away | **Member**: the Canceled report the member receives on its return |
| 72 | `run_scenario` | me_log | "dropping NOS" | the engine did not drop orders when it had no connection for reports | **Event**: OrderApplied for each order, at a checkpoint (the member receives nothing at that moment by design) |
| 23 | `run_scenario` | me_log | "accepted NOS OrderID=ME-ORD-" | how many orders the engine accepted in the in-flight scenario | **Member**: the execution reports the member receives, counted at the client |
| 23 | `run_scenario` | me_log | "ExecType=Canceled" | nothing cancelled the member's orders when its gateway died | **Member**: the execution reports the member receives after reconnecting: none Canceled |

## C. The matching engine's catch-up (2)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 58 | `run_scenario` | me_log | "book reconciled to seq_no=" \| "the catch-up accounted for all" | the restarted engine's catch-up completed and accounted for every record (R-0101) | **Event**: CaughtUp(through, records) or CatchUpIncomplete from the matching engine |
| 58 | `run_scenario` | me_log | "catch-up was not complete" \| restart_pos | the restarted engine's catch-up completed and accounted for every record (R-0101) | **Event**: CaughtUp(through, records) or CatchUpIncomplete from the matching engine |

## D. Replication, repair and the follower-behind rule in the sequencer (14)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 60 | `run_scenario` | primary_log | "the follower is too far behind" \| "no acknowledgement from it" | the leader started, or stopped, running as if alone | **Event**: RunningAlone(started or stopped, reason) from the library's ReplicatedWal |
| 60 | `run_scenario` | primary_log | "the follower is keeping up again" | the leader started, or stopped, running as if alone | **Event**: RunningAlone(started or stopped, reason) from the library's ReplicatedWal |
| 75 | `run_scenario` | primary_log | "the storage for waiting orders is full" | the leader's storage for held orders filled | **Event**: HeldOrdersFull from the sequencer |
| 75 | `run_scenario` | primary_log | "waited in the log -- the reading thread sends them" | the reading thread served, then handed over, the engine for orders that waited in the WAL | **Event**: Serving(follower, after) and HandedOver(follower, through, sent) from the library's WalFollowerService |
| 75 | `run_scenario` | primary_log | "the reading thread has sent the matching engine on connection" | the reading thread served, then handed over, the engine for orders that waited in the WAL | **Event**: Serving(follower, after) and HandedOver(follower, through, sent) from the library's WalFollowerService |
| 61 | `run_scenario` | primary_log | "the follower's log agrees with this one through record" | the leader's and the follower's WALs agree through a record | **Event**: WalsAgree(through) from the library's ReplicatedWal, on each side |
| 61 | `run_scenario` | log_dir / "sequencer_secondary.log" | "this log agrees with the leader's through record" | the leader's and the follower's WALs agree through a record | **Event**: WalsAgree(through) from the library's ReplicatedWal, on each side |
| 62 | `run_scenario` | primary_log | "this log agrees with the leader's through record" | the leader's and the follower's WALs agree through a record | **Event**: WalsAgree(through) from the library's ReplicatedWal, on each side |
| 62 | `run_scenario` | primary_log | "from this instance's log -- the leader does not hold them" | the rejoining old leader discarded the records its new leader does not hold | **Event**: RecordsDiscarded(after, count) from the library's ReplicatedWal; the test already checks the WAL itself as well |
| 63, 64 | `run_scenario` | primary_log | "saying it may lead again" | the leader's statement about whether its follower may lead changed | **Event**: StatementChanged(peer_may_lead) from the library's lease classes |
| 63, 64 | `run_scenario` | primary_log | "saying the follower may not lead" | the leader's statement about whether its follower may lead changed | **Event**: StatementChanged(peer_may_lead) from the library's lease classes |
| 63, 64 | `run_scenario` | log_dir / "arbiter_primary.log" | "says instance 2 may not lead" | the restarted arbiter still holds the statement that the follower may not lead | **Event**: StatementRecorded(group, instance, peer_may_lead) from the arbiter |
| 63, 64 | `run_scenario` | primary_log | "saying it may lead again" | the leader's statement about whether its follower may lead changed | **Event**: StatementChanged(peer_may_lead) from the library's lease classes |
| 67, 74 | `run_scenario` | leader_log | "commands that may already be in the log are arriving" | the leader built its index of the newest segments for the duplicate check | **Event**: DuplicateIndexBuilt from the sequencer |

## E. Halting, and cancelling on promotion (10)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 23 scenarios | `run_scenario` | me_secondary_log | 'me_cancel_on_failover_count' | how many orders the engine cancelled on promotion or after a long absence (or that it cancelled none) | **Member**: the Canceled reports the members receive; the count the engine reports in an event where no member is connected |
| 23 scenarios | `run_scenario` | me_secondary_log | 'me_resumed_book_size' | how many orders the promoted engine resumed with, or held when it began cancelling | **Event**: Promoted(book_size) from the matching engine |
| 23 scenarios | `run_scenario` | me_secondary_log | 'me_inherited_book_size' | how many orders the promoted engine resumed with, or held when it began cancelling | **Event**: Promoted(book_size) from the matching engine |
| 18 | `run_scenario` | me_log | 'me_cancel_on_failover_count' | how many orders the engine cancelled on promotion or after a long absence (or that it cancelled none) | **Member**: the Canceled reports the members receive; the count the engine reports in an event where no member is connected |
| 52 | `run_scenario` | me_log | "cannot be used" \| "R-0102" | the engine refused a damaged region, said it cannot account for what it held, or halted | **Event**: Halted(reason) from the matching engine; the refusal of the region is one reason |
| 52 | `run_scenario` | me_log | "cannot account for what the venue was holding" | the engine refused a damaged region, said it cannot account for what it held, or halted | **Event**: Halted(reason) from the matching engine; the refusal of the region is one reason |
| 52 | `run_scenario` | me_log | "trading is halted" \| "a person must lift this" | the engine refused a damaged region, said it cannot account for what it held, or halted | **Event**: Halted(reason) from the matching engine; the refusal of the region is one reason |
| 52 | `run_scenario` | me_log | 'me_cancel_on_failover_count' | how many orders the engine cancelled on promotion or after a long absence (or that it cancelled none) | **Member**: the Canceled reports the members receive; the count the engine reports in an event where no member is connected |
| 51 | `run_scenario` | me_log | 'me_cancel_on_failover_count' | how many orders the engine cancelled on promotion or after a long absence (or that it cancelled none) | **Member**: the Canceled reports the members receive; the count the engine reports in an event where no member is connected |
| 51 | `run_scenario` | me_log | "trading is halted" \| "a person must lift this" | the engine refused a damaged region, said it cannot account for what it held, or halted | **Event**: Halted(reason) from the matching engine; the refusal of the region is one reason |

## F. Gateway sessions: binding, disconnection, cancelling on disconnection (14)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 21 | `run_scenario` | gw_log | "announced session" \| "bound to instance" | the gateway announced the reconnected session to the sequencer | **Event**: SessionBound(comp_id, instance) from the gateway |
| 23 scenarios | `run_scenario` | gw_log | "has no gateway_session_conn_id -- dropping" | the sequencer or gateway dropped execution reports for a dead or missing session | **Event**: ReportDropped(session) from the sequencer or the gateway |
| 18 | `run_scenario` | seq_primary_log | drop_marker | the sequencer or gateway dropped execution reports for a dead or missing session | **Event**: ReportDropped(session) from the sequencer or the gateway |
| 18 | `run_scenario` | seq_primary_log | drop_marker | the sequencer or gateway dropped execution reports for a dead or missing session | **Event**: ReportDropped(session) from the sequencer or the gateway |
| 50 | `run_scenario` | gw_log | "disconnected with" \| "open order" | the gateway held, cancelled, or gave back a dropped session's open orders | **Event**: SessionHeld, CancelsSent(count) and SessionReclaimed from the gateway; the cancels are also seen by the member |
| 19 | `run_scenario` | gw_log | "disconnected with" \| "before cancelling" | the gateway held, cancelled, or gave back a dropped session's open orders | **Event**: SessionHeld, CancelsSent(count) and SessionReclaimed from the gateway; the cancels are also seen by the member |
| 19 | `run_scenario` | gw_log | gw_pos | the grace period the gateway says it is holding a dropped session's orders for | **Event**: SessionHeld(comp_id, grace_seconds) from the gateway |
| 19 | `run_scenario` | gw_log | "cancel drain complete" | the gateway held, cancelled, or gave back a dropped session's open orders | **Event**: SessionHeld, CancelsSent(count) and SessionReclaimed from the gateway; the cancels are also seen by the member |
| 19 | `run_scenario` | gw_log | "reconnected within the grace period" \| "none cancelled" | the gateway held, cancelled, or gave back a dropped session's open orders | **Event**: SessionHeld, CancelsSent(count) and SessionReclaimed from the gateway; the cancels are also seen by the member |
| 19 | `run_scenario` | gw_log | "cancel drain complete" | the gateway held, cancelled, or gave back a dropped session's open orders | **Event**: SessionHeld, CancelsSent(count) and SessionReclaimed from the gateway; the cancels are also seen by the member |
| 70 | `run_scenario` | primary_log | "comp_id='" + FIX8_COMP_ID + "'" \| "unbound from instance" | a member's session is unbound (or, where the check is a count, that it was not) | **Event**: SessionUnbound(comp_id) from the gateway and the sequencer |
| 70 | `run_scenario` | primary_log | "bound again -- sent it 1 report(s) produced while it had no connection" | the sequencer sent the one report produced while the session had no connection | **Member**: the report the member receives on reconnecting, exactly one |
| 22 | `run_scenario` | gw_log | f"announced session comp_id='{FIX8_COMP_ID}' unbound from instance" | a member's session is unbound (or, where the check is a count, that it was not) | **Event**: SessionUnbound(comp_id) from the gateway and the sequencer |
| 23 | `run_scenario` | gw_b_log | f"announced session comp_id='{FIX8_COMP_ID}' unbound from instance" | a member's session is unbound (or, where the check is a count, that it was not) | **Event**: SessionUnbound(comp_id) from the gateway and the sequencer |

## G. Resends and session replay (18)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 40, 73 | `run_scenario` | gw_log | f"ResendRequest BeginSeqNo={_BOUNDED_RESEND_BEGIN} EndSeqNo={_BOUNDED_RESEND_END}" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 40, 73 | `run_scenario` | gw_log | "resend complete" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 40, 73 | `run_scenario` | path | _REPLAY_DECLINED | exactly one sequencer answered a replay and the other declined, and the gateway asked the new leader again | **Event**: ReplayAnswered and ReplayDeclined from the sequencers; ReplayAskedAgain from the gateway |
| 40, 73 | `run_scenario` | path | _REPLAY_ANSWERED \| sequencer_positions[name] | exactly one sequencer answered a replay and the other declined, and the gateway asked the new leader again | **Event**: ReplayAnswered and ReplayDeclined from the sequencers; ReplayAskedAgain from the gateway |
| 40, 73 | `run_scenario` | path | _REPLAY_DECLINED \| sequencer_positions[name] | exactly one sequencer answered a replay and the other declined, and the gateway asked the new leader again | **Event**: ReplayAnswered and ReplayDeclined from the sequencers; ReplayAskedAgain from the gateway |
| 40, 73 | `run_scenario` | gw_log | _REPLAY_ASKED_AGAIN | exactly one sequencer answered a replay and the other declined, and the gateway asked the new leader again | **Event**: ReplayAnswered and ReplayDeclined from the sequencers; ReplayAskedAgain from the gateway |
| 22 | `run_scenario` | gw_log | "resuming the venue's sequence state" \| "outbound=" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 22 | `run_scenario` | gw_log | f"ResendRequest BeginSeqNo={asks_from}" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 22 | `run_scenario` | gw_log | '_resend_resume_point' | the number the gateway resumed at after a resend, or the range it gap-filled | **Member**: the sequence numbers and the gap-fill the member receives |
| 22 | `run_scenario` | gw_log | "resend complete" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 22 | `run_scenario` | gw_log | '_resent_report_count' | how many reports the gateway resent, and the record of which numbers held reports | **Member**: the resent reports the member receives (PossDup set); the inherited record by an event from the gateway |
| 22 | `run_scenario` | gw_log | '_resend_gap_fill' | the number the gateway resumed at after a resend, or the range it gap-filled | **Member**: the sequence numbers and the gap-fill the member receives |
| 23 | `run_scenario` | gw_b_log | "resuming the venue's sequence state" \| "outbound=" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 23 | `run_scenario` | gw_b_log | "ResendRequest BeginSeqNo=" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 23 | `run_scenario` | gw_b_log | "resend complete" | the member's resend request was read with its bounds, answered, and the numbering resumed | **Member**: what the member sends and receives: the ResendRequest, the resent messages and the numbers they carry |
| 23 | `run_scenario` | gw_b_log | '_inherited_report_range_count' | how many reports the gateway resent, and the record of which numbers held reports | **Member**: the resent reports the member receives (PossDup set); the inherited record by an event from the gateway |
| 23 | `run_scenario` | gw_b_log | '_resent_report_count' | how many reports the gateway resent, and the record of which numbers held reports | **Member**: the resent reports the member receives (PossDup set); the inherited record by an event from the gateway |
| 23 | `run_scenario` | gw_b_log | '_resend_resume_point' | the number the gateway resumed at after a resend, or the range it gap-filled | **Member**: the sequence numbers and the gap-fill the member receives |

## H. Connections made, lost and refused (10)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 22 scenarios | `run_scenario` | me_secondary_log | "ME-primary replication connection" \| "established" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| general | `run_scenario` | seq_primary_log | "SequencerThread: matching engine order connection" \| "established" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 71 | `run_scenario` | gw_log | _SEQUENCER_CONNECTION_CLOSED | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 71 | `run_scenario` | gw_log | _SEQUENCER_CONNECTION_CLOSED | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 71 | `run_scenario` | gw_log | "established again -- sending again on it" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 71 | `run_scenario` | gw_log | "InboundConnectionManager::send_or_wait: connection from" | the gateway closed the member's connection for not reading | **Member**: the member's connection closing, seen by the client |
| 72 | `run_scenario` | me_log | "no connection to a sequencer for reports -- acting on orders and holding their reports" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 72 | `run_scenario` | me_log | "a connection to a sequencer for reports is established -- sending" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 57 | `run_scenario` | binary_log | "primary sequencer connection" \| "established" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |
| 20 | `run_scenario` | gw_log | "authentication service connection" \| "established" | a connection between components was made, lost or made again | **Event**: ConnectionChanged(service, made or lost) from the framework's connection classes; for the engine's report connections, ReportsHeld and ReportsReleased |

## I. Throttles and logons (7)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 56 | `run_scenario` | gw_log | applied_marker | the gateway applied a member's throttle limits, and started and stopped throttling it once each | **Event**: ThrottleApplied(limits), ThrottleStarted and ThrottleStopped from the gateway; the refusals themselves are seen by the member |
| 56 | `run_scenario` | gw_log | stopped_marker | the gateway applied a member's throttle limits, and started and stopped throttling it once each | **Event**: ThrottleApplied(limits), ThrottleStarted and ThrottleStopped from the gateway; the refusals themselves are seen by the member |
| 56 | `run_scenario` | gw_log | marker \| limits_pos | the gateway applied a member's throttle limits, and started and stopped throttling it once each | **Event**: ThrottleApplied(limits), ThrottleStarted and ThrottleStopped from the gateway; the refusals themselves are seen by the member |
| 56 | `run_scenario` | gw_log | unlimited_marker | the gateway applied a member's throttle limits, and started and stopped throttling it once each | **Event**: ThrottleApplied(limits), ThrottleStarted and ThrottleStopped from the gateway; the refusals themselves are seen by the member |
| 20 | `run_scenario` | gw_log | accepted_marker | a logon was accepted or refused at the right instance, and no session was left behind | **Member**: the Logon or Logout the member receives |
| 20 | `run_scenario` | gw_log | refused_marker | a logon was accepted or refused at the right instance, and no session was left behind | **Member**: the Logon or Logout the member receives |
| 20 | `run_scenario` | gw_log | _GW_LOGON_OK | a logon was accepted or refused at the right instance, and no session was left behind | **Member**: the Logon or Logout the member receives |

## J. The WAL written in order and undamaged, over the whole run (2)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| general | `check_write_ahead_logs_in_order` | sequencer_log | _WAL_OUT_OF_ORDER | no sequencer wrote a record out of order or found its WAL damaged, during the whole run | **WAL files**: the test reads both WALs after the scenario and checks every record is in order and its checksum right |
| general | `check_write_ahead_logs_in_order` | sequencer_log | _WAL_DAMAGED | no sequencer wrote a record out of order or found its WAL damaged, during the whole run | **WAL files**: the test reads both WALs after the scenario and checks every record is in order and its checksum right |

## K. The test's own fault-injection library (5)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 59 | `run_scenario` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" | the fault-injection library has begun discarding the sequencer's sends | **File (decision 5)**: a confirmation file the library creates |
| 76 | `run_scenario` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" | the fault-injection library has begun discarding the sequencer's sends | **File (decision 5)**: a confirmation file the library creates |
| 62 | `run_scenario` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" | the fault-injection library has begun discarding the sequencer's sends | **File (decision 5)**: a confirmation file the library creates |
| 65 | `run_scenario` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" | the fault-injection library has begun discarding the sequencer's sends | **File (decision 5)**: a confirmation file the library creates |
| 66 | `run_scenario` | log_dir / "sequencer_secondary.stdout" | "block_sends_to_ports: the flag file exists" | the fault-injection library has begun discarding the sequencer's sends | **File (decision 5)**: a confirmation file the library creates |

## L. The helper that reads complete lines (4)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| general | `poll_log_for` | log_path | pos | (reads whole lines for the helpers that wait and count) | **Goes with its callers** |
| general | `wait_for_fix_logon` | gw_log | pos | (reads whole lines for the helpers that wait and count) | **Goes with its callers** |
| general | `wait_for_me_ord` | me_log | pos | (reads whole lines for the helpers that wait and count) | **Goes with its callers** |
| general | `wait_for_accepted_count` | me_log | pos | (reads whole lines for the helpers that wait and count) | **Goes with its callers** |

## M. The gateway's progress counts (4)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 18 | `run_scenario` | gw_b_log | '_gateway_progress_lines' | the gateway's counts of orders sent and answered, from the line it logs every few seconds | **Member**: the test client's own counts of what it sent and what was answered |
| 18 | `run_scenario` | gw_b_log | 'GW-PROGRESS' | the gateway's counts of orders sent and answered, from the line it logs every few seconds | **Member**: the test client's own counts of what it sent and what was answered |
| 51 | `run_scenario` | gw_log | 'gateway_progress_totals' | the gateway's counts of orders sent and answered, from the line it logs every few seconds | **Member**: the test client's own counts of what it sent and what was answered |
| 42 | `run_scenario` | gw_log | quiet_from | the gateway's counts of orders sent and answered, from the line it logs every few seconds | **Member**: the test client's own counts of what it sent and what was answered |

## N. The FIX test client's own record of what it received (2)

| Scenarios | Function | Log | Phrase or helper | Proves | Replaced by |
|---|---|---|---|---|---|
| 22 | `run_scenario` | client_output | '_client_logon_seq_num' | the sequence number of the Logon the member received | **Stays**: the FIX test client's own record of what it received; not a Quill log of the venue |
| 23 | `run_scenario` | client_output | '_client_logon_seq_num' | the sequence number of the Logon the member received | **Stays**: the FIX test client's own record of what it received; not a Quill log of the venue |

## Unit tests that read a captured log (17)

Decision 6 brings these into the phase. Each checks a class directly, so each is replaced by checking the class's own
state, or the event the class sends in step 3, not by a test event connection.

| Test file | Phrase | Proves | Replaced by |
|---|---|---|---|
| `PairLeaseAgentTest` | leading at epoch 1 -- granted a lease by | the agent leads at epoch 1 | its state and epoch |
| `PairLeaseAgentTest` | lease ran out -- neither the peer nor the arbiter renewed it in time | the lease ran out | its state, and the change it reports |
| `PairLeaseAgentTest` | this group has no leader | the agent reports that no instance leads | the change it reports |
| `PairLeaseAgentTest` | could not write the lease promise record | a failed promise write is reported | the recorder's outcome as the agent reports it |
| `PairLeaseAgentTest` | this instance was leading, so it asks to lead again at once | a restarted leader asks to lead again at once | the request it sends |
| `PairLeaseAgentTest` | says this instance may not lead | the agent holds its peer's statement that it may not lead | the statement its voter holds |
| `ReplicatedWalTest` | the follower is too far behind | the leader runs as if alone | its running-alone state |
| `ReplicatedWalTest` | saying the follower may not lead (two places) | the leader's statement changed | the lease agent's statement |
| `ReplicatedWalTest` | the follower is keeping up again | the leader waits for its follower again | its running-alone state |
| `ReplicatedWalTest` | saying it may lead again (two places) | the statement changed back | the lease agent's statement |
| `ReplicatedWalTest` | from this instance's log -- the leader does not hold them | the follower discarded records | the WAL's last record |
| `ReplicatedWalTest` | are being sent to it from the WAL | a catch-up started | the calls on the test's catch-up stand-in |
| `ReplicatedWalTest` | has been sent every record through (two places) | the follower was handed over | the calls on the test's catch-up stand-in |
| `ReplicatedWalTest` | it is answered once that has stopped | a new request is held until the catch-up stops | the calls on the test's catch-up stand-in |

## The events needed

| Event | Kinds of check | Sent by |
|---|---|---|
| `Ready` | A | the framework, when a component's threads are operational |
| `RoleChanged` | A | the library's lease classes, for sequencers, matching engines and arbiters |
| `OrderApplied` | B | the matching engine, only when a test asks |
| `OrderOutcome` | B | the matching engine, for orders no member will hear of |
| `CaughtUp` | C | the matching engine |
| `CatchUpIncomplete` | C | the matching engine |
| `RunningAlone` | D | the library's ReplicatedWal |
| `HeldOrdersFull` | D | the sequencer |
| `Serving` | D | the library's WalFollowerService |
| `HandedOver` | D | the library's WalFollowerService |
| `WalsAgree` | D | the library's ReplicatedWal |
| `RecordsDiscarded` | D | the library's ReplicatedWal |
| `StatementChanged` | D | the library's lease classes |
| `StatementRecorded` | D | the arbiter (the library's voting service) |
| `DuplicateIndexBuilt` | D | the sequencer |
| `Promoted` | E | the matching engine |
| `Halted` | E | the matching engine |
| `SessionBound` | F | the gateways |
| `ReportDropped` | F | the sequencer and the gateways |
| `CancelsSent` | F | the FIX gateway |
| `SessionReclaimed` | F | the FIX gateway |
| `SessionHeld` | F | the FIX gateway |
| `SessionUnbound` | F | the gateways and the sequencer |
| `ReplayAnswered` | G | the sequencer |
| `ReplayDeclined` | G | the sequencer |
| `ReplayAskedAgain` | G | the gateways |
| `ConnectionChanged` | H | the framework's connection classes |
| `ReportsHeld` | H | the matching engine |
| `ReportsReleased` | H | the matching engine |
| `ThrottleApplied` | I | the gateways |
| `ThrottleStarted` | I | the gateways |
| `ThrottleStopped` | I | the gateways |

That is 32 events. Several may merge when they are defined, for instance the session events into one
event with a field saying what happened; step 3 settles that.
