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

Startup order (the same dependency order devenv.py uses), with the dev environment's ports.
These orient a reader; they are not the authority.  The ports come from the environment file
and are expanded into installed/etc, and preflight_ports() reads that deployed set rather than
any list kept here -- which is just as well, because every port below except the gateway's FIX
port had drifted by exactly 4000 before that check was written and read the real ones.
  1. witness                  -- arbiters connect outbound to it (listens on 11100)
  2. arbiter_primary          -- component listener 11200, peer listener 11203
  3. arbiter_secondary        -- component listener 11201, peer listener 11204
  4. authentication_service_a -- listens on 11070 (administration listener 11072)
  5. authentication_service_b -- listens on 11071 (administration listener 11073)
  6. fix_order_gateway_a      -- FIX 9879 (TLS 9880), execution reports inbound on 11010
  7. sequencer_primary        -- listens on 11001, execution-report listener 11021, peer 11003
  8. sequencer_secondary      -- listens on 11002, execution-report listener 11022, peer 11004
  9. matching_engine          -- order listener 11020; connects out to the sequencers' 11021/11022

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
# How long to wait for the gateway's cancels to be answered one way or the other.
_OPEN_ORDER_CANCEL_TIMEOUT = 45.0
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
# How many messages the reconnecting member is made to believe it missed.
#
# A gap has to be manufactured, because nothing in an ordinary reconnect creates one. The
# venue only advances a session's outbound number when it actually sends -- and a report for
# a session that has gone is dropped before that, at the sequencer ('session not bound to any
# instance') and again at the gateway. So a member that leaves and returns finds the venue
# expecting exactly the number it is expecting, with nothing missing on either side.
#
# f8test takes -R, "set next expected receive sequence number", so the reconnecting client is
# started believing it has received this many fewer than it has. That is what a member looks
# like after messages died in a socket, it is exact rather than timing-dependent, and it needs
# no change to the venue to arrange.
#
# Twenty is chosen to be small enough to replay quickly and wide enough that the assertions
# discriminate: a venue that marked every message PossDupFlag=Y, or none, fails either way.
_RESEND_GAP_MESSAGES          = 20
# How long to watch the session after the resend says it succeeded.
#
# Long enough to span a heartbeat, because that is what exposes a resend that left the two
# sides disagreeing about the numbering: nothing is wrong until the venue next sends, and with
# the member idle the next thing it sends is a heartbeat reply. The client config asks for a
# 10s interval, so this covers two.
_RESEND_SURVIVAL_WAIT         = 25.0
# Long enough for the client to send a heartbeat, which the gateway answers with a numbered one
# of its own. That puts a number the venue cannot replay in the middle of the stream rather than
# only at the end, which is the case the replay has to gap-fill in place rather than leave to the
# completion. The generated config asks for a 10s interval.
_RESEND_HEARTBEAT_IDLE        = 14.0
# Orders sent after that heartbeat, so it has reports on both sides of it. Sent one at a time --
# f8test's 'n' -- because the burst command sends a thousand, and this only has to be enough to
# sit inside the gap. Must stay comfortably below _RESEND_GAP_MESSAGES, or the gap no longer
# reaches back past the heartbeat and the case being set up disappears.
_RESEND_TAIL_ORDERS           = 8
# The bounded range scenario 40 asks for, out of the MIDDLE of the session's history. Chosen well
# below where the numbering has reached, because that is the whole point: a request for the tail
# is answered correctly by a venue that ignores the bounds entirely, so only a range with history
# on both sides of it can tell the two apart. Fifty is a plausible thing for a member to ask.
_BOUNDED_RESEND_BEGIN         = 100
_BOUNDED_RESEND_END           = 149
# The client is asked for the range on its stdin -- f8test's 'R' reads BeginSeqNo then EndSeqNo --
# and the reply comes back through the sequencer's WAL, so give it room.
_BOUNDED_RESEND_SETTLE        = 5.0
# Scenario 41, driven by the raw client. _RAW_SILENCE_TIMEOUT is how long the venue is watched for
# something it must NOT send; it is short on purpose, because every assertion that expects silence
# pays it.
_RAW_CLIENT_SETTLE            = 2.0
_RAW_LOGON_TIMEOUT            = 15.0
_RAW_REPLY_TIMEOUT            = 10.0
_RAW_SILENCE_TIMEOUT          = 2.0

# Scenario 42. The venue stops accepting when a deferral outlives order_deferral_refusal_age in
# SequencerThread.hpp, currently 45s. This is NOT a copy of that constant to assert against: the
# scenario asserts that refusal happens within the deadline and did not happen on the first order,
# which holds whatever the threshold is set to. The deadline is generous for that reason -- a
# tighter one would turn a change to the venue's policy into a mysterious test failure.
_REFUSAL_DEADLINE             = 120.0
# How often an order is offered while waiting for the venue to start refusing. Acceptance is
# re-evaluated when an order arrives, so something has to keep arriving or nothing decides.
_REFUSAL_PROBE_INTERVAL       = 5.0
# How long an order is watched for an answer before it is taken as deferred. A deferred order gets
# no ExecutionReport at all -- the report is the matching engine's to send and there is none -- so
# this is a silence window, and it only has to outlast the round trip a real answer would take.
_REFUSAL_DEFERRED_SILENCE     = 3.0
# Long enough for at least two of the gateway's five-second progress lines to land, so "it kept
# reporting" is a claim about a sequence rather than about one line that happened to be there.
_REFUSAL_QUIET_WATCH          = 12.0

# Scenarios 43-47. With high availability off a sequencer leads the moment it starts -- there is no
# election to wait for -- so this only has to cover process start and the log reaching disk.
_HA_OFF_LEAD_TIMEOUT          = 20.0
# How long a lone secondary is watched for a gateway connection that is not coming. A silence
# window, so it only has to outlast the connection retry interval (2s) several times over.
_HA_OFF_UNREACHABLE_SETTLE    = 8.0
# Any past time will do: what matters is that OrigSendingTime is present on a retransmission, not
# what it says.
_RAW_ORIG_SENDING_TIME        = "20260101-00:00:00.000"
# Scenario 23. The client that takes over on instance b after instance a is killed.
_INFLIGHT_A_CFG               = "myfix_gateway_client_inflight_a.xml"
_INFLIGHT_CFG                 = "myfix_gateway_client_inflight_b.xml"
# Bursts left in flight when the gateway is killed. Enough that some are certain to be
# mid-pipeline at the moment of death -- one burst could conceivably complete first.
_INFLIGHT_BURSTS              = 5
# Orders that must be through before the kill, proving the pipeline is full. A fraction of
# the burst, so the remainder is genuinely still in flight when the gateway dies.
_INFLIGHT_STARTED             = 500
# Long enough for the gateway's session sequence report to fire at least once before the kill.
# The gateway reports every 2s; this is comfortably more than one interval, and what it buys is
# that the sequencer holds a record of which of the member's numbers held a report -- which is
# what the surviving instance answers the member's resend from. See
# docs/availability/resend_provenance.md.
_INFLIGHT_REPORT_SETTLE       = 5.0
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


class MachineKillStep(NamedTuple):
    """
    Every process a machine was running, killed together and left dead.

    A machine cannot be stopped in an environment that has one, so this is the
    venue's view of the event rather than the event itself: several components
    stop at the same instant and none of them come back. That is what
    distinguishes it from KillStep, which removes one component while the rest
    of its machine carries on -- the case every other scenario in this file
    covers, and the reason none of them exercises the failure a second machine
    exists for.

    proc_names:    every process on the machine, killed in one pass with no
                   waiting in between.
    ready_log_name: a log to poll for the survivors taking over, or None.
    ready_markers:  markers that must all appear on one line of that log.
    """
    proc_names: tuple
    ready_log_name: str | None
    ready_markers: tuple
    ready_timeout: float
    settle_secs: float


class IsolateStep(NamedTuple):
    """
    Stop a process without killing it, let its peer take over, then let it run
    again holding an entitlement that has since passed to the other instance.

    A partition cannot be produced on one machine without privileges the test
    does not have, so this produces the condition a partition leads to, which is
    the part that matters: an instance that is alive, believes it may act, and is
    wrong.  Freezing it also reproduces the case a lease exists for -- the
    process keeps its sockets open while renewing nothing, so presence alone
    still reports it as healthy.

    Nothing else in this file produces a live instance holding a superseded
    entitlement, which is what the generation check on every message exists to
    refuse.

    The assertion is NOT that the resumed instance learns it has been superseded.
    It has no way to: freezing it tears down none of its connections, so no status
    is exchanged and nothing tells it anything.  It wakes still believing it may
    act and says so on the wire.  What must hold is that saying so achieves
    nothing -- every message it sends is refused for quoting a generation that has
    been superseded, and the instance that took over keeps the entitlement.

    refusal_markers:   markers on the SURVIVOR's log showing it refused what the
                       resumed instance sent.  This is the assertion.
    forbidden_markers: markers that must NOT appear on the survivor's log
                       afterwards -- it must not give the entitlement back.
    """
    proc_name: str
    takeover_log_name: str
    takeover_markers: tuple
    takeover_timeout: float
    refusal_markers: tuple
    refusal_timeout: float
    forbidden_markers: tuple
    settle_secs: float


class RestartStep(NamedTuple):
    """
    One kill-and-restart action within a scenario.

    Kills the named process (if still running), deletes its old log so Quill
    starts fresh, relaunches the process, and polls ready_log_name for a line
    containing ALL of ready_markers.

    resets_me_counter: True when restarting the matching engine.  Phase 5 adjusts
                       its order-count target and log-read position accordingly.

                       The name is now half true and kept because the adjustment
                       still is.  The counter used to restart at 0 because the
                       engine held nothing across a restart; it now carries
                       forward from the highest order number in the recovered
                       region, so a successor cannot reissue an ME-ORD-N that
                       already names a different order.  What Phase 5 needs is
                       unchanged: the log was deleted, so it reads from the
                       start of a new one.
    settle_secs:       how long to wait after readiness is confirmed.
    damage_region:     the file name of a region to corrupt while the process is dead, so
                       that the successor finds a record of what it held that it cannot
                       read.  Resolved against the deployment's var directory, because the
                       scenario table is built before the install prefix is known.
    down_secs:         how long to leave the process dead before restarting it.  Zero
                       for every scenario whose subject is a fast restart.  A scenario
                       testing what a LONG absence does needs the venue to be absent for
                       real, and there is no faking it: the engine measures the absence
                       from a wall-clock stamp it wrote before it died.
    """
    proc_name: str
    ready_log_name: str
    ready_markers: tuple
    ready_timeout: float
    resets_me_counter: bool
    settle_secs: float
    down_secs: float = 0.0
    damage_region: str = ""


class SupervisedKillStep(NamedTuple):
    """
    Kill a supervised component and wait for its launcher to bring it back.

    Different from KillStep in what it targets and who restores it. The Popen the harness
    holds is the launcher, so the component is killed by pid from <run_dir>/<name>.pid, and
    nothing here restarts it -- launch.py does, which is the point. The harness restarting it
    would be simulating a supervisor rather than exercising one.

    Returns when a DIFFERENT pid appears in that file, which is the launcher having replaced
    the process rather than merely the old one still being listed.

    proc_name:      component to kill, and the pid file to watch
    restart_timeout: how long the launcher is given to bring it back
    settle_secs:    pause after it returns, before the next step
    """
    proc_name: str
    restart_timeout: float
    settle_secs: float


class AssertAbsentStep(NamedTuple):
    """
    Wait, then assert a log does NOT contain a line with all of the given markers.

    For properties that are only expressible as something not having happened. A peer that
    correctly declines to promote produces no log line saying so -- the evidence is the
    absence of the promotion it would otherwise have made -- and that can only be judged after
    leaving it long enough to have made one.

    after_secs must comfortably exceed the window in which the thing could occur, or the
    absence proves nothing except that the test was impatient.
    """
    log_name: str
    markers: tuple
    after_secs: float
    description: str
    from_byte: int = 0


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
    # me_ha scenarios also assert what a promotion does to the book (Phase 5): the
    # ME never matches, so every baseline order rests in the book and replicates to
    # the secondary; on promotion the secondary keeps that book rather than
    # cancelling it, because its own region plus the sequencer's tail say what it
    # holds.  See run_scenario's "ME-HA: the promoted book" block.
    # Components to start under scripts/launch.py, so that killing one has it restarted by a
    # real supervisor rather than by this harness. Needed for any scenario whose subject is
    # what happens when a restart beats a timeout: a harness restarting the process itself
    # would be simulating the supervisor rather than testing one.
    supervised: tuple = ()

    me_ha: bool = False
    # For an me_ha scenario in which leadership does NOT move, so recovery orders return to
    # the primary. Without it the run waits for them on the secondary, which correctly
    # received nothing.
    recovery_on_primary: bool = False
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
    # When True, assert that the orders a member had open before the matching engine was
    # restarted are still open afterwards -- by dropping the session and counting how many of
    # the cancels the gateway then sends are refused as orders the venue does not recognise.
    assert_open_orders_survive: bool = False
    # How long the venue may be unable to match before the orders open at the time are
    # cancelled and trading halts, written into the deployed configs before launch. The
    # deployed figure is 300s, which no test is going to wait out; the scenario that exercises
    # the rule shortens it and keeps the engine down past it.
    absence_limit_seconds: int = 300
    # When True, keep the matching engine down for longer than absence_limit_seconds before
    # restarting it, then assert that it cancelled every order it recovered, told each member,
    # and halted. See run_scenario's "long absence" block.
    assert_absence_halt: bool = False
    # When True, damage the matching engine's open-order region before restarting it, and
    # assert that the venue says it cannot account for what it was holding rather than
    # resuming as though the region had been empty. See run_scenario's "damaged region" block.
    assert_damaged_region: bool = False
    # Why this scenario is expected to fail, when the venue is known not to meet the
    # requirement it asserts. A scenario marked this way does not fail the suite when it
    # fails; it fails the suite when it PASSES, because the gap has closed and the marking
    # must be removed. A gap recorded as a passing test is the failure this file exists to
    # avoid, so it is never recorded that way.
    expected_failure: str = ""
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
    # When True, the member asks mid-session for a BOUNDED range out of the middle of its own
    # history, and what comes back is compared against what it was originally sent under those
    # very numbers. See run_scenario's "bounded resend" block.
    assert_bounded_resend: bool = False
    # When True, the venue's inbound sequence checking is driven with scripts/fix_raw_client.py --
    # a client with no session layer, which will send what a conforming engine will not. See
    # run_scenario's "inbound sequence" block and docs/fix/inbound_sequence_checking.md.
    assert_inbound_sequence: bool = False
    # When True, kill the gateway with orders IN FLIGHT and then bring the client back on the
    # OTHER instance, asserting what the member can recover. See run_scenario's "in-flight"
    # block. Needs gateway_b, since the whole point is that the member returns elsewhere.
    assert_inflight_recovery: bool = False
    # When True, kill the matching engine and DO NOT restart it, then prove the venue stops
    # accepting orders it cannot process rather than acknowledging them forever. See
    # run_scenario's "order refusal" block and docs/availability/order_acceptance.md.
    assert_order_refusal: bool = False

    # When True, run this scenario against a venue deployed with high availability OFF: every
    # installed config's [ha] switch is set false before launch, and the witness and both arbiters
    # are not started. The switch is written on every launch rather than restored afterwards, so a
    # scenario that dies half way cannot leave the tree set for the next one. See BUG-0061.
    ha_disabled: bool = False
    # Which sequencer instances to start with high availability off. Which FILE a sequencer was
    # started from stops meaning anything without a peer or an arbiter, so this names them
    # explicitly rather than assuming the primary.
    ha_off_sequencers: tuple = ("sequencer_primary",)
    # Name of an ha_only binary that must REFUSE to start with high availability off: "arbiter" or
    # "witness". Launched by hand after the venue is up, and expected to exit non-zero saying why.
    ha_off_refuser: str = ""
    # When True, assert that two sequencers started with high availability off BOTH lead, and that
    # each says so loudly enough for an operator to notice before the next start under HA.
    assert_ha_off_both_lead: bool = False
    # When True, assert what a secondary started ALONE with high availability off can and cannot do.
    # It leads, and the gateway cannot reach it -- see the block in run_scenario for why that is
    # recorded rather than fixed here.
    assert_ha_off_secondary_alone: bool = False
    # Skip the baseline burst. Only for a scenario whose subject is a venue that cannot trade, where
    # sending a thousand orders would fail for the reason the scenario exists to state.
    skip_baseline_orders: bool = False


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
    # instances gives you today" section of docs/availability/gateway_ha.md.
    #
    # Getting a deterministic ER for an order whose gateway is dead is the awkward
    # part. Killing the gateway mid-burst would work but is a race. Instead this
    # reuses the ME-HA cancel-on-failover path purely as an ER generator: the ME stub
    # never matches, so every baseline order rests in the book and replicates to the
    # secondary; killing the primary ME makes the promoted secondary cancel the whole
    # book, emitting exactly one cancel ER per resting order. Those orders came from
    # gateway a, which by then is dead, so every one of those ERs has nowhere to go.
    #
    # THAT GENERATOR IS GONE, 2026-08-30, and it is why this scenario is now recorded as
    # failing. A promotion no longer cancels the book: the engine keeps its open orders in
    # a memory-mapped region, so a promoted instance can say what it holds and an order
    # still on the reconciled book is genuinely outstanding (R-0073, and
    # docs/durability/open_order_checkpoint.md). No cancels means no reports, and the
    # assertions below have nothing to observe.
    #
    # Nothing about the venue got worse and nothing this scenario asserts has changed --
    # there is still no session handover, and reports for a dead instance are still dropped
    # rather than rerouted. What is missing is a way to produce a report for an order whose
    # gateway is dead. Two candidates, in preference order:
    #   1. R-0123's cancel-each-and-halt, which is the remaining user of the seq_no=0 report
    #      path and will emit exactly this burst when a region cannot be used;
    #   2. orders in flight across the kill, using the pipeline-full guard scenario 23
    #      already has, which answers the race objection that ruled this out originally.
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
        expected_failure=(
            "this scenario has no execution reports left to observe: its generator was the "
            "cancel burst a promotion used to emit, and a promotion now keeps the book "
            "(R-0073). The gap it asserts is unchanged and still true -- see the note above "
            "for the two ways to give it a generator again"
        ),
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
    # The gap is manufactured, and has to be. An ordinary reconnect creates none: the venue
    # advances a session's outbound number only when it actually sends, and a report for a
    # session that has gone is dropped before that point, so the member returns expecting
    # exactly the number the venue is about to send. The reconnecting client is therefore
    # started with -R below where the venue has reached -- see _RESEND_GAP_MESSAGES -- which
    # is what a member looks like after messages died in a socket.
    #
    # A heartbeat is let through mid-stream with orders on both sides of it, because the venue
    # cannot replay what a heartbeat's number carried -- the WAL holds reports and nothing else
    # -- and gap-filling it where it stands is a different path from gap-filling the tail.
    #
    # Seven things are asserted, and every one after the first reads the CLIENT's own received
    # messages rather than a gateway log line, because what the member was handed is the fact
    # under test: the venue continues the member's numbering rather than restarting it; the
    # member notices the gap and asks for the right range; the reports that come back are real
    # ones marked PossDupFlag=Y, inside the requested gap and nowhere beyond it; every one of
    # them carries OrigSendingTime; the numbers the venue cannot replay are gap-filled in place
    # rather than overwritten; the range is closed so the member ends up expecting the number
    # the venue will send next; and the session is still alive after the heartbeat that follows.
    Scenario(
        number=22,
        short_name="resend_recovery",
        description="A reconnecting member with a gap is sent the execution reports it missed",
        expected_outcome=(
            "the venue resumes the session's sequence numbering across the reconnect, the "
            "member notices the manufactured gap and asks for it, and receives real execution "
            "reports marked PossDupFlag=Y and stamped OrigSendingTime instead of a blanket "
            "gap-fill, leaving it expecting the venue's next number"
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
    # This is the regression test for BUG-0031 in docs/bug_list.md. The arbiter used to recompute leadership from instance
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

    # 25 — an arbiter that has restarted must not hand leadership back to the wrong instance.
    #
    # The arbiter's leadership map lives only in memory and nothing reads it back at startup.
    # It is what stops a restarted primary taking leadership from a working secondary, so an
    # arbiter that has forgotten it would apply the cold-start tie-break, prefer the lower
    # instance id, and reproduce the split-brain that rule exists to prevent. See
    # docs/bug_list.md, BUG-0033.
    #
    # BOTH arbiters are restarted deliberately. Restarting one leaves the other holding the
    # state and answering from it, which proves nothing about recovery; with neither holding
    # anything, the only source left is the leases the sitting leader keeps sending. That is
    # the mechanism under test.
    Scenario(
        number=25,
        short_name="arbiters_restart_then_me_rejoins",
        description="Arbiters restart with no state, then the primary ME rejoins",
        expected_outcome=(
            "both arbiters relearn who leads from the promoted secondary's leases; the "
            "restarted matching_engine_primary is told to follow and never adopts LEADER"
        ),
        me_ha=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            # 1. Fail the primary ME so the secondary is genuinely leading.
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
            # 2. Wipe the arbiters' knowledge. BOTH must be down at the same time, which is
            #    why they are killed before either is restarted: RestartStep kills and
            #    restarts one component before moving to the next, so restarting them in
            #    turn leaves the second one alive and holding the state while the first comes
            #    back, and the first simply learns it from the peer. That passes without
            #    exercising anything -- it is what this scenario did on its first run, and the
            #    logs showed the state surviving hop by hop through the peer replay.
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=2.0,
            ),
            # Now both are dead and nothing anywhere remembers who leads except the matching
            # engine that is doing the leading. Bring them back.
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready_markers=(_ARB_ROLE,),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=1.0,
            ),
            RestartStep(
                proc_name="arbiter_secondary",
                ready_log_name="arbiter_secondary.log",
                ready_markers=("ArbiterThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            # 3. Bring the primary ME back. It asks whichever arbiter is active, and the
            #    answer depends entirely on whether that arbiter has relearned.
            RestartStep(
                proc_name="matching_engine_primary",
                ready_log_name="matching_engine_primary.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            # 4. The assertion. Before the relearning work an arbiter with an empty map
            #    answered leader=1, and the restarted primary took leadership back from a
            #    secondary holding the book.
            VerifyStep(
                log_name="matching_engine_primary.log",
                markers=("MatchingEngineThread:", "arbiter assigned follower role"),
                timeout=90.0,
                description="restarted ME follows, even though the arbiters lost their state",
                absent_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
            VerifyStep(
                log_name="matching_engine_secondary.log",
                markers=("MatchingEngineThread:", "adopting LEADER role"),
                timeout=5.0,
                description="the secondary kept leadership throughout",
            ),
        ],
    ),

    # 26 — a supervised matching engine that dies is restarted before its peer promotes.
    #
    # The case a supervisor makes ordinary, and the one the follower's grace period exists
    # for. On 2026-08-21 the primary was OOM-killed and the venue stopped for sixteen seconds
    # against a documented target of under fifty milliseconds for local process recovery --
    # not because the timer was wrong, but because nothing filled the window it opens. See
    # docs/bug_list.md, BUG-0029.
    #
    # matching_engine_primary is started under scripts/launch.py so a real supervisor
    # restarts it. The harness doing that itself would prove nothing: the question is whether
    # a genuine restart completes before the peer's promotion timeout, and a harness that
    # restarts instantly is not evidence about a supervisor that does not.
    #
    # The assertion is an absence. A secondary that correctly declines to promote logs
    # nothing saying so; the evidence is the promotion it never made, judged after leaving it
    # comfortably longer than the timeout in which it would have made one.
    Scenario(
        number=26,
        short_name="supervised_me_restart_beats_promotion",
        description="Supervised ME restart completes before the peer promotes",
        expected_outcome=(
            "the launcher restarts matching_engine_primary within seconds; "
            "matching_engine_secondary never promotes and the primary keeps leadership"
        ),
        me_ha=True,
        supervised=("matching_engine_primary",),
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            SupervisedKillStep(
                proc_name="matching_engine_primary",
                restart_timeout=30.0,
                settle_secs=2.0,
            ),
            # The promotion timeout is ~15s (ha_timing.heartbeat_timeout_seconds). Waiting 25
            # leaves no room for argument about whether the secondary simply had not got
            # round to it.
            AssertAbsentStep(
                log_name="matching_engine_secondary.log",
                markers=("MatchingEngineThread:", "adopting LEADER role"),
                after_secs=25.0,
                description="the secondary never promoted, because the restart beat its timeout",
            ),
        ],
    ),

    # 27 — a supervised sequencer restart must beat its peer's promotion timeout.
    #
    # R1 for the sequencer pair, the same property scenario 26 establishes for the matching
    # engine. The follower arms a timeout when peer heartbeats stop; a local restart that
    # completes inside it means no election happens at all and the venue never changes leader
    # for a failure that never left the machine.
    #
    # Scenario 1 is the counterpart: the same kill with nothing to restart the process, where
    # the secondary correctly does promote. The two together say the mechanism distinguishes
    # a dead process from a dead machine, which is the distinction the whole restart family of
    # defects came from missing.
    Scenario(
        number=27,
        short_name="supervised_sequencer_restart_beats_election",
        description="Supervised sequencer restart completes before its peer elects itself",
        expected_outcome=(
            "the launcher restarts sequencer_primary within seconds; sequencer_secondary "
            "never transitions to leader and order flow is uninterrupted"
        ),
        supervised=("sequencer_primary",),
        steps=[],
        restart_steps=[],
        extra_steps=[
            SupervisedKillStep(
                proc_name="sequencer_primary",
                restart_timeout=30.0,
                settle_secs=2.0,
            ),
            AssertAbsentStep(
                log_name="sequencer_secondary.log",
                markers=(_SEQ_ROLE, _TO_LEADER),
                after_secs=25.0,
                description="sequencer_secondary never elected itself, because the restart beat its timeout",
            ),
        ],
    ),

    # 28 — a supervised arbiter restart must beat its peer taking over as active.
    #
    # R1 for the arbiter pair. Worth having separately from the other two because an arbiter
    # that goes away and returns is now stateful in a way it was not before: it holds who
    # leads each group, in memory, and relearns it from leases. A restart quick enough that
    # its peer never takes over should leave the venue unable to tell anything happened.
    #
    # Scenario 2 is the counterpart, where nothing restarts it and arbiter_secondary does
    # become active.
    Scenario(
        number=28,
        short_name="supervised_arbiter_restart_beats_takeover",
        description="Supervised arbiter restart completes before its peer becomes active",
        expected_outcome=(
            "the launcher restarts arbiter_primary within seconds; arbiter_secondary never "
            "becomes active and the sequencer keeps its leader"
        ),
        supervised=("arbiter_primary",),
        steps=[],
        restart_steps=[],
        extra_steps=[
            SupervisedKillStep(
                proc_name="arbiter_primary",
                restart_timeout=30.0,
                settle_secs=2.0,
            ),
            AssertAbsentStep(
                log_name="arbiter_secondary.log",
                markers=(_ARB_ROLE, _TO_LEADER),
                after_secs=25.0,
                description="arbiter_secondary never became active, because the restart beat its timeout",
            ),
        ],
    ),

    # 29 — restarting the FOLLOWER must change nothing about who leads.
    #
    # R3, and the case where a wrong answer is quietest. The other restart scenarios are
    # about an instance returning to a role it is entitled to; this is about one returning to
    # a subordinate role while its peer carries on serving. If it came back believing it led,
    # there would be two leaders with nothing having visibly failed -- no outage, no error, no
    # promotion in any log -- which is the one failure mode that does not announce itself.
    #
    # A starting secondary adopts Follower silently, with no line saying so, so the assertion
    # has to be built the other way round: prove it came back and rejoined properly (the
    # primary's replication connection is re-established to it), then require that it never
    # promoted. RestartStep deletes its log first, so the absence covers only what happened
    # after it returned.
    Scenario(
        number=29,
        short_name="me_follower_restart_changes_nothing",
        description="Restarting the follower ME leaves leadership untouched",
        expected_outcome=(
            "matching_engine_secondary returns as a follower, rejoins replication, and never "
            "adopts LEADER; the primary keeps leading and orders are uninterrupted"
        ),
        me_ha=True,
        # Leadership never moves, so recovery orders come back to the primary.
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            RestartStep(
                proc_name="matching_engine_secondary",
                ready_log_name="matching_engine_secondary.log",
                ready_markers=("ME-primary replication connection", "established"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            AssertAbsentStep(
                log_name="matching_engine_secondary.log",
                markers=("MatchingEngineThread:", "adopting LEADER role"),
                after_secs=20.0,
                description="the restarted follower never promoted itself",
            ),
        ],
    ),

    # 30 — R3 for the sequencer pair: restarting the follower changes nothing.
    #
    # A restarted sequencer learns from its peer through StatusQuery/StatusResponse that the
    # peer already leads, and adopts follower without troubling the arbiter -- the mechanism
    # the DSL documents for exactly this. What must not happen is the restarted instance
    # deciding for itself and producing a second leader while the first carries on serving.
    Scenario(
        number=30,
        short_name="sequencer_follower_restart_changes_nothing",
        description="Restarting the follower sequencer leaves leadership untouched",
        expected_outcome=(
            "sequencer_secondary returns as follower via StatusResponse and never transitions "
            "to leader; sequencer_primary keeps leading and orders are uninterrupted"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            RestartStep(
                proc_name="sequencer_secondary",
                ready_log_name="sequencer_secondary.log",
                ready_markers=_SEQ_FOLLOWER_MARKERS,
                ready_timeout=_SEQ_FOLLOWER_TIMEOUT,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            AssertAbsentStep(
                log_name="sequencer_secondary.log",
                markers=(_SEQ_ROLE, _TO_LEADER),
                after_secs=20.0,
                description="the restarted follower sequencer never elected itself",
            ),
        ],
    ),

    # 31 — R3 for the arbiter pair: restarting the passive arbiter changes nothing.
    #
    # Worth having on its own because a returning arbiter now rebuilds a leadership map it
    # never persisted. One coming back passive must not take over, and must not disturb the
    # component leadership it has just relearned.
    Scenario(
        number=31,
        short_name="arbiter_passive_restart_changes_nothing",
        description="Restarting the passive arbiter leaves everything untouched",
        expected_outcome=(
            "arbiter_secondary returns passive and never becomes active; arbiter_primary "
            "stays active and the sequencer keeps its leader"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            RestartStep(
                proc_name="arbiter_secondary",
                ready_log_name="arbiter_secondary.log",
                ready_markers=("ArbiterThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            AssertAbsentStep(
                log_name="arbiter_secondary.log",
                markers=(_ARB_ROLE, _TO_LEADER),
                after_secs=20.0,
                description="the restarted passive arbiter never became active",
            ),
        ],
    ),

    # 32 — R4: the pair survives a SECOND failure, in the other direction.
    #
    # Kill the primary, let the secondary take over, bring the primary back as a follower,
    # then kill the secondary. The primary -- by then the follower -- must take leadership
    # again. This is the property that makes the pair durable rather than good for one
    # failure: roles are positions either instance can hold, and holding one once must not
    # stop it holding the other later.
    #
    # This failed when written, on a real defect: replication was hard-wired
    # primary-to-secondary rather than leader-to-follower, so after a failover the leader had
    # no channel to the follower and the follower's promotion path was never armed. Fixed by
    # holding both directions permanently; see docs/bug_list.md, BUG-0034.
    #
    # It is also the case most likely to expose leftover state. The primary has been leader,
    # then follower, then leader again, and the arbiter has issued three decisions about it.
    Scenario(
        number=32,
        short_name="me_pair_survives_a_second_failure",
        description="After a failover and rejoin, the pair survives a failure the other way",
        expected_outcome=(
            "matching_engine_secondary promotes, the restarted primary rejoins as follower, "
            "and when the secondary then dies the primary takes leadership back"
        ),
        me_ha=True,
        # The primary ends up leading again, so recovery orders return to it.
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
            RestartStep(
                proc_name="matching_engine_primary",
                ready_log_name="matching_engine_primary.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            VerifyStep(
                log_name="matching_engine_primary.log",
                markers=("MatchingEngineThread:", "arbiter assigned follower role"),
                timeout=60.0,
                description="the rejoined primary is a follower before the second failure",
            ),
            # Wait for the leader's replication connection to reach the rejoined instance
            # before failing the leader. This is not test tidiness: a follower learns the
            # leader has died by losing the connection it RECEIVES replication on, so until
            # that connection exists there is nothing whose loss would arm a promotion. The
            # pair is genuinely unprotected for as long as the reconnect takes, and a version
            # of this scenario without the wait raced that window -- passing alone and failing
            # inside the suite, which is worse than failing outright.
            VerifyStep(
                log_name="matching_engine_primary.log",
                markers=("ME-primary replication connection", "established"),
                timeout=60.0,
                description="the new leader's replication connection has reached the rejoined instance",
            ),
            # Now the second failure, in the other direction.
            KillStep(
                proc_name="matching_engine_secondary",
                secondary_log_name="matching_engine_primary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_primary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
        ],
    ),

    # 33 — R4 for the arbiter pair: it must survive a second failure too.
    #
    # Kill the active arbiter, let its peer take over, restart the first one so it rejoins
    # passive, then kill the peer. The restarted arbiter must become active again. An arbiter
    # that can only take over once leaves the venue with no arbitration after two failures --
    # and everything else now depends on an arbiter being reachable to answer.
    Scenario(
        number=33,
        short_name="arbiter_pair_survives_a_second_failure",
        description="After an arbiter failover and rejoin, the pair survives a failure the other way",
        expected_outcome=(
            "arbiter_secondary becomes active, arbiter_primary rejoins passive, and when the "
            "secondary then dies the primary becomes active again"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name="arbiter_secondary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready_markers=("ArbiterThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            # The second failure, the other way round. The restarted arbiter -- passive since
            # rejoining -- must now take over.
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name="arbiter_primary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="arbiter_primary",
            ),
        ],
    ),

    # 34 — R5: the cold-start tie-break must give the same answer whichever instance is up.
    #
    # Lowest instance id wins, and the primary always holds the lower one. That rule is doing
    # more work than it was: several fixes this session lean on it being the answer when no
    # incumbent is known. Nothing has ever checked that it actually produces a deterministic
    # outcome.
    #
    # Both engines are taken down and brought back with the SECONDARY first, so the order they
    # appear in is the opposite of the order the rule should prefer. If the tie-break is doing
    # its job the primary leads regardless.
    Scenario(
        number=34,
        short_name="cold_start_tiebreak_is_deterministic",
        description="Restarting both engines secondary-first still leaves the primary leading",
        expected_outcome=(
            "with both engines restarted and the secondary up first, the primary is still the "
            "one that ends up leading"
        ),
        me_ha=True,
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="matching_engine_primary",
                secondary_log_name="matching_engine_secondary.log",
                role_prefix=None,
                settle_secs=SETTLE_AFTER_FAILOVER,
                failover_to="matching_engine_secondary",
                leader_markers=("MatchingEngineThread:", "adopting LEADER role"),
            ),
            KillStep(
                proc_name="matching_engine_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=2.0,
            ),
            # Secondary back first, deliberately.
            RestartStep(
                proc_name="matching_engine_secondary",
                ready_log_name="matching_engine_secondary.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            RestartStep(
                proc_name="matching_engine_primary",
                ready_log_name="matching_engine_primary.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            VerifyStep(
                log_name="matching_engine_primary.log",
                markers=("MatchingEngineThread:", "adopting LEADER role"),
                timeout=90.0,
                description="the primary leads despite the secondary having started first",
            ),
        ],
    ),

    # 35 — R6: with no arbiter reachable, the degraded rule must still pick ONE leader.
    #
    # docs/availability/design_notes.md is explicit that a two-node system with no fencing and no arbiter
    # is unsafe, and the fallback is "lowest instance id wins". The point of this scenario is
    # that the fallback must actually apply that rule rather than simply promoting whoever
    # notices: if both instances self-promote when the arbiter pool is gone, the degraded mode
    # produces the split brain the whole design exists to avoid.
    Scenario(
        number=35,
        short_name="degraded_promotion_picks_one_leader",
        description="With no arbiter, only the lower instance id may self-promote",
        expected_outcome=(
            "with both arbiters dead, the restarted primary self-promotes via the instance-id "
            "rule and says so; the secondary does not promote"
        ),
        me_ha=True,
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=2.0,
            ),
            # The primary was leading; take it away and bring it back with nothing to ask.
            RestartStep(
                proc_name="matching_engine_primary",
                ready_log_name="matching_engine_primary.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            VerifyStep(
                log_name="matching_engine_primary.log",
                markers=("MatchingEngineThread:", "self-promoting", "degraded"),
                timeout=90.0,
                description="the primary self-promotes and records that it is degraded",
            ),
            # And the other one must not have done the same. Two instances that both
            # self-promote when the arbiter pool is gone is the split brain the arbiter exists
            # to prevent, arrived at by the path that has no arbiter.
            AssertAbsentStep(
                log_name="matching_engine_secondary.log",
                markers=("MatchingEngineThread:", "adopting LEADER role"),
                after_secs=25.0,
                description="the secondary did not also promote itself",
            ),
        ],
    ),

    # 36 — R5 for the sequencer pair: the cold-start tie-break must be deterministic.
    #
    # As scenario 34 for the matching engine. Both sequencers are taken down and brought back
    # with the SECONDARY first, so the order they appear in is the opposite of the order the
    # rule should prefer.
    Scenario(
        number=36,
        short_name="sequencer_cold_start_tiebreak",
        description="Restarting both sequencers secondary-first still leaves the primary leading",
        expected_outcome="the primary sequencer leads despite the secondary having started first",
        # In-flight orders during phase 4 advance the matching engine's counter past the
        # recovery target, so the count phase then waits for a number that has already gone by.
        # Suppressed for a deterministic count, as the ME-HA scenarios do.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                secondary_log_name="sequencer_secondary.log",
                role_prefix=_SEQ_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="sequencer_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=2.0,
            ),
            RestartStep(
                proc_name="sequencer_secondary",
                ready_log_name="sequencer_secondary.log",
                ready_markers=("SequencerThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready_markers=("SequencerThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                # Longer than the usual settle: restarting a sequencer takes the gateway's
                # connection to it with it, and orders sent before that reconnects are dropped
                # rather than queued -- "primary sequencer not connected -- PDU not forwarded".
                # The venue needs a moment to knit itself back together, and measuring before
                # it has is measuring reconnection rather than the property under test.
                settle_secs=10.0,
            ),
            VerifyStep(
                log_name="sequencer_primary.log",
                markers=(_SEQ_ROLE, _TO_LEADER),
                timeout=90.0,
                description="the primary sequencer leads despite the secondary starting first",
            ),
            # Both sequencers restarted, so the connection each opens to the matching engine
            # had to be re-established before anything can be sequenced. Waiting for it is the
            # honest precondition of the order phase rather than test tidiness -- without it
            # the scenario measures reconnection time and calls it a failure.
            VerifyStep(
                log_name="sequencer_primary.log",
                markers=("matching engine order connection", "established"),
                timeout=60.0,
                description="the restarted leader has reached the matching engine again",
            ),
        ],
    ),

    # 37 — R6 for the sequencer: with no arbiter, exactly one instance may self-promote.
    #
    # The matching engine's equivalent of this path existed, was documented, and had never
    # once executed -- it was only reachable when an arbiter connection came up, which with no
    # arbiter never happens. The sequencer arms its startup election timer unconditionally, so
    # it should not have that fault; this scenario is what says so rather than assuming it.
    Scenario(
        number=37,
        short_name="sequencer_degraded_promotion",
        description="With no arbiter, the sequencer pair settles leadership between themselves",
        expected_outcome=(
            "with both arbiters dead, sequencer_primary and its peer settle leadership between "
            "themselves by the instance-id rule; the secondary does not also elect itself"
        ),
        # In-flight orders during phase 4 advance the matching engine's counter past the
        # recovery target, so the count phase then waits for a number that has already gone by.
        # Suppressed for a deterministic count, as the ME-HA scenarios do.
        orders_during_override=0,
        # No recovery-order phase. Restarting a sequencer takes the gateway's connection with
        # it, and orders sent during the reconnect window are dropped rather than queued --
        # "primary sequencer not connected -- PDU not forwarded". That is documented venue
        # behaviour and not what this scenario is about: the property under test is that the
        # pair settles leadership between themselves with no arbiter present, which the two
        # checks below establish directly. Measuring order flow here measures how long the
        # venue takes to knit itself back together, which varies and is a different question.
        orders_after_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=2.0,
            ),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready_markers=("SequencerThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                # Longer than the usual settle: restarting a sequencer takes the gateway's
                # connection to it with it, and orders sent before that reconnects are dropped
                # rather than queued -- "primary sequencer not connected -- PDU not forwarded".
                # The venue needs a moment to knit itself back together, and measuring before
                # it has is measuring reconnection rather than the property under test.
                settle_secs=10.0,
            ),
            # Asserts the RULE, not the outcome. The sequencer does not need an arbiter for
            # this: it resolves leadership peer-to-peer through StatusQuery/StatusResponse and
            # applies the lowest-instance-id rule with its peer's agreement, so it is not
            # degraded at all -- it has a deterministic answer. That is a capability the
            # matching engine lacks, which must ask an arbiter or fall back.
            VerifyStep(
                log_name="sequencer_primary.log",
                markers=("SequencerThread:", "my instance_id=1 < peer instance_id=2", "adopting leader"),
                timeout=90.0,
                description="the sequencer resolves leadership with its peer, needing no arbiter",
            ),
            VerifyStep(
                log_name="sequencer_primary.log",
                markers=("matching engine order connection", "established"),
                timeout=60.0,
                description="the restarted leader has reached the matching engine again",
            ),
            # 45s rather than 20: it is both the absence check and the settle before orders are
            # measured. With no arbiter AND a restarted sequencer the venue takes about a
            # minute to resume order flow -- nothing is lost, but a shorter wait measures the
            # healing rather than the property. The longer window also makes the absence
            # claim stronger, since the peer has had three times the promotion timeout to
            # elect itself and has not.
            AssertAbsentStep(
                log_name="sequencer_secondary.log",
                markers=(_SEQ_ROLE, _TO_LEADER),
                after_secs=45.0,
                description="the secondary did not also elect itself with no arbiter present",
            ),
        ],
    ),

    # 38 — R5 for the arbiter pair.
    #
    # The arbiter prefers the lower instance id and yields to a peer that is already active,
    # which is the same shape as the rule the matching engine now follows. Nothing has checked
    # that the preference actually decides a genuine cold start.
    Scenario(
        number=38,
        short_name="arbiter_cold_start_tiebreak",
        description="Restarting both arbiters secondary-first still leaves the primary active",
        expected_outcome="arbiter_primary becomes active despite arbiter_secondary starting first",
        # In-flight orders during phase 4 advance the matching engine's counter past the
        # recovery target, so the count phase then waits for a number that has already gone by.
        # Suppressed for a deterministic count, as the ME-HA scenarios do.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                secondary_log_name="arbiter_secondary.log",
                role_prefix=_ARB_ROLE,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=2.0,
            ),
            RestartStep(
                proc_name="arbiter_secondary",
                ready_log_name="arbiter_secondary.log",
                ready_markers=("ArbiterThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready_markers=("ArbiterThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            VerifyStep(
                log_name="arbiter_primary.log",
                markers=(_ARB_ROLE, _TO_LEADER),
                timeout=90.0,
                description="the primary arbiter becomes active despite the secondary starting first",
            ),
        ],
    ),

    # 39 — R6 for the arbiter: alone, with no peer and no witness, it must still decide.
    #
    # The arbiter's analogue of "no arbiter reachable" is having neither of the two parties it
    # would otherwise consult. An arbiter that will not act alone leaves every component unable
    # to arbitrate, which is the failure the whole design is arranged to avoid.
    Scenario(
        number=39,
        short_name="arbiter_alone_still_decides",
        description="An arbiter restarted with no peer and no witness still becomes active",
        expected_outcome=(
            "with the witness and its peer dead, a restarted arbiter_primary becomes active "
            "rather than waiting for parties that are not coming"
        ),
        # In-flight orders during phase 4 advance the matching engine's counter past the
        # recovery target, so the count phase then waits for a number that has already gone by.
        # Suppressed for a deterministic count, as the ME-HA scenarios do.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="witness",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                secondary_log_name=None,
                role_prefix=None,
                settle_secs=1.0,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready_markers=("ArbiterThread:",),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            VerifyStep(
                log_name="arbiter_primary.log",
                markers=(_ARB_ROLE, _TO_LEADER),
                timeout=90.0,
                description="the lone arbiter becomes active without a peer or a witness",
            ),
        ],
    ),

    # 41 -- what the venue does about a member that is WRONG.
    #
    # Driven by scripts/fix_raw_client.py rather than fix8, and that is the whole point. f8test is
    # an engine: it always writes a valid MsgSeqNum and will not send below its own expected
    # without marking it. Half the rules here are about members that do neither, and a client that
    # refuses to misbehave cannot test them.
    #
    # Six rules, each asserted on what the member is handed rather than on a log line:
    #
    #   * a number above expected      -- ResendRequest, and the message is NOT processed
    #   * a further message meanwhile  -- no second request; asked once
    #   * the gap filled               -- processed, and the session carries on
    #   * below expected, PossDupFlag  -- discarded, session kept usable
    #   * below expected, unmarked     -- Logout naming both numbers, session ended
    #   * no MsgSeqNum at all          -- Reject, and the counter does not advance
    #
    # The last is the one whose absence would be silent: if the counter advanced over a message the
    # venue could not place, the NEXT message would look like a gap and the member would be asked
    # to resend something it had already sent.
    Scenario(
        number=41,
        short_name="inbound_sequence_checking",
        description="What the venue does about a member whose sequence numbers are wrong",
        expected_outcome=(
            "a gap is asked about once and nothing past it is processed; a marked retransmission "
            "is discarded; an unmarked low number ends the session; and a message with no "
            "sequence number is rejected without moving the counter"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_inbound_sequence=True,
        steps=[],
    ),

    # 40 -- a bounded resend, asked for out of the middle of the member's own history.
    #
    # Nothing is killed and nothing reconnects. The member simply exercises a right it has at
    # any time: asking for a specific range of what the venue sent it. FIX allows it, real
    # engines do it, and until 2026-08-27 this venue read BeginSeqNo and ignored EndSeqNo, so
    # every resend ran to the head of the stream and the case could not arise.
    #
    # The range is taken from the MIDDLE deliberately. A venue that ignores what was asked for
    # and returns the most recent reports answers a request for the TAIL correctly by accident,
    # so a tail request discriminates nothing. Only a range with the member's history on both
    # sides of it can tell a venue that listened from one that guessed.
    #
    # What is asserted is the strongest thing available, and it is available only because the
    # member keeps its own record of what it was sent: every resent report is compared, by
    # ClOrdID, against the report the member originally received under that very number. A
    # resend is not "fifty messages numbered 100 to 149"; it is "the fifty messages that WERE
    # 100 to 149". The difference is invisible in every other property -- the numbering is
    # right, PossDupFlag is right, the session survives -- and it is the whole of BUG-0053.
    Scenario(
        number=40,
        short_name="bounded_resend",
        description="A member asks for a bounded range out of the middle of its own history",
        expected_outcome=(
            "the venue resends exactly the messages that occupied the requested numbers, and "
            "nothing outside the range"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_bounded_resend=True,
        steps=[],
    ),

    # 42 -- the venue refuses what it cannot process, and starts again on its own.
    #
    # The matching engine is killed and NOT restarted, which is the case the deferral policy was
    # never written for. Deferring is right for a failover: the order is in the WAL and a promoted
    # engine replays it. It is wrong when there is no engine to promote, and the venue could not
    # tell the two apart -- it deferred 1,087,912 orders across seven minutes, acknowledged every
    # one of them, and reported dropped=0 throughout.
    #
    # What makes this scenario worth having is that the OLD behaviour passes every obvious check.
    # The session is up, orders are acknowledged, nothing is dropped, no error is logged above
    # Info. A test that asks "did the venue answer?" cannot see the defect at all. So this asks a
    # harder question: does the answer CHANGE when the venue can no longer do what it is promising?
    #
    # Both halves are asserted, and the first is not padding. A venue that refused from the first
    # order would pass a refusal-only test while being badly wrong -- it would reject orders during
    # every routine failover, which members currently survive. The scenario therefore proves the
    # order is accepted first and refused later, which is the whole of the design.
    Scenario(
        number=42,
        short_name="order_refusal",
        description="A venue with no matching engine refuses orders instead of acknowledging them",
        expected_outcome=(
            "an order is accepted while a failover is still plausible and refused once it is not; "
            "cancels are refused too; the health line keeps reporting while nothing progresses; "
            "and acceptance resumes on its own when an engine returns"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_order_refusal=True,
        steps=[],
    ),

    # 43 to 47 -- high availability turned off.
    #
    # There was no coverage of this at all, which is why BUG-0061 survived: `devenv.py --no-ha`
    # skipped the arbiters while every deployed config still said high availability was on, so the
    # sequencer waited for an arbiter that would never exist, never became leader, and forwarded no
    # orders -- while acknowledging every member and logging nothing wrong. A venue that cannot
    # trade at all was invisible to forty-two scenarios.
    #
    # These run against a venue with the [ha] switch actually off, not merely with components
    # missing. Both halves matter and testing either alone would have missed the bug.
    Scenario(
        number=43,
        short_name="ha_off_trades",
        description="With high availability off, the primaries lead immediately and the venue trades",
        expected_outcome=(
            "the sequencer leads without waiting for an arbiter that will never exist, and a "
            "member's orders are accepted and answered end to end"
        ),
        ha_disabled=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),

    # 44, 45 -- a component that exists only to arbitrate must refuse a venue that has disowned
    # arbitration. Not launching it is what devenv.py does; refusing is what covers the case it
    # cannot -- a hand-started process, or a supervisor with a stale manifest. An operator who sees
    # an arbiter running will believe the venue has the mechanism it names.
    Scenario(
        number=44,
        short_name="ha_off_arbiter_refuses",
        description="With high availability off, an arbiter started by hand refuses and says why",
        expected_outcome="the arbiter exits non-zero, naming [ha] enabled = false as the reason",
        ha_disabled=True,
        ha_off_refuser="arbiter",
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),
    Scenario(
        number=45,
        short_name="ha_off_witness_refuses",
        description="With high availability off, a witness started by hand refuses and says why",
        expected_outcome="the witness exits non-zero, naming [ha] enabled = false as the reason",
        ha_disabled=True,
        ha_off_refuser="witness",
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),

    # 46 -- the case someone actually reaches for high availability off to do: run the venue on the
    # surviving machine after the primary's hardware has died. Refusing to let a secondary lead
    # would block exactly that, so it must lead and trade like any other instance.
    Scenario(
        number=46,
        short_name="ha_off_secondary_alone",
        description="With high availability off, a secondary started alone leads but cannot be reached",
        expected_outcome=(
            "the secondary leads on its own, because role means nothing without a peer or an "
            "arbiter -- and the gateway cannot reach it, which is recorded rather than fixed here"
        ),
        ha_disabled=True,
        ha_off_sequencers=("sequencer_secondary",),
        assert_ha_off_secondary_alone=True,
        skip_baseline_orders=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),

    # 47 -- both started together, both leading. This is NOT split brain: with high availability
    # off the gateway reaches only one sequencer, so the same order cannot enter two books. It is
    # still a trap, because each instance advances its own WAL and burns epochs from the state file
    # that exists so a restart cannot reuse a spent generation. The damage lands at the next start
    # under high availability, which is BUG-0062. What this scenario asserts is that the venue says
    # so at the time, loudly enough that an operator would notice before then.
    Scenario(
        number=47,
        short_name="ha_off_both_lead",
        description="With high availability off, two sequencers started together both lead",
        expected_outcome=(
            "both instances lead and both say so, so the condition is visible at the time rather "
            "than only at the next start under high availability"
        ),
        ha_disabled=True,
        ha_off_sequencers=("sequencer_primary", "sequencer_secondary"),
        assert_ha_off_both_lead=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),

    # 48 -- a machine stops.
    #
    # Every scenario before this one kills a process, which models a component
    # failing on a machine that is still there.  None of them models the failure
    # the second machine exists for: the machine itself stopping, taking
    # everything on it at the same instant, with nothing to restart.
    #
    # The split assumed here puts the arbitration pair's primary, the sequencer
    # primary and the matching engine primary on the machine that stops.  The
    # witness is deliberately NOT on it: an arbitration pair and its witness on
    # one machine is a single point of failure with three processes in it, which
    # the specification requires a deployment not to be (R-0084).  The gateway
    # is likewise elsewhere, so that the member's session survives the machine
    # and the venue's ability to trade can be observed at all.
    Scenario(
        number=48,
        short_name="machine_loss",
        description="A machine stops, taking every component on it at once",
        expected_outcome=(
            "the sequencer and matching engine on the surviving machine take "
            "over, the member's session is unaffected, and recovery orders are "
            "answered -- with nothing on the lost machine restarted"
        ),
        me_ha=True,
        # In-flight orders would advance the promoted secondary's counter during
        # reconciliation, as in scenario 16.
        orders_during_override=0,
        steps=[],
        extra_steps=[
            MachineKillStep(
                proc_names=("arbiter_primary", "sequencer_primary", "matching_engine_primary"),
                ready_log_name="matching_engine_secondary.log",
                ready_markers=("MatchingEngineThread:", "adopting LEADER role"),
                ready_timeout=45.0,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 49 -- an instance that was unreachable comes back still believing it leads.
    #
    # The condition a partition produces, reached by freezing the leader rather
    # than by breaking a network the test cannot touch.  While it is stopped it
    # holds every socket it had and renews nothing, which is precisely the case
    # a lease exists for: presence still reports it as healthy, and only the
    # lease expiring says otherwise.
    #
    # It is then let run again, holding an entitlement that has passed to the
    # other instance.  Nothing else in this file produces that, and it is what
    # the generation carried on every message exists to refuse -- the mechanism
    # this venue uses in place of removing the node by force.
    Scenario(
        number=49,
        short_name="superseded_instance_returns",
        description="A frozen leader is resumed after its peer has taken over",
        expected_outcome=(
            "the peer takes over while the leader is stopped, and everything the "
            "leader sends on waking is refused for quoting a generation that has "
            "been superseded -- the peer keeping the entitlement throughout"
        ),
        orders_during_override=0,
        steps=[],
        extra_steps=[
            IsolateStep(
                proc_name="sequencer_primary",
                takeover_log_name="sequencer_secondary.log",
                takeover_markers=(_SEQ_ROLE, _TO_LEADER),
                takeover_timeout=45.0,
                refusal_markers=("SequencerThread:", "stale peer", "ignoring"),
                refusal_timeout=30.0,
                forbidden_markers=(_SEQ_ROLE, "-> follower"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 50 -- do a member's open orders survive a restart of the matching engine?
    #
    # It asserts R-0018. The engine writes every open order to a memory-mapped
    # region as it accepts it and reads the region back at startup, so a restarted
    # engine comes back holding what it held.
    #
    # It asks the member's question rather than an internal one. Dropping the
    # session makes the gateway cancel what it believes is open; the engine either
    # accepts each cancel or refuses it as an order it does not recognise.
    Scenario(
        number=50,
        short_name="open_orders_survive_me_restart",
        description="A member's open orders across a restart of the matching engine",
        expected_outcome=(
            "every order open before the restart is still open afterwards, so every "
            "cancel the gateway sends for it is accepted rather than refused as unknown"
        ),
        assert_open_orders_survive=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[_me_restart_step()],
    ),

    # 51 -- what a long absence does to the orders a member was locked out of.
    #
    # An order is not dangerous because it is old. One that rested all morning in a working
    # market is fine: its owner could have cancelled it at any moment and chose not to. What
    # makes an order dangerous is that its owner was LOCKED OUT of it -- the venue could not
    # match, so the member could not cancel, and the market moved while it could do nothing.
    #
    # So this kills the engine and leaves it dead. The absence has to be real: the engine
    # measures it from a wall-clock stamp it wrote before it died, and nothing here can fake
    # that. The deployed limit is five minutes, which no test is going to sit out, so the
    # scenario shortens it to a few seconds and stays down past it.
    #
    # What must then happen, per R-0117: every recovered order cancelled, the member that
    # placed each one told, and trading halted. And per R-0023, the halt stays until a person
    # lifts it -- so the venue must refuse an order sent afterwards rather than quietly
    # carrying on.
    Scenario(
        number=51,
        short_name="long_absence_cancels_and_halts",
        description="Orders a member was locked out of are cancelled, reported, and trading halts",
        expected_outcome=(
            "the restarted engine recovers its open orders, cancels every one of them, sends "
            "the member that placed it a report saying so, halts, and refuses the orders sent "
            "afterwards"
        ),
        absence_limit_seconds=5,
        assert_absence_halt=True,
        # No orders in flight across the kill: the subject is what happens to the orders that
        # were already resting, and in-flight traffic only makes the counts non-deterministic.
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[
            RestartStep(
                proc_name="matching_engine",
                ready_log_name="matching_engine.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=_ME_SETTLE,
                # Comfortably past the five seconds this scenario allows, and short enough that
                # the suite does not notice.
                down_secs=12.0,
            ),
        ],
    ),

    # 52 -- the venue's own record of what it held cannot be read.
    #
    # R-0102 says such a record is not to be used. That leaves the engine unable to name what
    # it held, and it cannot cancel what it cannot name -- so R-0123 sends it to the other
    # record the venue keeps, the sequencer's log of what it took, and requires it to cancel
    # each order and tell each member from that instead.
    #
    # But that log is truncated as it is consumed, so a replay from the beginning starts
    # wherever truncation left off rather than at the start of the day. In a sandbox that has
    # been running scenarios for hours it starts a long way in, which is the case this asserts:
    # the engine cannot establish what was open, so it says it cannot account for what it was
    # holding and halts WITHOUT cancelling. Cancelling only the orders it could name would
    # leave every earlier one unmentioned, which is the silence the whole chapter exists to
    # prevent, arrived at while appearing to have done something.
    #
    # The region itself must survive: whatever made it unreadable is evidence, and a venue that
    # overwrites it destroys the only account of its own failure.
    Scenario(
        number=52,
        short_name="damaged_region_cannot_account",
        description="A matching engine whose record of what it held cannot be read",
        expected_outcome=(
            "the engine refuses the damaged region rather than reading it, keeps it aside, "
            "says it cannot account for what it was holding, and halts without cancelling"
        ),
        assert_damaged_region=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[
            RestartStep(
                proc_name="matching_engine",
                ready_log_name="matching_engine.log",
                ready_markers=_ME_READY_MARKERS,
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=_ME_SETTLE,
                damage_region="matching_engine_open_orders.region",
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


# The venue binds exactly the *_listen_port settings of the deployed configs. Every other
# port in there is one a component connects *out* to, and is bound by another member of the
# same set -- so checking the listen ports covers the lot without flagging our own peers.
_LISTEN_PORT_SETTING = re.compile(r"^\s*(\w*listen_port)\s*=\s*(\d+)", re.MULTILINE)

# The hex state /proc/net/tcp gives a listening socket.
_TCP_STATE_LISTEN = "0A"

# How long to wait for ports held by a previous run to clear. A run started before the last
# one's sockets have gone dies during startup with nothing but an exit code -- it cost a whole
# 19-minute suite to find that the cause was starting too soon after a stop, because the
# message named the process that could not bind rather than the process still holding the port.
# Sockets go within a second or so once the holder has actually exited, so the wait is free
# except in the case it exists to catch.
PORT_CLEAR_TIMEOUT = 15.0


def deployed_listen_ports(prefix: Path) -> dict[int, str]:
    """Map each port the deployed configs will bind to the setting that asks for it."""
    ports: dict[int, str] = {}
    for toml_path in sorted((prefix / "etc").rglob("*.toml")):
        try:
            text = toml_path.read_text(errors="replace")
        except OSError:
            continue
        for match in _LISTEN_PORT_SETTING.finditer(text):
            ports.setdefault(int(match.group(2)), f"{toml_path.name}:{match.group(1)}")
    return ports


class PortHolder(NamedTuple):
    """The process holding a listening port: its pid, its executable, and its command line."""
    pid:        int
    executable: str
    command:    str

    def describe(self) -> str:
        what = self.executable or "an executable this user may not read"
        return f"pid {self.pid} — {what}" + (f"  [{self.command}]" if self.command else "")


def _executable_of(pid_dir: Path) -> str:
    """Absolute path of the running executable, from /proc/<pid>/exe, or "" if unreadable.

    Read the exe link rather than comm: comm is the thread name, truncated to 15 characters,
    and for anything started through an interpreter it says "python3" rather than naming the
    program. The exe link is what tells one of our own binaries apart from a stranger.
    """
    try:
        return os.readlink(pid_dir / "exe")
    except OSError:
        return ""


def _command_line_of(pid_dir: Path, limit: int = 100) -> str:
    """The process's command line on one line, truncated, or "" if unreadable.

    Worth having beside the executable because an interpreter's executable is the same for
    every script: it is the arguments that say which one is holding the port.
    """
    try:
        raw = pid_dir.joinpath("cmdline").read_bytes()
    except OSError:
        return ""
    line = " ".join(raw.decode("utf-8", "replace").split("\0")).strip()
    return line if len(line) <= limit else line[:limit - 3] + "..."


def listening_ports() -> dict[int, PortHolder]:
    """Every TCP port in the LISTEN state right now, mapped to a description of its holder.

    Read out of /proc rather than by running ss or netstat: neither is installed on every host
    this harness runs on, and the shape of their output has changed between versions. The
    socket inode from /proc/net/tcp identifies the owner -- it is the process carrying that
    inode among its file descriptors. Only processes this user owns can be read, which is
    enough: a port held over from a previous run is held by one of ours.
    """
    inode_to_port: dict[str, int] = {}
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            rows = Path(table).read_text(errors="replace").splitlines()[1:]
        except OSError:
            continue
        for row in rows:
            fields = row.split()
            if len(fields) < 10 or fields[3] != _TCP_STATE_LISTEN:
                continue
            try:
                inode_to_port[fields[9]] = int(fields[1].rsplit(":", 1)[1], 16)
            except (IndexError, ValueError):
                continue

    held = {port: PortHolder(0, "", "") for port in inode_to_port.values()}
    inode_of_link = {f"socket:[{inode}]": inode for inode in inode_to_port}
    for pid_dir in Path("/proc").glob("[0-9]*"):
        try:
            descriptors = list(pid_dir.joinpath("fd").iterdir())
        except OSError:
            continue                      # another user's, or it exited while we looked
        for descriptor in descriptors:
            try:
                inode = inode_of_link.get(os.readlink(descriptor))
            except OSError:
                continue
            if inode is None:
                continue
            held[inode_to_port[inode]] = PortHolder(int(pid_dir.name),
                                                    _executable_of(pid_dir),
                                                    _command_line_of(pid_dir))
    return held


def preflight_ports(prefix: Path, timeout: float = PORT_CLEAR_TIMEOUT) -> None:
    """Refuse to start while anything still holds a port the venue has to bind.

    Naming the executable matters as much as naming the port, because the two cases want
    opposite responses: a venue binary under the install prefix means the last run has not
    finished shutting down and the fix is to stop it, while anything else means a stranger has
    taken a port the venue needs and no amount of stopping our own processes will free it.
    """
    wanted = deployed_listen_ports(prefix)
    if not wanted:
        die(f"no *_listen_port settings found under {prefix / 'etc'} — run deploy.py first")

    venue_bin = str(prefix / "bin") + os.sep
    deadline  = time.monotonic() + timeout
    announced = False
    while True:
        clashes = {port: holder for port, holder in listening_ports().items() if port in wanted}
        if not clashes:
            if announced:
                log("  ports clear")
            return
        if time.monotonic() >= deadline:
            break
        if not announced:
            log(f"{len(clashes)} of the {len(wanted)} ports the venue binds are still held; "
                f"waiting up to {timeout:.0f}s for them to clear ...")
            announced = True
        time.sleep(LOG_POLL_INTERVAL)

    ours = 0
    for port in sorted(clashes):
        holder = clashes[port]
        if holder.executable.startswith(venue_bin):
            ours += 1
        log(f"  port {port} ({wanted[port]}) is held by {holder.describe()}")
    if ours == len(clashes):
        die(f"{len(clashes)} port(s) are held by a venue that is still running — stop it with "
            f"'scripts/devenv.py stop' before starting this run")
    die(f"{len(clashes)} port(s) still held after {timeout:.0f}s, {len(clashes) - ours} of them by "
        f"something that is not a venue binary — see the lines above for what to go and stop")


def preflight(prefix: Path) -> None:
    if not FIX8_BIN.is_file() or not os.access(FIX8_BIN, os.X_OK):
        die(f"f8test not found or not executable: {FIX8_BIN}")
    for name in ("witness", "arbiter", "sequencer",
                 "matching_engine", "fix_order_gateway",
                 "authentication_service"):
        exe = prefix / "bin" / name
        if not exe.is_file() or not os.access(exe, os.X_OK):
            die(f"binary not found or not executable: {exe}")
    preflight_ports(prefix)


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


def me_resumed_book_size(log_path: Path, from_byte: int = 0,
                         timeout: float = 10.0) -> int | None:
    """Return the order count the ME resumed as leader with, or None if it never said.

    On promotion the ME logs "reconciliation complete at seq_no=S with N order(s) on
    the book -- resuming as leader". N is what it is holding at the moment it starts
    serving again, which is the member-visible promise: those orders are still open
    and still cancellable. Only bytes beyond from_byte are scanned, so this promotion
    is seen rather than a prior one. Polls up to `timeout` because the logger is
    asynchronous and the line may lag the event slightly.
    """
    marker = "resuming as leader"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as fh:
                fh.seek(from_byte)
                for line in fh.read().splitlines():
                    if marker in line and " order(s) on the book" in line:
                        try:
                            return int(line.split(" with ", 1)[1].split(" order(s)", 1)[0])
                        except (IndexError, ValueError):
                            return None
        time.sleep(LOG_POLL_INTERVAL)
    return None


def me_cancel_on_failover_count(log_path: Path, from_byte: int = 0,
                                timeout: float = 10.0) -> int:
    """Return N from the ME's cancel-on-failover summary line, or -1 if absent.

    The ME logs "cancel-on-failover complete -- N cancel ER(s) sent, book cleared"
    when it cancels its whole book. That is no longer what a promotion does: the
    promoted instance keeps the book its region and the sequencer's tail vouch for.
    The line belongs to the path taken when the region cannot be used -- cancel each,
    report each, halt (R-0102, R-0123) -- so a caller checking that a promotion did
    NOT cancel should pass a short timeout, since it is waiting to confirm an absence.

    Only bytes beyond from_byte are scanned (so we see this promotion, not a prior
    run). Polls up to `timeout` because the logger is asynchronous and the line may
    lag the event slightly.
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


def _client_messages(client_output: Path) -> list[dict[str, str]]:
    """Every FIX message the CLIENT printed, as {field name: value}, in the order received.

    f8test prints a message as a header block, a body block and a trailer block, one indented
    "FieldName (tag): value" line per field. Enum-valued fields print as "NAME (value)" and
    plain ones print the value alone; both are reduced to the value here, so a caller compares
    against what the specification calls it rather than against how fix8 chose to render it.

    Parsed per message rather than counted across the file, because the assertions that matter
    are positional. PossDupFlag belongs on the messages inside the requested gap and on no
    others, and a total cannot tell those apart.
    """
    if not client_output.is_file():
        return []
    messages: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for line in client_output.read_text(errors="replace").splitlines():
        if line.startswith('header ("header")'):
            current = {}
            messages.append(current)
            continue
        if current is None:
            continue
        match = re.match(r"\s+(\w+) \((\d+)\): (.*)$", line)
        if not match:
            continue
        name, _tag, raw = match.groups()
        enum_value = re.match(r"^.*\((.+)\)$", raw.strip())
        current[name] = enum_value.group(1) if enum_value else raw.strip()
    return messages


def _fix_flag_is_set(value: str | None) -> bool:
    """True for a FIX boolean the client rendered as set, however it chose to render it."""
    return value is not None and value.strip().lower() in ("y", "yes", "true", "1")


def _wait_for_client_prompt(client_output: Path, prompt: str, timeout: float) -> bool:
    """Wait for f8test to print one of its interactive prompts.

    The menu reads keys in raw mode and takes what it finds in one go, so a whole command written
    at once is swallowed by the keystroke read and the numbers after it are lost. Each line has to
    be written only once the client has asked for it, and this is how that is known.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if client_output.is_file() and prompt in client_output.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False


def _client_highest_seq_num(client_output: Path) -> int | None:
    """The highest MsgSeqNum the CLIENT was sent, or None if it was sent nothing.

    This is where the venue's numbering had reached from the member's side, which is the only
    side that can say so without the venue vouching for itself.
    """
    numbers = [int(msg["MsgSeqNum"]) for msg in _client_messages(client_output) if msg.get("MsgSeqNum", "").isdigit()]
    return max(numbers) if numbers else None


def _inherited_report_range_count(log_path: Path, from_byte: int = 0) -> int | None:
    """How many outbound numbers the gateway was told had held execution reports, on binding.

    The record travels in the sequencer's session state, so a gateway that never sent those
    messages still knows which numbers carried one. None means the line was not found.
    """
    try:
        with log_path.open("r", errors="replace") as handle:
            handle.seek(from_byte)
            for line in handle:
                match = re.search(r"report range\(s\) covering (\d+) number\(s\)", line)
                if match:
                    return int(match.group(1))
    except OSError:
        return None
    return None


def _resend_resume_point(log_path: Path, from_byte: int = 0) -> int | None:
    """The outbound number the gateway said it would resume at after the resend, or None.

    The end of the requested range, and the number the member must be left expecting. Taken
    from the gateway rather than assumed from where the numbering stood before the reconnect,
    because the member detects its gap on the first message after the Logon -- so by the time
    it asks, a heartbeat or a live report may have moved the venue on. Only the boundary comes
    from here; every assertion about what the member was handed reads the client's own output.
    """
    try:
        with log_path.open("r", errors="replace") as handle:
            handle.seek(from_byte)
            for line in handle:
                match = re.search(r"will resume at (\d+)\)", line)
                if match:
                    return int(match.group(1))
    except OSError:
        return None
    return None


def _resend_gap_fill(log_path: Path, from_byte: int = 0) -> tuple[int, int] | None:
    """The (from, to) range named on the gateway's gap-fill line, or None if it filled nothing."""
    try:
        with log_path.open("r", errors="replace") as handle:
            handle.seek(from_byte)
            for line in handle:
                match = re.search(r"gap-filled (\d+)\.\.(\d+) \(administrative traffic\)", line)
                if match:
                    return int(match.group(1)), int(match.group(2))
    except OSError:
        return None
    return None


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


def _gateway_progress_lines(log_path: Path) -> list[dict[str, int]]:
    """Every GW-PROGRESS line in a gateway log, as its figures.

    The line is emitted on a timer as well as per N orders accounted, so a gateway serving nobody
    still writes one every few seconds -- the whole point of BUG-0009's change, and the reason a
    caller must read what a line SAYS rather than count how many there are.
    """
    try:
        return _progress_figures(log_path.read_text(errors="replace"))
    except OSError:
        return []


# The trailing fields are optional here, not because the gateway ever omits them, but so that
# appending the next one does not silently stop every line from matching at all -- a parser that
# returns nothing looks exactly like a gateway that logged nothing. Fields are appended for the
# same reason: see the TEST CONTRACT note beside the line in FixOrderGatewayThread.cpp.
_GW_PROGRESS_PATTERN = re.compile(
    r"GW-PROGRESS accounted=(\d+) sent=(\d+) dropped=(\d+) nos_received=(\d+)"
    r"(?: awaiting=(\d+))?(?: refused=(\d+))?(?: refused_cancels=(\d+))?")


def _progress_figures(text: str) -> list[dict[str, int]]:
    """Parse every GW-PROGRESS line in some log text into its figures."""
    lines: list[dict[str, int]] = []
    for match in _GW_PROGRESS_PATTERN.finditer(text):
        figures = {"accounted": int(match.group(1)), "sent": int(match.group(2)),
                   "dropped": int(match.group(3)), "nos_received": int(match.group(4))}
        for index, name in ((5, "awaiting"), (6, "refused"), (7, "refused_cancels")):
            if match.group(index) is not None:
                figures[name] = int(match.group(index))
        lines.append(figures)
    return lines


# The venue switch as it appears in a deployed config, which is in two spellings and three
# sections. `ha_enabled` is unambiguous wherever it appears -- the gateways carry it under
# [sequencer] and the publishers under their own section -- while a bare `enabled` means the switch
# only inside [ha], since half the file is full of enabled flags for other things.
#
# All of them are filled from the one [ha] enabled in the environment file. The spread of spellings
# is a wart, and knowing where it is beats papering over it: this is the list of places that a
# venue-wide switch has to reach, and BUG-0061 was four of them disagreeing.
_HA_ENABLED_RE = re.compile(r"(^\s*ha_enabled\s*=\s*)(true|false)", re.MULTILINE)
_HA_SECTION_RE = re.compile(r"(^\[ha\]\n(?:(?!^\[).*\n)*?^\s*enabled\s*=\s*)(true|false)",
                            re.MULTILINE)


def set_installed_ha(prefix: Path, enabled: bool) -> int:
    """Set the [ha] switch in every deployed config that has one. Returns how many were changed.

    Written on every launch rather than restored after an HA-off scenario. A restore only runs when
    the scenario reaches it, and a scenario that dies half way would leave the whole tree deployed
    with high availability off -- which the next twenty scenarios would fail on, for a reason having
    nothing to do with them.
    """
    wanted = "true" if enabled else "false"
    changed = 0
    for toml_path in sorted((prefix / "etc").rglob("*.toml")):
        try:
            text = toml_path.read_text(errors="replace")
        except OSError:
            continue
        result, count = _HA_SECTION_RE.subn(lambda m: m.group(1) + wanted, text)
        result, extra = _HA_ENABLED_RE.subn(lambda m: m.group(1) + wanted, result)
        count += extra
        if count and result != text:
            toml_path.write_text(result)
            changed += 1
    return changed


def set_installed_absence_limit(prefix: Path, seconds: int) -> int:
    """Set order_book.absence_limit_seconds in every deployed matching engine config.

    The deployed figure is five minutes, which is a trading decision about how long a member's
    resting order stays meaningful. No test is going to keep the venue down for five minutes to
    watch it elapse, so the scenario that exercises the rule shortens it and keeps the engine
    down past the shorter figure instead.

    Written on every launch rather than restored afterwards, for the same reason
    set_installed_ha is: a restore only runs when the scenario reaches it, and a scenario that
    dies half way would leave every later one running with a three-second limit and cancelling
    its book for no reason anyone could see.
    """
    changed = 0
    pattern = re.compile(r"^(absence_limit_seconds\s*=\s*)\d+", re.M)
    for toml_path in sorted((prefix / "etc").rglob("*.toml")):
        try:
            text = toml_path.read_text(errors="replace")
        except OSError:
            continue
        result, count = pattern.subn(lambda m: m.group(1) + str(seconds), text)
        if count and result != text:
            toml_path.write_text(result)
            changed += 1
    return changed


def _gateway_progress_lines_since(log_path: Path, from_byte: int) -> list[dict[str, int]]:
    """The GW-PROGRESS lines written after from_byte, as their figures.

    Reading from an offset rather than the whole file is what lets a caller ask "did it keep
    reporting DURING this window", which is a different question from "has it ever reported".
    The second is answered by a run that was healthy an hour ago.
    """
    try:
        with open(log_path, "r", errors="replace") as handle:
            handle.seek(from_byte)
            text = handle.read()
    except OSError:
        return []
    return _progress_figures(text)


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
               bin_dir: Path, log_dir: Path, run_dir: Path | None = None) -> subprocess.Popen:
    """Start one component, optionally under scripts/launch.py.

    With run_dir given the component is supervised: launch.py restarts it if it dies, and the
    Popen returned is the LAUNCHER, not the component. Anything that needs to kill the
    component itself must read run_dir/<name>.pid, which launch.py keeps pointed at the child.

    Supervision is what makes the "restart inside the grace period" case testable at all. A
    harness that killed and restarted the process itself would be simulating a supervisor
    rather than exercising one, and the thing under test is whether a real restart beats the
    peer's promotion timeout.
    """
    if not config.is_file():
        die(f"config not found: {config}")
    # Run each process from its config directory (etc/<component>), matching
    # devenv's per-component workdir.  Config-relative paths — notably the
    # gateway's fix_gateway.crt — resolve against this dir.  Log and config are
    # passed as absolute paths, so they are unaffected by the cwd.
    command = [str(bin_dir / bin_name), str(log_dir / f"{name}.log"), str(config)]
    if run_dir is not None:
        run_dir.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(Path(__file__).resolve().parent / "launch.py"),
                   "--name", name, "--run-dir", str(run_dir), "--"] + command

    with open(log_dir / f"{name}.stdout", "w") as stdout_fh:
        proc = subprocess.Popen(
            command,
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
                       keep_sequence_numbers: bool = False,
                       ignore_logon_sequence_check: bool = False,
                       persist_to_disk: str | None = None) -> str:
    """Generate an f8test config from the stock one with specific attributes rewritten.

    Generated rather than checked in because it must track the stock config: a divergence
    in host, comp id or target would make a scenario fail for reasons unrelated to what it
    tests. Written beside the original because f8test resolves the name relative to its own
    directory.

    keep_sequence_numbers turns OFF reset_sequence_numbers, so the client keeps its own
    numbering rather than telling the venue to forget the session's -- which is what makes
    it notice a gap and ask for what it missed.

    ignore_logon_sequence_check is what lets it survive noticing. fix8 handles a too-high
    inbound number in Session::enforce, and the branch it takes depends on the session state:
    once continuous it sends a ResendRequest, but at logon -- where a member reconnecting into
    a gap first sees the number -- it throws InvalidMsgSequence and logs off, unless this flag
    is set. So a client without it cannot ask for a gap it detects on the Logon, and answers a
    venue that resumed its numbering correctly by dropping the session. Set for the resend
    scenario only, because it is a property of that client, not of the venue.

    persist_to_disk names a file-backed session store, so a restarted client CONTINUES its own
    numbering instead of beginning again at 1.

    Memory persistence was the original choice and the reasoning was recorded here: a client that
    remembers, talking to a venue that also remembers, agrees with it -- no gap, no ResendRequest,
    no replay, and the recovery machinery never exercised. **That reasoning was right about the
    outbound gap and has been overtaken.** The gap is now manufactured explicitly with -R, so it
    no longer depends on the client having forgotten anything.

    What memory persistence did instead was make the client a member that no venue would accept.
    It restarted its own numbering at 1 while `reset_sequence_numbers="false"` told the venue not
    to reset -- a member saying "keep our numbering" while abandoning its own. That passed only
    while nothing checked; inbound sequence checking (BUG-0038) logs such a member out, and
    rightly. A file store makes the client behave the way a real member with a persistent store
    behaves, which is what these scenarios were always meant to be about.

    The store must be deleted before a run, or a client resumes from where the PREVIOUS run left
    it while the venue starts fresh -- see clear_fix8_persisted_state.
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
    if ignore_logon_sequence_check:
        text, count = re.subn(r'(\n\s*)reset_sequence_numbers=',
                              r'\1ignore_logon_sequence_check="true"\1reset_sequence_numbers=',
                              text, count=1)
        if count == 0:
            die(f"nowhere to add ignore_logon_sequence_check in {source}")
    if persist_to_disk is not None:
        text, count = re.subn(r'persist="[^"]*"', 'persist="ha_test_file_persist"', text, count=1)
        if count == 0:
            die(f"no persist attribute to rewrite in {source}")
        store = (f'    <persist name="ha_test_file_persist"\n'
                 f'                type="file" dir="./run"\n'
                 f'                use_session_id="true"\n'
                 f'                db="{persist_to_disk}" />\n')
        text, count = re.subn(r'(\n\s*<persist\b)', "\n" + store + r"\1", text, count=1)
        if count == 0:
            die(f"nowhere to add a file persister in {source}")
    if listen_port is not None:
        text, count = re.subn(r'port="\d+"', f'port="{listen_port}"', text, count=1)
        if count == 0:
            die(f"no port attribute to rewrite in {source}")

    (FIX8_DIR / filename).write_text(text)
    return filename


# The file-backed session store the reconnecting clients share, so one continues where the last
# left off -- which is what a real member with a persistent store does, and what the venue's
# inbound sequence checking now requires. Written under FIX8_DIR/run by fix8 itself.
_FIX8_PERSIST_DB = "ha_test_session"


def clear_fix8_persisted_state() -> None:
    """Delete the clients' session store before a run.

    Without this a client resumes from where the PREVIOUS run left it while the venue starts
    fresh, so the member's numbering is a thousand ahead of the venue's and every scenario fails
    for a reason that has nothing to do with what it tests.
    """
    run_dir = FIX8_DIR / "run"
    if not run_dir.is_dir():
        return
    for path in run_dir.glob(f"{_FIX8_PERSIST_DB}*"):
        try:
            path.unlink()
        except OSError as error:
            die(f"could not clear the fix8 session store at {path}: {error}")


def clear_open_order_regions(prefix: Path) -> None:
    """Delete the matching engines' open-order regions before a run.

    The region holds the venue's open orders so that they outlive the process holding them,
    which is exactly what it should do between a death and a restart -- and exactly what a
    scenario must not inherit from the run before it. Left in place, a scenario starts with the
    previous scenario's orders resting on the book and with the order numbering carried forward
    past them, so the baseline waits for an ME-ORD-1000 that will never be issued again.

    A scenario's premise is a venue whose book is empty and whose numbering starts at one. That
    is a thing the harness has to arrange deliberately, the same way it deletes the clients'
    session store, because in a deployment finding the previous process's region is the whole
    point.

    The engine creates the region again at startup when it finds none.
    """
    var_dir = prefix / "var"
    if not var_dir.is_dir():
        return
    for path in sorted(var_dir.glob("*open_orders.region")):
        try:
            path.unlink()
        except OSError as error:
            die(f"could not clear the open-order region at {path}: {error}")


def write_no_reset_fix8_config() -> None:
    """The resend scenario's client: instance a, not asking the venue to forget the session,
    prepared to ask for a gap it first sees on the Logon rather than logging off over it, and
    remembering its own numbering across a reconnect as a real member does."""
    write_fix8_variant(FIX8_NO_RESET_CFG, keep_sequence_numbers=True, ignore_logon_sequence_check=True,
                       persist_to_disk=_FIX8_PERSIST_DB)


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
               client_output: Path | None = None,
               next_expected_receive: int | None = None) -> subprocess.Popen:
    """
    Launch one f8test session, wait for confirmed FIX logon, then send `count`
    T commands.  Each T command sends 1000 NOS.  Returns the Popen object.
    Dies immediately on authentication failure or timeout instead of hanging.

    client_output captures what the CLIENT received, which is the only place some things can
    be checked: whether a resent report carried PossDupFlag is visible to the member and to
    nobody else, the gateway's own record of it being below the deployed log level.

    next_expected_receive starts the client believing the venue's numbering stands at that
    value. Setting it below where the venue has actually reached manufactures a gap the member
    detects for itself and asks about -- see _RESEND_GAP_MESSAGES.
    """
    gw_pos = file_end(gw_log)
    stdout_target = client_output.open("w") if client_output is not None else subprocess.DEVNULL
    command = [str(FIX8_BIN), "-c", config, "-N", "GW1"]
    if next_expected_receive is not None:
        command += ["-R", str(next_expected_receive)]
    proc = subprocess.Popen(
        command,
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
            "and the retry on connect did not close the window; see docs/bug_list.md, BUG-0019")
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


def do_machine_kill_step(
    step: MachineKillStep,
    proc_by_name: dict[str, subprocess.Popen],
    log_dir: Path,
) -> tuple[bool, str, float]:
    """
    Kill every named process without waiting between them, then poll for the
    survivors taking over.  Nothing is restarted: a machine that has stopped
    does not come back on its own, and a test that restarts these processes is
    testing something else.
    """
    label = ", ".join(step.proc_names)
    poll_from = file_end(log_dir / step.ready_log_name) if step.ready_log_name else 0

    log(f"  SIGKILL -> every process on one machine: {label}")
    killed = []
    for name in step.proc_names:
        proc = proc_by_name.get(name)
        if proc is None:
            die(f"machine kill: '{name}' was never started")
        if proc.poll() is not None:
            die(f"machine kill: '{name}' had already exited before the kill")
        proc.kill()
        killed.append((name, proc))

    for name, proc in killed:
        proc.wait()
    log(f"  {len(killed)} process(es) confirmed dead, and none will be restarted")

    if step.ready_log_name is None:
        time.sleep(step.settle_secs)
        return True, label, 0.0

    markers = " + ".join(repr(m) for m in step.ready_markers)
    log(f"  Waiting for {step.ready_log_name}: {markers} (timeout {step.ready_timeout:.0f}s) ...")
    found, elapsed, _ = poll_log_for(
        log_dir / step.ready_log_name, *step.ready_markers,
        timeout=step.ready_timeout, from_byte=poll_from,
    )
    if not found:
        die(f"machine kill: nothing on the surviving machine took over within {step.ready_timeout:.0f}s")
    log(f"  the surviving machine took over ({elapsed:.1f}s)")
    time.sleep(step.settle_secs)
    return True, label, elapsed


def do_isolate_step(
    step: IsolateStep,
    proc_by_name: dict[str, subprocess.Popen],
    log_dir: Path,
) -> tuple[bool, str, float]:
    """Freeze the named process, wait for its peer to take over, then let it run again."""
    proc = proc_by_name.get(step.proc_name)
    if proc is None or proc.poll() is not None:
        die(f"isolate: '{step.proc_name}' is not running")

    own_log = log_dir / f"{step.proc_name}.log"
    resume_from = 0
    takeover_from = file_end(log_dir / step.takeover_log_name)

    log(f"  SIGSTOP -> {step.proc_name} (PID {proc.pid}): alive, holding its sockets, renewing nothing")
    os.kill(proc.pid, signal.SIGSTOP)

    markers = " + ".join(repr(m) for m in step.takeover_markers)
    log(f"  Waiting for {step.takeover_log_name}: {markers} (timeout {step.takeover_timeout:.0f}s) ...")
    found, elapsed, _ = poll_log_for(
        log_dir / step.takeover_log_name, *step.takeover_markers,
        timeout=step.takeover_timeout, from_byte=takeover_from,
    )
    if not found:
        os.kill(proc.pid, signal.SIGCONT)
        die(f"isolate: the peer did not take over within {step.takeover_timeout:.0f}s")
    log(f"  the peer took over ({elapsed:.1f}s) while {step.proc_name} still believed it was entitled to act")

    survivor_log = log_dir / step.takeover_log_name
    refusal_from = file_end(survivor_log)
    log(f"  SIGCONT -> {step.proc_name}: it wakes still believing it may act, and says so on the wire")
    os.kill(proc.pid, signal.SIGCONT)

    markers = " + ".join(repr(m) for m in step.refusal_markers)
    log(f"  Waiting for the survivor to refuse it: {markers} (timeout {step.refusal_timeout:.0f}s) ...")
    found, refuse_elapsed, _ = poll_log_for(
        survivor_log, *step.refusal_markers,
        timeout=step.refusal_timeout, from_byte=refusal_from,
    )
    if not found:
        die("isolate: the survivor never refused the superseded instance -- either it accepted what "
            f"it sent, or the instance sent nothing, within {step.refusal_timeout:.0f}s")
    log(f"  the survivor refused it on its superseded generation ({refuse_elapsed:.1f}s)")

    time.sleep(step.settle_secs)

    # And the entitlement must not have gone back.
    tail = survivor_log.read_text(errors="replace")[refusal_from:]
    for line in tail.splitlines():
        if all(m in line for m in step.forbidden_markers):
            die(f"isolate: the survivor gave the entitlement back to a superseded instance: {line.strip()}")
    log(f"  the survivor kept the entitlement -- checked {len(tail.splitlines())} line(s) since the resume")
    return True, step.proc_name, elapsed


def do_restart_step(
    step: RestartStep,
    proc_by_name: dict[str, subprocess.Popen],
    app_procs: list[tuple[str, subprocess.Popen]],
    launch_table: list,
    bin_dir: Path,
    log_dir: Path,
    var_dir: Path,
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

    if step.damage_region:
        # Overwrite the header, which is where the store keeps what it is and how it is laid
        # out. A region whose header does not describe what the engine is configured to read is
        # refused outright rather than read as though it did -- reading it the wrong way round
        # would produce orders that are wrong in ways nothing downstream could detect.
        region = var_dir / step.damage_region
        if not region.is_file():
            die(f"damaged region: {region} does not exist, so there is nothing to damage and "
                "the scenario would pass without testing anything")
        with open(region, "r+b") as handle:
            handle.write(b"\xDE\xAD\xBE\xEF" * 8)
        log(f"  Damaged the open-order region at {region.name}, so the engine cannot read it")

    if step.down_secs > 0:
        log(f"  Leaving {step.proc_name} down for {step.down_secs:.0f}s, so that the absence is real")
        time.sleep(step.down_secs)

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
    run_dir = prefix / "run"

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
        if isinstance(s, IsolateStep):
            return f"ISOLATE:{s.proc_name}"
        if isinstance(s, MachineKillStep):
            return "MACHINE:" + "+".join(s.proc_names)
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

    # High availability off: the switch is written into every deployed config, and the components
    # that exist only to arbitrate are not started. Both halves are needed. Skipping the launches
    # without setting the switch is precisely BUG-0061 -- the sequencer waits for an arbiter that
    # never arrives and forwards nothing -- and setting the switch without skipping the launches
    # leaves an arbiter that now refuses to start.
    # The absence limit, set on every launch for the same reason. Scenarios that do not test
    # the rule get the deployed figure back, so one that shortened it cannot leave it short.
    set_installed_absence_limit(prefix, scenario.absence_limit_seconds)

    changed = set_installed_ha(prefix, not scenario.ha_disabled)
    if scenario.ha_disabled:
        log(f"  HA OFF: [ha] switched off in {changed} deployed config(s)")
        keep = {"witness": False, "arbiter_primary": False, "arbiter_secondary": False}
        launch_table = [entry for entry in launch_table
                        if keep.get(entry[0], True)
                        and (not entry[0].startswith("sequencer_") or entry[0] in scenario.ha_off_sequencers)]
        log("  HA OFF: launching " + ", ".join(entry[0] for entry in launch_table))
    elif changed:
        log(f"  [ha] restored to on in {changed} deployed config(s)")

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
        elif scenario.assert_open_orders_survive:
            # Zero, not the default: the question is what the engine does with the cancels,
            # and a grace period only delays asking it. A value left behind in the database
            # by another scenario would otherwise decide how long this one waits.
            log("=== Provisioning cancel-on-disconnect with no grace period ===")
            provision_cancel_on_disconnect(FIX8_COMP_ID, 0)
        else:
            # Every other scenario needs the baseline orders to KEEP RESTING when a session
            # drops, because those orders are what a failover promotes with and what a restart
            # recovers. With no grace period the gateway cancels the lot the instant a session
            # goes, and the scenario proceeds against an empty book having tested nothing.
            #
            # Provisioned rather than inherited. Until 2026-08-31 a scenario that set nothing
            # ran with whatever the last one left in the database, so its meaning depended on
            # what preceded it: scenario 21 after 50 found a zero grace period, dropped its
            # baseline session, and promoted with an empty book; scenario 12 after 50 had its
            # whole book cancelled the moment Phase 5 restarted the session, and the recovery
            # orders queued behind that burst until the timeout. Both looked like defects in
            # the venue and were defects in the harness.
            log("=== Provisioning cancel-on-disconnect so the baseline orders keep resting ===")
            provision_cancel_on_disconnect(FIX8_COMP_ID, _PROVISIONED_GRACE_PERIOD_SECONDS)

        # Likewise for session provisioning: the comp id is pinned to the instance this
        # harness runs, so the baseline session is admitted and the gateway has real
        # numbers to name. The scenario then moves it elsewhere and requires a refusal.
        # Always, not only for the scenarios that use it: a store left by an earlier run would
        # have a client resume a thousand messages ahead of a venue that has just started.
        clear_fix8_persisted_state()

        # And the venue's own memory of what it was holding, for the same reason: a scenario
        # that began with the previous one's open orders would be testing something nobody
        # wrote. See clear_open_order_regions.
        clear_open_order_regions(prefix)

        if scenario.assert_resend_recovery:
            write_no_reset_fix8_config()

        if scenario.assert_inflight_recovery:
            write_fix8_variant(_INFLIGHT_A_CFG, keep_sequence_numbers=True, ignore_logon_sequence_check=True,
                               persist_to_disk=_FIX8_PERSIST_DB)

        # Written for every scenario, not only the one that logs on afresh. Phase 5 also needs
        # it after a matching engine restart, where the baseline orders are still open and the
        # baseline comp id could only send duplicates of them. Generating one small file is
        # cheaper than working out in advance which scenarios will want it.
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
            proc = launch_app(name, bin_name, config, bin_dir, log_dir,
                              run_dir if name in scenario.supervised else None)
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

        # Which sequencer is expected to lead. Normally the primary; with high availability off it
        # is whichever instance was started, because which FILE a sequencer was started from stops
        # meaning anything once there is no peer and no arbiter to give the role weight.
        seq_ready_name = scenario.ha_off_sequencers[0] if scenario.ha_disabled else "sequencer_primary"
        seq_ready_log = log_dir / f"{seq_ready_name}.log"

        log(
            f"  Polling {seq_ready_name}.log for leader election "
            f"(timeout {args.ready_timeout:.0f}s) ..."
        )
        found, elapsed, _ = poll_log_for(
            seq_ready_log, _SEQ_ROLE, _TO_LEADER,
            timeout=args.ready_timeout, from_byte=0,
        )
        if not found:
            die(
                f"{seq_ready_name} did not elect leader within "
                f"{args.ready_timeout:.0f}s — check ha_enabled in sequencer.toml"
            )
        log(f"  {seq_ready_name}: leader elected ({elapsed:.1f}s)")

        # No arbiter is started with high availability off, and none is wanted. Polling for one
        # would spend ten seconds per scenario waiting for a process this run deliberately did not
        # launch, and print a line that reads as though something were missing.
        if scenario.ha_disabled:
            # No arbiter is started and none is wanted. But the ten seconds spent polling for one
            # were load-bearing by accident: they were how long the matching engine had to finish
            # starting before the baseline burst went out. Removing that poll made the first
            # thousand orders arrive at a sequencer with no engine connected, which deferred them
            # -- and a cold-starting engine applies nothing it was sent while it was away
            # (BUG-0064), so they were never processed and the scenario timed out waiting.
            #
            # Waiting for the engine says what was actually meant. An incidental delay that
            # happens to be long enough is not a wait.
            log("  No arbiter expected (high availability off); waiting for the matching engine ...")
            found, elapsed, _ = poll_log_for(me_log, *_ME_READY_MARKERS,
                                             timeout=_ME_READY_TIMEOUT, from_byte=0)
            if not found:
                die(f"matching engine not ready within {_ME_READY_TIMEOUT:.0f}s with high "
                    "availability off -- orders sent now would be deferred and lost.")
            log(f"  matching engine ready ({elapsed:.1f}s)")
        else:
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
        if scenario.skip_baseline_orders:
            log("=== Phase 3: skipped — this scenario's subject is a venue that cannot trade ===")
            log("")
        before_total = 0 if scenario.skip_baseline_orders else args.orders_before * 1000
        log(f"=== Phase 3: {before_total} baseline orders ===")

        # The resend scenario needs a client that keeps its own sequence numbering; every
        # other scenario uses the stock one, which resets it on each logon.
        baseline_config = FIX8_CFG
        baseline_output: Path | None = None
        bounded_output = log_dir / "f8test_bounded_resend.txt"
        if scenario.assert_bounded_resend:
            # The member's own record of what it was sent, which is what the reply is checked
            # against. Captured from the start, because the numbers asked about are baseline ones.
            baseline_output = bounded_output
        elif scenario.assert_resend_recovery:
            baseline_config = FIX8_NO_RESET_CFG
            # Captured because the gap is measured backwards from where the venue's numbering
            # actually reached, and this is where that is visible without asking the venue.
            baseline_output = log_dir / "f8test_resend_baseline.txt"
        elif scenario.assert_inflight_recovery:
            baseline_config = _INFLIGHT_A_CFG
        if scenario.skip_baseline_orders:
            f8proc = None
        else:
            f8proc = send_burst(args.orders_before, gw_log, baseline_config, baseline_output)
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
        if orders_during > 0 and not scenario.extra_steps and f8proc is not None:
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
            # Let the gateway report this session to the sequencer at least once before it is
            # killed. Without the pause the whole session lives and dies inside one reporting
            # interval, so the sequencer holds no record of which of the member's numbers held a
            # report -- and the surviving instance, having no provenance, correctly gap-fills
            # the lot. That is real behaviour and it is the DEGENERATE case: it is what the
            # venue does when a gateway dies seconds after a member logs on, and it exercises
            # none of the recovery this scenario is named for. A failover worth testing happens
            # to a session that has been up longer than an interval, which is what this makes it.
            log(f"=== Letting the gateway report the session before killing it "
                f"({_INFLIGHT_REPORT_SETTLE:.0f}s) ===")
            time.sleep(_INFLIGHT_REPORT_SETTLE)

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
            elif isinstance(step, IsolateStep):
                failover_occurred, label, elapsed = do_isolate_step(step, proc_by_name, log_dir)
                kill_results.append((failover_occurred, label, elapsed))
                phase4_results.append(("kill", failover_occurred, label, elapsed))
            elif isinstance(step, MachineKillStep):
                failover_occurred, label, elapsed = do_machine_kill_step(step, proc_by_name, log_dir)
                kill_results.append((failover_occurred, label, elapsed))
                phase4_results.append(("kill", failover_occurred, label, elapsed))
            elif isinstance(step, RestartStep):
                elapsed = do_restart_step(
                    step, proc_by_name, app_procs, launch_table, bin_dir, log_dir, prefix / "var",
                )
                restart_results.append((step.proc_name, elapsed))
                phase4_results.append(("restart", step.proc_name, elapsed))
            elif isinstance(step, SupervisedKillStep):
                pid_path = run_dir / f"{step.proc_name}.pid"
                if not pid_path.is_file():
                    die(f"{step.proc_name} is not supervised: no {pid_path}")
                before = int(pid_path.read_text().strip())
                log(f"  SIGKILL -> {step.proc_name} component PID {before} (its launcher should restore it)")
                try:
                    os.kill(before, signal.SIGKILL)
                except ProcessLookupError:
                    die(f"{step.proc_name} PID {before} was already gone")

                started = time.monotonic()
                after = before
                while time.monotonic() - started < step.restart_timeout:
                    if pid_path.is_file():
                        try:
                            current = int(pid_path.read_text().strip())
                        except ValueError:
                            current = before
                        if current != before:
                            after = current
                            break
                    time.sleep(0.05)
                if after == before:
                    die(f"{step.proc_name} was not restarted within {step.restart_timeout:.0f}s "
                        f"-- the launcher did not replace PID {before}")
                elapsed = time.monotonic() - started
                log(f"  {step.proc_name} restarted by its launcher as PID {after} ({elapsed:.1f}s)")
                phase4_results.append(("supervised restart", step.proc_name, elapsed))
                time.sleep(step.settle_secs)
            elif isinstance(step, AssertAbsentStep):
                markers_repr = " + ".join(repr(m) for m in step.markers)
                log(f"  VERIFY: {step.description}")
                log(f"    waiting {step.after_secs:.0f}s, then requiring {step.log_name} NOT to contain {markers_repr} ...")
                time.sleep(step.after_secs)
                seen, _, _ = poll_log_for(
                    log_dir / step.log_name, *step.markers,
                    timeout=0.0,
                    from_byte=step.from_byte,
                )
                if seen:
                    die(f"Verification failed: {step.description} -- {step.log_name} contains {markers_repr}")
                log("    confirmed absent")
                phase4_results.append(("verify", step.description, step.after_secs))
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
        #   ME restart   → count accepted NOS on the restarted engine's log from the
        #                  point it came ready.
        #   orders_during> 0 → some in-flight orders may have been processed
        #                  (or lost) during Phase 4; scan from me_pos to find
        #                  the actual current ME-ORD count, then add after_total.
        #   otherwise    → simple cumulative: before_total + after_total.
        me_restarted = any(
            isinstance(s, RestartStep) and s.resets_me_counter
            for s in effective_steps
        )
        after_total  = orders_after * 1000

        if scenario.me_ha and scenario.recovery_on_primary:
            # No failover happened: the primary kept leadership, so recovery orders come back
            # to it rather than to the secondary. The scenario that needs this is the one where
            # a supervised restart beats the peer's promotion timeout -- the whole point being
            # that leadership never moved.
            recovery_log = me_log
            me_log_from  = file_end(me_log)
            count_based  = True
            after_target = after_total
        elif scenario.me_ha:
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
            # Counted rather than waiting for an absolute ME-ORD-N.
            #
            # The engine used to come back with its order numbering at zero, because it held
            # nothing across a restart, so ME-ORD-1000 named the thousandth recovery order.
            # It now reads its open orders back from a memory-mapped region and carries the
            # numbering forward past the highest it recovered -- a repeated number would name
            # two different orders in the reports a member reconciles against. So that name is
            # never issued a second time and waiting for it times out having tested nothing.
            #
            # Counting is what the me_ha branches above already do, for the same reason: it
            # holds however far the numbering was advanced. Reconciliation logs no accept
            # line, so the count is exactly the recovery orders.
            recovery_log = me_log
            count_based  = True
            after_target = after_total
            me_log_from  = file_end(me_log)
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
                # Under its own comp id, for the reason FIX8_RECOVERY_COMP_ID gives: the
                # baseline orders are still open, so the baseline comp id could only send
                # duplicates of them.
                #
                # It did not have to be until 2026-08-31. A restarted engine held nothing, so
                # ord1..ord1000 were free again and the same client could send them a second
                # time. The engine now reads its open orders back from a region and still holds
                # them, and rejects the repeat -- correctly, because two live orders under one
                # name cannot be told apart by anyone downstream. Exactly 1000 rejections, and
                # no accepted order, is what that looks like from here.
                log("  Restarting FIX session under the recovery comp id "
                    "(old session blocked, and its ClOrdIDs are still live) ...")
                stop_f8test(f8proc)
                f8proc = send_burst(0, gw_log, FIX8_RECOVERY_CFG)

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

        # ── ME-HA: the book a promotion resumes with ──────────────────────────
        # The matching engine does no matching: every accepted NewOrderSingle is added to
        # the book as OrdStatus=New and rests there until cancelled (see
        # MatchingEngineThread::handle_new_order_single). So at the ME-primary kill the
        # promoted secondary's replicated book is non-empty.
        #
        # It used to cancel that whole book on promotion and emit one seq_no=0 cancel
        # ExecutionReport per resting order. It no longer does. The secondary writes every
        # replicated order into a memory-mapped region of its own, so after reconciling the
        # sequencer's tail it can say what it holds, and an order still on the book is
        # genuinely outstanding -- R-0018 and R-0073, and
        # docs/durability/open_order_checkpoint.md.
        #
        # So this asserts the promotion KEPT the book, which is the member-visible promise,
        # and that no ER was dropped for a missing conn id.
        #
        # WHAT THIS NO LONGER COVERS, deliberately recorded rather than lost: the cancel
        # burst was also the only exerciser of seq_no=0 ER routing via the WalRecord
        # envelope, where the originating session's connection id rides on the envelope
        # because the sequencer has no order sequence to resolve. That path still exists and
        # is still needed -- it is how R-0123's cancel-each-and-halt will report -- but
        # nothing here reaches it until that halt is built. A scenario for it belongs with
        # that work.
        #
        # Skipped when leadership never moved: there is no promotion, so no book to keep.
        if scenario.me_ha and not scenario.assert_gateway_orphaned and not scenario.recovery_on_primary:
            # A short timeout: this is waiting to confirm the line is absent, and the
            # promotion has already been observed by the time the check runs.
            cancel_count = me_cancel_on_failover_count(me_secondary_log, from_byte=me_secondary_pos_pre_kill, timeout=2.0)
            if cancel_count > 0:
                die(f"ME-HA: the promoted secondary cancelled {cancel_count} order(s) on promotion. "
                    "It should keep them: its region and the sequencer's tail say what it holds, so "
                    "an order still on the reconciled book is genuinely outstanding (R-0073). "
                    "Cancelling everything is the answer only when the region cannot be used, which "
                    "is R-0102 and R-0123 and is a halt, not a resumption.")

            resumed = me_resumed_book_size(me_secondary_log, from_byte=me_secondary_pos_pre_kill)
            if resumed is None:
                die("ME-HA: the promoted secondary logged no 'resuming as leader' line -- "
                    "cannot confirm what book it resumed with")
            if resumed <= 0:
                die(f"ME-HA: the promoted secondary resumed as leader holding {resumed} order(s). "
                    "The baseline orders rest in the book and replicate, so it should hold them: "
                    "an empty book here means the region was not recovered or was recovered empty.")
            log(f"  ME-HA: the promoted secondary resumed as leader holding {resumed} order(s), "
                "none cancelled -- OK")

            dropped = count_log_marker(gw_log, "has no gateway_session_conn_id -- dropping", from_byte=gw_pos_pre_kill)
            if dropped > 0:
                die(f"ME-HA: gateway dropped {dropped} ExecutionReport(s) for a missing "
                    "gateway_session_conn_id -- ER routing via the envelope is broken")

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
        # WHEN SESSION HANDOVER LANDS (steps 3b-6 of docs/availability/gateway_ha.md) THIS
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
                    "scenario's assertions rather than removing them. See docs/availability/gateway_ha.md.")
            dropped_ers = count_log_marker(seq_primary_log, drop_marker, from_byte=seq_primary_pos_pre_kill)
            log(f"  gateway orphan: sequencer dropped {dropped_ers} ER(s) bound for the dead "
                f"instance 1, first after {elapsed:.1f}s -- not rerouted to b")

            # The load-bearing assertion: b was alive throughout, and the question is only whether
            # anything reached it.
            #
            # This used to count GW-PROGRESS lines and require zero. That stopped being a test of
            # traffic on 2026-08-28, when the line gained a timer so that a stalled venue keeps
            # reporting -- BUG-0009. A gateway serving nobody now emits one every five seconds, all
            # of them zeros, so the presence of a line says nothing and its CONTENTS say everything.
            #
            # Worth noting for whoever changes that line next: its wording is a declared test
            # contract, and this scenario shows the wording is not the whole contract. When it is
            # emitted matters too.
            b_traffic = [line for line in _gateway_progress_lines(gw_b_log)
                         if line["accounted"] > 0 or line["nos_received"] > 0]
            if b_traffic:
                die(f"gateway orphan: instance b reported traffic it should never have seen -- "
                    f"{b_traffic[-1]}. It was sent no orders of its own, so either instance a's "
                    "reports were rerouted to it -- which nothing implements -- or session handover "
                    "now exists and this scenario's assertions need inverting, not removing.")
            log(f"  gateway orphan: instance b reported only idle progress lines "
                f"({count_log_marker(gw_b_log, 'GW-PROGRESS')} of them, all zeros) -- it inherited "
                "nothing from a -- OK")

            # The member is never told: the orders were cancelled in the book, and the
            # reports saying so had nowhere to go.
            log(f"  gateway orphan: {cancel_count} order(s) cancelled in the book, "
                "0 cancel reports delivered to any client -- orders silently orphaned")

        # ── A record of what it held that cannot be read ──────────────────────
        # R-0102 and R-0123. Three things must hold, and the third is the one that matters
        # most, because it is the one an implementation is most likely to get wrong in the
        # helpful direction.
        if scenario.assert_damaged_region:
            region_pos = 0  # the log was deleted on restart, so read it whole

            refused = poll_log_for(me_log, "cannot be used", "R-0102",
                                   timeout=args.failover_timeout, from_byte=region_pos)[0]
            if not refused:
                die("damaged region: the engine did not refuse the damaged region. Reading it "
                    "the wrong way round produces orders that are wrong in ways nothing "
                    "downstream can detect, which is why it is refused outright (R-0102).")
            log("  damaged region: refused rather than read")

            # The region is the only account of whatever went wrong with it. A venue that
            # overwrites it leaves nobody able to find out.
            kept = prefix / "var" / "matching_engine_open_orders.region.unusable"
            if not kept.is_file():
                die(f"damaged region: {kept.name} was not kept. Whatever made the region "
                    "unreadable is the evidence, and starting a fresh region over the top of it "
                    "destroys the only account of the failure.")
            log(f"  damaged region: kept aside as {kept.name}")

            # The venue must say it cannot account for what it was holding. The sequencer's log
            # is truncated as it is consumed, so it does not reach the orders taken earlier in
            # the day, and the engine has no way to name them.
            cannot_account = poll_log_for(me_log, "cannot account for what the venue was holding",
                                          timeout=args.failover_timeout, from_byte=region_pos)[0]
            if not cannot_account:
                die("damaged region: the engine did not say it cannot account for what it was "
                    "holding. The sequencer's log is truncated, so it does not reach the orders "
                    "taken earlier in the day; resuming on what it does reach would conceal "
                    "exactly the loss being recovered from (R-0123).")
            log("  damaged region: the venue says it cannot account for what it was holding")

            halted = poll_log_for(me_log, "trading is halted", "a person must lift this",
                                  timeout=args.failover_timeout, from_byte=region_pos)[0]
            if not halted:
                die("damaged region: the engine did not halt. Admitting it cannot say is the "
                    "last resort and is still better than silence, but only if it stops (R-0123).")
            log("  damaged region: trading halted")

            # And it must NOT have cancelled. Cancelling the orders it can name would leave
            # every earlier one unmentioned -- the same silence, reached while appearing to
            # have dealt with it.
            cancelled = me_cancel_on_failover_count(me_log, from_byte=region_pos, timeout=2.0)
            if cancelled > 0:
                die(f"damaged region: the engine cancelled {cancelled} order(s) while unable to "
                    "account for what it held. Cancelling only what it can name leaves every "
                    "order it cannot name unmentioned, which is the silence R-0020 exists to "
                    "prevent, arrived at while appearing to have done something about it.")
            log("  damaged region: nothing cancelled, because not everything could be named -- OK")

        # ── A long absence: cancelled, reported, halted ───────────────────────
        # The engine was kept down past the period the venue says it may be absent for, with
        # orders resting. R-0117 requires all three things, and each is checked separately
        # because any one of them alone would be a worse outcome than doing nothing:
        # cancelling without reporting loses the member's position silently, reporting without
        # halting carries on as though nothing happened, and halting without cancelling leaves
        # the stale orders in place.
        if scenario.assert_absence_halt:
            me_pos_after_restart = 0  # the log was deleted on restart, so read it whole

            cancelled = me_cancel_on_failover_count(me_log, from_byte=me_pos_after_restart,
                                                    timeout=args.failover_timeout)
            if cancelled <= 0:
                die("long absence: the engine did not cancel the orders it recovered "
                    f"(count={cancelled}). It was down for longer than the venue says it may be "
                    "with orders open, so every one of them should have been cancelled -- their "
                    "members were locked out of them while the market moved (R-0117).")
            log(f"  long absence: {cancelled} order(s) cancelled")

            halted = poll_log_for(me_log, "trading is halted", "a person must lift this",
                                  timeout=args.failover_timeout, from_byte=me_pos_after_restart)[0]
            if not halted:
                die("long absence: the engine cancelled the book but did not halt. Carrying on "
                    "as though nothing had happened is what the halt exists to prevent (R-0117); "
                    "lifting it is a judgement and needs a person (R-0023).")
            log("  long absence: trading halted")

            # The reports have to reach the members. A cancel nobody is told about is the
            # silent loss this whole chapter exists to prevent, and it looks identical from
            # here to a cancel that worked.
            sent, nos_received = gateway_progress_totals(gw_log)
            if sent is None:
                die("long absence: no GW-PROGRESS line in the gateway log -- cannot confirm the "
                    "cancellation reports were delivered to the member")
            if sent < nos_received + cancelled:
                die(f"long absence: the gateway sent {sent} report(s) for {nos_received} order(s), "
                    f"but {cancelled} cancellation(s) were emitted. Expected at least "
                    f"{nos_received + cancelled}: a member that is not told its order was "
                    "cancelled cannot tell that from the venue having lost it (R-0117, R-0020).")
            log(f"  long absence: {sent} report(s) delivered for {nos_received} order(s) -- "
                f"includes the {cancelled} cancellation(s)")

            # And the halt has to mean something. An order sent now must be refused, in the
            # reply to that order, which is how a member trading into a halt finds out.
            log("=== Sending an order into the halt, which must be refused ===")
            refused_pos = file_end(me_log)
            f8proc.stdin.write(b"T\n")
            f8proc.stdin.flush()
            refused = poll_log_for(me_log, "trading is halted -- refusing NOS",
                                   timeout=args.recovery_timeout, from_byte=refused_pos)[0]
            if not refused:
                die("long absence: the venue accepted an order while halted. A halt that still "
                    "takes orders is not a halt, and the member is not told the venue is closed "
                    "to business (R-0117).")
            log("  long absence: an order sent into the halt was refused -- OK")

        # ── Do the member's open orders survive a restart of the matching engine? ──
        # The member-visible form of the question is whether those orders are still
        # cancellable, so the probe is a cancel rather than an inspection of anything
        # internal. Dropping the session makes the gateway send one for every order it
        # believes the member has open, and the matching engine's own log says what became
        # of each: a cancel report for an order it holds, or a rejection naming it as an
        # order it does not recognise.
        if scenario.assert_open_orders_survive:
            gw_pos = file_end(gw_log)
            me_pos = file_end(me_log)

            log("=== Dropping the client session, so the gateway cancels what it believes is open ===")
            stop_f8test(f8proc)
            f8proc = None

            found, _, _ = poll_log_for(
                gw_log, "disconnected with", "open order",
                timeout=_CANCEL_GRACE_HOLD_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("open orders: the gateway never reported the dropped session's open orders, so "
                    "nothing was cancelled and the question was not asked")

            # Whichever answer comes back, it comes back from the engine.
            deadline = time.monotonic() + _OPEN_ORDER_CANCEL_TIMEOUT
            unknown = cancelled = 0
            while time.monotonic() < deadline:
                tail = me_log.read_text(errors="replace")[me_pos:]
                unknown = tail.count("UnknownOrder")
                cancelled = tail.count("sent cancel ER")
                if unknown or cancelled:
                    break
                time.sleep(1.0)

            if unknown:
                die(f"open orders: the matching engine refused {unknown} of the member's cancels as "
                    "orders it does not recognise. The orders open before it restarted are gone, and "
                    "neither the member nor the gateway was told -- see R-0018, and docs/bug_list.md "
                    "BUG-0064")
            if not cancelled:
                die("open orders: the engine neither cancelled nor refused anything within "
                    f"{_OPEN_ORDER_CANCEL_TIMEOUT:.0f}s -- the cancels never reached it, so the "
                    "question was not asked")
            log(f"  the engine cancelled {cancelled} order(s) and refused none: the orders open "
                "before the restart survived it -- OK")

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

        # ── What the venue does about a member that is wrong (scenario 41) ────
        # Driven by the raw client, which has no session layer and will send what fix8 will not.
        if scenario.assert_inbound_sequence:
            log("=== Driving the venue's inbound sequence checking ===")
            stop_f8test(f8proc)
            f8proc = None
            time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            gateway_port = gateway_listen_port(prefix, "a")

            def open_member() -> "FixRawClient":
                """A logged-on member starting its numbering afresh, so the venue's expectation
                is known exactly rather than inherited from whatever ran before."""
                member = FixRawClient("127.0.0.1", gateway_port, FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
                member.connect()
                member.logon(reset_seq_num=True)
                if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                    member.close()
                    die("inbound sequence: the raw client could not log on. Check the gateway is "
                        f"listening on {gateway_port} and that credentials.toml has {FIX8_COMP_ID}.")
                return member

            # 1. A number above what the venue expects: asked about, and NOT processed.
            member = open_member()
            member.new_order_single("seq-base")
            if member.receive_until("8", timeout=_RAW_REPLY_TIMEOUT) is None:
                die("inbound sequence: an in-sequence order produced no ExecutionReport, so the "
                    "session is not working and nothing below would mean anything.")
            expected_next = member.next_send_seq_num  # the venue expects this from us now

            member.new_order_single("seq-ahead", seq_num=expected_next + 40)
            request = member.receive_until("2", timeout=_RAW_REPLY_TIMEOUT)
            if request is None:
                die(f"inbound sequence: an order numbered {expected_next + 40} arrived when the "
                    f"venue expected {expected_next} and it asked for nothing. A member can lose "
                    "messages without either side noticing -- this is BUG-0038 itself.")
            if request.get(7) != str(expected_next):
                die(f"inbound sequence: the venue asked from BeginSeqNo={request.get(7)}, not "
                    f"{expected_next}. It must ask from the first number it is missing.")
            if member.receive_until("8", timeout=_RAW_SILENCE_TIMEOUT) is not None:
                die("inbound sequence: the order past the gap was processed. Nothing after a gap "
                    "may be, or a cancel can be applied to an order the venue never received.")
            log(f"  inbound sequence: a gap is asked about from {expected_next} and the message "
                "past it is not processed -- OK")

            # 2. A further message while the gap is open must not provoke a second request. A
            #    member answering the first ignores a second, and the session then waits forever.
            member.new_order_single("seq-ahead-2", seq_num=expected_next + 41)
            if member.receive_until("2", timeout=_RAW_SILENCE_TIMEOUT) is not None:
                die("inbound sequence: a second ResendRequest went out while the first was "
                    "outstanding. A member answering one ignores the other, and both sides then "
                    "wait on each other.")
            log("  inbound sequence: a further message while the gap is open provokes no second "
                "request -- OK")

            # 3. Filling the gap: processed, and the session carries on.
            for filled in range(expected_next, expected_next + 42):
                member.new_order_single(f"seq-fill-{filled}", seq_num=filled, poss_dup=True,
                                        orig_sending_time=_RAW_ORIG_SENDING_TIME)
            time.sleep(_RAW_CLIENT_SETTLE)
            filled_reports = [m for m in member.receive(timeout=_RAW_REPLY_TIMEOUT) if m.get(35) == "8"]
            if not filled_reports:
                die("inbound sequence: the member filled the gap and no order was processed. The "
                    "messages that close a gap are the venue's own recovery -- discarding them "
                    "leaves the session stuck.")
            log(f"  inbound sequence: filling the gap processed {len(filled_reports)} order(s) -- OK")
            member.close()

            # 4. Below expected and marked PossDupFlag: discarded, session kept.
            member = open_member()
            member.new_order_single("dup-base")
            member.receive_until("8", timeout=_RAW_REPLY_TIMEOUT)
            member.new_order_single("dup-retransmit", seq_num=2, poss_dup=True,
                                    orig_sending_time=_RAW_ORIG_SENDING_TIME)
            if member.receive_until("8", timeout=_RAW_SILENCE_TIMEOUT) is not None:
                die("inbound sequence: a retransmission below the expected number was forwarded as "
                    "a new order. The matching engine's duplicate-ClOrdID rejection is then the "
                    "only thing stopping it, which is the session layer's job -- see BUG-0038.")
            if member.receive_until("5", timeout=_RAW_SILENCE_TIMEOUT) is not None:
                die("inbound sequence: a retransmission marked PossDupFlag=Y ended the session. It "
                    "is the member saying 'you may already have this', not an error.")
            member.new_order_single("dup-after")
            if member.receive_until("8", timeout=_RAW_REPLY_TIMEOUT) is None:
                die("inbound sequence: the session stopped working after a marked retransmission.")
            log("  inbound sequence: a marked retransmission is discarded and the session carries "
                "on -- OK")
            member.close()

            # 5. Below expected and NOT marked: the session ends, and the member is told why.
            member = open_member()
            member.new_order_single("low-base")
            member.receive_until("8", timeout=_RAW_REPLY_TIMEOUT)
            member.new_order_single("low", seq_num=2)
            logout = member.receive_until("5", timeout=_RAW_REPLY_TIMEOUT)
            if logout is None:
                die("inbound sequence: a number below expected with no PossDupFlag did not end the "
                    "session. FIXT.1.1 calls it a serious error: the far side has gone backwards "
                    "and its state cannot be trusted.")
            if "too low" not in (logout.get(58) or ""):
                die(f"inbound sequence: the Logout said {logout.get(58)!r}, which does not tell the "
                    "member what was wrong with its numbering.")
            log(f"  inbound sequence: an unmarked low number ends the session -- {logout.get(58)!r} -- OK")
            member.close()

            # 6. No MsgSeqNum at all: rejected, and the counter must NOT advance. If it did, the
            #    next message would look like a gap and the member would be asked to resend
            #    something it had already sent -- silently, and on every validation failure.
            member = open_member()
            member.new_order_single("noseq-base")
            member.receive_until("8", timeout=_RAW_REPLY_TIMEOUT)
            member.new_order_single("noseq", omit_seq_num=True)
            if member.receive_until("3", timeout=_RAW_REPLY_TIMEOUT) is None:
                die("inbound sequence: a message with no MsgSeqNum drew no Reject.")
            member.new_order_single("noseq-after")
            if member.receive_until("8", timeout=_RAW_REPLY_TIMEOUT) is None:
                die("inbound sequence: the next in-sequence order was not processed, so the counter "
                    "advanced over a message the venue could not place. Every validation failure "
                    "would then cost the member a spurious resend.")
            if member.receive_until("2", timeout=_RAW_SILENCE_TIMEOUT) is not None:
                die("inbound sequence: the venue asked for a resend after a message with no "
                    "sequence number, so its counter had moved when it should not have.")
            log("  inbound sequence: a message with no MsgSeqNum is rejected and the counter does "
                "not move -- OK")
            member.close()

        # ── High availability off: a component that only arbitrates must refuse ──
        if scenario.ha_off_refuser:
            name = scenario.ha_off_refuser
            log(f"=== Starting {name} by hand against a venue with high availability off ===")
            config = (etc_dir / "arbiter" / "arbiter_primary.toml") if name == "arbiter" \
                else (etc_dir / "witness" / "witness.toml")
            refuser_log = log_dir / f"{name}_refusal_probe.log"
            refuser_log.unlink(missing_ok=True)
            environment = dict(os.environ)
            completed = subprocess.run([str(bin_dir / name), str(refuser_log), str(config)],
                                       capture_output=True, text=True, timeout=30, env=environment,
                                       check=False)
            if completed.returncode == 0:
                die(f"ha off: {name} started and stayed up in a venue with [ha] enabled = false. "
                    "Not launching it is what devenv.py does; refusing is what covers a "
                    "hand-started process or a stale supervisor manifest. An operator who sees it "
                    "running will believe the venue has the mechanism it names.")
            said = (refuser_log.read_text(errors="replace") if refuser_log.is_file() else "") \
                + completed.stdout + completed.stderr
            if "refusing to start" not in said or "[ha] enabled = false" not in said:
                die(f"ha off: {name} exited {completed.returncode} without saying why. Exiting is "
                    "not enough -- an operator finding a component that will not start needs the "
                    "reason and the fix in the message, or the venue looks broken rather than "
                    f"configured. What it said: {said.strip()[-300:]!r}")
            log(f"  ha off: {name} refused to start (exit {completed.returncode}) and named the "
                "reason -- OK")

        # ── High availability off: a secondary started alone ────────────────────
        if scenario.assert_ha_off_secondary_alone:
            log("=== A secondary sequencer started alone with high availability off ===")
            found, elapsed, _ = poll_log_for(log_dir / "sequencer_secondary.log", _SEQ_ROLE, _TO_LEADER,
                                             timeout=_HA_OFF_LEAD_TIMEOUT, from_byte=0)
            if not found:
                die("ha off: the secondary did not lead when started alone. Refusing to lead would "
                    "block the case someone actually turns high availability off to do -- run the "
                    "venue on the surviving machine after the primary's hardware has died.")
            log(f"  ha off: the secondary leads on its own ({elapsed:.1f}s) -- OK")

            # And what it cannot do, asserted so the gap is a stated fact rather than a surprise.
            #
            # The gateway sends to the secondary only inside `if (config_.ha_enabled)`, so with high
            # availability off it talks to the primary and nothing else. A secondary-only venue
            # therefore leads and trades nothing.
            #
            # Recorded here rather than fixed, because the obvious fix is wrong: making the gateway
            # send to both would put the same order into two books in scenario 47, where both
            # instances lead. What a non-HA venue needs is to be TOLD which single sequencer to use,
            # and that is a configuration decision this entry does not settle. See BUG-0061.
            # Asked of the member rather than of a log marker. The gateway CONNECTS to both
            # sequencers regardless -- connecting and forwarding are different things, and an
            # earlier version of this assertion tested the wrong one and failed. What is true is
            # that `forward_pdu_to_sequencers` sends to the secondary only inside
            # `if (config_.ha_enabled)`, so with the switch off nothing the member sends is
            # forwarded anywhere.
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"),
                                  FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("ha off: the member could not even log on to a secondary-only venue, so this "
                    "assertion cannot tell an unreachable sequencer from a broken gateway.")
            log("  ha off: the member logs on -- the gateway is up and serving")
            member.new_order_single("ha-off-secondary")
            answered = member.receive_until("8", timeout=_HA_OFF_UNREACHABLE_SETTLE)
            member.close()
            if answered is not None:
                die(f"ha off: the order was answered (OrdStatus={answered.get(39)}) on a "
                    "secondary-only venue. If the gateway has deliberately been made to forward to "
                    "the secondary with high availability off, check scenario 47 first -- sending "
                    "to both when both lead would put the same order into two books.")
            log("  ha off: the member logs on and its order is never answered, because the gateway "
                "forwards only to a primary that is not running. A secondary-only venue leads and "
                "trades nothing -- the known gap, recorded in BUG-0061 -- OK")

        # ── High availability off: two sequencers started together both lead ─────
        if scenario.assert_ha_off_both_lead:
            log("=== Two sequencers started with high availability off ===")
            leading = []
            for name in scenario.ha_off_sequencers:
                found, _, _ = poll_log_for(log_dir / f"{name}.log", _SEQ_ROLE, _TO_LEADER,
                                           timeout=_HA_OFF_LEAD_TIMEOUT, from_byte=0)
                if found:
                    leading.append(name)
            if len(leading) != len(scenario.ha_off_sequencers):
                die("ha off: expected every started sequencer to lead, and "
                    f"{leading} did out of {list(scenario.ha_off_sequencers)}. With no peer and no "
                    "arbiter an instance has nothing to defer to, and one that sits waiting is a "
                    "venue that will not trade -- which is BUG-0061 in the other direction.")
            log(f"  ha off: both instances lead ({', '.join(leading)}) -- OK")

            # Not split brain, and still a trap. Each instance advances its own WAL and burns
            # epochs from the state file that exists so a restart cannot reuse a spent generation.
            # The damage lands at the next start under high availability -- BUG-0062 -- so the
            # question here is whether the venue says anything at the time.
            announced = 0
            for name in scenario.ha_off_sequencers:
                text = (log_dir / f"{name}.log").read_text(errors="replace")
                if "ha_enabled=false" in text:
                    announced += 1
            if announced != len(scenario.ha_off_sequencers):
                die(f"ha off: only {announced} of {len(scenario.ha_off_sequencers)} instances said "
                    "why they were leading. Two leaders is the agreed behaviour and it is still a "
                    "trap: each burns epochs from its own state file and the damage lands at the "
                    "next start under high availability. An operator has to be able to see it now.")
            log(f"  ha off: both instances recorded ha_enabled=false as the reason -- OK")
            log("  NOTE: two leaders here is agreed behaviour, not split brain -- the gateway "
                "reaches only one sequencer with high availability off. What it costs is recorded "
                "as BUG-0062.")

        # ── The venue refuses what it cannot process ──────────────────────────
        # The matching engine is killed and not restarted. What is being tested is not that the
        # venue notices -- step 1 made it notice -- but that the noticing reaches the member.
        if scenario.assert_order_refusal:
            log("=== Refusing orders the venue cannot process ===")
            stop_f8test(f8proc)
            f8proc = None
            time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            gateway_port = gateway_listen_port(prefix, "a")

            me_proc = proc_by_name.get("matching_engine")
            if me_proc is None or me_proc.poll() is not None:
                die("order refusal: the matching engine is not running, so there is nothing to "
                    "take away and the scenario would pass without testing anything.")
            log(f"  SIGKILL -> matching_engine (PID {me_proc.pid}) -- and it will NOT be restarted")
            me_proc.kill()
            me_proc.wait()

            member = FixRawClient("127.0.0.1", gateway_port, FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("order refusal: the raw client could not log on. A test that cannot log on "
                    "looks identical to a venue that will not answer, so this is checked first.")

            def report_for(cl_ord_id: str, timeout: float) -> dict[str, str] | None:
                """Wait for the ExecutionReport belonging to this order, or None.

                Matched on ClOrdID rather than taking the first report to arrive, because the
                orders deferred earlier are still in the WAL: the moment an engine comes back it
                replays them and their reports land in the middle of this conversation. A test
                that took the next report would read one of those and conclude whatever it liked.
                """
                deadline = time.monotonic() + timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return None
                    report = member.receive_until("8", timeout=min(1.0, remaining))
                    if report is None:
                        continue
                    if report.get(11) == cl_ord_id:
                        return report

            def send_and_wait(cl_ord_id: str, timeout: float) -> dict[str, str] | None:
                member.new_order_single(cl_ord_id)
                return report_for(cl_ord_id, timeout)

            # 1. While a failover is still plausible the order is DEFERRED, and the member is told
            #    nothing at all.
            #
            #    Not "acknowledged": measured on 2026-08-28, a NewOrderSingle placed with no
            #    matching engine reachable produces no ExecutionReport whatsoever. The report is
            #    the engine's to send and there is no engine. The member is left holding an order
            #    in an unknown state, which it cannot cancel because a cancel needs the same
            #    engine -- worse than an acknowledgement, not better, because silence is not a
            #    state a risk system can reason about.
            #
            #    This half also guards the opposite defect. A venue that refused from the first
            #    order would pass a refusal-only test while rejecting orders during every routine
            #    failover, which members survive today. That would be a regression dressed as a fix.
            if send_and_wait("refuse-early", _REFUSAL_DEFERRED_SILENCE) is not None:
                die("order refusal: the first order after the engine died was answered. Deferring "
                    "is the RIGHT behaviour for the first seconds -- a promoted engine replays the "
                    "WAL and picks the order up -- so an immediate answer means either the venue "
                    "refused far too eagerly, or an engine is still alive and this scenario is "
                    "testing nothing.")
            log("  order refusal: the first order is deferred and the member is told nothing -- OK")

            # 2. Refused once the outage has outlived any failover.
            deadline = time.monotonic() + _REFUSAL_DEADLINE
            probe = 0
            refusal_text = ""
            began = time.monotonic()
            while True:
                probe += 1
                cl_ord_id = f"refuse-probe-{probe}"
                report = send_and_wait(cl_ord_id, _REFUSAL_DEFERRED_SILENCE)
                if report is not None:
                    if report.get(39) != "8":
                        die(f"order refusal: probe {probe} was answered with OrdStatus="
                            f"{report.get(39)}. The only answer a venue with no matching engine "
                            "can honestly give is a rejection.")
                    refusal_text = report.get(58, "")
                    break
                if time.monotonic() >= deadline:
                    member.close()
                    die(f"order refusal: {probe} orders were taken over "
                        f"{_REFUSAL_DEADLINE:.0f}s with no matching engine in existence, and the "
                        "member was told nothing about a single one of them. It cannot cancel "
                        "them either, because a cancel needs the engine that does not exist, so "
                        "it is left holding orders it can neither confirm nor withdraw. This is "
                        "BUG-0009 itself.")
                time.sleep(_REFUSAL_PROBE_INTERVAL)
            log(f"  order refusal: orders are refused after {time.monotonic() - began:.0f}s "
                f"({probe} probes), Text='{refusal_text}' -- OK")
            if not refusal_text:
                die("order refusal: the rejection carried no Text. A member told only that its "
                    "order was rejected cannot tell a venue outage from a bad order, and will "
                    "retry the one and not the other.")

            # 3. Cancels are refused too -- the half that reads as the wrong way round.
            member.order_cancel_request("refuse-cancel", "refuse-early")
            reply = member.receive_until("8", "9", timeout=_RAW_REPLY_TIMEOUT)
            if reply is None:
                die("order refusal: the cancel got no answer at all.")
            if reply.get(39) != "8" and reply.get(35) != "9":
                die(f"order refusal: the cancel came back OrdStatus={reply.get(39)} on MsgType="
                    f"{reply.get(35)}, which reads as accepted. A cancel needs the matching engine "
                    "exactly as an order does, and a member wrongly told its cancel succeeded "
                    "STOPS WATCHING the order -- worse off than one told it was refused.")
            log(f"  order refusal: cancels are refused too, Text='{reply.get(58, '')}' -- OK")

            # 4. The health line keeps reporting while nothing progresses.
            #
            # Before BUG-0009 step 2 this line was emitted once per N orders accounted, so when
            # accounting stalled the reporting stalled with it. It went quiet for 2m19s during the
            # incident. A line driven by progress cannot report the absence of progress.
            quiet_from = file_end(gw_log)
            log(f"  watching the gateway for {_REFUSAL_QUIET_WATCH:.0f}s with nothing in flight ...")
            time.sleep(_REFUSAL_QUIET_WATCH)
            quiet_lines = _gateway_progress_lines_since(gw_log, quiet_from)
            if len(quiet_lines) < 2:
                die(f"order refusal: only {len(quiet_lines)} GW-PROGRESS line(s) in "
                    f"{_REFUSAL_QUIET_WATCH:.0f}s of a degraded venue. The line has to be driven "
                    "by a clock, not by progress, or it goes silent exactly when it matters.")
            latest = quiet_lines[-1]
            if latest.get("refused", 0) < 1:
                die(f"order refusal: the progress line reports refused={latest.get('refused')} "
                    "after orders were demonstrably refused. The count an operator would watch "
                    "does not reflect what the members were told.")
            if latest.get("awaiting", 0) < 1:
                die("order refusal: awaiting=0 while orders deferred before the refusal began are "
                    "still unprocessed. Deferred and refused are different states and the line "
                    "must not fold them together -- only one of them is somebody's problem later.")
            log(f"  order refusal: {len(quiet_lines)} progress lines while nothing progressed, "
                f"awaiting={latest.get('awaiting')} refused={latest.get('refused')} "
                f"refused_cancels={latest.get('refused_cancels')} -- OK")

            # 5. Acceptance resumes on its own.
            #
            # No operator action. Requiring one was considered and rejected: this bug is precisely
            # a case where nobody was watching, and a recovery that depends on the watching which
            # has already failed is not safer.
            log("  restarting the matching engine -- nothing else is done")
            do_restart_step(_me_restart_step(), proc_by_name, app_procs, launch_table,
                            bin_dir, log_dir, prefix / "var")
            resumed_by = time.monotonic() + _REFUSAL_DEADLINE
            attempt = 0
            while True:
                attempt += 1
                report = send_and_wait(f"resume-{attempt}", _RAW_REPLY_TIMEOUT)
                if report is not None and report.get(39) == "0":
                    break
                if time.monotonic() >= resumed_by:
                    member.close()
                    die(f"order refusal: the venue was still refusing {_REFUSAL_DEADLINE:.0f}s "
                        "after a matching engine came back. Refusing has to lift by itself, or "
                        "the fix has turned a transient outage into one that needs a human.")
                time.sleep(_REFUSAL_PROBE_INTERVAL)
            log(f"  order refusal: acceptance resumed on its own after {attempt} probe(s), with no "
                "operator action -- OK")
            member.close()

        # ── A bounded resend, out of the middle of the member's history ───────
        # Nothing is killed. The member asks for a range it is entitled to ask for, and what
        # comes back is checked against its own record of what it was sent under those numbers.
        if scenario.assert_bounded_resend:
            gw_pos = file_end(gw_log)

            # Let the reports for the baseline settle in the client's output before it is read.
            # The ME accepting an order and the member receiving the report are separate events.
            time.sleep(_BOUNDED_RESEND_SETTLE)
            originals = {int(msg["MsgSeqNum"]): msg.get("ClOrdID", "")
                         for msg in _client_messages(bounded_output)
                         if msg.get("MsgType") == "8" and msg.get("MsgSeqNum", "").isdigit()
                         and not _fix_flag_is_set(msg.get("PossDupFlag"))}
            requested = range(_BOUNDED_RESEND_BEGIN, _BOUNDED_RESEND_END + 1)
            missing = [n for n in requested if n not in originals]
            if missing:
                die(f"bounded resend: the member was never sent {len(missing)} of the numbers it "
                    f"is about to ask about ({missing[:10]}). The range has to sit inside what "
                    "this session actually received, or the test asks the venue for messages that "
                    "never existed. Lower the range or raise --orders-before.")
            log(f"  bounded resend: the member holds its own record of numbers "
                f"{_BOUNDED_RESEND_BEGIN}..{_BOUNDED_RESEND_END}, against which the reply is checked")

            # f8test's 'R' reads BeginSeqNo then EndSeqNo from stdin. Asking on the live session
            # rather than after a reconnect, because nothing here is about recovery: it is about
            # whether the venue sends what was asked for.
            log(f"=== Asking for a bounded resend of {_BOUNDED_RESEND_BEGIN}..{_BOUNDED_RESEND_END} ===")
            for keystroke, prompt in (("R", "Enter BeginSeqNo"),
                                      (str(_BOUNDED_RESEND_BEGIN), "Enter EndSeqNo"),
                                      (str(_BOUNDED_RESEND_END), None)):
                f8proc.stdin.write(f"{keystroke}\n".encode())
                f8proc.stdin.flush()
                if prompt is not None and not _wait_for_client_prompt(bounded_output, prompt, _BOUNDED_RESEND_SETTLE):
                    die(f"bounded resend: the client never asked for '{prompt}' after being sent "
                        f"{keystroke!r}. Its menu reads keys in raw mode, so each line has to be "
                        "written only once it has asked for it.")

            found, elapsed, _ = poll_log_for(
                gw_log, f"ResendRequest BeginSeqNo={_BOUNDED_RESEND_BEGIN} EndSeqNo={_BOUNDED_RESEND_END}",
                timeout=_RESEND_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("bounded resend: the gateway never logged the request with both bounds. Either "
                    "the client did not send it, or EndSeqNo is not being read -- which was "
                    "BUG-0039, and this is the test that was owed for it.")
            log(f"  bounded resend: the gateway read both bounds off the request ({elapsed:.1f}s)")

            found, elapsed, _ = poll_log_for(
                gw_log, "resend complete", timeout=_RESEND_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("bounded resend: the gateway never finished the resend.")
            log(f"  bounded resend: the venue answered ({elapsed:.1f}s)")

            time.sleep(_BOUNDED_RESEND_SETTLE)
            resent = {int(msg["MsgSeqNum"]): msg.get("ClOrdID", "")
                      for msg in _client_messages(bounded_output)
                      if msg.get("MsgType") == "8" and msg.get("MsgSeqNum", "").isdigit()
                      and _fix_flag_is_set(msg.get("PossDupFlag"))}
            if not resent:
                die(f"bounded resend: the member received no resent report (see {bounded_output}).")

            # 1. Nothing outside the range. This is BUG-0039's assertion: a venue that ignores
            #    EndSeqNo replays to the head of the stream, which shows up here as numbers past
            #    the bound.
            beyond = sorted(n for n in resent if n not in requested)
            if beyond:
                die(f"bounded resend: {len(beyond)} message(s) came back outside the requested "
                    f"range {_BOUNDED_RESEND_BEGIN}..{_BOUNDED_RESEND_END}: {beyond[:10]}. The "
                    "member asked for a bounded range and was sent more than it asked for.")
            log(f"  bounded resend: {len(resent)} message(s) came back, none outside "
                f"{_BOUNDED_RESEND_BEGIN}..{_BOUNDED_RESEND_END} -- OK")

            # 2. And they are the right messages. Compared by ClOrdID against what the member
            #    itself received under those numbers, which is the only record of the fact that
            #    does not come from the venue vouching for itself.
            wrong = [(n, originals[n], resent[n]) for n in sorted(resent)
                     if n in originals and resent[n] != originals[n]]
            if wrong:
                sample = ", ".join(f"{n}: was {was!r}, resent {now!r}" for n, was, now in wrong[:5])
                die(f"bounded resend: {len(wrong)} of {len(resent)} resent message(s) carry a "
                    f"different order than the member was originally sent under that number "
                    f"({sample}). The numbering is right and the contents are not, so nothing on "
                    "the member's side distinguishes this from a correct resend -- see BUG-0053. "
                    "The sequencer returns the most recent reports for the session rather than "
                    "the ones the range names, which is only correct when the range is the tail.")
            log(f"  bounded resend: every one of the {len(resent)} resent message(s) carries the "
                "order the member was originally sent under that number -- OK")

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

            # A number the venue cannot replay, with reports on both sides of it. Without this
            # the only such numbers are the Logon and heartbeat of the reconnect itself, which
            # sit at the end of the range where the replay's own completion gap-fills them --
            # so the harder case, gap-filling in place and putting the reports after it back on
            # the numbers they came from, would never run.
            log("=== Letting a heartbeat land mid-stream, then sending a few more orders ===")
            time.sleep(_RESEND_HEARTBEAT_IDLE)
            for _ in range(_RESEND_TAIL_ORDERS):
                f8proc.stdin.write(b"n\n")
            f8proc.stdin.flush()
            tail_total = before_total + _RESEND_TAIL_ORDERS
            found, elapsed, _ = wait_for_me_ord(me_log, tail_total, timeout=30.0, from_byte=running_me_pos)
            if not found:
                die(f"resend: the {_RESEND_TAIL_ORDERS} orders sent after the heartbeat did not "
                    f"reach the matching engine (waited for ME-ORD-{tail_total}).")
            log(f"  resend: {_RESEND_TAIL_ORDERS} order(s) reported after the heartbeat ({elapsed:.1f}s)")

            log("=== Dropping the session and reconnecting a client that keeps its sequence numbers ===")
            stop_f8test(f8proc)
            f8proc = None

            # Where the venue's numbering stands, read from what the baseline client was sent
            # rather than from the venue's account of itself. The member is then brought back
            # believing it has received _RESEND_GAP_MESSAGES fewer, and that is the gap.
            baseline_seen = _client_highest_seq_num(baseline_output)
            if baseline_seen is None:
                die(f"resend: the baseline client recorded no messages (see {baseline_output}), so "
                    "there is no numbering to measure a gap against.")
            venue_next = baseline_seen + 1
            resume_from = venue_next - _RESEND_GAP_MESSAGES
            if resume_from < 2:
                die(f"resend: the venue had only reached MsgSeqNum={baseline_seen}, which is fewer "
                    f"than the {_RESEND_GAP_MESSAGES}-message gap this scenario asks for. Run with "
                    "more baseline orders.")
            log(f"  resend: the venue's numbering stands at {venue_next}; the member returns "
                f"expecting {resume_from}, a gap of {_RESEND_GAP_MESSAGES}")

            f8proc = send_burst(0, gw_log, FIX8_NO_RESET_CFG, client_output,
                                next_expected_receive=resume_from)

            found, elapsed, _ = poll_log_for(
                gw_log, "resuming the venue's sequence state", "outbound=",
                timeout=_PROVISIONING_LOGON_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("resend: the gateway did not resume the session's numbering. The venue "
                    "remembers it at SessionUnbound and hands it back on SessionBoundAck -- check "
                    "that the unbind carried a number and that the sequencer replied.")
            log(f"  resend: the venue resumed the session's numbering ({elapsed:.1f}s)")

            # 1. The venue continued the member's numbering rather than restarting it. Visible
            #    in the very first message the member is sent, and nowhere else from its side.
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

            # 2. The member noticed the gap and asked. Asserted on the number it asked for, not
            #    merely that it asked: a member that requested the wrong range would still log a
            #    ResendRequest, and the range is what the rest of this depends on.
            # One past what -R set. The member accepts the Logon it is holding a gap over --
            # that is what ignore_logon_sequence_check buys -- and its expected-receive advances
            # over it before the gap is acted on, so it asks from the number after.
            asks_from = resume_from + 1
            found, elapsed, _ = poll_log_for(
                gw_log, f"ResendRequest BeginSeqNo={asks_from}",
                timeout=_RESEND_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die(f"resend: the member never asked for the gap. It was started expecting "
                    f"MsgSeqNum={resume_from} and was sent a Logon numbered {logon_seq}, so it had "
                    f"{_RESEND_GAP_MESSAGES} messages missing and should have sent a "
                    f"ResendRequest BeginSeqNo={asks_from}. Check, in order: that -R took (the "
                    "client's session log names the number it expected); that the generated config "
                    "carries ignore_logon_sequence_check, without which fix8 logs off over a gap it "
                    "sees on the Logon instead of asking about it; and that the gateway parsed the "
                    "request.")
            log(f"  resend: the member asked for the gap from {asks_from} ({elapsed:.1f}s)")

            # The end of the range, and the number the member must be left expecting. The member
            # detects its gap on the first message after the Logon, so the venue may have moved
            # on by a heartbeat or a live report before it asks; where the numbering stood at the
            # reconnect is no longer the answer.
            resume_at = _resend_resume_point(gw_log, from_byte=gw_pos)
            if resume_at is None:
                die("resend: the gateway did not say where it would resume after the replay, so "
                    "the range the member asked about cannot be bounded.")

            found, elapsed, _ = poll_log_for(
                gw_log, "resend complete", timeout=_RESEND_TIMEOUT, from_byte=gw_pos,
            )
            if not found:
                die("resend: the gateway never finished the resend. The reports come from the "
                    "sequencer's WAL asynchronously -- check for a SessionReplayRequest that got "
                    "no SessionReplayComplete.")
            resent = _resent_report_count(gw_log, from_byte=gw_pos)
            if not resent:
                die(f"resend: the gateway resent {resent} reports. The whole point of this "
                    "scenario is that the member is sent real execution reports rather than a "
                    "blanket gap-fill over the range.")
            log(f"  resend: the venue resent {resent} report(s) ({elapsed:.1f}s)")

            # Everything below reads the CLIENT's output. What the member was handed is the fact
            # under test; the gateway's own account of it proves only that it believes itself.
            time.sleep(_RESEND_CLIENT_SETTLE)
            received = _client_messages(client_output)
            gap_range = range(asks_from, resume_at)

            # 3. PossDupFlag inside the requested gap and not beyond it. A replay routinely runs
            #    past the gap -- reports the venue could not deliver while the session was away
            #    sit in the same slice and have never been sent -- and marking those would invite
            #    the member to discard news it is seeing for the first time.
            poss_dup = [msg for msg in received if _fix_flag_is_set(msg.get("PossDupFlag"))]
            if not poss_dup:
                die(f"resend: the member received no message marked PossDupFlag=Y (see "
                    f"{client_output}). The gateway says it resent {resent} report(s), so either "
                    "they went unmarked or they never reached the member.")
            outside = [msg["MsgSeqNum"] for msg in poss_dup
                       if not msg.get("MsgSeqNum", "").isdigit() or int(msg["MsgSeqNum"]) not in gap_range]
            if outside:
                die(f"resend: {len(outside)} message(s) were marked PossDupFlag=Y outside the "
                    f"requested gap {asks_from}..{resume_at - 1}: {outside[:10]}. A message the "
                    "member has never seen must not be offered to it as a possible duplicate.")
            log(f"  resend: {len(poss_dup)} message(s) marked PossDupFlag=Y, all within the "
                f"requested gap {asks_from}..{resume_at - 1} -- OK")

            # 4. OrigSendingTime on every resent message. FIX requires it: the member is being
            #    handed a message under a new SendingTime and needs the original to tell how old
            #    what it is looking at really is.
            missing_orig = [msg.get("MsgSeqNum", "?") for msg in poss_dup if not msg.get("OrigSendingTime")]
            if missing_orig:
                die(f"resend: {len(missing_orig)} resent message(s) carried no OrigSendingTime "
                    f"(MsgSeqNum {missing_orig[:10]}). A resent message stamps SendingTime with "
                    "the time it was resent, so without OrigSendingTime the member cannot tell "
                    "how old the report it is being handed is.")
            log(f"  resend: every one of the {len(poss_dup)} resent message(s) carried "
                "OrigSendingTime -- OK")

            # 5. The numbers the venue cannot replay were gap-filled where they stood, not
            #    filled with reports. The scenario put a heartbeat in the middle of the range on
            #    purpose, so a gap-fill must have gone out inside it -- ending before the number
            #    the resend resumes at, which is what distinguishes it from the terminating one.
            #    Without this the run below passes on a venue that only handles the trailing
            #    case, which is the half that was already working.
            gap_fills = [msg for msg in received
                         if msg.get("MsgType") == "4" and _fix_flag_is_set(msg.get("GapFillFlag"))
                         and msg.get("NewSeqNo", "").isdigit()]
            in_range_fills = [int(msg["NewSeqNo"]) for msg in gap_fills if int(msg["NewSeqNo"]) < resume_at]
            if not in_range_fills:
                die(f"resend: no gap-fill arrived inside the requested range. A heartbeat was put "
                    f"in the middle of it deliberately, and the venue cannot replay what that "
                    f"number carried, so it had to be gap-filled in place -- the reports after it "
                    f"belong on the numbers they were first sent on. The member received "
                    f"{len(gap_fills)} gap-fill(s), at NewSeqNo "
                    f"{[msg['NewSeqNo'] for msg in gap_fills]}, none below {resume_at}.")
            log(f"  resend: {len(in_range_fills)} gap-fill(s) inside the range, at NewSeqNo "
                f"{in_range_fills} -- the numbers the venue cannot replay were skipped, not "
                "overwritten -- OK")

            # 6. The member ends the resend expecting the number the venue will send next.
            #    Whichever way the range was closed -- real reports to the end of it, or a
            #    terminating gap-fill over the remainder -- this is the property that decides
            #    whether the session survives, and it is the one that has to hold.
            filled = _resend_gap_fill(gw_log, from_byte=gw_pos)
            if filled is not None:
                gap_from, gap_to = filled
                resets = [msg for msg in received
                          if msg.get("MsgType") == "4" and _fix_flag_is_set(msg.get("GapFillFlag"))]
                if not resets:
                    die(f"resend: the gateway gap-filled {gap_from}..{gap_to} but the member "
                        "received no SequenceReset-GapFill. The administrative remainder was "
                        "skipped on the venue's side only, and the member is now short.")
                new_seq_nums = [msg.get("NewSeqNo") for msg in resets]
                if str(resume_at) not in new_seq_nums:
                    die(f"resend: the terminating gap-fill carried NewSeqNo {new_seq_nums}, not "
                        f"{resume_at}. The member is left expecting a number other than the one "
                        "the venue will send next, which is a gap that never closes.")
                log(f"  resend: the gap-fill over {gap_from}..{gap_to} left the member expecting "
                    f"{resume_at}, the venue's next number -- OK")
            else:
                highest = _client_highest_seq_num(client_output)
                if highest is None or highest < resume_at - 1:
                    die(f"resend: the venue reported no gap left, so the resent reports should "
                        f"have run to {resume_at - 1}. The member's highest MsgSeqNum is "
                        f"{highest}, so the range was not closed and the member is still short.")
                log(f"  resend: the resent reports closed the range at {highest}, leaving the "
                    f"member expecting {resume_at} -- OK")

            # 7. The session is still there afterwards. This is the assertion the others
            #    cannot make between them: every one of them can hold while the two sides walk
            #    away from the resend disagreeing about where the numbering now stands, and
            #    nothing shows it until the venue next sends. A resend that is answered
            #    correctly message by message and then kills the session on the following
            #    heartbeat has not worked, and a test that stops at "resend complete" says it
            #    has.
            survival_pos = file_end(gw_log)
            log(f"  resend: watching the session for {_RESEND_SURVIVAL_WAIT:.0f}s, spanning a "
                "heartbeat, to see that it survives the resend ...")
            time.sleep(_RESEND_SURVIVAL_WAIT)

            unbound = count_log_marker(
                gw_log, f"announced session comp_id='{FIX8_COMP_ID}' unbound from instance",
                from_byte=survival_pos,
            )
            if unbound > 0:
                die("resend: the session died after the resend completed. The venue reported the "
                    "replay as successful and the member then dropped the session, which means "
                    "the two sides came out of it disagreeing about the numbering -- the "
                    "member's own session log names the number it received and the number it "
                    "expected. Check whether the replay reused a number the venue had already "
                    "sent: the range it rewinds into includes whatever administrative traffic "
                    "occupied those numbers, and the WAL holds no record of that.")
            if f8proc.poll() is not None:
                die("resend: the member's client exited after the resend completed. Its session "
                    "log records why; a sequence-number disagreement is the likely cause.")
            log("  resend: the session survived the resend and the heartbeat that followed -- OK")

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
                    "recovery guarantees in docs/availability/gateway_ha.md rest on it being a rule.")
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
                ignore_logon_sequence_check=True,
                # The same store instance a used, so the member arrives on b continuing its own
                # numbering rather than restarting it -- which the venue would now, correctly,
                # treat as the member having gone backwards.
                persist_to_disk=_FIX8_PERSIST_DB,
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

            # 6. The member asks for what it missed, and the session survives being answered.
            #
            #    This leg used to be skipped, on the grounds that f8test terminates rather than
            #    issuing a ResendRequest when the gap appears on the Logon itself. That is what
            #    it does by default, and it is a property of the client's configuration rather
            #    than of the venue: fix8 only throws there when ignore_logon_sequence_check is
            #    unset, which the config above now sets. No gap has to be manufactured, either
            #    -- the unclean death biased the numbering high, so the member is already short
            #    by everything the dead instance sent.
            #
            #    What is pinned here is the member-observable part: it asks, it is answered,
            #    and it comes out of the answer still logged in with its numbering agreeing
            #    with the venue's. That is enough to catch the failover half of BUG-0051 --
            #    instance b did not send the messages being asked about and cannot say which
            #    of those numbers carried reports, so it fills them all with reports, and the
            #    member is left expecting a number the venue will not send.
            #
            #    What no assertion here can reach is whether the reports it was handed belong
            #    on the numbers they arrived under. The sequencer returns the most recent
            #    reports for the session, so after a failover the member is handed recent ones
            #    presented as the old missing ones, and nothing on its side distinguishes that
            #    from a correct answer. See docs/availability/resend_provenance.md.
            found, elapsed, _ = poll_log_for(
                gw_b_log, "ResendRequest BeginSeqNo=", timeout=_RESEND_TIMEOUT, from_byte=gw_b_pos,
            )
            if not found:
                die("in-flight: the member never asked for what it missed. It was sent a Logon "
                    f"numbered {logon_seq} while expecting 1, so it was short by everything the "
                    "dead instance had sent. Check the generated config carries "
                    "ignore_logon_sequence_check.")
            log(f"  in-flight: the member asked instance b for the gap ({elapsed:.1f}s)")

            found, elapsed, _ = poll_log_for(
                gw_b_log, "resend complete", timeout=_RESEND_TIMEOUT, from_byte=gw_b_pos,
            )
            if not found:
                die("in-flight: instance b never finished answering the resend. The reports come "
                    "from the sequencer's WAL asynchronously -- check for a SessionReplayRequest "
                    "that got no SessionReplayComplete.")
            log(f"  in-flight: instance b answered the resend ({elapsed:.1f}s)")

            # 6a. And it answered with real reports, not a gap-fill over the whole range.
            #
            #     This is the assertion that separates a venue that recovered from one that
            #     merely survived. Instance b never sent the messages being asked about, so it
            #     can only replay them if the record of which numbers held a report outlived
            #     the instance that made it -- which is the point of keeping that record in the
            #     sequencer's session state rather than in the gateway. Without it, b has no
            #     provenance, correctly declines to guess, and gap-fills everything: the member
            #     keeps its session and loses every report it missed.
            # 6a. Instance b inherited the record of which numbers held reports.
            #
            #     NOT "it replayed real reports", which is what this asserted first and which was
            #     measuring an artefact. That held only while the client had amnesia: with a
            #     memory store it reconnected believing it had received nothing, asked from the
            #     beginning, and the whole of the real history fell inside the range. A client
            #     that remembers correctly asks from where it actually got to -- and everything
            #     above that is the venue's deliberate high bias after an unclean death, numbers
            #     NOBODY ever sent. Gap-filling those is the right answer, so requiring real
            #     reports would be requiring the venue to invent them.
            #
            #     What matters, and is asserted here, is that the record crossed the instance
            #     boundary at all. Without it instance b cannot tell a number that held a report
            #     from one that held a heartbeat, which is BUG-0051 in the case a gateway-local
            #     record cannot reach.
            inherited = _inherited_report_range_count(gw_b_log, from_byte=gw_b_pos)
            if inherited is None or inherited == 0:
                die("in-flight: instance b inherited no record of which of this session's numbers "
                    "held execution reports. It never sent them itself, so without that record it "
                    "cannot tell a report's number from a heartbeat's, and a resend would refill "
                    "both -- see BUG-0051. The gateway reports the record on SessionSequenceUpdate "
                    "every 2s; a session that lives and dies inside one interval reports nothing.")
            resent = _resent_report_count(gw_b_log, from_byte=gw_b_pos)
            log(f"  in-flight: instance b inherited a record covering {inherited} number(s) for a "
                f"session it never served, and replayed {resent} report(s) into the range asked "
                "for -- the rest of that range is the venue's own high bias, which nobody sent -- OK")

            resume_at = _resend_resume_point(gw_b_log, from_byte=gw_b_pos)
            if resume_at is None:
                die("in-flight: instance b did not say where it would resume after the replay.")

            time.sleep(_RESEND_CLIENT_SETTLE)
            received = _client_messages(client_output)
            gap_fills = [int(msg["NewSeqNo"]) for msg in received
                         if msg.get("MsgType") == "4" and _fix_flag_is_set(msg.get("GapFillFlag"))
                         and msg.get("NewSeqNo", "").isdigit()]
            if resume_at not in gap_fills:
                die(f"in-flight: the member's gap was not closed at {resume_at}, the number the "
                    f"venue will send next. It received gap-fills at NewSeqNo {gap_fills}. The "
                    "member is left expecting something other than what arrives, which is a gap "
                    "that never closes.")
            log(f"  in-flight: the member's gap closed at {resume_at}, the venue's next number -- OK")

            # The same watch scenario 22 keeps, and for the same reason: everything above can
            # hold while the two sides come out of the resend disagreeing, and nothing shows it
            # until the venue next sends.
            survival_pos = file_end(gw_b_log)
            log(f"  in-flight: watching the session for {_RESEND_SURVIVAL_WAIT:.0f}s, spanning a "
                "heartbeat, to see that it survives the resend ...")
            time.sleep(_RESEND_SURVIVAL_WAIT)
            unbound = count_log_marker(
                gw_b_log, f"announced session comp_id='{FIX8_COMP_ID}' unbound from instance",
                from_byte=survival_pos,
            )
            if unbound > 0:
                die("in-flight: the session died after instance b answered the resend. A member "
                    "that fails over to the surviving gateway and asks for what it missed must "
                    "come out of it still logged in -- losing the session at a failover is the "
                    "moment it can least afford it.")
            if f8proc.poll() is not None:
                die("in-flight: the member's client exited after the resend. Its session log "
                    "records why; a sequence-number disagreement is the likely cause.")
            log("  in-flight: the session survived the resend and the heartbeat that followed -- OK")

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
    # die() raises, and outside a scenario there is no handler for it. A traceback here would
    # bury the one line that says what to do -- and these failures are the ones a reader is most
    # likely to be reading in a hurry.
    try:
        preflight(prefix)
    except TestFailure:
        sys.exit(1)

    # A scenario run against a venue built before the current edits produces a result about the
    # older code, and nothing in the output would say so. Warns rather than refuses: running an
    # older release deliberately -- bisecting, or reproducing an incident -- is a real thing to do.
    # See scripts/deployment_freshness.py and docs/bug_list.md, BUG-0015.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import deployment_freshness  # noqa: PLC0415  -- deferred: needs the path set above
    stale = deployment_freshness.staleness_warnings(prefix)
    if stale:
        log("WARNING: the deployed venue predates changes in this tree, so this run measures the older code")
        for line in stale:
            log(f"  {line.strip()}")

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
        if scenario.expected_failure:
            if passed:
                log("")
                log(f"Scenario {scenario.number} PASSED and was marked as expected to fail:")
                log(f"  {scenario.expected_failure}")
                log("  The gap has closed. Remove expected_failure from the scenario, so that a")
                log("  future regression fails the suite instead of being absorbed by the marking.")
                overall_pass = False
            else:
                log("")
                log(f"Scenario {scenario.number} failed, as expected: {scenario.expected_failure}")
                passed = True
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
