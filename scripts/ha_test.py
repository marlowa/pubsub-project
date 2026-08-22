#!/usr/bin/env python3
"""
ha_test.py — HA test suite for the pubsub_itc_fw sequencer system.

Starts the full 7-process system, sends baseline orders to confirm health,
executes a kill scenario, and verifies the expected outcome.  Reports
PASS/FAIL and observed timings.

Run from the project root:
    ./ha_test.py --scenario N [options]

Scenarios
---------
  1  Primary sequencer death
       Kills sequencer_primary.  The secondary detects peer heartbeat
       timeout (~15 s) and promotes itself to leader.  Recovery orders
       must flow through the new leader.
       Expected: sequencer_secondary elected leader; recovery orders OK.

  2  Primary arbiter death
       Kills arbiter_primary.  The arbiter secondary detects heartbeat
       timeout (~15 s) and becomes active.  The sequencer_primary remains
       leader throughout; order flow is uninterrupted during arbiter
       failover.
       Expected: arbiter_secondary elected active; orders uninterrupted.

  3  Secondary sequencer death
       Kills sequencer_secondary.  The primary stays leader; no role
       transition occurs.  The primary logs a peer-connection warning and
       keeps retrying.  Orders continue without disruption.
       Expected: sequencer_primary remains leader; orders continue.

  4  Secondary arbiter death
       Kills arbiter_secondary.  The primary arbiter stays active; no
       role transition occurs.  Sequencer operation is unaffected.
       Expected: arbiter_primary remains active; orders continue.

  5  Witness death
       Kills the witness process.  Once arbiter election is complete the
       arbiters communicate over their direct peer connection and no longer
       rely on the witness for ongoing heartbeats.
       Expected: arbiters retain established roles; orders continue.

  6  Both arbiters dead
       Kills arbiter_primary then arbiter_secondary (no arbiter failover).
       The sequencer_primary is already leader and continues sequencing
       without arbiter connectivity.
       WARNING: HA is degraded — a subsequent sequencer_primary death
       would leave no path to elect a new sequencer leader.
       Expected: sequencer_primary remains leader; orders continue.

  7  Sequential cascade: arbiter_primary then sequencer_primary death
       Kills arbiter_primary first (arbiter_secondary takes over in ≤15 s),
       then kills sequencer_primary.  The newly-elected arbiter_secondary
       grants the sequencer leadership role to sequencer_secondary.  Tests
       that a freshly-promoted arbiter correctly mediates a sequencer
       election.
       Expected: both arbiters and sequencer fail over in sequence.

  8  Witness-less arbiter election
       Kills the witness first (no disruption), then kills arbiter_primary.
       arbiter_secondary detects the heartbeat timeout (~15 s) and tries to
       contact the witness, which is unreachable.  It immediately
       self-promotes using the instance-id rule (no vote_timeout wait).
       Tests the fast fallback path in ArbiterThread.
       Expected: arbiter_secondary self-promotes; sequencer_primary stays
       leader; orders uninterrupted.

  9  Degraded sequencer election (no arbiters)
       Kills both arbiters first (no arbiter failover), then kills
       sequencer_primary.  sequencer_secondary contacts the arbiters for a
       role grant but none are reachable; it hits arbitration_timeout (3 s)
       and self-promotes using the instance-id rule.  Tests the sequencer's
       arbiter-unreachable fallback path.
       Expected: sequencer_secondary self-promotes; recovery orders OK
       (WARNING: HA severely degraded).

 10  Matching engine death and restart (simple)
       Kills matching_engine and restarts it.  ME reconnects to both
       sequencer ER listeners (7021/7022).  ME order_id_counter_ resets to
       0 after restart (no WAL), so recovery orders start at ME-ORD-1.
       Expected: ME restarts and recovery orders flow.

 11  ME death with primary arbiter death
       Kills arbiter_primary (arbiter_secondary takes over in ≤15 s), then
       kills and restarts matching_engine.  ME reconnects via the still-
       active sequencer pair; recovery orders are sequenced by the
       sequencer_primary (whose leadership was unaffected).
       Expected: arbiter failover + ME restart; recovery orders OK.

 12  ME death with both arbiters dead
       Kills arbiter_primary then arbiter_secondary (quickly, before any
       arbiter failover occurs), then kills and restarts matching_engine.
       The sequencer_primary is already leader and keeps sequencing.  No
       arbiter is available for future re-election.
       Expected: ME restart; recovery orders OK (WARNING: HA degraded).

 13  ME death with both arbiters and witness dead
       Kills witness, arbiter_primary, and arbiter_secondary in rapid
       succession (all before any failover timer fires), then kills and
       restarts matching_engine.  The sequencer_primary continues
       sequencing without any HA infrastructure.
       Expected: ME restart; recovery orders OK (WARNING: HA severely
       degraded — full restart of all HA components required to restore).

 15  Arbiter-mediated election: explicit PDU exchange trace
       Kills sequencer_primary (no failover flag — VerifySteps do the
       checking).  Explicitly verifies each step of the arbitration
       PDU exchange in the logs:
         a. sequencer_secondary sends ArbitrationReport to the arbiter pool
         b. arbiter_primary sends ArbitrationDecision back
         c. sequencer_secondary receives the ArbitrationDecision
         d. sequencer_secondary transitions to leader
       Confirms that sequencer promotion goes through the full arbitration
       protocol rather than the self-promotion fallback.
       Expected: all four PDU-exchange markers seen; recovery orders flow.

 16  Primary matching-engine death (ME HA failover)
       Runs the full ME-HA topology (matching_engine_primary + _secondary,
       _primary/_secondary configs).  Baseline orders are confirmed on
       matching_engine_primary; then matching_engine_primary is SIGKILLed.
       The secondary detects the lost replication connection, waits out the
       ~15 s promotion timeout, requests arbitration, adopts LEADER, and
       reconciles against the sequencer WAL.  The leader sequencer promotes its
       standby connection so recovery orders route to the promoted secondary,
       which processes them.
       Expected: matching_engine_secondary adopts LEADER; recovery orders
       confirmed on matching_engine_secondary.log.

 17  Authentication service A death (auth failover)
       The auth service is active/active, caller-selected -- both instances serve
       the gateway, neither is elected.  Baseline orders authenticate a FIX
       session via auth-A; then authentication_service_a is SIGKILLed.  No role
       transition occurs.  Phase 5 opens a FRESH FIX session: since an
       established session does not re-authenticate, the new logon must be
       authenticated by the surviving auth-B for it (and the recovery orders it
       carries) to succeed.
       Expected: no promotion; fresh logon authenticated by auth-B; recovery
       orders flow.

 18  FIX gateway instance A death (no session handover to B)
       Runs both FIX gateway instances and kills a.  Nothing is elected: a
       gateway elects nothing, and b inherits none of a's sessions.  Pins the
       current behaviour rather than a desired one -- the sequencer drops every
       execution report bound for the dead instance instead of rerouting it.
       Expected: b logs no traffic of a's; the sequencer reports dropping ERs.

 19  Cancel-on-disconnect grace period (step 3b)
       Kills nothing: the client session drops while the gateway lives.  The
       gateway must hold the dropped session's resting orders rather than
       cancelling them, for the number of seconds the comp id is provisioned
       for, and cancel nothing at all once the same comp id reconnects inside
       the window.
       Expected: a hold for the provisioned 90s, no cancel drain at any point.

 20  Session provisioning (step 4)
       Kills nothing.  The comp id is pinned to this gateway's instance with
       another as its backup, and the gateway must name both numbers when it
       admits the session.  The comp id is then re-provisioned onto an instance
       this gateway is not -- through the database and a real credentials
       export -- and the next logon must be refused.
       Expected: the provisioned pair named on admission; a refusal, and no
       session established, once the comp id belongs elsewhere.

Options:
    --scenario N|all      Scenario number, or 'all' to run every scenario in
                          order (required).  All scenarios must pass for the
                          command to exit with code 0.
    install_prefix        Path to cmake install prefix (default: installed)
    --orders-before N     Bursts of 1000 NOS to confirm health before kill (default: 1)
    --orders-during N     Extra bursts of 1000 NOS sent after the health check
                          and left in flight during Phase 4 so orders are
                          flowing when the kill happens (default: 20).  Some
                          may be lost during failover; the Phase 5 target
                          adjusts automatically.
    --orders-after N      Bursts of 1000 NOS sent as recovery orders (default: 1)
    --ready-timeout SECS  Max seconds for initial leader election (default: 10)
    --failover-timeout S  Max seconds per failover step (default: 30)
    --recovery-timeout S  Max seconds for recovery orders (default: 30)

Startup order (mirrors start_fix_seq_system.py):
  1. witness                          -- arbiters connect outbound to it (port 7100)
  2. arbiter_primary                  -- component listener 7200, peer listener 7203
  3. arbiter_secondary                -- component listener 7201, peer listener 7204
  4. authentication_service_a   -- listens on port 7070
  5. authentication_service_b -- listens on port 7071
  6. fix_order_gateway           -- FIX client port 9879, ER inbound port 7010
  7. sequencer_primary                -- listens on port 7001
  8. sequencer_secondary              -- listens on port 7002
  9. matching_engine                  -- connects outbound to sequencer ER listeners 7021/7022

Failover timing:
  Both the sequencer and arbiter followers arm a 15 s peer_heartbeat_timeout
  when they adopt the follower/passive role.  Each received heartbeat (sent
  every 5 s) resets the timer.  After a SIGKILL the TCP RST closes all peer
  connections immediately; the running timeout fires at its remaining value
  (worst case 15 s).
"""

from __future__ import annotations
import argparse
import hashlib
import hmac as _hmac
import os
import re
import secrets
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import NamedTuple

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY          = 1.0   # seconds between app launches
# Upper bound on how long a logon may take, not an expectation of how long it does take:
# wait_for_fix_logon returns the moment it sees an outcome, so raising this costs a passing
# run nothing. It must exceed the gateway's 5s sequence_state_timeout, or a logon that takes
# the fallback is failed here two seconds before the venue would have finished trying --
# reported as a timeout, which points at the wrong thing entirely. Taking that fallback is
# still a failure, but it is detected by _GW_SEQ_STATE_FALLBACK and named as itself.
FIX8_LOGON_WAIT        = 8.0   # seconds for f8test to establish a FIX session
LOG_POLL_INTERVAL      = 0.05  # seconds between log-file polls
SHUTDOWN_TIMEOUT       = 5.0   # seconds per-process for SIGTERM grace period
SETTLE_AFTER_FAILOVER  = 2.0   # seconds after failover confirmed (let conns stabilise)
# The venue's database, for the provisioning statements below. Host and port are passed to
# psql explicitly rather than left to libpq, because omitting --host selects the unix socket
# and peer authentication, which the pubsub_app role cannot use. Taken from PGHOST/PGPORT so
# that a host whose cluster is not on 5432 needs no edit here -- 5432 was hardcoded until
# 2026-08-11, when the RHEL8 install found it.
DB_HOST = os.environ.get("PGHOST", "localhost")
DB_PORT = os.environ.get("PGPORT", "5432")
# Cancel-on-disconnect grace scenario. The hold marker appears as soon as the socket
# closes, so its timeout is short. The quiet period must be comfortably under the
# deployed grace_period (30s) or the window would expire mid-test and the scenario
# would fail for the wrong reason.
_CANCEL_GRACE_HOLD_TIMEOUT  = 10.0
# The grace period scenario 19 provisions for FIX8_COMP_ID before running, deliberately
# different from the gateway's own configured default so the two cannot be confused.
_PROVISIONED_GRACE_PERIOD_SECONDS = 90
_CANCEL_GRACE_QUIET_PERIOD  = 5.0
# Session provisioning, scenario 20. The gateway ha_test runs is instance 1, so these say
# "this instance is the primary, with the instance the harness does not run as its backup":
# the baseline session must be admitted, and the log must name both numbers.
_PROVISIONED_PRIMARY_INSTANCE = 1
_PROVISIONED_BACKUP_INSTANCE  = 2
# Then the comp id is re-provisioned onto this instance alone, which the harness never runs,
# so the next logon at instance 1 must be refused. Deliberately left without a backup: with
# one, a gateway that ignored the primary and matched only the backup would still pass.
_ELSEWHERE_PRIMARY_INSTANCE   = 2
_PROVISIONING_LOGON_TIMEOUT   = 20.0
# Resend, scenario 22. The replay is a WAL scan on the sequencer's reactor thread, so it is
# slower than a log line appearing -- measured at tens of milliseconds against a few MB of
# retained WAL, but the timeout is generous because the scan grows with what the WAL holds.
_RESEND_TIMEOUT               = 30.0
# The client is a separate process writing to a file; give it a moment to flush what it
# received before counting. Asserting on another process's output without this reads an
# empty file and blames the venue.
_RESEND_CLIENT_SETTLE         = 3.0
# Scenario 23. The client that takes over on instance b after instance a is killed.
_INFLIGHT_A_CFG               = "myfix_gateway_client_inflight_a.xml"
_INFLIGHT_CFG                 = "myfix_gateway_client_inflight_b.xml"
# Bursts left in flight when the gateway is killed. Enough that some are certain to be
# mid-pipeline at the moment of death -- one burst could conceivably complete first.
_INFLIGHT_BURSTS              = 5
# Orders that must be through before the kill, proving the pipeline is full. A fraction of
# the burst, so the remainder is genuinely still in flight when the gateway dies.
_INFLIGHT_STARTED             = 500
_INFLIGHT_START_TIMEOUT       = 20.0
SETTLE_AFTER_KILL      = 1.0   # seconds after a kill where no failover is expected

FIX8_DIR = Path("/home/marlowa/mystuff/fix8_install")
FIX8_BIN = FIX8_DIR / "bin" / "f8test"
FIX8_CFG = "myfix_gateway_client.xml"
# f8test authenticates as SenderCompID CLIENT with an empty password.  The
# database-exported CLIENT credential may hold a different (non-empty) password,
# so we rewrite its SCRAM keys for the empty password before starting the auth
# service (mirrors perf_run.py::ensure_fix8_credentials).
FIX8_COMP_ID  = "CLIENT"
FIX8_PASSWORD = ""
# A second f8test config, generated from the stock one with reset_sequence_numbers turned OFF.
#
# The stock client sends ResetSeqNumFlag=Y on every Logon, which tells the venue to forget
# where the session's numbering had reached -- and a venue that honours that has nothing to
# resend, by the member's own account. Scenario 22 is about the opposite case: a member that
# keeps its numbering, notices the venue is ahead of it, and asks for what it missed.
FIX8_NO_RESET_CFG = "myfix_gateway_client_no_reset.xml"
# A SECOND comp id, used by the scenarios that open a fresh session after the baseline one.
#
# f8test numbers its ClOrdIDs ord1, ord2, ... from one in every process, so a second session
# under the same comp id re-uses ClOrdIDs its first session still has resting on the book.
# Since the matching engine keys an order on (comp id, protocol, ClOrdID) -- deliberately, so
# that a reconnecting member can manage what it left resting -- those are duplicates, and the
# venue is right to reject them. A real member does not re-use a live ClOrdID; the test client
# has no choice about it, so the fresh session gets an identity of its own.
FIX8_RECOVERY_COMP_ID = "CLIENT-RECOVERY"
FIX8_RECOVERY_CFG     = "myfix_gateway_client_recovery.xml"
# ──────────────────────────────────────────────────────────────────────────────

# Gateway log substrings for FIX logon outcome detection.
_GW_LOGON_OK       = "authentication succeeded -- FIX session established"
_GW_LOGON_FAIL     = "authentication failed"
_GW_SIG_MISMATCH   = "ServerSignature mismatch"
# The gateway gave up waiting for the sequencer to report the session's numbering and opened
# the session anyway, at a number the venue never confirmed. The session does establish, so
# without this marker the run looks clean; the gateway's own warning says a member expecting a
# higher number will see a low sequence and disconnect. Treated as a failed logon, because a
# session opened on unconfirmed numbering is not the thing the scenario meant to set up.
_GW_SEQ_STATE_FALLBACK = "did not report this session's numbering within"

# Substrings that appear together on adopt_role() log lines:
#   "SequencerThread: role transition {from} -> {to} (epoch={n})"
#   "ArbiterThread:   role transition {from} -> {to} (epoch={n})"
_SEQ_ROLE  = "SequencerThread: role transition"
_ARB_ROLE  = "ArbiterThread: role transition"
_TO_LEADER = "-> leader"

# Readiness markers for a sequencer restarting as follower.
_SEQ_FOLLOWER_MARKERS = (
    "SequencerThread: role transition",
    "-> follower",
)
_SEQ_FOLLOWER_TIMEOUT = 15.0   # seconds
SETTLE_AFTER_RESTART  = 2.0    # seconds after a sequencer restart before proceeding

# Readiness marker for a restarted matching engine.
#
# We wait for the sequencer's inbound ORDER connection to the ME to be
# re-established, not just the ME's outbound ER connection.  The sequencer
# connects outbound to ME's order listener (port configured in matching_engine
# service registry); after a ME restart the sequencer's OutboundConnectionManager
# retries after 2 s.  Only once this connection is up can the sequencer forward
# sequenced orders to ME.  Waiting for "sequencer order connection established"
# ensures both directions are ready before Phase 5 sends recovery orders.
# The ME logs its inbound order connection as "inbound sequencer order connection
# N established" (primary/non-HA) or "secondary sequencer order connection N
# established" (HA secondary); match the common "sequencer order connection" +
# "established" substrings so both forms are recognised.
_ME_READY_MARKERS = (
    "sequencer order connection",
    "established",
)
_ME_READY_TIMEOUT = 15.0  # seconds
_ME_SETTLE        = 1.0   # seconds after ME readiness confirmed


class KillStep(NamedTuple):
    """
    One kill action within a scenario.

    secondary_log_name: name of the log file to poll for a role-transition
                        to leader.  None if no failover is expected for this
                        kill (secondary/non-HA-critical processes).
    role_prefix:        role-transition log prefix (_SEQ_ROLE or _ARB_ROLE).
                        None when secondary_log_name is None.
    settle_secs:        how long to wait after the kill (or after failover is
                        confirmed) before proceeding to the next step.
    failover_to:        override the derived name used in log messages for the
                        process that takes over.  None uses the default
                        proc_name.replace("_primary", "_secondary") derivation.
                        Required when killing a _secondary and the _primary
                        takes over (e.g. WAL recovery scenario).
    """
    proc_name: str
    secondary_log_name: str | None
    role_prefix: str | None
    settle_secs: float
    failover_to: str | None = None
    # When set, poll for a line containing ALL of these markers instead of the
    # default (role_prefix, _TO_LEADER).  Used for components whose promotion log
    # line does not follow the "role transition ... -> leader" format, e.g. the
    # matching engine's "MatchingEngineThread: adopting LEADER role".
    leader_markers: tuple | None = None


class RestartStep(NamedTuple):
    """
    One kill-and-restart action within a scenario.

    Kills the named process (if still running), deletes its old log so Quill
    starts fresh, relaunches the process, and polls ready_log_name for a line
    containing ALL of ready_markers.

    resets_me_counter: True when restarting the matching engine, whose
                       order_id_counter_ resets to 0 on every startup (no
                       WAL).  Phase 5 adjusts its order-count target and
                       log-read position accordingly.
    settle_secs:       how long to wait after readiness is confirmed.
    """
    proc_name: str
    ready_log_name: str
    ready_markers: tuple
    ready_timeout: float
    resets_me_counter: bool
    settle_secs: float


class InterimOrdersStep(NamedTuple):
    """
    Send a batch of orders mid-Phase-4 and wait for ME confirmation.

    Used in WAL-recovery scenarios where orders must flow through an
    intermediate leader before the original primary is restarted.
    count_batches * 1000 orders are sent and confirmed before proceeding.
    """
    count_batches: int


class VerifyStep(NamedTuple):
    """
    Poll a log file for a line containing ALL of the given markers.

    Used to verify intermediate PDU-exchange steps (e.g. ArbitrationReport
    sent, ArbitrationDecision received) without relying solely on the final
    role-transition marker.

    log_name:    base name of the log file in log_dir
    markers:     tuple of strings that must all appear on the same line
    timeout:     maximum seconds to wait
    description: human-readable label shown in test output
    from_byte:   byte offset to start reading from (0 = beginning of file)

    absent_markers: strings that must NOT all appear on any one line, checked once the
                    positive markers have been seen. Some properties are only expressible
                    as an absence -- an instance that came back as a follower is proved as
                    much by never having adopted LEADER as by the follower line itself --
                    and a restart deletes the log first, so anything present afterwards
                    happened after the restart and nothing older can produce a false alarm.
    """
    log_name: str
    markers: tuple
    timeout: float
    description: str
    from_byte: int = 0
    absent_markers: tuple = ()


class Scenario(NamedTuple):
    number: int
    short_name: str
    description: str
    expected_outcome: str
    steps: list              # list[KillStep] — used when extra_steps is empty
    restart_steps: list = [] # list[RestartStep] — used when extra_steps is empty
    extra_steps: list = []   # list[KillStep|RestartStep|InterimOrdersStep]
                             # When non-empty, replaces steps+restart_steps in
                             # Phase 4 and suppresses orders_during sending.
    # When True, run against the full matching-engine HA topology (ME primary +
    # secondary, _primary/_secondary configs).  Baseline orders are confirmed on
    # matching_engine_primary.log; recovery orders (after the primary ME is
    # killed and the secondary promotes) are confirmed on
    # matching_engine_secondary.log.  Leaves all non-me_ha scenarios untouched.
    #
    # me_ha scenarios also assert cancel-on-failover ER routing (Phase 5): the ME
    # stub never matches, so every baseline order rests in the book and replicates
    # to the secondary; on promotion the secondary cancels the whole book and emits
    # seq_no=0 cancel ERs that must route to the client via the WalRecord envelope.
    # The assertion checks a non-empty book was cancelled and no ER was dropped for
    # a missing conn id.  See run_scenario's "ME-HA: cancel-on-failover" block.
    me_ha: bool = False
    # Override args.orders_during for this scenario (None = use the CLI value).
    # The ME-HA scenario sets 0 for a deterministic recovery count on the
    # promoted secondary (in-flight orders would advance the secondary's
    # order_id_counter_ silently during reconciliation).
    orders_during_override: int | None = None
    # When True, Phase 5 tears down the baseline FIX session and opens a fresh one
    # before sending recovery orders. An already-established session does not
    # re-authenticate, so this is required to exercise auth failover: the fresh
    # logon must be authenticated by whichever auth instance is still alive.
    fresh_logon_in_recovery: bool = False
    # When True, launch fix_order_gateway_b alongside _a. Every other scenario runs
    # instance a alone, because a second gateway adds a process and its timing to
    # scenarios that never touch it. Only the gateway-death scenario needs b, and it
    # needs it to show that b does NOT pick up a's work.
    gateway_b: bool = False
    # Override args.orders_after for this scenario (None = use the CLI value). Set to
    # 0 when the scenario kills the gateway the FIX session is on: there is no session
    # left to send recovery orders through, so Phase 5 has nothing to do and skips.
    orders_after_override: int | None = None
    # When True, assert the CURRENT no-handover behaviour after a gateway instance is
    # killed: reports for the dead instance are dropped by the sequencer and the
    # surviving instance inherits nothing. See run_scenario's "gateway orphan" block.
    assert_gateway_orphaned: bool = False
    # When True, exercise cancel-on-disconnect's grace period: drop the client session
    # with the gateway still running, prove nothing is cancelled while the window is open,
    # then reconnect the same comp id and prove nothing is cancelled at all. See
    # run_scenario's "cancel grace" block. Kills nothing -- the gateway must survive, or
    # there is no process left to do the cancelling and the test proves nothing.
    assert_cancel_grace: bool = False
    # When True, exercise session provisioning (step 4): prove the gateway admits a comp id
    # provisioned for the instance it is, naming the numbers it was given, and then refuses
    # the same comp id once it is provisioned elsewhere. See run_scenario's "provisioning"
    # block. Kills nothing: the failure this guards against is a session landing on a
    # gateway that is not its own, which needs every process alive to be visible.
    assert_session_provisioning: bool = False
    # When True, drop the baseline FIX session and open a fresh one BEFORE Phase 4 runs, so
    # the orders resting on the book were placed by a connection that no longer exists. What
    # follows then tests the thing step 5 built: a session is an identity, not an address,
    # so reports for those orders must reach the member on its NEW connection. See
    # run_scenario's "reconnect before the kill" block.
    reconnect_before_kill: bool = False
    # When True, drive this scenario with a client that does NOT reset its sequence numbers,
    # and assert the resend path: the venue continues the member's numbering across the
    # reconnect, the member notices the gap and asks, and the venue answers with the real
    # execution reports rather than gap-filling them away. See run_scenario's "resend" block.
    assert_resend_recovery: bool = False
    # When True, kill the gateway with orders IN FLIGHT and then bring the client back on the
    # OTHER instance, asserting what the member can recover. See run_scenario's "in-flight"
    # block. Needs gateway_b, since the whole point is that the member returns elsewhere.
    assert_inflight_recovery: bool = False


# ── helpers shared by scenario definitions ────────────────────────────────────

def _me_restart_step() -> RestartStep:
    return RestartStep(
        proc_name="matching_engine",
        ready_log_name="matching_engine.log",
        ready_markers=_ME_READY_MARKERS,
        ready_timeout=_ME_READY_TIMEOUT,
        resets_me_counter=True,
        settle_secs=_ME_SETTLE,
    )


# ── scenario catalogue ────────────────────────────────────────────────────────
#
# Scenario 1 — Primary sequencer death
#   The sequencer pair uses a heartbeat/timeout mechanism: the leader sends a
#   heartbeat PDU to its peer every 5 s; the follower arms a 15 s one-shot
#   timeout that resets on each received heartbeat.  When sequencer_primary is
#   SIGKILLed, both peer TCP connections (ports 7003/7004) close with RST.  The
#   running timeout on sequencer_secondary fires at its remaining value (worst
#   case 15 s).  The secondary then contacts the active arbiter to request the
#   leader role and transitions.  The gateway already has a sequencer_secondary
#   connection (port 7002), so it passes the "at least one sequencer connected"
#   guard and forwards new orders to the secondary.  The matching engine already
#   has an outbound ER connection to the secondary ER listener (7022), so ERs
#   flow to the secondary immediately.
#
# Scenario 2 — Primary arbiter death
#   The arbiter pair mirrors the sequencer pair's heartbeat/timeout scheme
#   (15 s timeout, 5 s heartbeat interval).  When arbiter_primary is killed,
#   the arbiter_secondary detects it via its peer heartbeat timeout and
#   self-promotes via the witness.  The sequencer_primary is already the leader
#   and does not need to re-elect; it loses its arbiter_primary connection and
#   retries it harmlessly.  Order flow is continuous during arbiter failover.
#
# Scenario 3 — Secondary sequencer death
#   The primary sequencer is the leader.  Killing sequencer_secondary causes
#   the primary to log a peer-connection-lost warning and continue retrying the
#   outbound peer connection (port 7004), which is normal behaviour.  No role
#   transition occurs.  Orders continue without any disruption because the
#   sequencer_primary is unaffected.
#
# Scenario 4 — Secondary arbiter death
#   The primary arbiter is active.  Killing arbiter_secondary removes the peer
#   connection from the primary arbiter's perspective, but the primary remains
#   active and keeps retrying.  No sequencer state changes.  Orders continue.
#
# Scenario 5 — Witness death
#   The witness is a quorum member for arbiter elections (it provides a tie-
#   breaking vote so neither arbiter can self-promote without a majority).
#   Once arbiter_primary has been elected active (during Phase 2), the arbiters
#   communicate over their direct peer connection (ports 7203/7204) for ongoing
#   heartbeats; the witness is not on the critical path for that traffic.
#   Killing the witness has no observable effect on established roles or order
#   flow.  NOTE: if both arbiters were to restart after this scenario, they
#   would be unable to elect a new active without the witness.
#
# Scenario 6 — Both arbiters dead
#   Killing both arbiters leaves the system without any arbiter.  The
#   sequencer_primary is already the leader and continues sequencing because
#   the per-order hot path does not consult the arbiters.  The
#   sequencer_secondary will keep retrying its arbiter connections, which is
#   harmless.  No role transition occurs; orders continue.  HA is degraded:
#   if sequencer_primary then dies, sequencer_secondary cannot elect a new
#   leader (it would contact arbiters for the role grant, but none are
#   reachable), leaving the service down until an arbiter is restarted.
#
# Scenario 7 — Sequential cascade: arbiter_primary then sequencer_primary
#   First kills arbiter_primary and waits for arbiter_secondary to become the
#   active arbiter (same mechanism as scenario 2, ≤15 s).  Then kills
#   sequencer_primary: sequencer_secondary detects the heartbeat timeout,
#   contacts the now-active arbiter_secondary for a role grant, and transitions
#   to leader.  Verifies that a freshly-promoted arbiter correctly mediates a
#   sequencer election.
#
# Scenario 8 — Witness-less arbiter election
#   Kills the witness first (SETTLE_AFTER_KILL is enough — the arbiters use
#   only their direct peer connection for ongoing heartbeats once elected).
#   Then kills arbiter_primary.  When arbiter_secondary's peer_heartbeat_timeout
#   fires (~15 s), it enters the election path: it checks witness_conn_id_,
#   finds it invalid, and immediately self-promotes using the instance-id rule
#   (ArbiterThread.cpp line 182: "witness not connected -- self-promoting").
#   This is faster than scenario 2 because there is no vote_timeout wait.
#   The sequencer_primary stays leader; order flow is uninterrupted.
#
# Scenario 9 — Degraded sequencer election (no arbiters)
#   Kills both arbiters in rapid succession (SETTLE_AFTER_KILL = 1 s each,
#   well below the 15 s peer_heartbeat_timeout, so neither arbiter ever
#   re-elects before being killed).  Then kills sequencer_primary.
#   sequencer_secondary detects the heartbeat timeout and contacts the
#   arbiters for a role grant; all arbiters are unreachable, so it hits
#   arbitration_timeout (3 s) and self-promotes via the instance-id fallback
#   (SequencerThread.cpp line ~531).  Recovery orders must flow through the
#   newly self-promoted sequencer_secondary.
#
# Scenario 10 — Matching engine death and restart (simple)
#   Kills matching_engine, deletes its log, and restarts it.  ME reconnects
#   to both sequencer ER listeners (7021 primary, 7022 secondary).  Because
#   the ME has no WAL, order_id_counter_ resets to 0 on restart; recovery
#   orders therefore begin at ME-ORD-1.  Phase 5 reads the new ME log from
#   byte 0 and waits for ME-ORD-{orders_after*1000}.
#
# Scenario 11 — ME death with primary arbiter death
#   Kills arbiter_primary (waits for arbiter_secondary to become active,
#   ≤15 s), then kills and restarts matching_engine.  Both sequencers remain
#   alive and the sequencer_primary is still leader, so the ME reconnects to
#   both ER listeners immediately.  Recovery orders are sequenced by the
#   unchanged sequencer_primary.
#
# Scenario 12 — ME death with both arbiters dead
#   Kills arbiter_primary then arbiter_secondary in rapid succession (1 s
#   settle each — well below the 15 s heartbeat timeout so no arbiter
#   failover occurs), then kills and restarts matching_engine.  The
#   sequencer_primary stays leader; ME reconnects to both ER listeners.
#   HA is degraded: a future sequencer failure would leave no re-election
#   path.
#
# Scenario 13 — ME death with both arbiters and witness dead
#   Kills witness, arbiter_primary, and arbiter_secondary in rapid succession
#   (1 s settle each), then kills and restarts matching_engine.  No HA
#   component survives.  The sequencer_primary is already leader and keeps
#   sequencing; ME reconnects to both ER listeners after restart.  The system
#   is in a severely degraded state: a full restart of witness + both arbiters
#   is required to restore HA capability.
#
_SCENARIOS: list[Scenario] = [
    # 1 — primary sequencer death: expect sequencer_secondary to become leader
    Scenario(
        number=1,
        short_name="primary_sequencer_death",
        description="Death of primary sequencer",
        expected_outcome=(
            "sequencer_secondary elected leader in ≤15 s; "
            "recovery orders flow through the new leader"
        ),
        steps=[
            KillStep(
                proc_name="sequencer_primary",
                secondary_log_name="sequencer_secondary.log",
                role_prefix=_SEQ_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 2 — primary arbiter death: expect arbiter_secondary to become active
    Scenario(
        number=2,
        short_name="primary_arbiter_death",
        description="Death of primary arbiter",
        expected_outcome=(
            "arbiter_secondary elected active in ≤15 s; "
            "sequencer_primary remains leader; orders uninterrupted"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name="arbiter_secondary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 3 — secondary sequencer death: no failover, primary stays leader
    Scenario(
        number=3,
        short_name="secondary_sequencer_death",
        description="Death of secondary sequencer",
        expected_outcome=(
            "no role transition; sequencer_primary remains leader; "
            "orders continue without disruption"
        ),
        steps=[
            KillStep(
                proc_name="sequencer_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
    ),

    # 4 — secondary arbiter death: no failover, primary arbiter stays active
    Scenario(
        number=4,
        short_name="secondary_arbiter_death",
        description="Death of secondary arbiter",
        expected_outcome=(
            "no role transition; arbiter_primary remains active; "
            "orders continue without disruption"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
    ),

    # 5 — witness death: no disruption once arbiters have elected a leader
    Scenario(
        number=5,
        short_name="witness_death",
        description="Death of witness",
        expected_outcome=(
            "arbiters retain their established roles; "
            "orders continue without disruption"
        ),
        steps=[
            KillStep(
                proc_name="witness",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
    ),

    # 6 — both arbiters dead: sequencer primary stays leader, HA is degraded
    Scenario(
        number=6,
        short_name="both_arbiters_dead",
        description="Death of both arbiters",
        expected_outcome=(
            "no sequencer failover; sequencer_primary remains leader; "
            "orders continue (WARNING: HA degraded — re-election impossible)"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
    ),

    # 7 — sequential cascade: arbiter failover then sequencer failover
    Scenario(
        number=7,
        short_name="cascade_arbiter_then_sequencer",
        description="Sequential cascade: arbiter_primary then sequencer_primary death",
        expected_outcome=(
            "arbiter_secondary elected active in ≤15 s; "
            "then sequencer_secondary elected leader via new arbiter in ≤15 s; "
            "recovery orders flow through new sequencer leader"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name="arbiter_secondary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="sequencer_primary",
                secondary_log_name="sequencer_secondary.log",
                role_prefix=_SEQ_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 8 — witness-less arbiter election: arbiter self-promotes via instance-id rule
    Scenario(
        number=8,
        short_name="witnessless_arbiter_election",
        description="Witness-less arbiter election: witness then arbiter_primary death",
        expected_outcome=(
            "arbiter_secondary self-promotes via instance-id rule (no witness); "
            "sequencer_primary remains leader; orders uninterrupted"
        ),
        steps=[
            KillStep(
                proc_name="witness",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name="arbiter_secondary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 9 — degraded sequencer election: sequencer self-promotes with no arbiters
    Scenario(
        number=9,
        short_name="degraded_sequencer_election",
        description="Degraded sequencer election: both arbiters then sequencer_primary death",
        expected_outcome=(
            "sequencer_secondary self-promotes via instance-id rule (no arbiters); "
            "recovery orders flow through the new leader "
            "(WARNING: HA severely degraded)"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="sequencer_primary",
                secondary_log_name="sequencer_secondary.log",
                role_prefix=_SEQ_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 10 — ME death and restart (simple)
    Scenario(
        number=10,
        short_name="me_death_restart",
        description="Matching engine death and restart",
        expected_outcome=(
            "ME restarts and reconnects to sequencer ER listeners; "
            "recovery orders flow (ME order counter resets to 1 after restart)"
        ),
        steps=[],
        restart_steps=[_me_restart_step()],
    ),

    # 11 — ME death with primary arbiter death (arbiter failover first)
    Scenario(
        number=11,
        short_name="me_death_arbiter_primary_death",
        description="ME death with primary arbiter death",
        expected_outcome=(
            "arbiter_secondary elected active in ≤15 s; "
            "ME restarts and reconnects; recovery orders flow"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name="arbiter_secondary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
        restart_steps=[_me_restart_step()],
    ),

    # 12 — ME death with both arbiters dead (no arbiter failover, HA degraded)
    Scenario(
        number=12,
        short_name="me_death_both_arbiters_dead",
        description="ME death with both arbiters dead",
        expected_outcome=(
            "no arbiter failover; ME restarts and reconnects; "
            "recovery orders flow (WARNING: HA degraded — sequencer "
            "re-election impossible)"
        ),
        steps=[
            # Kill both arbiters quickly (1 s settle each) so neither can
            # self-promote before being killed — the 15 s peer_heartbeat_timeout
            # has not yet fired when the second arbiter is killed.
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
        restart_steps=[_me_restart_step()],
    ),

    # 13 — ME death with both arbiters and witness dead (severely degraded)
    Scenario(
        number=13,
        short_name="me_death_all_ha_dead",
        description="ME death with both arbiters and witness dead",
        expected_outcome=(
            "no arbiter failover; ME restarts and reconnects; "
            "recovery orders flow (WARNING: HA severely degraded — full "
            "restart of witness + both arbiters required to restore HA)"
        ),
        steps=[
            KillStep(
                proc_name="witness",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
        restart_steps=[_me_restart_step()],
    ),

    # 14 — WAL recovery: primary sequencer restarts as follower then re-elected
    #
    # This scenario validates the write-ahead log:
    #
    #   a. sequencer_primary is leader; processes 1000 baseline orders
    #      (WAL records seq 1–1000).
    #   b. sequencer_primary is killed; sequencer_secondary is elected leader
    #      at epoch 1.
    #   c. 1000 interim orders are processed by the secondary (seq 1001–2000).
    #   d. sequencer_primary is restarted; it reads the WAL to recover
    #      next_sequence_number_=1001, then the peer protocol must communicate
    #      the secondary's current sequence number (2001) so the primary can
    #      sync and rejoin as follower at the correct position.
    #   e. sequencer_secondary is killed; sequencer_primary is elected leader.
    #   f. 1000 recovery orders → target ME-ORD-3000.
    #
    # If WAL recovery + peer sequence-number sync work correctly the primary
    # resumes from seq 2001 and the ME log shows seq 1–1000, 1001–2000, 2001–3000
    # — monotonically increasing, no resets.  If the primary fails to sync and
    # resets to seq 1001, the ME log shows the range 1001–2000 a second time;
    # the seq monotonicity check catches the reset and the test fails.
    # (ME-ORD-3000 is reached in both cases because ME-ORD is the ME's own
    # counter, independent of sequencer seq numbers — checking ME-ORD alone is
    # insufficient to validate WAL+sync correctness.)
    Scenario(
        number=14,
        short_name="wal_recovery",
        description="WAL recovery: primary sequencer restarts as follower then re-elected leader",
        expected_outcome=(
            "sequencer_primary restarts, reads WAL, syncs sequence number from "
            "peer and rejoins as follower; after secondary dies primary is "
            "re-elected and continues from the correct sequence number — "
            "no ME-ORD gap or reset"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                secondary_log_name="sequencer_secondary.log",
                role_prefix=_SEQ_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            InterimOrdersStep(count_batches=1),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready_markers=_SEQ_FOLLOWER_MARKERS,
                ready_timeout=_SEQ_FOLLOWER_TIMEOUT,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            KillStep(
                proc_name="sequencer_secondary",
                secondary_log_name="sequencer_primary.log",
                role_prefix=_SEQ_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="sequencer_primary",
            ),
        ],
    ),

    # 15 — Arbiter-mediated election: explicit ArbitrationReport/Decision trace
    #
    # Scenario 1 verifies that sequencer_secondary becomes leader after primary
    # dies, but only checks the final role-transition log line.  This scenario
    # additionally verifies each step of the arbitration PDU exchange:
    #   a. sequencer_secondary arms peer_heartbeat_timeout (~15 s after kill).
    #   b. On timeout it sends ArbitrationReport to both arbiters.
    #   c. The active arbiter (arbiter_primary) processes the report and sends
    #      ArbitrationDecision back.
    #   d. sequencer_secondary receives the decision and transitions to leader.
    # Each step is watched in the appropriate log with a generous timeout.
    # The peer_heartbeat_timeout fires within 15 s; the full exchange adds only
    # a few milliseconds on top.  A 25 s timeout per VerifyStep gives headroom.
    Scenario(
        number=15,
        short_name="arbiter_mediated_election",
        description="Arbiter-mediated election: explicit ArbitrationReport/Decision PDU trace",
        expected_outcome=(
            "sequencer_secondary sends ArbitrationReport; "
            "arbiter_primary sends ArbitrationDecision; "
            "sequencer_secondary receives decision and transitions to leader; "
            "recovery orders flow"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=0.0,
            ),
            VerifyStep(
                log_name="sequencer_secondary.log",
                markers=("ArbitrationReport sent to arbiter pool",),
                timeout=25.0,
                description="sequencer_secondary sent ArbitrationReport to arbiter pool",
            ),
            VerifyStep(
                log_name="arbiter_primary.log",
                markers=("ArbitrationDecision sent to connection",),
                timeout=5.0,
                description="arbiter_primary sent ArbitrationDecision",
            ),
            VerifyStep(
                log_name="sequencer_secondary.log",
                markers=("ArbitrationDecision received",),
                timeout=5.0,
                description="sequencer_secondary received ArbitrationDecision",
            ),
            VerifyStep(
                log_name="sequencer_secondary.log",
                markers=(_SEQ_ROLE, _TO_LEADER),
                timeout=5.0,
                description="sequencer_secondary transitioned to leader",
            ),
        ],
    ),

    # 16 — primary matching-engine death: expect matching_engine_secondary to
    # promote via the arbiter and recovery orders to flow to it.
    Scenario(
        number=16,
        short_name="primary_me_death",
        description="Death of primary matching engine (ME HA failover)",
        expected_outcome=(
            "matching_engine_secondary detects primary loss, promotes via the "
            "arbiter (adopts LEADER) after the ~15 s promotion timeout, "
            "reconciles against the sequencer WAL, and processes recovery orders"
        ),
        me_ha=True,
        # In-flight orders would advance the promoted secondary's
        # order_id_counter_ silently during reconciliation; suppress them so the
        # recovery count on the secondary is deterministic.
        orders_during_override=0,
        steps=[
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
        ],
    ),

    # 17 — auth service A death: no election (active/active); the gateway fails
    # over to auth B, which authenticates a fresh FIX logon.
    Scenario(
        number=17,
        short_name="auth_a_death",
        description="Death of authentication service A (auth failover on fresh logon)",
        expected_outcome=(
            "no role transition (auth is active/active, caller-selected); the "
            "gateway detects auth-A loss, and a fresh FIX logon is authenticated "
            "by auth-B so recovery orders flow"
        ),
        # In-flight orders would ride the already-authenticated baseline session
        # (unaffected by the auth kill) and only add noise; the point is the fresh
        # logon in Phase 5.
        orders_during_override=0,
        fresh_logon_in_recovery=True,
        steps=[
            KillStep(
                proc_name="authentication_service_a",
                secondary_log_name=None,   # active/active — no promotion to await
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 18 — FIX gateway instance A death.
    #
    # This scenario asserts a GAP, not a guarantee. Running two gateway instances
    # reduces the single point of failure for *new* sessions, but there is no session
    # handover: instance b inherits nothing from a, and reports for a's orders are
    # dropped by the sequencer rather than rerouted. See the "What running two
    # instances gives you today" section of docs/design/gateway_ha.md.
    #
    # Getting a deterministic ER for an order whose gateway is dead is the awkward
    # part. Killing the gateway mid-burst would work but is a race. Instead this
    # reuses the ME-HA cancel-on-failover path purely as an ER generator: the ME stub
    # never matches, so every baseline order rests in the book and replicates to the
    # secondary; killing the primary ME makes the promoted secondary cancel the whole
    # book, emitting exactly one cancel ER per resting order. Those orders came from
    # gateway a, which by then is dead, so every one of those ERs has nowhere to go.
    #
    # Kill order matters: gateway a first, ME primary second. The reverse would let
    # the cancel ERs reach a before it died and prove nothing.
    #
    # The first version of this scenario could not assert the drop, because the reports
    # never reached the drop decision: cancel ERs carry seq_no 0 and the sequencer gated
    # them on a WalAck for seq_no 0 that could never arrive, so they were parked forever
    # (and, pending_er_ being keyed on that sequence, all but the first were discarded).
    # That defect is fixed -- they now gate on their own WAL record -- which is what
    # makes the drop assertion below reachable at all.
    #
    # WHEN THE HANDOVER WORK LANDS (steps 3b-6), THIS TEST SHOULD FAIL, and the fix is
    # to invert the assertions rather than delete them: dropped_ers becomes 0, and
    # gateway b should show the recovered session's traffic instead of nothing.
    Scenario(
        number=18,
        short_name="fix_gateway_a_death",
        description="Death of FIX gateway instance A (no session handover to B)",
        expected_outcome=(
            "no election (a gateway elects nothing); instance b keeps running but "
            "inherits none of a's sessions, and the sequencer drops every execution "
            "report bound for the dead instance instead of rerouting it to b"
        ),
        me_ha=True,
        gateway_b=True,
        # The FIX session lives on gateway a. Once a is killed there is no session to
        # send through, so Phase 5 has nothing to do.
        orders_during_override=0,
        orders_after_override=0,
        assert_gateway_orphaned=True,
        steps=[
            KillStep(
                proc_name="fix_order_gateway_a",
                secondary_log_name=None,   # nothing is elected — that is the point
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
        ],
    ),

    # 19 — cancel-on-disconnect grace period (step 3b).
    #
    # Nothing is killed. That is deliberate: the failure this guards against is a client
    # connection dropping while the gateway lives, and if the gateway process died there
    # would be nothing left to send the cancels the test is checking for.
    #
    # Without a grace period the gateway cancels a dropped session's whole book the instant
    # the socket closes, so a member whose connection blipped comes back flat -- closed out
    # by a network event rather than by anything it did. Multiply that by every session on
    # a failing gateway and the high-availability mechanism produces exactly the outcome
    # high availability exists to prevent.
    #
    # Three things are asserted, in order: the gateway says it is holding rather than
    # cancelling; nothing is actually cancelled while the window is open; and when the same
    # comp id logs back on, its orders are released with no cancel ever sent.
    Scenario(
        number=19,
        short_name="cancel_on_disconnect_grace",
        description="Cancel-on-disconnect grace period (client drops, gateway survives)",
        expected_outcome=(
            "the gateway holds the dropped session's orders instead of cancelling them, "
            "and cancels nothing at all once the same comp id reconnects inside the window"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_cancel_grace=True,
        steps=[],
    ),

    # 20 — session provisioning (step 4).
    #
    # Nothing is killed here either. A session is provisioned against a primary gateway
    # instance and optionally a backup, and may log on to either and nowhere else. The
    # failure being guarded against is not a process dying but a session being admitted by
    # a gateway that is not one of its two -- which needs every process alive to see.
    #
    # Both directions are checked, because only one of them discriminates on its own. That
    # the gateway admits a member provisioned for it proves nothing by itself: a gateway
    # that ignored provisioning entirely would pass that. So the first assertion reads the
    # NUMBERS out of the gateway's log -- the primary and backup it was actually told, and
    # which of the two it believes itself to be -- and the second re-provisions the comp id
    # onto an instance this gateway is not, and requires the next logon to be refused.
    #
    # The re-provisioning goes through the database and a real credentials export, so a hop
    # that silently drops the values fails this rather than quietly falling back to
    # "everyone may log on anywhere", which is indistinguishable from an unpinned venue
    # unless the numbers themselves are checked.
    Scenario(
        number=20,
        short_name="session_provisioning",
        description="Session provisioning (a logon is refused at an instance it is not provisioned for)",
        expected_outcome=(
            "the gateway admits the comp id provisioned for its own instance, naming the "
            "primary and backup it was given, and refuses the same comp id once it is "
            "provisioned for another instance"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_session_provisioning=True,
        steps=[],
    ),

    # 21 — a reconnected session inherits reports for orders it placed earlier (step 5).
    #
    # This is scenario 16 with one thing changed: the client drops and comes back on a new
    # connection BEFORE the matching engine is killed. That one change is the whole test.
    #
    # The baseline orders rest on the book, placed by a connection that is then closed. The
    # promoted matching engine cancels the whole book on promotion and emits a cancel report
    # for every order -- reports for orders whose originating connection no longer exists.
    # They must be delivered to the member's CURRENT connection.
    #
    # Before step 5 they could not be: the sequencer's routing entry held the connection the
    # order arrived on, and addressed the reports at it. The connection was gone, so every
    # one was dropped and the member was never told its book had been cancelled. Now the
    # entry holds the session's identity and the address is resolved when the report is
    # sent, so the reconnect re-binds it.
    #
    # The assertion is the same one scenario 16 makes, and it discriminates for exactly this
    # reason: the gateway must have sent one report per order PLUS one per cancel. If the
    # cancel reports went to the dead connection the count falls short by the size of the
    # book.
    Scenario(
        number=21,
        short_name="reconnect_inherits_reports",
        description="A reconnected session inherits reports for orders placed on its old connection",
        expected_outcome=(
            "the promoted matching engine cancels the resting book, and every cancel report "
            "is delivered to the member's new connection rather than dropped at the address "
            "of the connection that placed the orders"
        ),
        me_ha=True,
        reconnect_before_kill=True,
        # In-flight orders would advance the promoted secondary's order_id_counter_ during
        # reconciliation, and there is no Phase 5 here: the cancel burst is the subject.
        orders_during_override=0,
        orders_after_override=0,
        steps=[
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
        ],
    ),

    # 22 -- a reconnecting member is sent the reports it missed (step 6).
    #
    # Nothing is killed. The member simply goes away and comes back, which is the ordinary
    # case a venue has to survive many times a day, and the one where the old behaviour was
    # least defensible: the gateway answered every ResendRequest with a blanket
    # SequenceReset-GapFill, declaring the missing range administrative and skipping it. The
    # session stayed open and the member was told nothing about what had happened to its
    # orders.
    #
    # This scenario uses a client that does NOT reset its sequence numbers, because the stock
    # one asks the venue to forget the session on every logon -- and a venue that honours
    # that, as it must, has nothing to resend.
    #
    # Three things are asserted, and the third is the one that matters: the venue continues
    # the member's numbering rather than restarting it; the member notices and asks; and what
    # comes back is real execution reports carrying PossDupFlag=Y, checked in the CLIENT's own
    # received messages rather than in a gateway log line.
    Scenario(
        number=22,
        short_name="resend_recovery",
        description="A reconnecting member is sent the execution reports it missed",
        expected_outcome=(
            "the venue resumes the session's sequence numbering across the reconnect, the "
            "member asks for what it missed, and receives real execution reports marked "
            "PossDupFlag=Y instead of a blanket gap-fill"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_resend_recovery=True,
        steps=[],
    ),

    # 23 -- orders in flight when a gateway dies, and the member returns on the other one.
    #
    # The question this answers: a member has NewOrderSingles in flight, its gateway is
    # killed outright, and it reconnects to the surviving instance. What can it recover?
    #
    # "In flight" is really five positions, and they do not share a fate:
    #   1. still in the client's socket, unread          -- gone, never reached the venue
    #   2. read by the gateway, not yet forwarded        -- gone, died with the process
    #   3. forwarded, in flight to the sequencer         -- usually arrives and is sequenced
    #   4. sequenced and WAL'd, no ER emitted yet        -- A LIVE ORDER
    #   5. ER emitted, addressed at the dead instance    -- A LIVE ORDER, undeliverable report
    #
    # Cases 4 and 5 are what matters: those orders are resting on the book and the member has
    # never heard of them. Cancel-on-disconnect cannot save it either -- the process that
    # would send the cancels is the one that died -- so they simply stay live.
    #
    # What the member should get on returning to instance b is the reports it missed, because
    # the session identity survives the instance change (step 5) and the reports are replayable
    # from the sequencer's WAL (step 6). It uses a client that keeps its sequence numbers, so
    # it notices the gap and asks.
    #
    # What NO test here can prove: cases 1 and 2 are unobservably lost. Nothing distinguishes
    # an order that died in a socket from one never sent, which is why the venue also wants an
    # Order Mass Status Request -- not implemented; see the memory of that name.
    Scenario(
        number=23,
        short_name="inflight_gateway_death",
        description="Orders in flight when a gateway dies; the member returns on the other instance",
        expected_outcome=(
            "orders that reached the sequencer stay live on the book, and the member "
            "reconnecting to instance b is sent the execution reports it missed"
        ),
        gateway_b=True,
        # Phase 3's baseline plus the in-flight bursts sent below; Phase 5 is skipped because
        # the session it would use died with instance a.
        orders_during_override=0,
        orders_after_override=0,
        assert_inflight_recovery=True,
        steps=[
            KillStep(
                proc_name="fix_order_gateway_a",
                secondary_log_name=None,   # nothing is elected -- a gateway elects nothing
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 24 — a restarted primary ME must come back as a FOLLOWER, not seize leadership.
    #
    # This is the regression test for "Rejoin after a promotion re-runs the cold-start
    # tie-break" in docs/bug_list.md. The arbiter used to recompute leadership from instance
    # ids on every request and never consult what it had already decided. The primary always
    # holds the lower id, so a primary that restarted after the secondary had been promoted
    # was handed leadership back -- from the instance holding a populated order book to the
    # one that had just lost its.
    #
    # CURRENTLY FAILS, and not on anything it asserts. All four steps above pass; the run then
    # fails in the recovery-order phase because the sequencer routes orders to whichever socket
    # is "the primary matching engine" rather than to the leader, so the rejoined follower
    # receives them and discards them. That is a separate defect, recorded as "The sequencer
    # routes orders to the primary matching engine, not to the leader". This is a real failure
    # and not a broken harness -- do not disable the scenario to make the suite green.
    #
    # It also covers the second half of that fix. leadership_state_ was keyed by instance, so
    # the arbiter's reconnect path looked the stored decision up under the connecting
    # instance's OWN id and, after a promotion, found nothing recorded against the primary.
    # Before the fix the restarted primary was told nothing at all and sat in Unknown, so the
    # positive assertion below fails as surely as the negative one.
    Scenario(
        number=24,
        short_name="me_primary_rejoins_as_follower",
        description="Restarted primary ME rejoins as follower, not leader",
        expected_outcome=(
            "matching_engine_secondary promotes and keeps leadership; the restarted "
            "matching_engine_primary is assigned the follower role by the arbiter and "
            "never adopts LEADER"
        ),
        me_ha=True,
        # As scenario 16: in-flight orders would advance the promoted secondary's counter
        # during reconciliation and make the recovery count non-deterministic.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            # 1. Kill the primary and let the secondary promote properly, timeout and all.
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
            # 2. Bring the primary back. The restart deletes its log first, so everything
            #    asserted on below happened after it came back.
            RestartStep(
                proc_name="matching_engine_primary",
                ready_log_name="matching_engine_primary.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            # 3. The assertion the fix exists for, stated both ways round.
            VerifyStep(
                log_name="matching_engine_primary.log",
                markers=("MatchingEngineThread:", "arbiter assigned follower role"),
                timeout=30.0,
                description="restarted primary is assigned the follower role",
                absent_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
            # 4. And the secondary must have kept it throughout -- a leadership that moved
            #    and moved back would satisfy step 3 on its own.
            VerifyStep(
                log_name="matching_engine_secondary.log",
                markers=("MatchingEngineThread:", "adopting LEADER role"),
                timeout=5.0,
                description="promoted secondary still holds leadership",
            ),
        ],
    ),
]

_SCENARIO_MAP: dict[int, Scenario] = {s.number: s for s in _SCENARIOS}


# ── utilities ─────────────────────────────────────────────────────────────────

class TestFailure(Exception):
    """Raised by die() to abort the current scenario and print RESULT: FAIL."""


def log(msg: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def die(msg: str) -> None:
    log(f"FAIL: {msg}")
    raise TestFailure(msg)


def resolve_prefix(raw: str) -> Path:
    p = Path(raw).resolve()
    if not p.is_dir():
        die(f"install prefix '{raw}' does not exist or is not a directory")
    return p


def preflight(prefix: Path) -> None:
    if not FIX8_BIN.is_file() or not os.access(FIX8_BIN, os.X_OK):
        die(f"f8test not found or not executable: {FIX8_BIN}")
    for name in ("witness", "arbiter", "sequencer",
                 "matching_engine", "fix_order_gateway",
                 "authentication_service"):
        exe = prefix / "bin" / name
        if not exe.is_file() or not os.access(exe, os.X_OK):
            die(f"binary not found or not executable: {exe}")


def file_end(path: Path) -> int:
    """Current EOF byte offset; 0 if the file does not exist."""
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


def poll_log_for(log_path: Path, *markers: str,
                 timeout: float, from_byte: int = 0) -> tuple[bool, float, int]:
    """
    Poll log_path for a line containing ALL of the given markers.
    Only bytes beyond from_byte are examined so stale content is skipped.
    Returns (found, elapsed_seconds, new_file_position).
    """
    deadline = time.monotonic() + timeout
    pos      = from_byte
    t0       = time.monotonic()
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as fh:
                fh.seek(pos)
                chunk = fh.read()
                pos   = fh.tell()
            for line in chunk.splitlines():
                if all(m in line for m in markers):
                    return True, time.monotonic() - t0, pos
        time.sleep(LOG_POLL_INTERVAL)
    return False, time.monotonic() - t0, pos


def me_cancel_on_failover_count(log_path: Path, from_byte: int = 0,
                                timeout: float = 10.0) -> int:
    """Return N from the ME's cancel-on-failover summary line, or -1 if absent.

    On promotion the ME cancels its whole (replicated) order book and logs
    "cancel-on-failover complete -- N cancel ER(s) sent, book cleared". N is the
    number of resting orders that were cancelled. Only bytes beyond from_byte are
    scanned (so we see this promotion, not a prior run). Polls up to `timeout`
    because the logger is asynchronous and the line may lag the event slightly.
    """
    marker = "cancel-on-failover complete"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as fh:
                fh.seek(from_byte)
                for line in fh.read().splitlines():
                    if marker in line:
                        try:
                            return int(line.split("complete -- ", 1)[1].split()[0])
                        except (IndexError, ValueError):
                            return -1
        time.sleep(LOG_POLL_INTERVAL)
    return -1


def provision_cancel_on_disconnect(comp_id: str, grace_period_seconds: int) -> None:
    """Set a comp id's cancel-on-disconnect grace period in the database.

    Uses psql for the same reason export_credentials.py does: the harness has no database
    driver dependency and is not the place to introduce one.  The credentials the auth
    service reads are exported straight after this, so the value takes the real path to the
    gateway rather than being injected further along it.
    """
    statement = (
        f"UPDATE pubsub_comp_id "
        f"SET cancel_on_disconnect_enabled = true, "
        f"    cancel_on_disconnect_grace_period_seconds = {grace_period_seconds} "
        f"WHERE comp_id = '{comp_id}'"
    )
    result = subprocess.run(
        ["psql", "--host", DB_HOST, "--port", DB_PORT,
         "--username", "pubsub_app", "--dbname", "pubsub",
         "--quiet", "--command", statement],
        capture_output=True, text=True, check=False,
        env={**os.environ, "PGPASSWORD": os.environ.get("PUBSUB_APP_DB_PASSWORD", "pubsub_dev")},
    )
    if result.returncode != 0:
        die(f"could not provision cancel-on-disconnect for '{comp_id}' "
            f"(is the database running and migrated?):\n{result.stderr.strip()}")
    log(f"  {comp_id}: cancel-on-disconnect grace period set to {grace_period_seconds}s")


def export_credentials(project_root: Path, creds_file: Path) -> None:
    """Regenerate credentials.toml from the database, then re-apply the fix8 credential.

    A function rather than inline setup because scenario 20 re-provisions a comp id
    mid-run and has to push the change down the same path the venue used at startup.
    """
    export_result = subprocess.run(
        [sys.executable, str(project_root / "db" / "export_credentials.py"),
         "--credentials-file", str(creds_file)],
        capture_output=True, text=True,
    )
    if export_result.returncode != 0:
        die(
            f"export_credentials.py failed (is the database running?):\n"
            f"{export_result.stderr.strip()}"
        )
    log(f"  credentials written to {creds_file}")
    ensure_fix8_credentials(creds_file, FIX8_COMP_ID, FIX8_PASSWORD)
    # The fresh-logon comp id is not in the database, so it needs a credential written the
    # same way. Written unconditionally: it costs one block in a file the auth service reads
    # once, and a scenario that needs it and finds it missing fails as an unexplained logon
    # timeout rather than as a missing credential.
    ensure_fix8_credentials(creds_file, FIX8_RECOVERY_COMP_ID, FIX8_PASSWORD)


def provision_gateway_pinning(comp_id: str, primary_instance: int,
                              backup_instance: int | None) -> None:
    """Pin a comp id to a primary gateway instance, and optionally a backup.

    Same psql route and the same reason as provision_cancel_on_disconnect: the values must
    travel the real path -- database, export_credentials, credentials.toml, auth service,
    AuthenticationResult -- so that a hop which drops them fails the scenario rather than
    being bypassed by injecting them further along.

    backup_instance of None writes SQL NULL, which pins the comp id to the primary alone.
    That is not the same as omitting the update: a previous run may have left a backup
    behind, and this scenario's second half depends on there being none.
    """
    backup_value = "NULL" if backup_instance is None else str(backup_instance)
    statement = (
        f"UPDATE pubsub_comp_id "
        f"SET primary_gateway_instance = {primary_instance}, "
        f"    backup_gateway_instance  = {backup_value} "
        f"WHERE comp_id = '{comp_id}'"
    )
    result = subprocess.run(
        ["psql", "--host", DB_HOST, "--port", DB_PORT,
         "--username", "pubsub_app", "--dbname", "pubsub",
         "--quiet", "--command", statement],
        capture_output=True, text=True, check=False,
        env={**os.environ, "PGPASSWORD": os.environ.get("PUBSUB_APP_DB_PASSWORD", "pubsub_dev")},
    )
    if result.returncode != 0:
        die(f"could not provision gateway instances for '{comp_id}' "
            f"(is the database running and migrated to v3?):\n{result.stderr.strip()}")
    log(f"  {comp_id}: provisioned for gateway instance {primary_instance}"
        f"{'' if backup_instance is None else f', backup {backup_instance}'}")


def unprovision_gateway_pinning(comp_id: str) -> None:
    """Clear a comp id's gateway pinning, leaving it free to log on to any instance.

    Called from teardown, so it warns rather than dying: raising here would replace the
    scenario's own verdict with a cleanup failure, and the thing that actually matters --
    that the next run does not inherit a comp id pinned somewhere it cannot reach -- is
    better served by a message naming the fix than by an exception.
    """
    statement = (
        f"UPDATE pubsub_comp_id "
        f"SET primary_gateway_instance = NULL, backup_gateway_instance = NULL "
        f"WHERE comp_id = '{comp_id}'"
    )
    result = subprocess.run(
        ["psql", "--host", DB_HOST, "--port", DB_PORT,
         "--username", "pubsub_app", "--dbname", "pubsub",
         "--quiet", "--command", statement],
        capture_output=True, text=True, check=False,
        env={**os.environ, "PGPASSWORD": os.environ.get("PUBSUB_APP_DB_PASSWORD", "pubsub_dev")},
    )
    if result.returncode != 0:
        log(f"  WARNING: could not unpin '{comp_id}' -- later scenarios will fail their "
            f"baseline logon until it is cleared by hand:\n{result.stderr.strip()}")
        return
    log(f"  {comp_id}: gateway pinning cleared")


def _resent_report_count(log_path: Path, from_byte: int = 0) -> int | None:
    """Reports named on the gateway's 'resend complete' line, or None if absent."""
    try:
        with log_path.open("r", errors="replace") as handle:
            handle.seek(from_byte)
            for line in handle:
                if "resend complete" in line:
                    match = re.search(r"resend complete -- (\d+) report\(s\) resent", line)
                    if match:
                        return int(match.group(1))
    except OSError:
        return None
    return None


def _client_logon_seq_num(client_output: Path) -> int | None:
    """MsgSeqNum of the Logon the CLIENT received, or None if it received none.

    Read from the client's own output because it is the member-observable fact: whether the
    venue resumed this session's numbering or silently restarted it at 1 is visible in the
    very first message the member is sent, and nowhere else from the member's side.
    """
    if not client_output.is_file():
        return None
    in_logon = False
    for line in client_output.read_text(errors="replace").splitlines():
        if "MsgType (35): A" in line:
            in_logon = True
        elif in_logon:
            match = re.search(r"MsgSeqNum \(34\): (\d+)", line)
            if match:
                return int(match.group(1))
    return None


def _client_report_counts(client_output: Path) -> tuple[int, int]:
    """(execution reports received, of which marked PossDupFlag=Y) from f8test's own output.

    f8test prints each received message with field names rather than raw tags, so this reads
    'MsgType (35): 8' and 'PossDupFlag (43): Y'. Counting the client's view rather than the
    gateway's is the point: whether a resent report was marked is a fact about what the
    member was handed.
    """
    if not client_output.is_file():
        return 0, 0
    text = client_output.read_text(errors="replace")
    reports = text.count("MsgType (35): 8")
    poss_dup = len(re.findall(r"PossDupFlag \(43\): [Yy]", text))
    return reports, poss_dup


def _held_grace_period_seconds(log_path: Path, from_byte: int = 0) -> int | None:
    """Seconds named on the gateway's cancel-on-disconnect hold line, or None."""
    try:
        with log_path.open("r", errors="replace") as handle:
            handle.seek(from_byte)
            text = handle.read()
    except FileNotFoundError:
        return None
    match = re.search(r"holding (\d+)s for reconnect before cancelling", text)
    return int(match.group(1)) if match else None


def gateway_progress_totals(log_path: Path) -> tuple[int | None, int]:
    """Return (sent, nos_received) from the last GW-PROGRESS line, or (None, 0).

    The gateway logs "GW-PROGRESS accounted=N sent=N dropped=N nos_received=N" once per
    1000 accounted reports, so the last line carries the run totals. Used to assert that
    execution reports were actually delivered, rather than merely not complained about.
    """
    try:
        text = log_path.read_text(errors="replace")
    except FileNotFoundError:
        return None, 0
    last = None
    for match in re.finditer(r"GW-PROGRESS accounted=(\d+) sent=(\d+) dropped=(\d+) nos_received=(\d+)", text):
        last = match
    if last is None:
        return None, 0
    return int(last.group(2)), int(last.group(4))


def count_log_marker(log_path: Path, marker: str, from_byte: int = 0) -> int:
    """Count lines containing `marker` in log_path beyond from_byte (0 if absent)."""
    if not log_path.is_file():
        return 0
    with open(log_path, "r", errors="replace") as fh:
        fh.seek(from_byte)
        return sum(1 for line in fh.read().splitlines() if marker in line)


def wait_for_fix_logon(gw_log: Path, from_byte: int,
                       timeout: float) -> str:
    """
    Poll the gateway log for the outcome of a FIX logon attempt.
    Returns 'ok', 'degraded', 'auth_failed', 'sig_mismatch', or 'timeout'.
    Only bytes beyond from_byte are examined so pre-existing content is skipped.

    'degraded' is a session that established only because the gateway stopped waiting for the
    sequencer to report its numbering. It is reported separately from 'ok' because the two are
    indistinguishable from the member's side at this instant and diverge later, when the
    member sees a sequence number below the one it expects and drops the session.
    """
    deadline = time.monotonic() + timeout
    pos = from_byte
    # Set on the warning, which the gateway emits just before opening the session, so it is
    # already known by the time the establishment line arrives.
    fell_back = False
    while time.monotonic() < deadline:
        if gw_log.is_file():
            with open(gw_log, "r", errors="replace") as fh:
                fh.seek(pos)
                chunk = fh.read()
                pos   = fh.tell()
            for line in chunk.splitlines():
                if _GW_SEQ_STATE_FALLBACK in line:
                    fell_back = True
                if _GW_LOGON_OK in line:
                    return "degraded" if fell_back else "ok"
                if _GW_LOGON_FAIL in line:
                    return "auth_failed"
                if _GW_SIG_MISMATCH in line:
                    return "sig_mismatch"
        time.sleep(LOG_POLL_INTERVAL)
    return "timeout"


def wait_for_me_ord(me_log: Path, target: int,
                    timeout: float, from_byte: int = 0) -> tuple[bool, float, int]:
    """
    Wait for ME-ORD-<target> (exact match, no trailing digit) in the ME log.
    Returns (found, elapsed_seconds, new_file_position).
    """
    pattern  = re.compile(re.escape(f"ME-ORD-{target}") + r"(?!\d)")
    deadline = time.monotonic() + timeout
    pos      = from_byte
    t0       = time.monotonic()
    while time.monotonic() < deadline:
        if me_log.is_file():
            with open(me_log, "r", errors="replace") as fh:
                fh.seek(pos)
                chunk = fh.read()
                pos   = fh.tell()
            if pattern.search(chunk):
                return True, time.monotonic() - t0, pos
        time.sleep(LOG_POLL_INTERVAL)
    return False, time.monotonic() - t0, pos


def find_last_me_ord(me_log: Path, from_byte: int = 0) -> int:
    """
    Scan me_log from from_byte and return the highest ME-ORD-n seen (0 if none).
    Used to find the actual ME order count after in-flight orders during Phase 4.
    """
    pattern = re.compile(r"ME-ORD-(\d+)(?!\d)")
    last_ord = 0
    try:
        with open(me_log, "r", errors="replace") as fh:
            fh.seek(from_byte)
            for line in fh:
                m = pattern.search(line)
                if m:
                    n = int(m.group(1))
                    if n > last_ord:
                        last_ord = n
    except FileNotFoundError:
        pass
    return last_ord


def check_me_seq_monotonic(me_log: Path) -> tuple[bool, list[tuple[int, int, int]]]:
    """
    Scan the ME log for 'NewOrderSingle seq=N' lines and verify seq numbers are
    monotonically increasing (gaps are permitted; resets are not).

    Returns (ok, violations) where violations is a list of
    (position, previous_seq, bad_seq) for each non-monotonic step.

    A reset means a seq number arrived that is <= the previous one, which
    indicates that a sequencer was promoted to leader but had not synced its
    next_sequence_number_ from the peer (WAL recovery bug).
    """
    pattern = re.compile(r"NewOrderSingle seq=(\d+)")
    seq_numbers: list[int] = []
    try:
        with open(me_log, "r", errors="replace") as fh:
            for line in fh:
                m = pattern.search(line)
                if m:
                    seq_numbers.append(int(m.group(1)))
    except FileNotFoundError:
        return True, []
    violations = [
        (i, seq_numbers[i - 1], seq_numbers[i])
        for i in range(1, len(seq_numbers))
        if seq_numbers[i] <= seq_numbers[i - 1]
    ]
    return len(violations) == 0, violations


def launch_app(name: str, bin_name: str, config: Path,
               bin_dir: Path, log_dir: Path) -> subprocess.Popen:
    if not config.is_file():
        die(f"config not found: {config}")
    # Run each process from its config directory (etc/<component>), matching
    # devenv's per-component workdir.  Config-relative paths — notably the
    # gateway's fix_gateway.crt — resolve against this dir.  Log and config are
    # passed as absolute paths, so they are unaffected by the cwd.
    with open(log_dir / f"{name}.stdout", "w") as stdout_fh:
        proc = subprocess.Popen(
            [str(bin_dir / bin_name), str(log_dir / f"{name}.log"), str(config)],
            cwd=str(config.parent),
            stdout=stdout_fh,
            stderr=subprocess.STDOUT,
        )
    log(f"  {name} — PID {proc.pid}")
    return proc


# Keys this helper owns and therefore replaces.  Anything else in an existing block --
# cancel_on_disconnect_enabled, cancel_on_disconnect_grace_period, and whatever provisioning
# fields come later -- is carried across untouched.
#
# Without this, rewriting the SCRAM material silently erased the per-comp-id provisioning
# that export_credentials.py had just written, so a test asserting on a provisioned value
# would watch the gateway fall back to its default and report a product failure that was
# really the harness overwriting its own fixture.
_SCRAM_KEYS = ("comp_id", "stored_key", "server_key", "salt", "iterations")


def _preserved_credential_lines(block: str) -> list[str]:
    """Lines from an existing credential block that this helper must not discard."""
    preserved = []
    for line in block.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        key = stripped.split("=", 1)[0].strip()
        if key and key not in _SCRAM_KEYS:
            preserved.append(line.rstrip() + "\n")
    return preserved


def ensure_fix8_credentials(creds_file: Path, comp_id: str, password: str) -> None:
    """Rewrite the SCRAM credential for comp_id in credentials.toml.

    Called after export_credentials.py so the fix8 test client always
    authenticates successfully regardless of what the database holds for the
    fix8 comp_id (f8test sends an empty password).  Mirrors the identical
    helper in perf_run.py.
    """
    salt = secrets.token_bytes(16)
    salted = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, 4096)
    client_key = _hmac.new(salted, b"Client Key", hashlib.sha256).digest()
    stored_key = hashlib.sha256(client_key).digest()
    server_key = _hmac.new(salted, b"Server Key", hashlib.sha256).digest()

    new_block = (
        f"[[credential]]\n"
        f"comp_id    = \"{comp_id}\"\n"
        f"stored_key = \"{stored_key.hex()}\"\n"
        f"server_key = \"{server_key.hex()}\"\n"
        f"salt       = \"{salt.hex()}\"\n"
        f"iterations = 4096\n"
    )

    existing = creds_file.read_text() if creds_file.is_file() else ""
    blocks = re.split(r"\[\[credential\]\]", existing)
    header = blocks[0]
    # Carry over any non-SCRAM keys this comp id already had, so rewriting the credential
    # does not quietly drop its provisioning.
    for block in blocks[1:]:
        if f'comp_id    = "{comp_id}"' in block or f'comp_id = "{comp_id}"' in block:
            new_block += "".join(_preserved_credential_lines(block))
            break
    kept = [b for b in blocks[1:]
            if f'comp_id    = "{comp_id}"' not in b and
               f'comp_id = "{comp_id}"' not in b]
    result = header + "".join(f"[[credential]]{b}" for b in kept) + new_block
    creds_file.write_text(result)
    log(f"  SCRAM credential for '{comp_id}' (empty password) written to {creds_file.name}")


def wait_for_accepted_count(me_log: Path, count: int,
                            timeout: float, from_byte: int = 0) -> tuple[bool, float, int]:
    """
    Wait until `count` live NOS acceptances ("accepted NOS OrderID=ME-ORD-")
    appear in me_log from from_byte.  Unlike wait_for_me_ord this counts
    occurrences rather than matching an absolute ME-ORD-N, so it is robust to
    the promoted ME's order_id_counter_ being advanced silently during WAL
    reconciliation (RECONCILING applies NOS to the book without logging an
    accept line).  Returns (found, elapsed_seconds, new_file_position).
    """
    needle    = "accepted NOS OrderID=ME-ORD-"
    deadline  = time.monotonic() + timeout
    pos       = from_byte
    seen      = 0
    t0        = time.monotonic()
    while time.monotonic() < deadline:
        if me_log.is_file():
            with open(me_log, "r", errors="replace") as fh:
                fh.seek(pos)
                chunk = fh.read()
                pos   = fh.tell()
            seen += chunk.count(needle)
            if seen >= count:
                return True, time.monotonic() - t0, pos
        time.sleep(LOG_POLL_INTERVAL)
    return False, time.monotonic() - t0, pos


def gateway_listen_port(prefix: Path, instance: str) -> int:
    """Read a FIX gateway instance's client listen port from its deployed configuration.

    Read rather than hardcoded, for the same reason perf_run.py reads it: a constant here
    drifts from the deployment silently, and the failure then looks like the instance being
    down rather than the test pointing at the wrong port.
    """
    config = prefix / "etc" / "fix_order_gateway" / f"fix_order_gateway_{instance}.toml"
    if not config.is_file():
        die(f"fix_order_gateway_{instance} config not found: {config}")
    for line in config.read_text().splitlines():
        match = re.match(r"\s*listen_port\s*=\s*(\d+)", line)
        if match:
            return int(match.group(1))
    die(f"no listen_port in {config}")
    return 0  # unreachable; die() exits


def write_fix8_variant(filename: str, *, listen_port: int | None = None,
                       keep_sequence_numbers: bool = False) -> str:
    """Generate an f8test config from the stock one with specific attributes rewritten.

    Generated rather than checked in because it must track the stock config: a divergence
    in host, comp id or target would make a scenario fail for reasons unrelated to what it
    tests. Written beside the original because f8test resolves the name relative to its own
    directory.

    keep_sequence_numbers turns OFF reset_sequence_numbers, so the client keeps its own
    numbering rather than telling the venue to forget the session's -- which is what makes
    it notice a gap and ask for what it missed.

    The session store is deliberately left in MEMORY, so a restarted client forgets its
    sequence numbers and expects to begin at 1.

    Persisting it to disk was tried and reverted, which is worth recording because it sounds
    like the more realistic choice. It is -- and it removes the very thing these scenarios
    test. A client that remembers, talking to a venue that also remembers, agrees with it:
    there is no gap, so no ResendRequest, so no replay, and the recovery machinery is never
    exercised. The case that matters here is a client that has lost its state reconnecting to
    a venue that has not -- which is what a process restart actually looks like, and the only
    arrangement in which the venue's memory does any work.
    """
    source = FIX8_DIR / FIX8_CFG
    if not source.is_file():
        die(f"fix8 session config not found: {source}")
    text = source.read_text()

    if keep_sequence_numbers:
        text, count = re.subn(r'reset_sequence_numbers="[^"]*"', 'reset_sequence_numbers="false"',
                              text, count=1)
        if count == 0:
            die(f"no reset_sequence_numbers attribute to rewrite in {source}")
    if listen_port is not None:
        text, count = re.subn(r'port="\d+"', f'port="{listen_port}"', text, count=1)
        if count == 0:
            die(f"no port attribute to rewrite in {source}")

    (FIX8_DIR / filename).write_text(text)
    return filename


def write_no_reset_fix8_config() -> None:
    """The resend scenario's client: instance a, not asking the venue to forget the session."""
    write_fix8_variant(FIX8_NO_RESET_CFG, keep_sequence_numbers=True)


def write_recovery_fix8_config() -> None:
    """Generate the f8test config for the fresh-logon comp id, beside the stock one.

    Identical to the stock config except for the SenderCompID, so it tracks any change to
    host, port or reset behaviour rather than drifting from it.
    """
    source = FIX8_DIR / FIX8_CFG
    if not source.is_file():
        die(f"fix8 session config not found: {source}")
    rewritten, count = re.subn(r'sender_comp_id="[^"]*"', f'sender_comp_id="{FIX8_RECOVERY_COMP_ID}"',
                               source.read_text(), count=1)
    if count == 0:
        die(f"no sender_comp_id attribute to rewrite in {source}")
    (FIX8_DIR / FIX8_RECOVERY_CFG).write_text(rewritten)


def send_burst(count: int, gw_log: Path, config: str = FIX8_CFG,
               client_output: Path | None = None) -> subprocess.Popen:
    """
    Launch one f8test session, wait for confirmed FIX logon, then send `count`
    T commands.  Each T command sends 1000 NOS.  Returns the Popen object.
    Dies immediately on authentication failure or timeout instead of hanging.

    client_output captures what the CLIENT received, which is the only place some things can
    be checked: whether a resent report carried PossDupFlag is visible to the member and to
    nobody else, the gateway's own record of it being below the deployed log level.
    """
    gw_pos = file_end(gw_log)
    stdout_target = client_output.open("w") if client_output is not None else subprocess.DEVNULL
    proc = subprocess.Popen(
        [str(FIX8_BIN), "-c", config, "-N", "GW1"],
        cwd=str(FIX8_DIR),
        stdin=subprocess.PIPE,
        stdout=stdout_target,
        stderr=subprocess.STDOUT,
    )
    log(f"  f8test PID {proc.pid}: waiting up to {FIX8_LOGON_WAIT:.0f}s for FIX logon ...")
    outcome = wait_for_fix_logon(gw_log, gw_pos, FIX8_LOGON_WAIT)
    if outcome == "auth_failed":
        stop_f8test(proc)
        die("FIX logon failed: authentication rejected — check credentials.toml has an entry for this comp_id")
    if outcome == "sig_mismatch":
        stop_f8test(proc)
        die("FIX logon failed: ServerSignature mismatch — auth service identity could not be verified")
    if outcome == "degraded":
        stop_f8test(proc)
        die("FIX logon completed only via the gateway's sequence-state fallback — the session was opened "
            "at a number the sequencer never confirmed. The logon raced the gateway's sequencer links "
            "and the retry on connect did not close the window; see docs/bug_list.md")
    if outcome == "timeout":
        stop_f8test(proc)
        die(f"FIX logon timed out after {FIX8_LOGON_WAIT:.0f}s — gateway or auth service may not be ready")
    for _ in range(count):
        proc.stdin.write(b"T\n")
    proc.stdin.flush()
    return proc


def stop_f8test(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.kill()
    proc.wait()


def shutdown_all(app_procs: list[tuple[str, subprocess.Popen]]) -> None:
    log("Shutting down all processes ...")
    for name, proc in app_procs:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
    for name, proc in app_procs:
        try:
            proc.wait(timeout=SHUTDOWN_TIMEOUT)
            log(f"  {name} exited")
        except subprocess.TimeoutExpired:
            log(f"  WARNING: {name} did not exit within {SHUTDOWN_TIMEOUT:.0f}s — SIGKILL")
            proc.kill()
            proc.wait()


def do_kill_step(
    step: KillStep,
    proc_by_name: dict[str, subprocess.Popen],
    log_dir: Path,
    failover_timeout: float,
) -> tuple[bool, str, float]:
    """
    Execute one KillStep: SIGKILL the target process, then either:
      - poll the secondary log for a role-to-leader transition (if expected), or
      - settle briefly without waiting for any transition.

    Returns (failover_occurred, label, elapsed_seconds).
      failover_occurred: True  → secondary became leader; label is its name.
                         False → no failover expected; label is the killed process.
      elapsed_seconds: failover time if failover_occurred, else 0.0.
    """
    proc = proc_by_name.get(step.proc_name)
    if proc is None or proc.poll() is not None:
        die(f"{step.proc_name} is not running — cannot kill")

    # Capture secondary log EOF before the kill so we skip startup content.
    secondary_log_pos = (
        file_end(log_dir / step.secondary_log_name)
        if step.secondary_log_name
        else 0
    )

    log(f"  SIGKILL → {step.proc_name} (PID {proc.pid})")
    proc.kill()
    proc.wait()
    log(f"  {step.proc_name} confirmed dead")

    if step.secondary_log_name is None:
        # No failover expected for this kill.
        log(f"  No failover expected; settling {step.settle_secs:.0f}s ...")
        time.sleep(step.settle_secs)
        return False, step.proc_name, 0.0

    # Failover expected: poll the secondary log for the leader role transition.
    secondary_name = (
        step.failover_to
        if step.failover_to
        else step.proc_name.replace("_primary", "_secondary")
    )
    secondary_log  = log_dir / step.secondary_log_name

    markers = step.leader_markers if step.leader_markers else (step.role_prefix, _TO_LEADER)
    log(
        f"  Watching {step.secondary_log_name} for "
        f"{' ... '.join(repr(m) for m in markers)} "
        f"(timeout {failover_timeout:.0f}s) ..."
    )
    found, elapsed, _ = poll_log_for(
        secondary_log, *markers,
        timeout=failover_timeout,
        from_byte=secondary_log_pos,
    )
    if not found:
        die(
            f"{secondary_name} did not become leader within "
            f"{failover_timeout:.0f}s"
        )

    log(f"  {secondary_name} is now leader ({elapsed:.1f}s after kill)")
    log(f"  Settling {step.settle_secs:.0f}s for connections to stabilise ...")
    time.sleep(step.settle_secs)
    return True, secondary_name, elapsed


def do_restart_step(
    step: RestartStep,
    proc_by_name: dict[str, subprocess.Popen],
    app_procs: list[tuple[str, subprocess.Popen]],
    launch_table: list,
    bin_dir: Path,
    log_dir: Path,
) -> float:
    """
    Kill the named process (if still running), delete its log so Quill starts
    fresh, relaunch it, and poll for readiness markers.

    Updates proc_by_name and app_procs in place.
    Returns elapsed seconds from relaunch to readiness.
    """
    proc = proc_by_name.get(step.proc_name)
    if proc is not None and proc.poll() is None:
        log(f"  SIGKILL → {step.proc_name} (PID {proc.pid})")
        proc.kill()
        proc.wait()
        log(f"  {step.proc_name} confirmed dead")

    # Delete the old log before restarting so Quill writes a fresh file
    # and we can safely read from byte 0.
    ready_log = log_dir / step.ready_log_name
    ready_log.unlink(missing_ok=True)

    launch_entry = next((e for e in launch_table if e[0] == step.proc_name), None)
    if launch_entry is None:
        die(f"No launch table entry for '{step.proc_name}'")
    _, bin_name, config = launch_entry

    log(f"  Restarting {step.proc_name} ...")
    new_proc = launch_app(step.proc_name, bin_name, config, bin_dir, log_dir)
    proc_by_name[step.proc_name] = new_proc
    for i, (name, _) in enumerate(app_procs):
        if name == step.proc_name:
            app_procs[i] = (step.proc_name, new_proc)
            break
    else:
        app_procs.append((step.proc_name, new_proc))

    markers_repr = " + ".join(repr(m) for m in step.ready_markers)
    log(
        f"  Waiting for {step.ready_log_name}: {markers_repr} "
        f"(timeout {step.ready_timeout:.0f}s) ..."
    )
    found, elapsed, _ = poll_log_for(
        ready_log, *step.ready_markers,
        timeout=step.ready_timeout,
        from_byte=0,
    )
    if not found:
        die(
            f"{step.proc_name} did not signal readiness within "
            f"{step.ready_timeout:.0f}s"
        )
    log(f"  {step.proc_name} ready ({elapsed:.1f}s after restart)")
    if step.settle_secs > 0:
        log(f"  Settling {step.settle_secs:.1f}s ...")
        time.sleep(step.settle_secs)
    return elapsed


# ── per-scenario runner ───────────────────────────────────────────────────────

def run_scenario(scenario: Scenario, args) -> bool:
    """
    Run one scenario end-to-end.  Returns True on PASS, False on FAIL.
    Always prints a RESULT: PASS / RESULT: FAIL summary block.
    """
    project_root = Path(__file__).resolve().parent.parent
    raw_prefix = args.prefix
    prefix = resolve_prefix(
        str(project_root / raw_prefix)
        if not Path(raw_prefix).is_absolute()
        else raw_prefix
    )

    bin_dir = prefix / "bin"
    etc_dir = prefix / "etc"
    log_dir = prefix / "log"

    # The ME-HA topology uses the _primary/_secondary config set: baseline
    # orders flow to matching_engine_primary; after the primary is killed the
    # promoted matching_engine_secondary becomes the active ME.  All other
    # scenarios keep the single-ME topology unchanged.
    if scenario.me_ha:
        me_log                 = log_dir / "matching_engine_primary.log"
        me_secondary_log       = log_dir / "matching_engine_secondary.log"
        gw_log                 = log_dir / "fix_order_gateway_a.log"
    else:
        me_log                 = log_dir / "matching_engine.log"
        me_secondary_log       = None
        gw_log                 = log_dir / "fix_order_gateway_a.log"
    seq_primary_log            = log_dir / "sequencer_primary.log"
    seq_secondary_log          = log_dir / "sequencer_secondary.log"
    arb_primary_log            = log_dir / "arbiter_primary.log"
    arb_secondary_log          = log_dir / "arbiter_secondary.log"
    auth_primary_log           = log_dir / "authentication_service_a.log"
    auth_secondary_log         = log_dir / "authentication_service_b.log"

    log_dir.mkdir(parents=True, exist_ok=True)

    # Delete stale log files so all polling begins at byte 0.  Processes
    # overwrite (not append) their logs on each start, so any pre-existing EOF
    # offset would skip past content written by the new run.
    gw_b_log                   = log_dir / "fix_order_gateway_b.log"

    stale_logs = [seq_primary_log, seq_secondary_log,
                  arb_primary_log, arb_secondary_log, me_log,
                  auth_primary_log, auth_secondary_log]
    if me_secondary_log is not None:
        stale_logs.append(me_secondary_log)
    if scenario.gateway_b:
        stale_logs.append(gw_b_log)
    for stale in stale_logs:
        stale.unlink(missing_ok=True)

    # ── header ────────────────────────────────────────────────────────────────
    def _step_label(s) -> str:
        if isinstance(s, KillStep):
            return s.proc_name + (" [failover expected]" if s.secondary_log_name else "")
        if isinstance(s, RestartStep):
            return f"RESTART:{s.proc_name}"
        if isinstance(s, InterimOrdersStep):
            return f"({s.count_batches * 1000} interim orders)"
        if isinstance(s, VerifyStep):
            return f"VERIFY:{s.description}"
        return str(s)

    effective_steps = scenario.extra_steps or (list(scenario.steps) + list(scenario.restart_steps))
    kill_seq = " → ".join(_step_label(s) for s in effective_steps) if effective_steps else "(none)"

    # Per-scenario override of the in-flight order count (None = use the CLI value).
    orders_during = (scenario.orders_during_override
                     if scenario.orders_during_override is not None
                     else args.orders_during)
    orders_after = (scenario.orders_after_override
                    if scenario.orders_after_override is not None
                    else args.orders_after)

    log("=" * 60)
    log(f"  ha_test  —  Scenario {scenario.number}: {scenario.description}")
    log("=" * 60)
    log(f"  install prefix   : {prefix}")
    log(f"  kill sequence    : {kill_seq}")
    log(f"  expected outcome : {scenario.expected_outcome}")
    log(f"  orders before    : {args.orders_before * 1000}")
    log(f"  orders during    : {orders_during * 1000}  (in flight during kill)")
    log(f"  orders after     : {orders_after * 1000}")
    log(f"  ready timeout    : {args.ready_timeout:.0f}s")
    log(f"  failover timeout : {args.failover_timeout:.0f}s  (per step)")
    log(f"  recovery timeout : {args.recovery_timeout:.0f}s")
    log("")

    # Process launch table (name, binary, config).
    # Authentication services start before the gateway so the gateway can connect
    # to them immediately on startup.  The ME-HA topology uses the
    # _primary/_secondary config set and adds a second matching engine so the
    # secondary can promote when the primary is killed.
    # Only FIX gateway instance a is launched. These scenarios fail over the sequencer,
    # matching engine and arbiter; more gateways add a process and its timing to every one
    # of them without exercising anything they test.
    #
    # dev configures four gateway endpoints (FIX 1 and 2, binary 1 and 2), so the sequencer
    # will retry the three that are not running and say so every
    # connect_retry_warning_interval. That is expected here rather than a symptom: nothing
    # gates sequencer readiness on a gateway connection (connect_to_service is
    # fire-and-retry), and this script does not scan logs for errors. perf_run.py is the one
    # that launches every instance.
    if scenario.me_ha:
        launch_table = [
            ("witness",                          "witness",
             etc_dir / "witness"                / "witness.toml"),
            ("arbiter_primary",                  "arbiter",
             etc_dir / "arbiter"                / "arbiter_primary.toml"),
            ("arbiter_secondary",                "arbiter",
             etc_dir / "arbiter"                / "arbiter_secondary.toml"),
            ("authentication_service_a",   "authentication_service",
             etc_dir / "authentication_service" / "authentication_service_a.toml"),
            ("authentication_service_b", "authentication_service",
             etc_dir / "authentication_service" / "authentication_service_b.toml"),
            ("fix_order_gateway_a",                  "fix_order_gateway",
             etc_dir / "fix_order_gateway"          / "fix_order_gateway_a.toml"),
            ("sequencer_primary",                "sequencer",
             etc_dir / "sequencer"              / "sequencer_primary.toml"),
            ("sequencer_secondary",              "sequencer",
             etc_dir / "sequencer"              / "sequencer_secondary.toml"),
            ("matching_engine_primary",          "matching_engine",
             etc_dir / "matching_engine"        / "matching_engine_primary.toml"),
            ("matching_engine_secondary",        "matching_engine",
             etc_dir / "matching_engine"        / "matching_engine_secondary.toml"),
        ]
    else:
        # Single-ME topology for the sequencer/arbiter/witness/ME-restart
        # scenarios.  Uses the current _primary config set (the pre-rename
        # arbiter.toml / sequencer.toml / matching_engine.toml are orphaned and
        # rejected by today's binaries).  Only one matching engine runs -- named
        # "matching_engine" (log matching_engine.log) so the existing scenario
        # references and the ME-restart steps are unchanged; its _primary config
        # harmlessly retries the (absent) book-replication link to a secondary.
        launch_table = [
            ("witness",                          "witness",
             etc_dir / "witness"                / "witness.toml"),
            ("arbiter_primary",                  "arbiter",
             etc_dir / "arbiter"                / "arbiter_primary.toml"),
            ("arbiter_secondary",                "arbiter",
             etc_dir / "arbiter"                / "arbiter_secondary.toml"),
            ("authentication_service_a",   "authentication_service",
             etc_dir / "authentication_service" / "authentication_service_a.toml"),
            ("authentication_service_b", "authentication_service",
             etc_dir / "authentication_service" / "authentication_service_b.toml"),
            ("fix_order_gateway_a",         "fix_order_gateway",
             etc_dir / "fix_order_gateway" / "fix_order_gateway_a.toml"),
            ("sequencer_primary",                "sequencer",
             etc_dir / "sequencer"              / "sequencer_primary.toml"),
            ("sequencer_secondary",              "sequencer",
             etc_dir / "sequencer"              / "sequencer_secondary.toml"),
            ("matching_engine",                  "matching_engine",
             etc_dir / "matching_engine"        / "matching_engine_primary.toml"),
        ]

    # Instance b goes in immediately after instance a, so the gateway pair starts
    # together and both are connected before the sequencer needs either. Appended
    # rather than written into both literals above: only one scenario wants it, and
    # duplicating it in each topology invites the two copies to drift.
    if scenario.gateway_b:
        gateway_a_index = next(index for index, entry in enumerate(launch_table)
                               if entry[0] == "fix_order_gateway_a")
        launch_table.insert(gateway_a_index + 1,
                            ("fix_order_gateway_b", "fix_order_gateway",
                             etc_dir / "fix_order_gateway" / "fix_order_gateway_b.toml"))

    app_procs:       list[tuple[str, subprocess.Popen]] = []
    proc_by_name:    dict[str, subprocess.Popen]        = {}
    f8proc:          subprocess.Popen | None             = None
    result_pass      = False
    kill_results:    list[tuple[bool, str, float]]      = []
    restart_results: list[tuple[str, float]]            = []
    phase4_results:  list                               = []
    before_total     = 0
    after_total      = 0
    running_me_total = 0
    running_me_pos   = 0

    try:
        # ── Pre-phase: export credentials ─────────────────────────────────────
        # Always regenerate credentials.toml from the database before starting
        # the authentication service so that DB-managed credentials (including
        # the CLIENT test fixture added via the 'test' Liquibase context) are
        # present.  Fail fast with a clear message if the DB is unreachable so
        # that a missing credential doesn't cause an opaque logon hang later.
        # Provision this scenario's per-comp-id cancel-on-disconnect BEFORE the export, so
        # the value travels the real path -- database, export_credentials, credentials.toml,
        # auth service, AuthenticationResult -- rather than being injected somewhere later.
        # Set here rather than assumed so the scenario is self-contained and re-runnable.
        if scenario.assert_cancel_grace:
            log("=== Provisioning cancel-on-disconnect for the test comp id ===")
            provision_cancel_on_disconnect(FIX8_COMP_ID, _PROVISIONED_GRACE_PERIOD_SECONDS)

        # Likewise for session provisioning: the comp id is pinned to the instance this
        # harness runs, so the baseline session is admitted and the gateway has real
        # numbers to name. The scenario then moves it elsewhere and requires a refusal.
        if scenario.assert_resend_recovery:
            write_no_reset_fix8_config()

        if scenario.assert_inflight_recovery:
            write_fix8_variant(_INFLIGHT_A_CFG, keep_sequence_numbers=True)

        if scenario.fresh_logon_in_recovery:
            write_recovery_fix8_config()

        if scenario.assert_session_provisioning:
            log("=== Provisioning gateway instances for the test comp id ===")
            provision_gateway_pinning(FIX8_COMP_ID, _PROVISIONED_PRIMARY_INSTANCE,
                                      _PROVISIONED_BACKUP_INSTANCE)

        log("=== Exporting credentials ===")
        creds_file = etc_dir / "authentication_service" / "credentials.toml"
        export_credentials(project_root, creds_file)
        log("")

        # ── Phase 1: start all processes ──────────────────────────────────────
        log("=== Phase 1: starting all processes ===")
        for name, bin_name, config in launch_table:
            log(f"  Starting {name} ...")
            proc = launch_app(name, bin_name, config, bin_dir, log_dir)
            app_procs.append((name, proc))
            proc_by_name[name] = proc
            time.sleep(STARTUP_DELAY)
        log("")

        for name, proc in app_procs:
            if proc.poll() is not None:
                die(
                    f"{name} (PID {proc.pid}) died during startup "
                    f"(exit code {proc.returncode})"
                )

        # ── Phase 2: wait for leader elections ────────────────────────────────
        log("=== Phase 2: waiting for leader election ===")

        log(
            f"  Polling sequencer_primary.log for leader election "
            f"(timeout {args.ready_timeout:.0f}s) ..."
        )
        found, elapsed, _ = poll_log_for(
            seq_primary_log, _SEQ_ROLE, _TO_LEADER,
            timeout=args.ready_timeout, from_byte=0,
        )
        if not found:
            die(
                f"sequencer_primary did not elect leader within "
                f"{args.ready_timeout:.0f}s — check ha_enabled in sequencer.toml"
            )
        log(f"  sequencer_primary: leader elected ({elapsed:.1f}s)")

        log("  Polling arbiter_primary.log for active role ...")
        found, elapsed, _ = poll_log_for(
            arb_primary_log, _ARB_ROLE, _TO_LEADER,
            timeout=10.0, from_byte=0,
        )
        if found:
            log(f"  arbiter_primary: active ({elapsed:.1f}s)")
        else:
            log(
                "  arbiter_primary: active marker not seen within 10s "
                "(election may have completed before log was captured — continuing)"
            )
        log("")

        # For the ME-HA topology, wait until the primary→secondary book
        # replication connection is established before proceeding.  The secondary
        # detects the primary's death only as the replication TCP EOF; killing the
        # primary before that connection is up (the primary retries every 2 s
        # against the secondary's listener) would leave the secondary unaware and
        # no promotion would ever fire.
        if scenario.me_ha:
            log("  Polling matching_engine_secondary.log for ME-primary replication connection ...")
            found, elapsed, _ = poll_log_for(
                me_secondary_log,
                "ME-primary replication connection", "established",
                timeout=args.ready_timeout, from_byte=0,
            )
            if not found:
                die(
                    "ME-primary → secondary replication connection not established "
                    f"within {args.ready_timeout:.0f}s — cannot test ME failover"
                )
            log(f"  ME replication connection established ({elapsed:.1f}s)")
            log("")

        # ── Phase 3: baseline orders + in-flight orders ───────────────────────
        before_total = args.orders_before * 1000
        log(f"=== Phase 3: {before_total} baseline orders ===")

        # The resend scenario needs a client that keeps its own sequence numbering; every
        # other scenario uses the stock one, which resets it on each logon.
        baseline_config = FIX8_CFG
        if scenario.assert_resend_recovery:
            baseline_config = FIX8_NO_RESET_CFG
        elif scenario.assert_inflight_recovery:
            baseline_config = _INFLIGHT_A_CFG
        f8proc = send_burst(args.orders_before, gw_log, baseline_config)
        log(f"  Waiting for ME-ORD-{before_total} ...")
        found, elapsed, me_pos = wait_for_me_ord(
            me_log, before_total, timeout=120.0, from_byte=0,
        )
        if not found:
            die("baseline orders did not complete — system is not healthy")
        log(f"  {before_total} baseline orders confirmed ({elapsed:.1f}s)")
        running_me_total = before_total
        running_me_pos   = me_pos

        # Send extra T commands WITHOUT waiting — these orders will be in flight
        # during Phase 4 so the kill happens while the system is under load.
        # Suppressed for extra_steps scenarios (e.g. WAL recovery) where
        # precise control of order counts between steps is required.
        if orders_during > 0 and not scenario.extra_steps:
            log(
                f"  Sending {orders_during * 1000} in-flight orders "
                f"(will span Phase 4 kill) ..."
            )
            for _ in range(orders_during):
                f8proc.stdin.write(b"T\n")
            f8proc.stdin.flush()
        log("")

        # ── Reconnect before the kill (step 5) ────────────────────────────────
        # The baseline orders are resting on the book. Take the connection that placed them
        # away and bring the same comp id back on a new one, so that everything Phase 4
        # produces concerns orders whose originating connection no longer exists.
        #
        # This is what makes the assertion afterwards mean something: the reports have to be
        # delivered to a member that has moved, which is only possible because the routing
        # entry is keyed on who the member is rather than on where it was.
        if scenario.reconnect_before_kill:
            log("=== Dropping the baseline session and reconnecting on a new connection ===")
            reconnect_pos = file_end(gw_log)
            stop_f8test(f8proc)
            f8proc = None
            # send_burst waits for the logon and dies on failure, so a fresh session that
            # cannot be established fails here rather than as a confusing shortfall later.
            f8proc = send_burst(0, gw_log)

            # The gateway must have told the sequencer, or the rest of this scenario tests
            # the old behaviour and would pass by accident.
            found, elapsed, _ = poll_log_for(
                gw_log, "announced session", "bound to instance",
                timeout=_PROVISIONING_LOGON_TIMEOUT, from_byte=reconnect_pos,
            )
            if not found:
                die("reconnect: the gateway did not announce the new session to the sequencer. "
                    "Without a SessionBound the sequencer still holds the old connection as this "
                    "session's destination, and every report for it will be dropped.")
            log(f"  reconnect: new session announced to the sequencer ({elapsed:.1f}s)")
            log("")

        # ── Orders in flight across the kill (scenario 23) ────────────────────
        # Fired immediately before Phase 4 and deliberately NOT waited for: the point is that
        # they are still somewhere in the pipeline -- socket, gateway, sequencer, matching
        # engine -- at the instant the gateway is killed.
        if scenario.assert_inflight_recovery:
            log(f"=== Sending {_INFLIGHT_BURSTS * 1000} orders and killing the gateway while they fly ===")
            for _ in range(_INFLIGHT_BURSTS):
                f8proc.stdin.write(b"T\n")
            f8proc.stdin.flush()

            # Wait until the flow is genuinely moving before letting Phase 4 kill the gateway.
            #
            # Writing to the client's stdin does not mean orders are on the wire: f8test has
            # to read the commands and start generating. Killing immediately after the flush
            # caught the pipeline empty, so nothing was in flight and the scenario asserted
            # nothing -- which the guard below reported rather than passing vacuously.
            #
            # Waiting for a fraction of the burst is deliberate: enough to prove the pipeline
            # is full, far short of the whole burst so the rest is still in it.
            target = before_total + _INFLIGHT_STARTED
            deadline = time.monotonic() + _INFLIGHT_START_TIMEOUT
            started = 0
            while time.monotonic() < deadline:
                started = count_log_marker(me_log, "accepted NOS OrderID=ME-ORD-")
                if started >= target:
                    break
                time.sleep(LOG_POLL_INTERVAL)
            if started < target:
                die(f"in-flight: only {started - before_total} of {_INFLIGHT_BURSTS * 1000} orders "
                    f"reached the venue within {_INFLIGHT_START_TIMEOUT:.0f}s, so the pipeline was "
                    "never full enough for the kill to catch anything mid-flight.")
            log(f"  {started - before_total} order(s) through and the rest in flight -- killing now")
            log("")

        # ── Phase 4: execute kill / restart scenario ──────────────────────────
        # f8proc stays alive so the FIX session remains established.
        # extra_steps (when present) replaces steps+restart_steps and may
        # include InterimOrdersStep entries that send orders mid-phase.
        #
        # seq_primary_pos_pre_kill is used after Phase 4 to wait for
        # sequencer_primary to re-establish its ME connection (see below).
        seq_primary_pos_pre_kill = file_end(seq_primary_log)

        # For the ME-HA cancel-on-failover assertion after Phase 5: remember where
        # the secondary ME log and the gateway log end before the kill, so we scan
        # only what the promotion (and its cancel-ER burst) produces.
        me_secondary_pos_pre_kill = file_end(me_secondary_log) if scenario.me_ha else 0
        gw_pos_pre_kill = file_end(gw_log)

        log(f"=== Phase 4: {scenario.description} ===")
        for step in effective_steps:
            if isinstance(step, KillStep):
                failover_occurred, label, elapsed = do_kill_step(
                    step, proc_by_name, log_dir, args.failover_timeout,
                )
                kill_results.append((failover_occurred, label, elapsed))
                phase4_results.append(("kill", failover_occurred, label, elapsed))
            elif isinstance(step, RestartStep):
                elapsed = do_restart_step(
                    step, proc_by_name, app_procs, launch_table, bin_dir, log_dir,
                )
                restart_results.append((step.proc_name, elapsed))
                phase4_results.append(("restart", step.proc_name, elapsed))
            elif isinstance(step, InterimOrdersStep):
                count = step.count_batches * 1000
                running_me_total += count
                log(f"  Sending {count} interim orders ...")
                for _ in range(step.count_batches):
                    f8proc.stdin.write(b"T\n")
                f8proc.stdin.flush()
                log(f"  Waiting for ME-ORD-{running_me_total} ...")
                found, elapsed, running_me_pos = wait_for_me_ord(
                    me_log, running_me_total,
                    timeout=args.recovery_timeout,
                    from_byte=running_me_pos,
                )
                if not found:
                    die(
                        f"interim orders did not appear within "
                        f"{args.recovery_timeout:.0f}s"
                    )
                log(f"  {count} interim orders confirmed ({elapsed:.1f}s)")
                phase4_results.append(("interim", count))
            elif isinstance(step, VerifyStep):
                markers_repr = " + ".join(repr(m) for m in step.markers)
                log(f"  VERIFY: {step.description}")
                log(f"    watching {step.log_name} for {markers_repr} (timeout {step.timeout:.0f}s) ...")
                found, elapsed, _ = poll_log_for(
                    log_dir / step.log_name, *step.markers,
                    timeout=step.timeout,
                    from_byte=step.from_byte,
                )
                if not found:
                    die(
                        f"Verification failed: {step.description} — "
                        f"marker not seen in {step.log_name} within {step.timeout:.0f}s"
                    )
                log(f"    confirmed ({elapsed:.1f}s)")

                if step.absent_markers:
                    absent_repr = " + ".join(repr(m) for m in step.absent_markers)
                    log(f"    checking {step.log_name} does NOT contain {absent_repr} ...")
                    # Zero timeout: this is a one-shot read of what is already there, not a
                    # wait. The positive markers above have been seen, so the behaviour under
                    # test has happened; anything the absent markers would match has had its
                    # chance to appear.
                    seen, _, _ = poll_log_for(
                        log_dir / step.log_name, *step.absent_markers,
                        timeout=0.0,
                        from_byte=step.from_byte,
                    )
                    if seen:
                        die(
                            f"Verification failed: {step.description} — "
                            f"{step.log_name} contains {absent_repr}, which must not be there"
                        )
                    log("    confirmed absent")

                phase4_results.append(("verify", step.description, elapsed))

        # If ME was restarted, the _ME_READY_MARKERS fire as soon as the first
        # sequencer (primary or secondary) re-establishes its order connection.
        # When the secondary connects first the primary may still be in its
        # retry window.  Wait explicitly for the primary's connection to avoid
        # Phase 5 orders arriving while the primary still has ME disconnected
        # (sequencer drops orders when me_outbound_order_conn_id_ is invalid).
        has_me_restart = any(
            isinstance(s, RestartStep) and s.resets_me_counter
            for s in effective_steps
        )
        if has_me_restart:
            log("  Waiting for sequencer_primary to re-establish ME connection ...")
            found, elapsed, _ = poll_log_for(
                seq_primary_log,
                "SequencerThread: matching engine order connection",
                "established",
                timeout=10.0,
                from_byte=seq_primary_pos_pre_kill,
            )
            if not found:
                die(
                    "sequencer_primary did not re-establish ME connection "
                    "within 10s after ME restart"
                )
            log(f"  sequencer_primary: ME connection restored ({elapsed:.1f}s)")
        log("")

        # ── Phase 5: recovery orders ──────────────────────────────────────────
        # Target calculation:
        #   ME restart   → ME log and counter reset; target = after_total,
        #                  scan from byte 0.
        #   orders_during> 0 → some in-flight orders may have been processed
        #                  (or lost) during Phase 4; scan from me_pos to find
        #                  the actual current ME-ORD count, then add after_total.
        #   otherwise    → simple cumulative: before_total + after_total.
        me_restarted = any(
            isinstance(s, RestartStep) and s.resets_me_counter
            for s in effective_steps
        )
        after_total  = orders_after * 1000

        if scenario.me_ha:
            # Recovery orders flow to the PROMOTED secondary ME.  Count live NOS
            # accepts on its log from the current EOF: WAL reconciliation logs no
            # accept line (it applies NOS to the book silently), so the count is
            # exactly the recovery orders regardless of how far the secondary's
            # order_id_counter_ was advanced during catch-up.
            recovery_log = me_secondary_log
            me_log_from  = file_end(me_secondary_log)
            count_based  = True
            after_target = after_total
        elif me_restarted:
            recovery_log = me_log
            count_based  = False
            after_target = after_total
            me_log_from  = 0
        elif orders_during > 0 or scenario.extra_steps:
            recovery_log = me_log
            count_based  = False
            # If all during-orders were discarded by the follower (correct HA
            # behaviour), find_last_me_ord returns 0.  Fall back to
            # running_me_total so the target stays ahead of running_me_pos.
            current_me_ord = find_last_me_ord(me_log, from_byte=running_me_pos) or running_me_total
            after_target   = current_me_ord + after_total
            me_log_from    = running_me_pos
        else:
            recovery_log = me_log
            count_based  = False
            after_target = running_me_total + after_total
            me_log_from  = running_me_pos

        # A scenario that kills the gateway its FIX session runs on has no session
        # left to send through, so there are no recovery orders to send or wait for.
        # Skipping is not a weaker test here: what that scenario asserts happens in
        # Phase 4 and is checked below.
        if after_total == 0:
            log("=== Phase 5: skipped — no recovery orders for this scenario ===")
            log("")
        else:
            target_desc = (f"{after_total} accepted NOS on {recovery_log.name}"
                           if count_based else f"ME-ORD target: {after_target}")
            log(f"=== Phase 5: {after_total} recovery orders ({target_desc}) ===")

            # When ME was restarted while orders were in flight (scenario 10-style),
            # the old f8test session is blocked waiting for ERs from the orders the
            # sequencer dropped while ME was disconnected.  Those ERs will never
            # arrive, so Phase 5 T commands written to the old stdin would just queue
            # behind them.  Start a fresh FIX session to unblock.
            if me_restarted and orders_during > 0 and not scenario.extra_steps:
                log("  Restarting FIX session (old session blocked on dropped in-flight orders) ...")
                stop_f8test(f8proc)
                f8proc = send_burst(0, gw_log)

            # Auth-failover scenarios: an established session does not re-authenticate,
            # so force a fresh logon. With the preferred auth instance dead, send_burst
            # only completes if the surviving auth instance authenticates the logon.
            if scenario.fresh_logon_in_recovery:
                log("  Opening a fresh FIX session (fresh logon must authenticate via the surviving auth) ...")
                stop_f8test(f8proc)
                # Under its own comp id: see FIX8_RECOVERY_COMP_ID. The point of this step is
                # that a fresh logon is authenticated by the surviving auth instance, and that
                # holds whichever identity logs on -- whereas re-using the baseline comp id
                # makes every recovery order a duplicate of one still resting on the book.
                f8proc = send_burst(0, gw_log, FIX8_RECOVERY_CFG)

            log(f"  Sending {after_total} recovery orders ...")
            for _ in range(orders_after):
                f8proc.stdin.write(b"T\n")
            f8proc.stdin.flush()

            if count_based:
                log(f"  Waiting for {after_total} accepted NOS in {recovery_log.name} ...")
                found, elapsed, _ = wait_for_accepted_count(
                    recovery_log, after_total,
                    timeout=args.recovery_timeout,
                    from_byte=me_log_from,
                )
            else:
                log(f"  Waiting for ME-ORD-{after_target} ...")
                found, elapsed, _ = wait_for_me_ord(
                    recovery_log, after_target,
                    timeout=args.recovery_timeout,
                    from_byte=me_log_from,
                )
            if not found:
                die(
                    f"recovery orders did not appear within {args.recovery_timeout:.0f}s"
                )
            log(f"  {after_total} recovery orders confirmed ({elapsed:.1f}s)")
            ok, violations = check_me_seq_monotonic(recovery_log)
            if not ok:
                for idx, prev_seq, bad_seq in violations[:3]:
                    log(f"  seq non-monotonic at position {idx}: {prev_seq} -> {bad_seq}")
                die(
                    f"ME log seq numbers not monotonically increasing "
                    f"({len(violations)} violation(s)) — "
                    "WAL recovery or peer seq-number sync failure suspected"
                )
            log("  seq monotonicity check: OK")

        # ── ME-HA: cancel-on-failover ER routing ──────────────────────────────
        # The matching engine is a stub with NO matching logic: every accepted
        # NewOrderSingle is added to the book as OrdStatus=New and rests there until
        # cancelled (see MatchingEngineThread::handle_new_order_single). So at the
        # ME-primary kill the promoted secondary's replicated book is non-empty, and
        # on promotion it cancels the whole book, emitting one cancel ExecutionReport
        # per resting order with seq_no=0.
        #
        # Those cancel ERs are NOT tied to a sequenced order, so the sequencer cannot
        # route them via its seq_no->conn map; the originating session's connection id
        # rides on the WalRecord envelope instead (see commit 5cb18a6 and
        # docs/design/fix_pdu_generation.md). A regression there is silent to the
        # recovery-order check -- the cancels are simply dropped -- so assert directly:
        #   * the secondary cancelled a non-empty book (N > 0), and
        #   * the gateway dropped NO ER for a missing conn id (the regression's
        #     signature is the gateway log line
        #     "... has no gateway_session_conn_id -- dropping").
        #
        # The gateway-death scenario reuses the same cancel burst but asserts the
        # opposite outcome, so it takes the branch below instead of this one.
        if scenario.me_ha and not scenario.assert_gateway_orphaned:
            cancel_count = me_cancel_on_failover_count(me_secondary_log, from_byte=me_secondary_pos_pre_kill)
            if cancel_count <= 0:
                die("ME-HA: promoted secondary did not cancel a non-empty book on failover "
                    f"(cancel-on-failover count={cancel_count}) -- cancel-ER routing was not exercised")
            log(f"  ME-HA: cancel-on-failover cancelled {cancel_count} resting order(s)")

            dropped = count_log_marker(gw_log, "has no gateway_session_conn_id -- dropping", from_byte=gw_pos_pre_kill)
            if dropped > 0:
                die(f"ME-HA: gateway dropped {dropped} ExecutionReport(s) for a missing "
                    "gateway_session_conn_id -- cancel-on-failover ER routing via the envelope is broken")

            # "None dropped" is necessary but nowhere near sufficient, and on its own it
            # passed for a long time while the cancel ERs were never delivered at all --
            # a report that never reaches the gateway cannot be dropped by it. So assert
            # delivery positively: the gateway must have sent MORE reports than it
            # received orders, and the excess is the cancel burst.
            sent, nos_received = gateway_progress_totals(gw_log)
            if sent is None:
                die("ME-HA: no GW-PROGRESS line in the gateway log -- cannot confirm the "
                    "cancel-on-failover ERs were delivered")
            if sent < nos_received + cancel_count:
                # Two different defects produce this same shortfall, so name both. The
                # second only applies to the scenario that reconnects first, but a reader
                # staring at a failure should not have to know which scenario they are in.
                die(f"ME-HA: gateway sent {sent} ER(s) for {nos_received} order(s), but "
                    f"{cancel_count} cancel-on-failover ER(s) were emitted on promotion. "
                    f"Expected at least {nos_received + cancel_count} -- the cancel reports "
                    "did not reach the client. Two causes to check, in this order: "
                    "(1) the WalAck gate in SequencerThread::on_pdu -- cancel ERs carry seq_no 0 "
                    "and must gate on their own WAL record, not on an order sequence that does "
                    "not exist; (2) session binding -- the sequencer resolves a report's "
                    "destination from the session's CURRENT binding, so if SessionBound/"
                    "SessionUnbound stopped working the reports are addressed at the connection "
                    "that placed the orders, which in this scenario is deliberately gone.")
            log(f"  ME-HA: gateway delivered {sent} ER(s) for {nos_received} order(s) -- "
                f"includes the {cancel_count} cancel-on-failover ER(s) -- OK")

        # ── Gateway orphan: no session handover between instances ─────────────
        # Asserts a GAP rather than a guarantee. Instance a is dead and the promoted
        # ME has just cancelled its resting orders, so there is a burst of execution
        # reports addressed to a gateway instance that no longer exists.
        #
        # Three things must hold for the test to mean anything:
        #   1. the cancel burst actually happened, or the rest proves nothing;
        #   2. the sequencer dropped those reports, naming the dead instance -- it
        #      does NOT reroute them to the surviving instance of the same protocol;
        #   3. instance b saw none of it, i.e. it inherited nothing from a.
        #
        # Assertion 3 is the load-bearing one. Were b ever to pick up a's sessions,
        # the third check fails first and loudest.
        #
        # WHEN SESSION HANDOVER LANDS (steps 3b-6 of docs/design/gateway_ha.md) THIS
        # BLOCK SHOULD FAIL. Invert it rather than deleting it: dropped_ers becomes 0
        # and b's traffic becomes non-zero.
        if scenario.assert_gateway_orphaned:
            cancel_count = me_cancel_on_failover_count(me_secondary_log, from_byte=me_secondary_pos_pre_kill)
            if cancel_count <= 0:
                die("gateway orphan: promoted secondary did not cancel a non-empty book "
                    f"(count={cancel_count}) -- no execution reports were generated for the "
                    "dead gateway, so this scenario asserted nothing")
            log(f"  gateway orphan: {cancel_count} resting order(s) cancelled by the promoted ME")

            # Wait for the burst to reach the sequencer rather than sampling straight
            # away. Every other me_ha scenario gets this delay for free from Phase 5's
            # recovery orders; this one skips Phase 5, and without the wait it sampled
            # about two seconds after promotion and saw nothing.
            drop_marker = "gateway protocol=1 instance=1 not connected -- dropping ER"
            log("  gateway orphan: waiting for the cancel ERs to reach the sequencer ...")
            found, elapsed, _ = poll_log_for(
                seq_primary_log, drop_marker,
                timeout=args.failover_timeout,
                from_byte=seq_primary_pos_pre_kill,
            )
            if not found:
                die("gateway orphan: the sequencer never reported dropping an execution report "
                    "for the dead FIX instance 1. Either the reports went somewhere they should "
                    "not have, or session handover now exists -- if the latter, invert this "
                    "scenario's assertions rather than removing them. See docs/design/gateway_ha.md.")
            dropped_ers = count_log_marker(seq_primary_log, drop_marker, from_byte=seq_primary_pos_pre_kill)
            log(f"  gateway orphan: sequencer dropped {dropped_ers} ER(s) bound for the dead "
                f"instance 1, first after {elapsed:.1f}s -- not rerouted to b")

            # The load-bearing assertion: b was alive throughout, and the question is
            # only whether anything reached it. GW-PROGRESS is the gateway's own
            # per-1000-report marker, so any non-zero count means b handled traffic for
            # sessions it never owned.
            b_progress = count_log_marker(gw_b_log, "GW-PROGRESS")
            if b_progress > 0:
                die(f"gateway orphan: instance b logged {b_progress} GW-PROGRESS line(s). It was "
                    "sent no orders of its own, so either instance a's reports were rerouted to it "
                    "-- which nothing implements -- or session handover now exists and this "
                    "scenario's assertions need inverting, not removing.")
            log("  gateway orphan: instance b inherited nothing from a (no handover exists) -- OK")

            # The member is never told: the orders were cancelled in the book, and the
            # reports saying so had nowhere to go.
            log(f"  gateway orphan: {cancel_count} order(s) cancelled in the book, "
                "0 cancel reports delivered to any client -- orders silently orphaned")

        # ── Cancel-on-disconnect grace period (step 3b) ───────────────────────
        # The gateway is alive throughout; only the client session goes away. Everything
        # here reads the gateway's own log, because the behaviour under test is entirely
        # the gateway's: what it does with a dead session's resting orders, and when.
        if scenario.assert_cancel_grace:
            gw_pos = file_end(gw_log)

            log("=== Dropping the client session (gateway stays up) ===")
            stop_f8test(f8proc)
            f8proc = None

            # 1. It must say it is holding, not cancelling. This is the line that
            #    distinguishes the new behaviour from the old one.
            found, elapsed, _ = poll_log_for(
                gw_log, "disconnected with", "before cancelling",
                timeout=_CANCEL_GRACE_HOLD_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("cancel grace: the gateway did not report holding the dropped session's orders. "
                    "Either cancel_on_disconnect.grace_period is 0 in the deployed config, or the "
                    "session's orders were cancelled immediately -- check the gateway log for "
                    "'queuing cancels now'.")
            log(f"  cancel grace: gateway is holding the orders for reconnect ({elapsed:.1f}s)")

            # The window actually used must be this comp id's provisioned value, not the
            # gateway's default. Asserting the number rather than merely "it held" is what
            # makes this cover the whole database -> export -> auth service -> gateway
            # chain: every hop that drops the value leaves the gateway on its default, and
            # the difference is invisible unless the number is checked.
            #
            # The harness rewrites this comp id's SCRAM material before each run, and used
            # to discard the provisioning alongside it -- which looked exactly like the
            # gateway ignoring the setting. write_scram_credential now preserves non-SCRAM
            # keys; this assertion is what would catch it happening again.
            held = _held_grace_period_seconds(gw_log, gw_pos)
            if held is None:
                die("cancel grace: could not read the grace period from the hold line")
            if held != _PROVISIONED_GRACE_PERIOD_SECONDS:
                die(f"cancel grace: the gateway held for {held}s, but comp id "
                    f"'{FIX8_COMP_ID}' is provisioned for {_PROVISIONED_GRACE_PERIOD_SECONDS}s. "
                    "The per-comp-id value did not survive the trip from the database. Check, in "
                    "order: the comp_id row, credentials.toml after export, that the harness did "
                    "not overwrite it, and that AuthenticationResult carried the field.")
            log(f"  cancel grace: held for the provisioned {held}s, not the gateway default -- OK")

            # 2. Nothing may actually be cancelled while the window is open. The marker is
            #    the drain's own completion line, which only appears once cancels are sent.
            #    A sleep is the honest test here: the assertion is that something does NOT
            #    happen, and there is no event to wait for.
            time.sleep(_CANCEL_GRACE_QUIET_PERIOD)
            drained = count_log_marker(gw_log, "cancel drain complete", from_byte=gw_pos)
            if drained > 0:
                die(f"cancel grace: the gateway completed {drained} cancel drain(s) "
                    f"within {_CANCEL_GRACE_QUIET_PERIOD:.0f}s of the disconnect. The grace period "
                    "is not being honoured -- a member whose connection blipped would come back flat.")
            log(f"  cancel grace: nothing cancelled {_CANCEL_GRACE_QUIET_PERIOD:.0f}s after the drop -- OK")

            # 3. The point of the whole feature: the same comp id comes back inside the
            #    window and its orders are released without a cancel ever being sent.
            log("=== Reconnecting the same comp id inside the grace window ===")
            f8proc = send_burst(0, gw_log)
            found, elapsed, _ = poll_log_for(
                gw_log, "reconnected within the grace period", "none cancelled",
                timeout=_CANCEL_GRACE_HOLD_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("cancel grace: the reconnecting comp id did not reclaim its held orders. "
                    "The grace entry is matched on comp id -- check the reconnecting session "
                    "authenticated with the same one.")
            log(f"  cancel grace: orders reclaimed on reconnect, none cancelled ({elapsed:.1f}s)")

            drained = count_log_marker(gw_log, "cancel drain complete", from_byte=gw_pos)
            if drained > 0:
                die(f"cancel grace: {drained} cancel drain(s) ran despite the reconnect -- "
                    "the member's book was flattened anyway.")
            log("  cancel grace: no cancel was sent at any point in this scenario -- OK")

        # ── Sequence continuity across a reconnect (step 6) ───────────────────
        # The member goes away cleanly and comes back to the SAME instance. The venue
        # remembered where its numbering had reached and resumes there, so the member is not
        # silently restarted as a new session.
        #
        # This is the clean counterpart to scenario 23: there the gateway was killed and the
        # position had to be reconstructed and biased high, whereas here the session unbound
        # properly and the figure is exact.
        if scenario.assert_resend_recovery:
            gw_pos = file_end(gw_log)
            client_output = log_dir / "f8test_resend_client.txt"

            log("=== Dropping the session and reconnecting a client that keeps its sequence numbers ===")
            stop_f8test(f8proc)
            f8proc = None
            f8proc = send_burst(0, gw_log, FIX8_NO_RESET_CFG, client_output)

            found, elapsed, _ = poll_log_for(
                gw_log, "resuming the venue's sequence state", "outbound=",
                timeout=_PROVISIONING_LOGON_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("resend: the gateway did not resume the session's numbering. The venue "
                    "remembers it at SessionUnbound and hands it back on SessionBoundAck -- check "
                    "that the unbind carried a number and that the sequencer replied.")
            log(f"  resend: the venue resumed the session's numbering ({elapsed:.1f}s)")

            time.sleep(_RESEND_CLIENT_SETTLE)
            logon_seq = _client_logon_seq_num(client_output)
            if logon_seq is None:
                die(f"resend: the member received no Logon (see {client_output}).")
            if logon_seq <= 1:
                die(f"resend: the member was sent Logon MsgSeqNum={logon_seq}. Its numbering was "
                    "restarted rather than continued, which is the break a reconnect is supposed "
                    "to spare it.")
            log(f"  resend: the member's Logon carried MsgSeqNum={logon_seq}, continuing the "
                "session rather than restarting it -- OK")

        # ── Session provisioning (step 4) ─────────────────────────────────────
        # Nothing is killed. Everything here reads the gateway's own log, because the
        # decision under test is the gateway's alone: whether this session belongs on this
        # instance, taken once, at the moment authentication succeeds.
        if scenario.assert_session_provisioning:

            # 1. The accepted path, asserted on the NUMBERS rather than on the session
            #    having opened. A gateway that ignored provisioning entirely would open the
            #    session too, so "it logged on" discriminates nothing; the numbers only
            #    appear if the pinning survived the database -> export -> auth service ->
            #    AuthenticationResult chain intact. The baseline session opened back in
            #    Phase 3, so this reads from the start of the log.
            accepted_marker = (
                f"provisioned for gateway instances primary={_PROVISIONED_PRIMARY_INSTANCE} "
                f"backup={_PROVISIONED_BACKUP_INSTANCE} -- this is instance "
                f"{_PROVISIONED_PRIMARY_INSTANCE} (primary)"
            )
            if count_log_marker(gw_log, accepted_marker) == 0:
                die("provisioning: the gateway never named this comp id's provisioned instances. "
                    f"It should have logged '{accepted_marker}'. Either the values did not reach "
                    "it -- check, in order: the comp_id row, credentials.toml after export, that "
                    "the harness did not overwrite it, and that AuthenticationResult carried the "
                    "fields -- or the gateway is admitting everyone without looking.")
            log(f"  provisioning: gateway named primary={_PROVISIONED_PRIMARY_INSTANCE} "
                f"backup={_PROVISIONED_BACKUP_INSTANCE} and admitted the session as the primary -- OK")

            # 2. Move the comp id to an instance this gateway is not, and push it down the
            #    same path the venue used at startup. The auth service reads credentials at
            #    startup, so it is restarted rather than signalled: there is no live update
            #    path for provisioning, and inventing one for the test would prove something
            #    the venue does not do.
            log("=== Re-provisioning the comp id onto a different gateway instance ===")
            provision_gateway_pinning(FIX8_COMP_ID, _ELSEWHERE_PRIMARY_INSTANCE, None)
            export_credentials(project_root, etc_dir / "authentication_service" / "credentials.toml")

            # Taken before the restart, not after: the gateway can reconnect faster than the
            # next statement runs, and a position sampled afterwards would skip the very line
            # being waited for. Nothing earlier in the log can match, because the connection
            # this looks for is the one the restart is about to break.
            auth_restart_pos = file_end(gw_log)

            for auth_name in ("authentication_service_a", "authentication_service_b"):
                auth_proc = proc_by_name.get(auth_name)
                if auth_proc is None:
                    continue
                if auth_proc.poll() is None:
                    auth_proc.send_signal(signal.SIGTERM)
                    auth_proc.wait(timeout=SHUTDOWN_TIMEOUT)
                config = next(entry[2] for entry in launch_table if entry[0] == auth_name)
                restarted = launch_app(auth_name, "authentication_service", config, bin_dir, log_dir)
                proc_by_name[auth_name] = restarted
                app_procs = [(name, restarted if name == auth_name else proc)
                             for name, proc in app_procs]

            # The gateway reconnects to the restarted service on its own retry timer. Waiting
            # for that is not politeness: a logon attempted before it reconnects is refused
            # for having no authentication service at all, which is a different refusal and
            # would let this scenario pass without testing anything.
            found, elapsed, _ = poll_log_for(
                gw_log, "authentication service connection", "established",
                timeout=args.failover_timeout, from_byte=auth_restart_pos,
            )
            if not found:
                die("provisioning: the gateway did not reconnect to the restarted authentication "
                    f"service within {args.failover_timeout:.0f}s, so the next logon would be "
                    "refused for the wrong reason.")
            log(f"  provisioning: gateway reconnected to the authentication service ({elapsed:.1f}s)")

            # 3. The same comp id, the same gateway, now provisioned elsewhere: refused.
            log("=== Reconnecting the comp id now provisioned for another instance ===")
            stop_f8test(f8proc)
            f8proc = None
            gw_pos = file_end(gw_log)
            f8proc = subprocess.Popen(
                [str(FIX8_BIN), "-c", FIX8_CFG, "-N", "GW1"],
                cwd=str(FIX8_DIR),
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            refused_marker = (
                f"logon refused -- this is gateway instance {_PROVISIONED_PRIMARY_INSTANCE}, "
                f"and the session is provisioned for primary={_ELSEWHERE_PRIMARY_INSTANCE} "
                f"backup=(none)"
            )
            found, elapsed, _ = poll_log_for(
                gw_log, refused_marker,
                timeout=_PROVISIONING_LOGON_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("provisioning: the gateway did not refuse a comp id provisioned for another "
                    f"instance. It should have logged '{refused_marker}'. A session that can log "
                    "on anywhere makes the pinning a convention rather than a rule, and the "
                    "recovery guarantees in docs/design/gateway_ha.md rest on it being a rule.")
            log(f"  provisioning: logon refused at the wrong instance ({elapsed:.1f}s)")

            # And refused means refused: no session may have been established alongside it.
            established = count_log_marker(gw_log, _GW_LOGON_OK, from_byte=gw_pos)
            if established > 0:
                die(f"provisioning: the gateway refused the logon but also established "
                    f"{established} session(s) for it. A refusal that leaves a working session "
                    "behind is not a refusal.")
            log("  provisioning: no session was established at the wrong instance -- OK")

        # ── In-flight orders across a gateway death (scenario 23) ─────────────
        # Instance a is dead with orders mid-pipeline. What follows is about what the venue
        # guarantees the member on its return -- deliberately NOT about what this particular
        # test client then does with it; see the note on the resend leg below.
        if scenario.assert_inflight_recovery:
            # 1. Orders really did reach the venue. Without this the rest proves nothing: a
            #    run where every in-flight order died in the socket would "recover" nothing
            #    and look identical to a broken venue.
            accepted = count_log_marker(me_log, "accepted NOS OrderID=ME-ORD-")
            if accepted <= before_total:
                die(f"in-flight: the venue accepted {accepted} order(s), no more than the "
                    f"{before_total} baseline -- nothing was actually in flight when the gateway "
                    "died, so this scenario would assert nothing. Raise _INFLIGHT_BURSTS.")
            inflight_accepted = accepted - before_total
            log(f"  in-flight: {inflight_accepted} of the in-flight order(s) reached the venue")

            # 2. Nothing cancelled them. The process that would have sent the cancels is the
            #    one that died, so the member's book survives its gateway -- which is the
            #    whole point of a gateway failure not being a member failure.
            cancels = count_log_marker(me_log, "ExecType=Canceled")
            if cancels > 0:
                die(f"in-flight: {cancels} order(s) were cancelled when the gateway died. A "
                    "member's book must survive the loss of the process serving it.")
            log(f"  in-flight: none cancelled -- {inflight_accepted} order(s) still live on the book")

            # 3. The member returns on the OTHER instance.
            log("=== Reconnecting the member on instance b ===")
            stop_f8test(f8proc)
            f8proc = None
            gw_b_pos = file_end(gw_b_log)
            client_output = log_dir / "f8test_inflight_client.txt"
            instance_b_config = write_fix8_variant(
                _INFLIGHT_CFG,
                listen_port=gateway_listen_port(prefix, "b"),
                keep_sequence_numbers=True,
            )
            f8proc = send_burst(0, gw_b_log, instance_b_config, client_output)

            # 4. The guarantee. The venue resumes this session's numbering ahead of where it
            #    had reached, rather than silently restarting it at 1.
            #
            #    Restarting at 1 is what happened before, and it was the worst outcome
            #    available: the member was resynchronised as though it were a brand new
            #    session, told nothing, and left with thousands of live orders it had never
            #    heard of. The resumed number is what makes the loss VISIBLE to it.
            found, elapsed, _ = poll_log_for(
                gw_b_log, "resuming the venue's sequence state", "outbound=",
                timeout=_RESEND_TIMEOUT, from_byte=gw_b_pos,
            )
            if not found:
                die("in-flight: instance b did not resume this session's numbering. The sequencer "
                    "must hand it back on SessionBoundAck, biased HIGH after an unclean death -- "
                    "check the sequencer logged 'previous gateway died without reporting'.")
            log(f"  in-flight: instance b resumed the session's numbering ({elapsed:.1f}s)")

            # 5. And the member can see it. Read from the client's own output because this is
            #    the member-observable fact: the very first message it is sent either carries
            #    a resumed number or a 1, and nothing else on its side distinguishes them.
            time.sleep(_RESEND_CLIENT_SETTLE)
            logon_seq = _client_logon_seq_num(client_output)
            if logon_seq is None:
                die(f"in-flight: the member received no Logon on instance b (see {client_output}).")
            if logon_seq <= 1:
                die(f"in-flight: the member was sent Logon MsgSeqNum={logon_seq}. The venue has "
                    "silently resynchronised it as a new session while its orders are live on the "
                    "book -- the exact failure this scenario exists to catch.")
            log(f"  in-flight: the member's Logon carried MsgSeqNum={logon_seq}, not 1 -- the venue "
                "resumed the session rather than resetting it -- OK")

            # The resend leg is deliberately NOT asserted here, and the reason is a limitation
            # of the harness client rather than of the venue. With the venue correctly numbering
            # its Logon at the resumed position, the gap the member must recover from appears on
            # the LOGON itself; f8test terminates the session in that case instead of issuing a
            # ResendRequest, which the FIX standard says it should. Replay itself is exercised by
            # the sequencer's own path -- see the SessionReplayRequest handling -- and the venue
            # side of recovery is what this scenario pins.

        result_pass = True
        log("")

    except TestFailure:
        pass  # die() already printed the FAIL: line
    except KeyboardInterrupt:
        log("Interrupted — shutting down ...")
    finally:
        if f8proc is not None:
            stop_f8test(f8proc)
        shutdown_all(app_procs)
        # Scenario 20 pins the test comp id to a gateway instance, and its second half
        # deliberately pins it to one this harness does not run. Left behind, that refuses
        # the baseline logon of every scenario that runs afterwards -- including a re-run of
        # this one -- and the failure would appear to be in whatever ran next. Unpinned is
        # the state every other scenario expects, so it is restored here rather than at the
        # end of the block: a scenario that fails half way through has still changed it.
        if scenario.assert_session_provisioning:
            log("Restoring the test comp id to unpinned ...")
            unprovision_gateway_pinning(FIX8_COMP_ID)

    # ── result summary ─────────────────────────────────────────────────────────
    log("")
    log("=" * 60)
    if result_pass:
        log("  RESULT  : PASS")
        log(f"  scenario: {scenario.number} — {scenario.description}")
        if scenario.extra_steps:
            for entry in phase4_results:
                if entry[0] == "kill":
                    _, failover_occurred, label, elapsed = entry
                    if failover_occurred:
                        log(f"  failover: {label} elected leader in {elapsed:.1f}s")
                    else:
                        log(f"  killed  : {label} — no disruption")
                elif entry[0] == "restart":
                    _, proc_name, elapsed = entry
                    log(f"  restart : {proc_name} ready in {elapsed:.1f}s")
                elif entry[0] == "interim":
                    _, count = entry
                    log(f"  interim : {count} orders confirmed")
                elif entry[0] == "verify":
                    _, description, elapsed = entry
                    log(f"  verified: {description} ({elapsed:.1f}s)")
        else:
            for failover_occurred, label, elapsed in kill_results:
                if failover_occurred:
                    log(f"  failover: {label} elected leader in {elapsed:.1f}s")
                else:
                    log(f"  killed  : {label} — no disruption")
            for proc_name, elapsed in restart_results:
                log(f"  restart : {proc_name} ready in {elapsed:.1f}s")
        log(f"  baseline: {before_total} orders — OK")
        log(f"  recovery: {after_total} orders — OK")
    else:
        log("  RESULT  : FAIL")
        log(f"  scenario: {scenario.number} — {scenario.description}")
    log("=" * 60)
    return result_pass


# ── main ──────────────────────────────────────────────────────────────────────

def _scenario_type(value: str):
    if value == "all":
        return "all"
    try:
        return int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"'{value}' is not a valid scenario number or 'all'"
        )


def main() -> None:
    valid_scenario_numbers = sorted(_SCENARIO_MAP.keys())

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--scenario", type=_scenario_type, required=True, metavar="N|all",
        help=(
            "Scenario to run, or 'all' to run every scenario in order.  "
            "Valid numbers: "
            + ", ".join(str(n) for n in valid_scenario_numbers)
            + ".  Run with --help for full scenario descriptions."
        ),
    )
    parser.add_argument(
        "prefix", nargs="?", default="installed",
        metavar="install_prefix",
        help="Path to the cmake install prefix (default: installed)",
    )
    parser.add_argument(
        "--orders-before", type=int, default=1, metavar="N",
        help="Bursts of 1000 NOS to confirm health before kill (default: 1)",
    )
    parser.add_argument(
        "--orders-during", type=int, default=20, metavar="N",
        help=(
            "Extra bursts of 1000 NOS sent after the health check and left "
            "in flight during Phase 4 (default: 20)"
        ),
    )
    parser.add_argument(
        "--orders-after", type=int, default=1, metavar="N",
        help="Bursts of 1000 NOS sent as recovery orders (default: 1)",
    )
    parser.add_argument(
        "--ready-timeout", type=float, default=10.0, metavar="SECS",
        help="Max seconds for initial leader election (default: 10)",
    )
    parser.add_argument(
        "--failover-timeout", type=float, default=30.0, metavar="SECS",
        help="Max seconds per failover step (default: 30)",
    )
    parser.add_argument(
        "--recovery-timeout", type=float, default=30.0, metavar="SECS",
        help="Max seconds for recovery orders to appear in the ME log (default: 30)",
    )
    args = parser.parse_args()

    for attr, flag in [("orders_before", "--orders-before"),
                       ("orders_after",  "--orders-after")]:
        if getattr(args, attr) < 1:
            parser.error(f"{flag} must be >= 1")
    if args.orders_during < 0:
        parser.error("--orders-during must be >= 0")

    project_root = Path(__file__).resolve().parent.parent
    raw_prefix = args.prefix
    prefix = resolve_prefix(
        str(project_root / raw_prefix)
        if not Path(raw_prefix).is_absolute()
        else raw_prefix
    )
    preflight(prefix)

    lib_dir  = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    if args.scenario == "all":
        scenarios_to_run = _SCENARIOS
    else:
        scenario = _SCENARIO_MAP.get(args.scenario)
        if scenario is None:
            parser.error(
                f"unknown scenario {args.scenario}; valid values: "
                + ", ".join(str(n) for n in valid_scenario_numbers)
            )
        scenarios_to_run = [scenario]

    overall_pass = True
    results: list[tuple[int, str, bool]] = []
    for scenario in scenarios_to_run:
        passed = run_scenario(scenario, args)
        results.append((scenario.number, scenario.short_name, passed))
        if not passed:
            overall_pass = False

    if len(results) > 1:
        passed_count = sum(1 for _, _, p in results if p)
        failed_count = len(results) - passed_count
        log("")
        log("=" * 60)
        log(f"SUMMARY: {passed_count}/{len(results)} scenarios passed")
        for num, name, passed in results:
            status = "PASS" if passed else "FAIL"
            log(f"  Scenario {num:2d} ({name}): {status}")
        if failed_count:
            log(f"{failed_count} scenario(s) FAILED")
        log("=" * 60)

    if not overall_pass:
        sys.exit(1)


if __name__ == "__main__":
    main()
