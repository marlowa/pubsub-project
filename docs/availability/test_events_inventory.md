# Phase 6 inventory: every check in ha_test.py that reads a Quill log {#test_events_inventory}

This is the inventory for phase 6 ([test_events.md](test_events.md)): every place `scripts/ha_test.py` reads a
Quill log, found by parsing the script, grouped by the kind of fact the check proves. It was made on 18 October
2026 from commit `cef128c`, by parsing the script with Python's `ast` module and finding every call of the helpers
that read a log (`poll_log_for`, `count_log_marker`, `count_lines_with_all`, `me_cancel_on_failover_count`,
`_resent_report_count`, `_inherited_report_range_count` and `read_complete_lines`), then sorting each by its
phrase. The counts change as the script changes, so it is made again before each kind of check is moved.

Each row gives the function the read is in, the scenario setting that switches the check on (blank when it runs
in every scenario that reaches it), the helper that reads the log, the log, and the phrase or the variable
holding it. What each check proves, and whether a member could see it instead, is written for each kind in
step 1 of phase 6.

## A. Who leads: role changes of sequencers, engines and arbiters (47)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `do_kill_step` |  | `poll_log_for` | secondary_log | *markers |
| `do_machine_kill_step` |  | `poll_log_for` | log_dir / step.ready_log_name | *step.ready_markers |
| `do_isolate_step` |  | `poll_log_for` | log_dir / step.takeover_log_name | *step.takeover_markers |
| `do_isolate_step` |  | `poll_log_for` | own_log | *step.stopped_markers |
| `do_restart_step` |  | `poll_log_for` | ready_log | *step.ready_markers |
| `run_scenario` |  | `poll_log_for` | seq_ready_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.ha_disabled | `poll_log_for` | me_log | *_ME_READY_MARKERS |
| `run_scenario` | scenario.ha_disabled | `poll_log_for` | arb_primary_log | _ARB_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.me_ha | `poll_log_for` | path | "MatchingEngineThread:" \| "adopting LEADER role" |
| `run_scenario` | scenario.me_ha / not scenario.ha_disabled | `poll_log_for` | me_log | "MatchingEngineThread:" \| "adopting LEADER role" |
| `run_scenario` |  | `poll_log_for` | log_dir / name | *step.markers |
| `run_scenario` |  | `poll_log_for` | log_dir / name | *step.markers |
| `run_scenario` |  | `poll_log_for` | log_dir / step.log_name | *step.markers |
| `run_scenario` |  | `poll_log_for` | log_dir / step.log_name | *step.markers |
| `run_scenario` |  | `poll_log_for` | log_dir / step.log_name | *step.absent_markers |
| `run_scenario` |  | `poll_log_for` | restarted_log | "MatchingEngineThread:" \| "adopting LEADER role" |
| `run_scenario` | scenario.assert_ha_off_secondary_alone | `poll_log_for` | log_dir / "sequencer_secondary.log" | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_ha_off_both_lead | `poll_log_for` | log_dir / f"{name}.log" | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_engine_holds_only_logged_orders | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_engine_holds_only_logged_orders | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_publishers_follow_only_the_record | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_publishers_follow_only_the_record | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_stalled_follower_runs_alone | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_orders_that_waited_in_the_log_reach_engine | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_restarted_follower_log_repaired | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_restarted_follower_log_repaired | `poll_log_for` | log_dir / "sequencer_secondary.log" | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_rejoining_old_leader_log_repaired | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_rejoining_old_leader_log_repaired | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_kept_reports_forwarded | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_kept_reports_forwarded | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_logged_commands_reach_engine | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_logged_commands_reach_engine | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_sent_again_sequenced_once | `poll_log_for` | leader_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_sent_again_sequenced_once / scenario.sent_again_after_change_of_leader | `poll_log_for` | follower_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_orders_during_change_answered | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_orders_during_change_answered | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_few_repeats_after_change | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_few_repeats_after_change | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_unbound_reports_delivered | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_commands_resent_after_reconnection | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_commands_resent_after_reconnection | `poll_log_for` | secondary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_engine_holds_reports_without_connection | `poll_log_for` | primary_log | _SEQ_ROLE \| _TO_LEADER |
| `run_scenario` | scenario.assert_bounded_resend / scenario.bounded_resend_with_leader_killed | `count_log_marker` | primary_log | _SEQ_ROLE |
| `run_scenario` | scenario.assert_bounded_resend / scenario.bounded_resend_with_leader_killed | `count_log_marker` | primary_log | _TO_LEADER |

## B. What the engine did with an order or cancel (30)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.assert_inflight_recovery | `count_log_marker` | me_log | "accepted NOS OrderID=ME-ORD-" |
| `run_scenario` | scenario.assert_absence_halt | `poll_log_for` | me_log | "trading is halted -- refusing NOS" |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `poll_log_for` | me_log | "refusing OCR" \| cancel_marker |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_lines_with_all` | me_log | "refusing NOS" \| order_marker |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_lines_with_all` | me_log | "refusing OCR" \| cancel_marker |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_log_marker` | me_log | "accepted NOS" \| live_pos |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_log_marker` | me_log | "duplicate ClOrdID" \| live_pos |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_lines_with_all` | me_log | "refusing NOS" \| order_marker \| "during catch-up" |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_lines_with_all` | me_log | "refusing OCR" \| cancel_marker \| "during catch-up" |
| `engine_accepted` | scenario.assert_engine_holds_only_logged_orders | `count_lines_with_all` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " |
| `run_scenario` | scenario.assert_engine_holds_only_logged_orders | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={after_takeover} " |
| `run_scenario` | scenario.assert_publishers_follow_only_the_record | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={while_down}-{number} " |
| `run_scenario` | scenario.assert_publishers_follow_only_the_record | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={after_takeover} " |
| `run_scenario` | scenario.assert_stalled_follower_runs_alone | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={first} " |
| `run_scenario` | scenario.assert_stalled_follower_runs_alone | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={second} " |
| `run_scenario` | scenario.assert_orders_that_waited_in_the_log_reach_engine | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " |
| `run_scenario` | scenario.assert_restarted_follower_log_repaired | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={behind} " |
| `run_scenario` | scenario.assert_kept_reports_forwarded | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " |
| `run_scenario` | scenario.assert_logged_commands_reach_engine | `count_lines_with_all` | me_log | "accepted NOS" \| f"ClOrdID={c} " |
| `run_scenario` | scenario.assert_logged_commands_reach_engine | `count_lines_with_all` | me_log | "accepted NOS" \| f"ClOrdID={c} " |
| `accepted_by_engine` | scenario.assert_sent_again_sequenced_once | `count_lines_with_all` | me_log | "accepted NOS" \| f"ClOrdID={cl_ord_id} " |
| `refused_as_duplicate` | scenario.assert_sent_again_sequenced_once | `count_lines_with_all` | me_log | "duplicate ClOrdID" \| f"ClOrdID={cl_ord_id} " |
| `run_scenario` | scenario.assert_sent_again_sequenced_once | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={held} " |
| `run_scenario` | scenario.assert_sent_again_sequenced_once | `poll_log_for` | me_log | "accepted NOS" \| f"ClOrdID={never_sent} " |
| `run_scenario` | scenario.assert_orders_during_change_answered | `count_lines_with_all` | me_log | "accepted NOS" \| f"ClOrdID={c} " |
| `run_scenario` | scenario.assert_unbound_reports_delivered | `poll_log_for` | me_log | "sent cancel ER" \| f"ClOrdID={cancel} " |
| `run_scenario` | scenario.assert_engine_holds_reports_without_connection | `poll_log_for` | me_log | "dropping NOS" |
| `run_scenario` | scenario.assert_inflight_recovery | `count_log_marker` | me_log | "accepted NOS OrderID=ME-ORD-" |
| `run_scenario` | scenario.assert_inflight_recovery | `count_log_marker` | me_log | "ExecType=Canceled" |

## C. The engine's catch-up (2)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.assert_engine_identifier_refusal | `poll_log_for` | me_log | "book reconciled to seq_no=" \| "the catch-up accounted for all" |
| `run_scenario` | scenario.assert_engine_identifier_refusal | `count_log_marker` | me_log | "catch-up was not complete" \| restart_pos |

## D. Replication, repair and the follower-behind rule in the sequencer (14)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.assert_stalled_follower_runs_alone | `poll_log_for` | primary_log | "the follower is too far behind" \| "no acknowledgement from it" |
| `run_scenario` | scenario.assert_stalled_follower_runs_alone | `poll_log_for` | primary_log | "the follower is keeping up again" |
| `run_scenario` | scenario.assert_orders_that_waited_in_the_log_reach_engine | `poll_log_for` | primary_log | "the storage for waiting orders is full" |
| `run_scenario` | scenario.assert_orders_that_waited_in_the_log_reach_engine | `poll_log_for` | primary_log | "waited in the log -- the reading thread sends them" |
| `run_scenario` | scenario.assert_orders_that_waited_in_the_log_reach_engine | `poll_log_for` | primary_log | "the reading thread has sent the matching engine on connection" |
| `run_scenario` | scenario.assert_restarted_follower_log_repaired | `poll_log_for` | primary_log | "the follower's log agrees with this one through record" |
| `run_scenario` | scenario.assert_restarted_follower_log_repaired | `poll_log_for` | log_dir / "sequencer_secondary.log" | "this log agrees with the leader's through record" |
| `run_scenario` | scenario.assert_rejoining_old_leader_log_repaired | `poll_log_for` | primary_log | "this log agrees with the leader's through record" |
| `run_scenario` | scenario.assert_rejoining_old_leader_log_repaired | `poll_log_for` | primary_log | "from this instance's log -- the leader does not hold them" |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | primary_log | "saying it may lead again" |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | primary_log | "saying the follower may not lead" |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead / scenario.restart_arbiter_before_leader_dies | `poll_log_for` | log_dir / "arbiter_primary.log" | "says instance 2 may not lead" |
| `run_scenario` | scenario.assert_follower_behind_does_not_lead | `poll_log_for` | primary_log | "saying it may lead again" |
| `run_scenario` | scenario.assert_sent_again_sequenced_once | `poll_log_for` | leader_log | "commands that may already be in the log are arriving" |

## E. Halting, and cancelling on promotion (8)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.me_ha and not scenario.assert_gateway_orphaned and not scenario.recovery_on_primary and not scenario.assert_deferred_answered and not scenario.expects_no_new_leader | `me_cancel_on_failover_count` | me_secondary_log |  |
| `run_scenario` | scenario.assert_gateway_orphaned | `me_cancel_on_failover_count` | me_log |  |
| `run_scenario` | scenario.assert_damaged_region | `poll_log_for` | me_log | "cannot be used" \| "R-0102" |
| `run_scenario` | scenario.assert_damaged_region | `poll_log_for` | me_log | "cannot account for what the venue was holding" |
| `run_scenario` | scenario.assert_damaged_region | `poll_log_for` | me_log | "trading is halted" \| "a person must lift this" |
| `run_scenario` | scenario.assert_damaged_region | `me_cancel_on_failover_count` | me_log |  |
| `run_scenario` | scenario.assert_absence_halt | `me_cancel_on_failover_count` | me_log |  |
| `run_scenario` | scenario.assert_absence_halt | `poll_log_for` | me_log | "trading is halted" \| "a person must lift this" |

## F. Gateway sessions: binding, disconnection, cancel on disconnect (13)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.reconnect_before_kill | `poll_log_for` | gw_log | "announced session" \| "bound to instance" |
| `run_scenario` | scenario.me_ha and not scenario.assert_gateway_orphaned and not scenario.recovery_on_primary and not scenario.assert_deferred_answered and not scenario.expects_no_new_leader | `count_log_marker` | gw_log | "has no gateway_session_conn_id -- dropping" |
| `run_scenario` | scenario.assert_gateway_orphaned | `poll_log_for` | seq_primary_log | drop_marker |
| `run_scenario` | scenario.assert_gateway_orphaned | `count_log_marker` | seq_primary_log | drop_marker |
| `run_scenario` | scenario.assert_open_orders_survive | `poll_log_for` | gw_log | "disconnected with" \| "open order" |
| `run_scenario` | scenario.assert_cancel_grace | `poll_log_for` | gw_log | "disconnected with" \| "before cancelling" |
| `run_scenario` | scenario.assert_cancel_grace | `count_log_marker` | gw_log | "cancel drain complete" |
| `run_scenario` | scenario.assert_cancel_grace | `poll_log_for` | gw_log | "reconnected within the grace period" \| "none cancelled" |
| `run_scenario` | scenario.assert_cancel_grace | `count_log_marker` | gw_log | "cancel drain complete" |
| `run_scenario` | scenario.assert_unbound_reports_delivered | `poll_log_for` | primary_log | "comp_id='" + FIX8_COMP_ID + "'" \| "unbound from instance" |
| `run_scenario` | scenario.assert_unbound_reports_delivered | `poll_log_for` | primary_log | "bound again -- sent it 1 report(s) produced while it had no connection" |
| `run_scenario` | scenario.assert_resend_recovery | `count_log_marker` | gw_log | f"announced session comp_id='{FIX8_COMP_ID}' unbound from instance" |
| `run_scenario` | scenario.assert_inflight_recovery | `count_log_marker` | gw_b_log | f"announced session comp_id='{FIX8_COMP_ID}' unbound from instance" |

## G. Resends and session replay (15)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.assert_bounded_resend | `poll_log_for` | gw_log | f"ResendRequest BeginSeqNo={_BOUNDED_RESEND_BEGIN} EndSeqNo={_BOUNDED_RESEND_END}" |
| `run_scenario` | scenario.assert_bounded_resend | `poll_log_for` | gw_log | "resend complete" |
| `run_scenario` | scenario.assert_bounded_resend | `poll_log_for` | path | _REPLAY_DECLINED |
| `run_scenario` | scenario.assert_bounded_resend | `count_log_marker` | path | _REPLAY_ANSWERED \| sequencer_positions[name] |
| `run_scenario` | scenario.assert_bounded_resend | `count_log_marker` | path | _REPLAY_DECLINED \| sequencer_positions[name] |
| `run_scenario` | scenario.assert_bounded_resend / scenario.bounded_resend_with_leader_killed | `poll_log_for` | gw_log | _REPLAY_ASKED_AGAIN |
| `run_scenario` | scenario.assert_resend_recovery | `poll_log_for` | gw_log | "resuming the venue's sequence state" \| "outbound=" |
| `run_scenario` | scenario.assert_resend_recovery | `poll_log_for` | gw_log | f"ResendRequest BeginSeqNo={asks_from}" |
| `run_scenario` | scenario.assert_resend_recovery | `poll_log_for` | gw_log | "resend complete" |
| `run_scenario` | scenario.assert_resend_recovery | `_resent_report_count` | gw_log |  |
| `run_scenario` | scenario.assert_inflight_recovery | `poll_log_for` | gw_b_log | "resuming the venue's sequence state" \| "outbound=" |
| `run_scenario` | scenario.assert_inflight_recovery | `poll_log_for` | gw_b_log | "ResendRequest BeginSeqNo=" |
| `run_scenario` | scenario.assert_inflight_recovery | `poll_log_for` | gw_b_log | "resend complete" |
| `run_scenario` | scenario.assert_inflight_recovery | `_inherited_report_range_count` | gw_b_log |  |
| `run_scenario` | scenario.assert_inflight_recovery | `_resent_report_count` | gw_b_log |  |

## H. Connections made, lost and refused (10)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.me_ha | `poll_log_for` | me_secondary_log | "ME-primary replication connection" \| "established" |
| `run_scenario` |  | `poll_log_for` | seq_primary_log | "SequencerThread: matching engine order connection" \| "established" |
| `run_scenario` | scenario.assert_commands_resent_after_reconnection | `count_log_marker` | gw_log | _SEQUENCER_CONNECTION_CLOSED |
| `run_scenario` | scenario.assert_commands_resent_after_reconnection | `poll_log_for` | gw_log | _SEQUENCER_CONNECTION_CLOSED |
| `run_scenario` | scenario.assert_commands_resent_after_reconnection | `count_log_marker` | gw_log | "established again -- sending again on it" |
| `run_scenario` | scenario.assert_commands_resent_after_reconnection | `count_log_marker` | gw_log | "InboundConnectionManager::send_or_wait: connection from" |
| `run_scenario` | scenario.assert_engine_holds_reports_without_connection | `poll_log_for` | me_log | "no connection to a sequencer for reports -- acting on orders and holding their reports" |
| `run_scenario` | scenario.assert_engine_holds_reports_without_connection | `poll_log_for` | me_log | "a connection to a sequencer for reports is established -- sending" |
| `run_scenario` | scenario.assert_binary_checks | `poll_log_for` | binary_log | "primary sequencer connection" \| "established" |
| `run_scenario` | scenario.assert_session_provisioning | `poll_log_for` | gw_log | "authentication service connection" \| "established" |

## I. Throttles and logons (8)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.assert_gateway_orphaned | `count_log_marker` | gw_b_log | 'GW-PROGRESS' |
| `run_scenario` | scenario.assert_gateway_throttles | `poll_log_for` | gw_log | applied_marker |
| `run_scenario` | scenario.assert_gateway_throttles | `poll_log_for` | gw_log | stopped_marker |
| `run_scenario` | scenario.assert_gateway_throttles | `count_log_marker` | gw_log | marker \| limits_pos |
| `run_scenario` | scenario.assert_gateway_throttles | `poll_log_for` | gw_log | unlimited_marker |
| `run_scenario` | scenario.assert_session_provisioning | `count_log_marker` | gw_log | accepted_marker |
| `run_scenario` | scenario.assert_session_provisioning | `poll_log_for` | gw_log | refused_marker |
| `run_scenario` | scenario.assert_session_provisioning | `count_log_marker` | gw_log | _GW_LOGON_OK |

## J. The WAL written in order and undamaged, over the whole run (2)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `check_write_ahead_logs_in_order` |  | `count_log_marker` | sequencer_log | _WAL_OUT_OF_ORDER |
| `check_write_ahead_logs_in_order` |  | `count_log_marker` | sequencer_log | _WAL_DAMAGED |

## K. The test's own fault-injection library (5)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `run_scenario` | scenario.assert_engine_holds_only_logged_orders | `poll_log_for` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" |
| `run_scenario` | scenario.assert_publishers_follow_only_the_record | `poll_log_for` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" |
| `run_scenario` | scenario.assert_rejoining_old_leader_log_repaired | `poll_log_for` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" |
| `run_scenario` | scenario.assert_kept_reports_forwarded | `poll_log_for` | log_dir / "sequencer_primary.stdout" | "block_sends_to_ports: the flag file exists" |
| `run_scenario` | scenario.assert_logged_commands_reach_engine | `poll_log_for` | log_dir / "sequencer_secondary.stdout" | "block_sends_to_ports: the flag file exists" |

## L. Reading helpers used by the counts above (4)

| Function | Scenario setting | Helper | Log | Phrase |
|---|---|---|---|---|
| `poll_log_for` |  | `read_complete_lines` | log_path | pos |
| `wait_for_fix_logon` |  | `read_complete_lines` | gw_log | pos |
| `wait_for_me_ord` |  | `read_complete_lines` | me_log | pos |
| `wait_for_accepted_count` |  | `read_complete_lines` | me_log | pos |
