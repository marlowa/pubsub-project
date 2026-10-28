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
       Kills sequencer_primary.  The secondary's promise to it runs out
       after about one lease period, it asks to lead, and the arbiter
       grants it a lease.  Recovery orders must flow through the new leader.
       Expected: sequencer_secondary elected leader; recovery orders OK.

  2  Primary arbiter death
       Kills arbiter_primary.  The arbiter secondary's promise to it runs
       out, and the witness grants it a lease, so it becomes active.  The sequencer_primary remains
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
       Kills the witness process.  The active arbiter renews its lease with
       the passive one, so it does not need the witness.
       Expected: arbiters retain established roles; orders continue.

  6  Both arbiters dead
       Kills arbiter_primary then arbiter_secondary.  The sequencer_primary
       is already leader and keeps leading, renewing its lease with its peer.
       WARNING: a subsequent sequencer_primary death would leave the group
       with no leader until an arbiter returns (scenario 9).
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
       arbiter_secondary holds only its own vote, so it cannot become
       active, and reports that the pool has no active arbiter.
       Expected: arbiter_secondary does not become active; sequencer_primary
       stays leader; orders uninterrupted.

  9  No arbiters, then the leader lost
       Kills both arbiters, then kills sequencer_primary.  sequencer_secondary
       hears from nobody and cannot tell a dead leader from being cut off, so
       it must not take the lead (R-0147).
       Expected: sequencer_secondary does not lead, and reports that the
       group has no leader; no orders are sent.

 10  Matching engine death and restart (simple)
       Kills matching_engine and restarts it.  ME reconnects to both
       sequencer ER listeners (7021/7022), and catches up from the
       sequencer's WAL before it serves.
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

 15  Lease exchange: explicit trace
       Kills sequencer_primary and verifies each step of the lease exchange
       in the logs:
         a. sequencer_secondary asks to lead
         b. arbiter_primary grants it a lease
         c. sequencer_secondary leads on that grant
         d. sequencer_secondary transitions to leader
       Expected: all four markers seen; recovery orders flow.

 16  Primary matching-engine death (ME HA failover)
       Runs the full ME-HA topology (matching_engine_primary + _secondary,
       _primary/_secondary configs).  Baseline orders are confirmed on
       matching_engine_primary; then matching_engine_primary is SIGKILLed.
       The secondary's promise to it runs out, it asks to lead and is
       granted a lease, reconciles against the sequencer WAL, and adopts
       LEADER.  The leader sequencer promotes its
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
       gateway elects nothing, and b inherits none of a's sessions.  The
       sequencer holds the execution reports bound for the dead instance, to send
       them when it connects to that instance again, and does not reroute them.
       Expected: b reports no traffic of a's; the sequencer reports holding the
       reports for instance 1 and drops none of them.

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
    --orders-during N     Extra bursts of 1000 NOS sent after the health check,
                          without waiting, just before the Phase 4 kill
                          (default: 20).  At the venue's current speed they
                          are all processed before the kill happens, so they
                          load the venue but are not sent during the change
                          of leader; scenario 68 tests orders sent then.
    --orders-after N      Bursts of 1000 NOS sent as recovery orders (default: 1)
    --ready-timeout SECS  Max seconds for initial leader election (default: 10)
    --failover-timeout S  Max seconds per failover step (default: 30)
    --recovery-timeout S  Max seconds for recovery orders (default: 30)
    --secondary-engine-first
                          Start matching_engine_secondary before
                          matching_engine_primary, and start the primary only
                          once the secondary leads.  After any failover of the
                          matching engine the secondary leads, so a scenario
                          must pass from that start too.  Applies only to the
                          scenarios that run both matching engines; with
                          'all', only those are run.

When a scenario fails, its Quill logs and the test clients' output are copied to
tmp/ha_test_failures/<time>-scenario-<number>-<short name> under the project before the next
scenario deletes them, and the run's output says where.

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
  A follower takes over once the promises it and the arbiter made to the old leader have
  run out: about one lease period plus one renewal interval (3 s and 1 s in the dev
  environment's [shared] section).  Scenario 1 measures about five and a half seconds,
  including the new leader's first orders.  See docs/availability/majority_leases.md.
"""

from __future__ import annotations
import argparse
import hashlib
import hmac as _hmac
import os
import re
import secrets
import selectors
import shutil
import signal
import socket
import struct
import urllib.request
import zlib
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Callable, NamedTuple

from test_event_listener import (ReceivedEvent, TestEventListener, catch_up, connection_made, follows, granted_to, introduced, lease_changed,
                                 leads, message_of, order_outcome, role_changed)

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY          = 1.0   # seconds between app launches
# Upper bound on how long a logon may take, not an expectation of how long it does take:
# wait_for_fix_logon returns the moment it sees an outcome, so raising this costs a passing
# run nothing. It must exceed the gateway's 5s sequence_state_timeout, or a logon that takes
# the fallback is failed here two seconds before the venue would have finished trying --
# reported as a timeout, which points at the wrong thing entirely. Taking that fallback is
# still a failure, but the gateway's SessionEstablished event says so, and it is named as itself.
FIX8_LOGON_WAIT        = 8.0   # seconds for f8test to establish a FIX session
POLL_INTERVAL          = 0.05  # seconds between polls of a file, a client's output or a condition
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
# How long a member may wait for the answer to an order the venue deferred, measured from the
# engine reporting itself ready. It covers arbitration, the catch-up itself and the report's
# trip back through the sequencer and the gateway, so it is generous: what is being asserted is
# that the answer comes at all.
_DEFERRED_ANSWER_TIMEOUT      = 45.0
# Long enough for at least two of the gateway's five-second progress lines to land, so "it kept
# reporting" is a claim about a sequence rather than about one line that happened to be there.
_REFUSAL_QUIET_WATCH          = 12.0

# Scenario 56. The comp id given limits, and the limits it is given. Small enough that a burst of
# orders sent as fast as the raw client can write them crosses the limit within the same second,
# and different from each other, so a limit applied to the wrong kind of command is caught. The
# amend limit is not exercised -- the venue cannot amend -- but it is checked to have reached the
# gateway, since it travels the same route.
_THROTTLED_COMP_ID = "THROTTLED"
_THROTTLE_PLACE_LIMIT = 5
_THROTTLE_AMEND_LIMIT = 2
_THROTTLE_CANCEL_LIMIT = 3
_THROTTLE_BURST = 12
# Orders sent part way through the same second, after the limit is reached: refused, and must not
# count towards the limit. More of them than the limit, so that a gateway which recorded a refused
# order in place of the oldest accepted one would hold nothing but retries by the time the next
# order is sent, and refuse it; with fewer, some accepted times would expire first and hide that.
_THROTTLE_RETRIES = _THROTTLE_PLACE_LIMIT + 1
# How long the gateway may take to send a test event about a member's throttle after the member sees the cause.
_THROTTLE_EVENT_TIMEOUT = 5.0

# Scenarios 42 and 57, through the binary gateway. A comp id of its own, so the binary client never
# shares a session with the FIX test client: two gateways holding one comp id would each announce
# it, and reports for one session could be routed to the other. It is not in the database; its
# credential is written into credentials.toml, with the empty password, as the FIX test client's is.
_BINARY_COMP_ID = "BINARY-TEST"
# How long the binary client waits for each reply before giving up and saying so.
_BINARY_REPLY_TIMEOUT = 10
# How long the binary gateway may take to connect to the leading sequencer after the venue starts.
_BINARY_READY_TIMEOUT = 30.0
# The longest ClOrdID the venue accepts: fix_order_limits::max_cl_ord_id_length, the size of the
# matching engine's book key. Repeated here because the scenario must send one a character longer.
_MAX_CL_ORD_ID_LENGTH = 64

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

# What a kill step establishes when nothing else is said: the secondary took over.
_DEFAULT_KILL_OUTCOME = "elected leader"

_SEQ_FOLLOWER_TIMEOUT = 15.0   # seconds
SETTLE_AFTER_RESTART  = 2.0    # seconds after a sequencer restart before proceeding

# How long a restarted matching engine is given to start and have a sequencer connect to it
# (expect_engine_reachable).
_ME_READY_TIMEOUT = 15.0  # seconds
# How long Phase 2 waits for one of a pair of matching engines to lead: comfortably more than the
# lease period plus the secondary's head start, which is how long a pair started together takes.
_ME_LEADER_TIMEOUT = 15.0  # seconds
_ME_SETTLE        = 1.0   # seconds after ME readiness confirmed


# The two matching engines of a pair, named by the role each had when the scenario's steps began, rather than by the
# configuration it was started from. Either engine may win the first election, so a scenario that means "kill the
# engine that leads" must not name matching_engine_primary: about one start in three, the secondary leads. Phase 2
# resolves the two names once it has seen which engine leads (BUG-0192).
ENGINE_LEADING_AT_START = "matching_engine_leading_at_start"
ENGINE_FOLLOWING_AT_START = "matching_engine_following_at_start"
# What the two names stand for in the scenario being run. Until phase 2 has seen a leader, the primary is taken to lead.
_ENGINES_AT_START: dict[str, str] = {ENGINE_LEADING_AT_START: "matching_engine_primary", ENGINE_FOLLOWING_AT_START: "matching_engine_secondary"}


def resolved(name: str | None) -> str | None:
    """The component name stands for: the engine that led or followed when the steps began, for the two names above;
    otherwise name itself."""
    return _ENGINES_AT_START.get(name, name) if name is not None else None


def engine_service(engine: str) -> str:
    """The name a sequencer gives the matching engine engine's order connection: "matching_engine_secondary" for the
    secondary of a pair, and "matching_engine" for the primary of a pair and for the single engine, which is named
    "matching_engine"."""
    return "matching_engine_secondary" if engine == "matching_engine_secondary" else "matching_engine"


class Condition(NamedTuple):
    """One test event that an expectation needs (docs/availability/test_events.md).

    component: the component whose events are searched, as this test names it, or None for any component.
    matches:   given the generated test_events module, returns the predicate an event must satisfy. It is a function
               rather than a predicate because the scenario table is built before the module is loaded.
    """
    component: str | None
    matches: Callable[[object], Callable[[ReceivedEvent], bool]]


class Expect(NamedTuple):
    """
    What a scenario step waits for, or requires not to have happened, stated as test events rather than Quill log
    wording. Every condition must be met, each by an event of its own; the order between them is not checked.

    what:       how the run's output describes it, such as "sequencer_secondary leads".
    conditions: the events needed.
    """
    what: str
    conditions: tuple


def expect_leads(component: str) -> Expect:
    """component takes the lead (RoleChanged to Leader)."""
    return Expect(f"{component} leads", (Condition(component, lambda m: leads(m, resolved(component))),))


def expect_follows(component: str) -> Expect:
    """component follows (RoleChanged to Follower)."""
    return Expect(f"{component} follows", (Condition(component, lambda m: follows(m, resolved(component))),))


def expect_role(component: str) -> Expect:
    """component reports any role (any RoleChanged): its role has been decided."""
    return Expect(f"{component} has a role", (Condition(component, lambda m: role_changed(m, resolved(component))),))


def expect_started(component: str) -> Expect:
    """component has started: a thread of it introduced itself on the test event connection."""
    return Expect(f"{component} started", (Condition(component, lambda m: introduced(m, resolved(component))),))


def expect_engine_reachable(engine: str, service: str | None = None) -> Expect:
    """
    A matching engine has started and a sequencer has connected to it, so that orders the sequencer forwards reach it.

    Both are needed. The engine's introduction says its thread is running; a sequencer's connection to the engine's
    service says orders have a way in. The connection alone is not enough, because the kernel completes a connection as
    soon as the engine listens, which can be before its thread is running. service is the name the sequencer gives the
    engine: "matching_engine" for the primary, "matching_engine_secondary" for the secondary. When it is not given, it is
    the one for whichever engine engine stands for when the check runs.
    """
    return Expect(f"{engine} started and a sequencer connected to it",
                  (Condition(engine, lambda m: introduced(m, resolved(engine))),
                   Condition(None, lambda m: connection_made(m, None, service if service is not None else engine_service(resolved(engine))))))


def expect_lease(component: str, change: str, granted_by: str | None = None) -> Expect:
    """component's lease agent reports change (a LeaseChange name), granted by granted_by (a Voter name) if given."""
    by = f" by the {granted_by}" if granted_by else ""
    return Expect(f"{component}: lease {change}{by}",
                  (Condition(component, lambda m: lease_changed(m, resolved(component), getattr(m.LeaseChange, change),
                                                                getattr(m.Voter, granted_by) if granted_by else None)),))


def expect_catch_up(engine: str, stage: str) -> Expect:
    """The matching engine reached a stage (a CatchUpStage name) of catching up from the sequencer's WAL."""
    def matches(_library_events):
        engine_events = _TEST_EVENTS.modules["matching_engine_test_events"]
        return catch_up(engine_events, resolved(engine), getattr(engine_events.CatchUpStage, stage))
    return Expect(f"{engine}: catch-up {stage}", (Condition(engine, matches),))


def expect_message(component: str, what: str, message: str, module: str = "test_events", fields=None) -> Expect:
    """component sends a message named message, from the generated module named module. fields, when given, takes that
    module and returns the field values the message must have, so that an enum value can be named through the module."""
    def matches(_library_events):
        generated = _TEST_EVENTS.modules[module]
        return message_of(getattr(generated, message), resolved(component), **(fields(generated) if fields else {}))
    return Expect(f"{component}: {what}", (Condition(component, matches),))


def expect_connection_made(component: str, service: str) -> Expect:
    """component made a connection to service (ConnectionChanged), as the framework names it."""
    return Expect(f"{component}: connection to {service} made",
                  (Condition(component, lambda m: message_of(m.ConnectionChanged, resolved(component), made=True, service=service)),))


def expect_replication_connection(engine: str) -> Expect:
    """The other engine of the pair connected to engine's book replication listener, which engine accepts as an inbound
    connection named after the port. The port is read from engine's deployed configuration when the check runs, because
    the scenario table is built before the install directory is known."""
    def matches(library_events):
        actual = resolved(engine)
        config = _INSTALL_PREFIX / "etc" / "matching_engine" / f"{actual if actual != 'matching_engine' else 'matching_engine_primary'}.toml"
        port = installed_toml_section_value(config, "book_replication", "listen_port")
        return message_of(library_events.ConnectionChanged, actual, made=True, service=f"inbound:{port}")
    return Expect(f"{engine}: the other engine's replication connection made", (Condition(engine, matches),))


def expect_granted_to(arbiter: str, pair_id: int, instance: int) -> Expect:
    """arbiter, as the active arbiter, grants instance of pair_id a lease (LeaseGrantedTo)."""
    return Expect(f"{arbiter} grants instance {instance} of pair {pair_id} a lease",
                  (Condition(arbiter, lambda m: granted_to(m, arbiter, pair_id, instance)),))


def expect_session_announced_again(sequencer: str) -> Expect:
    """
    A gateway's link to sequencer came up and the gateway said the member's FIX session is still connected to it
    (SessionAnnouncedAgain). A sequencer keeps where each session is connected only in memory, so one that has restarted
    and then leads drops every report for a session it has not been told of (BUG-0182).
    """
    return expect_message(sequencer, "told the member's session is still connected", "SessionAnnouncedAgain", "sequencer_test_events",
                          fields=lambda m: {"comp_id": FIX8_COMP_ID})


def log_session_announcement_margins() -> None:
    """
    For each sequencer process that was told the member's session is still connected and that took the lead, log how long
    before taking the lead it was told; negative when it was told after. A report that comes back while the sequencer leads
    and has not been told is dropped. It is measured, not checked: the venue does not wait for the gateways before a
    sequencer takes the lead, so either order is correct, and the member-visible checks find any report that was lost.
    """
    sequencer_events = _TEST_EVENTS.modules["sequencer_test_events"]
    for sequencer in ("sequencer_primary", "sequencer_secondary"):
        since = _TEST_EVENTS.since_introduction(sequencer)
        announced = message_of(sequencer_events.SessionAnnouncedAgain, sequencer, comp_id=FIX8_COMP_ID)
        took_lead = leads(_TEST_EVENTS.messages, sequencer)
        first_announced = next((event for event in _TEST_EVENTS.events[since:] if announced(event)), None)
        first_led = next((event for event in _TEST_EVENTS.events[since:] if took_lead(event)), None)
        if first_announced is None or first_led is None:
            continue
        margin = first_led.arrived - first_announced.arrived
        when = "before" if margin >= 0 else "AFTER"
        log(f"  {sequencer}: told comp_id='{FIX8_COMP_ID}' is still connected {abs(margin):.1f}s {when} it took the lead")


def _condition_met(condition: Condition, since: int) -> bool:
    return _TEST_EVENTS.find(condition.matches(_TEST_EVENTS.messages), since=since) is not None


def await_expectation(expect: Expect, timeout: float, since: int) -> tuple[bool, float]:
    """Wait up to timeout seconds for every condition of expect to be met by an event at or after since.

    Returns whether they were, and how long it took. A failure on the test event channel raises at once."""
    started = time.monotonic()
    deadline = started + timeout
    while True:
        _TEST_EVENTS.check()
        if all(_condition_met(condition, since) for condition in expect.conditions):
            return True, time.monotonic() - started
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False, time.monotonic() - started
        _TEST_EVENTS.pump(min(remaining, 0.05))


def since_present_processes(expect: Expect) -> int:
    """
    The position from which expect is judged when a step asks what has happened since the components it names started,
    rather than since some step of the test: the earliest of their newest introductions. This matches what reading a
    component's Quill log from its start did, because a component the test restarts has its Quill log deleted first.
    """
    named = [resolved(condition.component) for condition in expect.conditions if condition.component is not None]
    if not named:
        return 0
    return min(_TEST_EVENTS.since_introduction(component) for component in named)


def expectation_seen(expect: Expect, since: int) -> bool:
    """Whether every condition of expect has been met at or after since, by what has been read once each component named
    has been asked for a checkpoint. Used to check that something did NOT happen: without the checkpoint, an event
    still on its way would be missed."""
    for component in {resolved(condition.component) for condition in expect.conditions if condition.component is not None}:
        if not _TEST_EVENTS.settle(component):
            die(f"{component} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so it is not known whether {expect.what}")
    _TEST_EVENTS.check()
    return all(_condition_met(condition, since) for condition in expect.conditions)


def component_event(component: str, module: str, message: str, since: int, timeout: float, settle: bool = False, **fields):
    """The first event named message, from the generated module named module, sent by component at or after since with
    the field values given, waiting up to timeout seconds for it; or None. With settle, the component is first asked for a
    checkpoint, so that None means it sent none."""
    predicate = message_of(getattr(_TEST_EVENTS.modules[module], message), component, **fields)
    if settle and not _TEST_EVENTS.settle(component):
        die(f"{component} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so whether it sent {message} is not known")
    found = _TEST_EVENTS.wait_for(predicate, timeout=timeout, since=since)
    return found.message if found is not None else None


def engine_event(engine: str, message: str, since: int, timeout: float, settle: bool = False):
    """component_event for the matching engine's events (matching_engine_test_events.dsl)."""
    return component_event(engine, "matching_engine_test_events", message, since, timeout, settle)


def gateway_event(gateway: str, message: str, since: int, timeout: float, settle: bool = False, **fields):
    """component_event for a FIX gateway's events (fix_order_gateway_test_events.dsl)."""
    return component_event(gateway, "fix_order_gateway_test_events", message, since, timeout, settle, **fields)


def count_seen(expect: Expect, since: int) -> int:
    """How many events at or after since meet expect's first condition, once each component it names has answered a
    checkpoint, so that the count includes everything those components had sent by then."""
    for component in {resolved(condition.component) for condition in expect.conditions if condition.component is not None}:
        if not _TEST_EVENTS.settle(component):
            die(f"{component} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so how often {expect.what} is not known")
    matches = expect.conditions[0].matches(_TEST_EVENTS.messages)
    _TEST_EVENTS.check()
    return sum(1 for event in _TEST_EVENTS.events[since:] if matches(event))


def has_happened(expect: Expect, timeout: float) -> bool:
    """Whether expect has been met since the components it names started, waiting up to timeout seconds for it."""
    return await_expectation(expect, timeout, since_present_processes(expect))[0]


def has_led(component: str, timeout: float) -> bool:
    """Whether component's present process has taken the lead, waiting up to timeout seconds for it to."""
    return await_expectation(expect_leads(component), timeout, _TEST_EVENTS.since_introduction(component))[0]


# The comp ids whose orders the matching engine reports the outcome of in every scenario: inject_order's, whose orders
# reach no member. A scenario that checks orders no member can judge adds its member's comp id (outcome_comp_ids).
_REPORT_ORDERS_OF = ("INJECT-TEST",)
# The comp ids whose reports a sequencer is told to expect to reach no session: inject_order's, which no gateway holds.
_COMP_IDS_WITHOUT_SESSION = ("INJECT-TEST",)


def order_outcomes(engine: str, since: int, cl_ord_id: str, outcome: str | None = None, during_catch_up: bool | None = False) -> int:
    """How many OrderOutcome events the matching engine has sent for cl_ord_id at or after since, with the given outcome (an
    OrderOutcomeKind name) where it is given, among the events read so far.

    By default only those it dealt with live: an engine reports every command it applies while catching up as well, so an
    order accepted live and applied again by a catch-up after a restart has two outcomes. during_catch_up=True counts only
    those applied while catching up, and None both."""
    module = _TEST_EVENTS.modules["matching_engine_test_events"]
    matches = order_outcome(module, engine, cl_ord_id, getattr(module.OrderOutcomeKind, outcome) if outcome else None, during_catch_up)
    _TEST_EVENTS.check()
    return sum(1 for event in _TEST_EVENTS.events[since:] if matches(event))


def await_order_outcome(engine: str, cl_ord_id: str, outcome: str, timeout: float, since: int, during_catch_up: bool | None = False) -> bool:
    """Wait up to timeout seconds for the matching engine to report outcome for cl_ord_id, at or after since; live only by
    default, as order_outcomes."""
    deadline = time.monotonic() + timeout
    while order_outcomes(engine, since, cl_ord_id, outcome, during_catch_up) == 0:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        _TEST_EVENTS.pump(min(remaining, 0.05))
    return True


def outcome_count(engine: str, since: int, outcome: str) -> int:
    """How many OrderOutcome events with outcome (an OrderOutcomeKind name), for any order, the matching engine has sent at
    or after since for commands it dealt with live, among the events read so far."""
    module = _TEST_EVENTS.modules["matching_engine_test_events"]
    wanted = getattr(module.OrderOutcomeKind, outcome)
    _TEST_EVENTS.check()
    return sum(1 for event in _TEST_EVENTS.events[since:]
               if event.component == engine and isinstance(event.message, module.OrderOutcome) and event.message.outcome == wanted
               and not event.message.during_catch_up)


def settled_order_outcomes(engine: str, since: int, cl_ord_id: str, outcome: str | None = None, during_catch_up: bool | None = False) -> int:
    """order_outcomes, once the matching engine has answered a checkpoint, so that a count of none means none was sent."""
    if not _TEST_EVENTS.settle(engine):
        die(f"{engine} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so what it did with {cl_ord_id} is not known")
    return order_outcomes(engine, since, cl_ord_id, outcome, during_catch_up)


class KillStep(NamedTuple):
    """
    One kill action within a scenario.

    takeover:      the test events that must follow the kill, normally the other instance
                   taking the lead; None when nothing is expected to follow (a secondary, or a
                   process whose loss changes no roles).
    settle_secs:   how long to wait after the kill (or after the takeover is seen) before
                   proceeding to the next step.
    outcome_label: what seeing the takeover establishes, as a past-tense phrase, when it is
                   not a promotion.  Scenario 8 asserts the opposite of one -- an arbiter that
                   declines -- and a summary reading "elected leader" because an event arrived
                   is how a run comes to report something it did not establish.
    """
    proc_name: str
    takeover: Expect | None
    settle_secs: float
    outcome_label: str | None = None


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
    takeover:      the test events showing the survivors took over, or None.
    machine_of:    when given, ENGINE_LEADING_AT_START or ENGINE_FOLLOWING_AT_START, and proc_names is
                   empty. The machine is then the one that engine runs on: the engine, with the arbiter
                   and the sequencer configured on the same side as it, primary or secondary. Which
                   machine that is depends on which engine won the first election, so it is worked out
                   once phase 2 has seen that.
    """
    proc_names: tuple
    takeover: Expect | None
    ready_timeout: float
    settle_secs: float
    machine_of: str | None = None


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
    entitlement.

    The resumed instance needs nobody to tell it that it has been superseded. It
    counts its lease on its own clock, so the first time it checks after waking it
    finds that its lease ran out while it was stopped, and it stops acting before
    it sends anything as leader. The epoch every message carries stays in place as
    a second defence, but this step asserts the first one: the instance stops by
    itself, and the instance that took over keeps the entitlement.

    takeover:   the test events showing the peer took over while this instance was stopped.
    stopped:    the test events from the RESUMED instance showing that it found its lease
                had run out and stopped acting.  This is the assertion.
    forbidden:  the test events that must NOT follow from the survivor afterwards -- it must
                not give the entitlement back.
    """
    proc_name: str
    takeover: Expect
    takeover_timeout: float
    stopped: Expect
    stopped_timeout: float
    forbidden: Expect
    settle_secs: float


class RestartStep(NamedTuple):
    """
    One kill-and-restart action within a scenario.

    Kills the named process (if still running), deletes its old Quill log
    (ready_log_name) so that Quill starts a fresh one, relaunches the process, and
    waits until it is ready: for the test events in ready.

    resets_me_counter: True when restarting the matching engine.  After the steps,
                       the test then waits for the primary sequencer's order
                       connection to the restarted engine before Phase 5.
    settle_secs:       how long to wait after readiness is confirmed.
    damage_region:     the file name of a region to corrupt while the process is dead, so
                       that the successor finds a record of what it held that it cannot
                       read.  Resolved against the deployment's var directory, because the
                       scenario table is built before the install prefix is known.
    forget_lease_promises: delete the matching engine's record of its lease promises while it
                       is dead, so that it restarts as it would after a reboot: knowing
                       nothing of what it promised, and so agreeing that nobody leads, and
                       asking for nothing, for one lease period.
    down_secs:         how long to leave the process dead before restarting it.  Zero
                       for every scenario whose subject is a fast restart.  A scenario
                       testing what a LONG absence does needs the venue to be absent for
                       real, and there is no faking it: the leading sequencer measures how
                       long it has had no matching engine to route orders to.
    """
    proc_name: str
    ready_log_name: str
    ready_timeout: float
    resets_me_counter: bool
    settle_secs: float
    ready: Expect | None = None
    down_secs: float = 0.0
    damage_region: str = ""
    forget_lease_promises: bool = False


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
    Wait, then assert that something did NOT happen: the test events in absent did not
    arrive from the components' present processes.

    For properties that are only expressible as something not having happened. A peer that
    correctly declines to promote reports nothing saying so -- the evidence is the
    absence of the promotion it would otherwise have made -- and that can only be judged after
    leaving it long enough to have made one.

    after_secs must comfortably exceed the window in which the thing could occur, or the
    absence proves nothing except that the test was impatient.
    """
    after_secs: float
    description: str
    absent: Expect | None = None


class InterimOrdersStep(NamedTuple):
    """
    Send a batch of orders mid-Phase-4 and wait until the client has received a New
    report for each of them.

    Used in WAL-recovery scenarios where orders must flow through an
    intermediate leader before the original primary is restarted.
    count_batches * 1000 orders are sent and confirmed before proceeding.
    Confirmed at the client, so the check holds whichever matching engine
    is doing the work and however far its order numbering has been carried.
    """
    count_batches: int


class OneLeaderStep(NamedTuple):
    """
    Wait until one of two instances of a pair leads, then require that the other did not.

    For a pair started in either order with some time between the starts. Which instance leads
    then depends on which started first, and is not what is asserted; what is asserted is that
    they settle on exactly one. The other instance is checked after settle_secs, which must be
    long enough for a second leader to have appeared if one were going to.

    components: the two instances, as this test names them.
    """
    components: tuple
    timeout: float
    settle_secs: float
    description: str


class VerifyStep(NamedTuple):
    """
    Wait for the test events in expect from the components' present processes.

    Used to verify intermediate steps of a change of leader (for example a lease
    request sent and a grant received) without relying solely on the final
    role-transition marker.

    timeout:     maximum seconds to wait
    description: human-readable label shown in test output

    absent:         test events that must NOT have arrived from the components' present
                    processes, checked once expect has been met.
    """
    timeout: float
    description: str
    expect: Expect | None = None
    absent: Expect | None = None


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
    # The longest the venue recovers by itself while no matching engine can trade, written into the deployed
    # configs before launch. The deployed figure is 10s, which many scenarios exceed on purpose, keeping an engine
    # down to see what the venue does meanwhile; they get 600s, so that the halt comes only in the scenarios about it.
    automatic_recovery_limit_seconds: int = 600
    # What a promoted matching engine does with the orders it inherits, written into the
    # deployed configs before launch: "cancel" or "keep". The deployed default is "cancel",
    # because keeping the book rests on a catch-up nothing verifies yet (R-0101, BUG-0074).
    # A scenario that promotes an engine asserts against whichever of the two is set here, so
    # the assertion follows the venue's stated policy rather than assuming one of them.
    open_orders_on_promotion: str = "cancel"
    # When True, keep the matching engine down for longer than absence_limit_seconds before
    # restarting it, then assert that it cancelled every order it recovered, told each member,
    # and halted. See run_scenario's "long absence" block.
    assert_absence_halt: bool = False
    # When True, the matching engine is kept down for longer than automatic_recovery_limit_seconds before it is restarted:
    # assert that the leading sequencer halted trading with a TechnicalEvent, that a gateway refused an order meanwhile
    # saying the venue is halted, and that the restarted engine halted and cancels nothing. See run_scenario's "recovery
    # limit" block.
    assert_recovery_halt: bool = False
    # When True, assert that no halt for the recovery limit was written: a failover finishes well within the limit.
    assert_no_recovery_halt: bool = False
    # When True, the leading matching engine is started with its sends on the link between the engines made to fail
    # while a flag file exists, so that the link breaks and is mended at moments the scenario chooses; the scenario then
    # places and cancels orders across the break, kills the leading engine, and requires the promoted engine to hold
    # exactly the orders the member was told are open. See run_scenario's "link between the engines" block.
    assert_book_survives_peer_link_break: bool = False
    # When True, the leading matching engine is started with its sends of reports to the sequencers blocked once a flag
    # file exists; the scenario sends orders after blocking them, kills the leading engine, and requires the member to be
    # sent a report of every one of those orders by the engine that takes the lead. See run_scenario's "kept reports" block.
    assert_kept_reports_sent_on_promotion: bool = False
    # When True, damage the matching engine's open-order region before restarting it, and
    # assert that the engine refuses it, keeps it aside, rebuilds its book from the WAL, which
    # reaches the first command of the day, and carries on with every order, cancelling nothing
    # (R-0123). See run_scenario's "damaged region" block.
    assert_damaged_region: bool = False
    # Why this scenario is expected to fail, when the venue is known not to meet the
    # requirement it asserts. A scenario marked this way does not fail the suite when it
    # fails; it fails the suite when it PASSES, because the gap has closed and the marking
    # must be removed. A gap recorded as a passing test is the failure this file exists to
    # avoid, so it is never recorded that way.
    expected_failure: str = ""
    # How long every gateway waits, after one of its links to a sequencer comes up, before it announces the sessions it
    # holds ([test_events] delay_session_announcements_ms); 0 for no wait. Lets a scenario make a restarted sequencer
    # lead before it has been told of the member's session. See
    # docs/availability/reports_held_until_sessions_are_announced.md.
    delay_session_announcements_ms: int = 0
    # When True, the restarted primary sequencer must hold the cancels the promoted matching engine sends for the
    # member's session, which it has not yet been told of, and send them when the gateway announces it; and the member
    # must receive a cancel for every order the engine cancelled. See run_scenario's "held reports" block.
    assert_held_reports_reach_member: bool = False
    # When True, the primary sequencer is started with libblock_sends_to_ports.so preloaded, so
    # that the test can stop everything it sends to its peer from arriving, by creating a flag
    # file. Until the file exists the library passes every send through. Used by the
    # "unreplicated orders" block, which creates the file.
    block_leader_peer_sends: bool = False
    # When True, make the leading sequencer die holding orders its follower never received, and
    # assert that the matching engine does not hold an order the new leader's log does not hold.
    # See run_scenario's "unreplicated orders" block.
    assert_engine_holds_only_logged_orders: bool = False
    # When True, note where the secondary sequencer's write-ahead log ends before Phase 4, and
    # after the scenario require every record written since, read from the log files, to carry a
    # higher sequence number than the one before it (BUG-0105). For a scenario in which the
    # secondary takes the lead and numbers new records.
    assert_new_leader_numbers_forward: bool = False
    # When True, stop the follower sequencer and assert that the leader runs as if alone and keeps
    # trading, and returns to waiting for the follower when it runs again. See run_scenario's
    # "stalled follower" block.
    assert_stalled_follower_runs_alone: bool = False
    # When True, both sequencers are started holding at most _FEW_HELD_ORDERS orders, the follower is
    # stopped, and a burst of orders is sent, so that the leader's storage for held orders fills and the
    # rest wait in the write-ahead log. Requires that the storage filled, that the reading thread sent the
    # orders that waited to the matching engine and handed it over, and that the engine accepted every
    # order exactly once. See run_scenario's "orders that waited in the log" block, and
    # docs/availability/following_the_wal.md, section 5.3.
    assert_orders_that_waited_in_the_log_reach_engine: bool = False
    # When True, both matching engine publishers are started too, each with an empty write-ahead log.
    with_publishers: bool = False
    # When True, restart a publisher during the day, then make the leading sequencer die holding records
    # its follower never received, and require each publisher's write-ahead log to hold every record once,
    # without a gap, each the same as the record the leading sequencer's log holds under that number. Needs
    # with_publishers and block_leader_peer_sends. See run_scenario's "publishers follow only the record"
    # block, and BUG-0153.
    assert_publishers_follow_only_the_record: bool = False
    # When True, kill the follower sequencer, have the leader take orders, restart the follower and
    # require its log to end identical to the leader's from where it stopped, then kill the leader
    # and require the new leader to hold every order the engine accepted. See run_scenario's
    # "follower restarts" block, and docs/availability/follower_log_repair.md.
    assert_restarted_follower_log_repaired: bool = False
    # When True, make the leading sequencer hold records its follower never received, kill it, have
    # the follower lead and take orders, restart the old leader, and require it to discard those
    # records and end with a log identical to the new leader's. Needs block_leader_peer_sends. See
    # run_scenario's "old leader rejoins" block.
    assert_rejoining_old_leader_log_repaired: bool = False
    # When True, stop the follower, have the leader take an order once a voter confirms that the
    # follower may not lead, kill the leader, and require the follower NOT to take the lead; then
    # restart the old leader and require it to lead holding the order. With
    # restart_arbiter_before_leader_dies, the active arbiter is restarted before the leader is killed,
    # and its record of the statement must survive. See run_scenario's "rule 11" block, and
    # docs/availability/a_follower_behind_does_not_lead.md.
    assert_follower_behind_does_not_lead: bool = False
    restart_arbiter_before_leader_dies: bool = False
    # When True, the primary sequencer is started with libblock_sends_to_ports.so preloaded, set to
    # block what it sends to the gateways once the flag file exists. The scenario sends orders whose
    # reports the leader therefore cannot forward, kills the leader, and requires each report to
    # reach the member from the new leader, marked PossResend. See run_scenario's "kept reports"
    # block, and section 4.4 of docs/availability/change_of_sequencer_leader.md.
    assert_kept_reports_forwarded: bool = False
    # When True, the secondary sequencer is started with libblock_sends_to_ports.so preloaded, set to
    # block what it sends to its peer once the flag file exists, so the follower writes and
    # acknowledges records whose acknowledgements never reach the leader. The scenario sends orders,
    # stops the leader before it can act on them, kills it, and requires every order to be answered
    # once the follower leads. See run_scenario's "logged commands reach the engine" block, and
    # section 3.5 of docs/availability/commands_during_a_change_of_leader.md.
    assert_logged_commands_reach_engine: bool = False
    # When True, send the leading sequencer an order, then the same order marked as sent again, then
    # an order marked as sent again that was never sent, and require the engine to apply each order
    # once. See run_scenario's "commands sent again" block, and section 3.4 of
    # docs/availability/commands_during_a_change_of_leader.md.
    assert_sent_again_sequenced_once: bool = False
    # With assert_sent_again_sequenced_once: kill the leading sequencer after the first order is accepted, and
    # send the command again to the follower once it leads. The follower holds the order only because its
    # leader replicated it, so this checks that a follower notes the commands in the records it writes.
    sent_again_after_change_of_leader: bool = False
    # When True, a member sends orders at a steady rate from before the leading sequencer is killed
    # until after the follower has taken the lead, and every order must be answered, and none applied
    # twice. See run_scenario's "orders during a change of leader" block, and
    # docs/availability/commands_during_a_change_of_leader.md.
    assert_orders_during_change_answered: bool = False
    # When True, a member sends a burst of orders and waits for every answer, the leading sequencer
    # is killed, and the new leader must send the member again only the few reports the old leader
    # had not said it forwarded. See run_scenario's "few repeats" block, and BUG-0116.
    assert_few_repeats_after_change: bool = False
    # When True, a member places an order and disconnects, the order is cancelled while it is away,
    # and the member must be sent the cancel report when it reconnects. See run_scenario's "reports
    # produced while unbound" block, R-0005, and BUG-0088.
    assert_unbound_reports_delivered: bool = False
    # When True, gateway a is started with a small queue of waiting sends, the leading sequencer is
    # stopped while a member sends orders until the gateway closes its connection to it, and every
    # order must be accepted exactly once after the sequencer runs again. See run_scenario's
    # "commands sent again after a reconnection" block, and BUG-0118.
    assert_commands_resent_after_reconnection: bool = False
    # When True, the matching engine's connections for reports go through a relay this script runs,
    # which refuses them at first, so that orders reach the engine while it has no route for their
    # reports. Every order must be accepted once the relay lets the connections through. See
    # run_scenario's "reports held without a connection" block, and BUG-0120.
    assert_engine_holds_reports_without_connection: bool = False
    # When True, each sequencer's connection for reports to fix_order_gateway_a goes through a relay this script runs,
    # which at first accepts nothing, so that the leading sequencer has no connection for reports to the gateway while
    # the gateway's connection to it for orders is up. A member sends orders, and every one must be reported to it once
    # the relay lets the connection through. See run_scenario's "reports held for a gateway with no connection" block,
    # and BUG-0202.
    assert_reports_held_for_unconnected_gateway: bool = False
    # When True, matching_engine_secondary is started holding fewer open orders than matching_engine_primary, so that the two
    # would apply the same commands differently. The engine that leads must say its peer may not lead, and the peer must not
    # take the lead when the leader is killed. See run_scenario's "engines whose settings differ" block, and
    # docs/availability/deterministic_matching_engine.md, section 5.
    assert_engines_with_different_settings: bool = False
    # With assert_engines_with_different_settings: when True, the two engines differ in the secret their identifiers'
    # keys are derived from rather than in the size of their regions. matching_engine_secondary is started with a secret
    # of its own, so the two would name the same reports differently. See docs/availability/deterministic_matching_engine.md,
    # section 3.3.
    different_identifier_secret: bool = False
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
    # With assert_bounded_resend: kill the leading sequencer just before the member asks, so the
    # request reaches only the follower, which declines it because it does not lead. The gateway must
    # ask again when the follower takes over and announces itself, and the answer must still be the
    # messages that occupied the numbers asked for (BUG-0148).
    bounded_resend_with_leader_killed: bool = False
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
    # When True, provision a comp id with small limits on new orders and cancels per second, send
    # bursts over them through the FIX gateway, and assert exactly the commands over the limit are
    # refused, with the limit in the text. See run_scenario's "throttles" block and
    # docs/venue/gateway_throttles.md.
    assert_gateway_throttles: bool = False
    # When True, also start binary_order_gateway_a, directly after the FIX gateway. Only the
    # scenarios that test the binary gateway want it.
    binary_gateway: bool = False
    # When True, send the binary gateway orders and cancels it must refuse, and some it must
    # accept, and check each reply. See run_scenario's "binary checks" block and BUG-0100.
    assert_binary_checks: bool = False
    # When True, hand the sequencer an order and a cancel with identifiers longer than the matching
    # engine's book key, bypassing both gateways, and check the engine refuses them -- and refuses
    # them again when it replays them after a restart, without stopping its catch-up. BUG-0101.
    assert_engine_identifier_refusal: bool = False
    # When True, take every matching engine away, place an order that the sequencer therefore
    # defers, start one engine COLD, and assert the member is answered for it. See
    # run_scenario's "deferred orders" block, BUG-0064 and the surviving half of BUG-0009.
    assert_deferred_answered: bool = False
    # When True, require the matching engine that leads at the end, matching_engine_primary, to report resuming as leader
    # once it has caught up, holding exactly as many orders as the member was told are open. See run_scenario's "an
    # engine that waited catches up before it leads" block, BUG-0198 and BUG-0199.
    assert_waiting_engine_catches_up: bool = False
    # For a scenario with both matching engines whose subject needs one of them to lead at the start: the engine named,
    # "matching_engine_primary" or "matching_engine_secondary", is started first, and the other only once it leads, and
    # the steps begin once the other follows, as --secondary-engine-first does for every scenario. Empty for the usual
    # start, in which either engine may win the first election. A scenario that sets it is not run with
    # --secondary-engine-first.
    engine_started_first: str = ""
    # Override args.orders_before for this scenario, in thousands (None = use the CLI value). For a scenario that needs
    # a WAL long enough that catching up from its start takes a measurable time.
    orders_before_override: int | None = None
    # When True, require the restarted matching_engine_primary to finish catching up from the start of the WAL, without
    # finding the stream go backwards, while the second sequencer connected to it after the catch-up began. See
    # run_scenario's "a second sequencer during a catch-up" block, and BUG-0200.
    assert_catch_up_survives_second_connection: bool = False
    # When True, no instance is expected to take the lead during the scenario: it tests the halt
    # when the leader and the arbiters are lost together (R-0147). The checks that follow a
    # promotion, such as which book the promoted instance resumed with, are skipped.
    expects_no_new_leader: bool = False

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
        ready=expect_engine_reachable("matching_engine", "matching_engine"),
        ready_timeout=_ME_READY_TIMEOUT,
        resets_me_counter=True,
        settle_secs=_ME_SETTLE,
    )


def _me_leader_restart_step() -> RestartStep:
    """Restart the matching engine of a pair that led when the steps began, as _me_restart_step does the single one."""
    return RestartStep(
        proc_name=ENGINE_LEADING_AT_START,
        ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
        ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
        ready_timeout=_ME_READY_TIMEOUT,
        resets_me_counter=True,
        settle_secs=_ME_SETTLE,
    )


def _waiting_engine_steps() -> list:
    """The steps of scenarios 78 and 79: kill matching_engine_primary, restart it without its record of the lease promises
    it made, send orders while it is caught up and waiting to learn its role, then kill matching_engine_secondary so that
    the primary is granted the lead. The comment above scenario 78 says why each step is there."""
    return [
        KillStep(
            proc_name="matching_engine_primary",
            takeover=None,
            settle_secs=0.0,
        ),
        # Judged over everything the secondary has reported since it started, not only since the kill, because when
        # the primary followed, the secondary has led since the start.
        VerifyStep(
            expect=expect_leads("matching_engine_secondary"),
            timeout=30.0,
            description="matching_engine_secondary leads",
        ),
        RestartStep(
            proc_name="matching_engine_primary",
            ready_log_name="matching_engine_primary.log",
            ready=expect_catch_up("matching_engine_primary", "CurrentAndWaiting"),
            ready_timeout=_ME_READY_TIMEOUT,
            resets_me_counter=True,
            settle_secs=0.0,
            forget_lease_promises=True,
        ),
        InterimOrdersStep(count_batches=1),
        VerifyStep(
            expect=expect_catch_up("matching_engine_primary", "CurrentAndWaiting"),
            absent=expect_follows("matching_engine_primary"),
            timeout=1.0,
            description="the restarted matching_engine_primary is caught up and waiting, and has not followed",
        ),
        KillStep(
            proc_name="matching_engine_secondary",
            takeover=expect_leads("matching_engine_primary"),
            settle_secs=SETTLE_AFTER_FAILOVER,
        ),
        VerifyStep(
            expect=expect_leads("matching_engine_primary"),
            absent=expect_follows("matching_engine_primary"),
            timeout=1.0,
            description="the restarted matching_engine_primary took the lead without having followed",
        ),
    ]


# A single matching engine with high availability on has no running peer, so its only voters are
# itself and the arbiter pool. Losing the arbiter pool then leaves it holding only its own vote, and
# it stops leading: the halt of R-0147, correctly. Losing only the active arbiter stops it too, for
# a while: the arbiter that becomes active grants nothing for one lease period, because it does not
# know what the other promised. So every scenario that takes away the active arbiter, or the whole
# pool, runs the pair of matching engines, as a deployment does, and the primary is expected to keep
# leading with its peer's vote.

# ── scenario catalogue ────────────────────────────────────────────────────────
#
# How leadership is decided, which every scenario below depends on. Each pair -- the two
# sequencers, the two matching engines, the two arbiters -- has three voters. For a component
# pair they are its two instances and the arbiter pool, which votes through whichever arbiter is
# active; for the arbiters they are the two arbiters and the witness. An instance leads only while
# its peer or the third voter has granted it a lease that has not run out, so its own vote and one
# grant make a majority. The lease period is 3 s and a leader renews every second. The rules are in
# docs/availability/majority_leases.md; scenario-specific notes are beside each scenario below.
#
# Scenario 1 — Primary sequencer death
#   sequencer_primary is killed. Its peer and the active arbiter had promised it their votes, and
#   those promises run out a lease period after its last renewal. sequencer_secondary then asks
#   to lead, the arbiter grants it, and it leads. The gateway already has a connection to the
#   secondary and forwards new orders to it; the matching engine already sends its execution
#   reports to both sequencers, so they reach the secondary at once.
#   The burst of orders sent just before the kill reaches the venue before the kill happens, so
#   this scenario does not send orders during the change of leader. Scenario 68 does.
#
# Scenario 2 — Primary arbiter death
#   arbiter_primary, the active arbiter, is killed. arbiter_secondary asks the witness once the
#   witness's promise to the primary has run out, is granted, and becomes active. It then grants
#   no component a lease for one lease period, because it does not know what the other promised.
#   Leaders renew with their peers meanwhile, so order flow continues. The pair of matching
#   engines is run for this reason: a lone engine would renew with nobody during that period.
#
# Scenario 3 — Secondary sequencer death
#   The follower is killed. sequencer_primary renews with the arbiter alone and goes on leading.
#   No role changes, and orders continue.
#
# Scenario 4 — Secondary arbiter death
#   The passive arbiter is killed. arbiter_primary renews with the witness alone and stays
#   active. No component notices, and orders continue.
#
# Scenario 5 — Witness death
#   The witness is the third voter for the arbiters only. The active arbiter renews with its peer
#   arbiter instead, and nothing else changes. If the active arbiter were then lost as well, the
#   remaining arbiter would hold only its own vote and could not become active: scenario 8.
#
# Scenario 6 — Both arbiters dead
#   Every leader renews with its peer, whose vote with its own is a majority, so no role changes
#   and orders continue. What is lost is the ability to fail over: a leader lost now leaves its
#   group without a leader until an arbiter returns (R-0147), which scenario 9 tests.
#
# Scenario 7 — Sequential cascade: arbiter_primary then sequencer_primary
#   As scenario 2, then as scenario 1 with arbiter_secondary as the active arbiter. The sequencer
#   takeover waits for the new active arbiter's lease period to pass as well. Shows that an
#   arbiter that has just become active grants a component lease correctly.
#
# Scenario 8 — An arbiter left with only its own vote
#   The witness is killed, then the active arbiter. arbiter_secondary holds only its own vote, so
#   it does not become active, and it reports that the arbiters have no leader. Component leaders
#   renew with their peers, so order flow is uninterrupted.
#
# Scenario 9 — No arbiters, then the leader lost (R-0147)
#   Both arbiters are killed, then sequencer_primary. sequencer_secondary cannot tell a dead
#   leader from being cut off itself, and holds only its own vote, so it must not lead. It reports
#   that the group has no leader, and no orders are sent afterwards. This is the case in which
#   trading halts, by design, until an arbiter or the leader returns.
#
# Scenario 10 — Matching engine death and restart
#   The single matching engine is killed, its log deleted, and it is restarted. Its record of
#   leading, kept on disk since this boot of the machine, lets it ask to lead again at once, and
#   the arbiter grants it. It reconnects to both sequencers and recovery orders are confirmed in
#   its new log.
#
# Scenario 11 — Matching engine death with primary arbiter death
#   As scenario 2 for the arbiters, then as scenario 10 for the matching engine. The restarted
#   engine is granted its lease by the arbiter that has become active, once that arbiter's lease
#   period has passed.
#
# Scenario 12 — Matching engine restart with both arbiters dead
#   Both arbiters are killed, then the primary of the pair of matching engines is restarted. It
#   reads its record of leading, asks again at once, and its peer grants it, because the peer's
#   vote is promised to it. It keeps the lead with no arbiter at all, and recovery orders flow.
#
# Scenario 13 — as scenario 12, with the witness dead as well
#   The witness takes no part in deciding which matching engine leads, so the outcome is the same.
#
_SCENARIOS: list[Scenario] = [
    # 1 — primary sequencer death: expect sequencer_secondary to become leader
    Scenario(
        number=1,
        short_name="primary_sequencer_death",
        description="Death of primary sequencer",
        expected_outcome=(
            "sequencer_secondary elected leader in ≤15 s; "
            "recovery orders flow through the new leader, numbered above every record its log holds"
        ),
        assert_new_leader_numbers_forward=True,
        steps=[
            KillStep(
                proc_name="sequencer_primary",
                takeover=expect_leads("sequencer_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 2 — primary arbiter death: expect arbiter_secondary to become active
    Scenario(
        number=2,
        short_name="primary_arbiter_death",
        me_ha=True,
        recovery_on_primary=True,
        description="Death of primary arbiter",
        expected_outcome=(
            "arbiter_secondary elected active in ≤15 s; "
            "sequencer_primary remains leader; orders uninterrupted"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=expect_leads("arbiter_secondary"),
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
                takeover=None,
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
                takeover=None,
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
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
    ),

    # 6 — both arbiters dead: sequencer primary stays leader, HA is degraded
    Scenario(
        number=6,
        short_name="both_arbiters_dead",
        me_ha=True,
        recovery_on_primary=True,
        description="Death of both arbiters",
        expected_outcome=(
            "no sequencer failover; sequencer_primary remains leader; "
            "orders continue (WARNING: HA degraded — re-election impossible)"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
    ),

    # 7 — sequential cascade: arbiter failover then sequencer failover
    Scenario(
        number=7,
        short_name="cascade_arbiter_then_sequencer",
        me_ha=True,
        recovery_on_primary=True,
        description="Sequential cascade: arbiter_primary then sequencer_primary death",
        expected_outcome=(
            "arbiter_secondary elected active in ≤15 s; "
            "then sequencer_secondary elected leader via new arbiter in ≤15 s; "
            "recovery orders flow through new sequencer leader"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=expect_leads("arbiter_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="sequencer_primary",
                takeover=expect_leads("sequencer_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 8 — witness then arbiter_primary death: the surviving arbiter holds only its own vote.
    #
    # An arbiter is active only while a majority of the two arbiters and the witness grants it a
    # lease. With the witness and the other arbiter gone, arbiter_secondary can gather no second
    # vote, so it must not become active, and it says the pool has no active arbiter. The
    # sequencer leader is unaffected: it renews its lease with its peer. The absence of a
    # promotion is checked long after it would have happened: a lease period for the promise to
    # the dead arbiter to run out, another for the request to go unanswered, and margin.
    Scenario(
        number=8,
        short_name="witnessless_arbiter_election",
        me_ha=True,
        recovery_on_primary=True,
        description="Witness-less arbiter election: witness then arbiter_primary death",
        expected_outcome=(
            "arbiter_secondary cannot gather a majority and does not become active; "
            "sequencer_primary remains leader; orders uninterrupted"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="witness",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=0.0,
            ),
            VerifyStep(
                expect=expect_lease("arbiter_secondary", "NoLeader"),
                timeout=25.0,
                description="arbiter_secondary reported that the arbiter pool has no active arbiter",
            ),
            AssertAbsentStep(
                absent=expect_leads("arbiter_secondary"),
                after_secs=5.0,
                description="arbiter_secondary did not become active with only its own vote",
            ),
        ],
    ),

    # 9 — both arbiters then sequencer_primary death: the venue halts, and says why (R-0147).
    #
    # sequencer_secondary hears from nobody: its peer is dead and there is no arbiter. It cannot
    # tell a dead leader from being cut off itself, so it must not take the lead, and it reports
    # that the group has no leader. Nothing can be traded until an arbiter or the peer returns, so
    # no orders are sent during or after the kill.
    Scenario(
        number=9,
        short_name="no_arbiters_and_leader_lost",
        description="Both arbiters then sequencer_primary death: the survivor does not take the lead",
        expected_outcome=(
            "sequencer_secondary does not take the lead with no majority, and reports that the "
            "group has no leader (R-0147)"
        ),
        steps=[],
        restart_steps=[],
        orders_during_override=0,
        orders_after_override=0,
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="sequencer_primary",
                takeover=None,
                settle_secs=0.0,
            ),
            VerifyStep(
                expect=expect_lease("sequencer_secondary", "NoLeader"),
                timeout=25.0,
                description="sequencer_secondary reported that the group has no leader",
            ),
            AssertAbsentStep(
                absent=expect_leads("sequencer_secondary"),
                after_secs=5.0,
                description="sequencer_secondary did not take the lead without a majority",
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
                takeover=expect_leads("arbiter_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
        restart_steps=[_me_restart_step()],
    ),

    # 12 — the leading matching engine restarted while both arbiters are dead.
    #
    # With the arbiter pool gone, the leading engine keeps leading with its peer's vote. When it
    # is restarted it reads its record of leading, asks again at once, and its peer grants it,
    # because the peer's vote is promised to it. So it keeps the lead with no arbiter at all.
    Scenario(
        number=12,
        short_name="me_death_both_arbiters_dead",
        me_ha=True,
        recovery_on_primary=True,
        description="ME death with both arbiters dead",
        expected_outcome=(
            "no arbiter failover; the restarted matching engine that led keeps the lead with its "
            "peer's vote, and recovery orders flow"
        ),
        steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
        restart_steps=[_me_leader_restart_step()],
    ),

    # 13 — as scenario 12, with the witness dead as well.
    Scenario(
        number=13,
        short_name="me_death_all_ha_dead",
        me_ha=True,
        recovery_on_primary=True,
        description="ME death with both arbiters and witness dead",
        expected_outcome=(
            "no arbiter failover; the restarted matching engine that led keeps the lead with its "
            "peer's vote, and recovery orders flow"
        ),
        steps=[
            KillStep(
                proc_name="witness",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=SETTLE_AFTER_KILL,
            ),
        ],
        restart_steps=[_me_leader_restart_step()],
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
    #   f. 1000 recovery orders, the 3000th order in all.
    #
    # If WAL recovery + peer sequence-number sync work correctly the primary
    # resumes from seq 2001 and the ME log shows seq 1–1000, 1001–2000, 2001–3000
    # — monotonically increasing, no resets.  If the primary fails to sync and
    # resets to seq 1001, the ME log shows the range 1001–2000 a second time;
    # the seq monotonicity check catches the reset and the test fails.
    Scenario(
        number=14,
        short_name="wal_recovery",
        description="WAL recovery: primary sequencer restarts as follower then re-elected leader",
        expected_outcome=(
            "sequencer_primary restarts, reads WAL, syncs sequence number from "
            "peer and rejoins as follower; after secondary dies primary is "
            "re-elected and continues from the correct sequence number — "
            "no gap or reset in the sequence numbers"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                takeover=expect_leads("sequencer_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            InterimOrdersStep(count_batches=1),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready=expect_follows("sequencer_primary"),
                ready_timeout=_SEQ_FOLLOWER_TIMEOUT,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            VerifyStep(
                expect=expect_session_announced_again("sequencer_primary"),
                timeout=10.0,
                description="the restarted sequencer_primary was told the member's session is still connected (BUG-0182)",
            ),
            KillStep(
                proc_name="sequencer_secondary",
                takeover=expect_leads("sequencer_primary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 15 — Lease exchange: explicit trace of a follower being granted the lead by the arbiter.
    #
    # Scenario 1 verifies that sequencer_secondary becomes leader after primary dies, but only
    # checks the final role-transition log line. This scenario verifies each step of the lease
    # exchange that gets it there:
    #   a. sequencer_secondary's promise to the dead leader runs out, and it asks to lead.
    #   b. The active arbiter (arbiter_primary) grants it a lease.
    #   c. sequencer_secondary receives the grant, which with its own vote is a majority.
    #   d. sequencer_secondary transitions to leader.
    # The request goes out about one lease period after the kill.
    Scenario(
        number=15,
        short_name="lease_exchange",
        description="Lease exchange: explicit trace of the arbiter granting the follower the lead",
        expected_outcome=(
            "sequencer_secondary asks to lead; arbiter_primary grants it a lease; "
            "sequencer_secondary leads on that grant; recovery orders flow"
        ),
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                takeover=None,
                settle_secs=0.0,
            ),
            VerifyStep(
                expect=expect_lease("sequencer_secondary", "AskingToLead"),
                timeout=25.0,
                description="sequencer_secondary asked to lead",
            ),
            VerifyStep(
                expect=expect_granted_to("arbiter_primary", 1, 2),
                timeout=5.0,
                description="arbiter_primary granted sequencer_secondary a lease",
            ),
            VerifyStep(
                expect=expect_lease("sequencer_secondary", "Granted", "ThirdVoter"),
                timeout=5.0,
                description="sequencer_secondary received the grant",
            ),
            VerifyStep(
                expect=expect_leads("sequencer_secondary"),
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
            "the engine that followed detects the loss of the one that led, promotes via the "
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
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
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
                takeover=None,   # active/active — no promotion to await
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
    # Getting a deterministic ER for an order whose gateway is dead is the awkward part.
    # Killing the gateway mid-burst would work but is a race. So this borrows a burst the
    # venue produces for its own reasons: the baseline orders rest on the book, the engine
    # is killed and left down past the period the venue says it may be unable to match, so
    # the leading sequencer writes a command cancelling the book, and the restarted engine
    # cancels every order it recovers and reports each cancellation (R-0117).
    # Those orders came from gateway a, which by then is dead, so every one of those reports
    # has nowhere to go.
    #
    # It used to borrow the burst a PROMOTION emitted, cancelling the whole book of a
    # promoted secondary. That stopped happening on 2026-08-30, when the engine gained a
    # record of its open orders and a promotion began keeping them (R-0073). The absence
    # rule is a better generator anyway: it needs one engine rather than a pair, and it is
    # the venue doing something it must do rather than something it was doing for want of
    # being able to do better.
    #
    # Kill order matters: gateway a first, the engine second. The reverse would let the
    # cancellation reports reach a before it died and prove nothing.
    #
    # Gateway a is never restarted and no member reconnects, so the leading sequencer has no
    # connection for reports to instance 1 when the cancellation reports are produced. It holds
    # them, up to the capacity it is configured with, to send them when that connection is made,
    # and it does not send them to instance b. The scenario checks that the sequencer says it is
    # holding reports for instance 1, that it drops none, and that b sees none of them.
    Scenario(
        number=18,
        short_name="fix_gateway_a_death",
        description="Death of FIX gateway instance A (no session handover to B)",
        expected_outcome=(
            "no election (a gateway elects nothing); instance b keeps running but "
            "inherits none of a's sessions, and the sequencer holds every execution "
            "report bound for the dead instance rather than dropping it or rerouting it to b"
        ),
        gateway_b=True,
        # Short enough that the scenario need not sit out the deployed five minutes, and the
        # restart below stays down comfortably past it.
        absence_limit_seconds=5,
        # The FIX session lives on gateway a. Once a is killed there is no session to
        # send through, so Phase 5 has nothing to do.
        orders_during_override=0,
        orders_after_override=0,
        assert_gateway_orphaned=True,
        steps=[
            KillStep(
                proc_name="fix_order_gateway_a",
                takeover=None,   # nothing is elected — that is the point
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
        restart_steps=[
            RestartStep(
                proc_name="matching_engine",
                ready_log_name="matching_engine.log",
                ready=expect_engine_reachable("matching_engine", "matching_engine"),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=_ME_SETTLE,
                down_secs=12.0,
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
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
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
                takeover=None,   # nothing is elected -- a gateway elects nothing
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
            "the engine that followed promotes and keeps leadership; the restarted "
            "engine that led is assigned the follower role by the arbiter and "
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
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            # 2. Bring the primary back. The restart deletes its log first, so everything
            #    asserted on below happened after it came back.
            RestartStep(
                proc_name=ENGINE_LEADING_AT_START,
                ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            # 3. The assertion the fix exists for, stated both ways round.
            VerifyStep(
                expect=expect_follows(ENGINE_LEADING_AT_START),
                timeout=30.0,
                description="the restarted engine that led is assigned the follower role",
                absent=expect_leads(ENGINE_LEADING_AT_START),
            ),
            # 4. And the engine promoted in its place must have kept the lead throughout -- a
            #    leadership that moved and moved back would satisfy step 3 on its own.
            VerifyStep(
                expect=expect_leads(ENGINE_FOLLOWING_AT_START),
                timeout=5.0,
                description="the engine promoted in its place still holds leadership",
            ),
        ],
    ),

    # 25 — arbiters that restart knowing nothing do not move the lead.
    #
    # An arbiter keeps its votes for the components only in memory. When both arbiters restart,
    # nothing they hold says which matching engine leads. That must not matter: the leader
    # renews its lease with its peer, whose vote with its own is a majority, so it keeps
    # leading while the arbiters are away and after they return. And an arbiter that has just
    # become active grants nothing for a lease period, so it cannot give the lead to the other
    # instance while it does not know what was promised.
    #
    # The primary is failed first, so that the instance leading is the secondary -- the one a
    # rule preferring the lower instance id would wrongly hand the lead away from. The primary
    # is back and following BEFORE the arbiters go down. With the primary still dead, losing
    # the arbiters would leave the secondary with only its own vote, and it would rightly stop:
    # that is the halt of R-0147, which scenario 35 tests.
    #
    # BOTH arbiters are down at the same time. Restarting one leaves the other holding what
    # was promised, which proves nothing.
    Scenario(
        number=25,
        short_name="arbiters_restart_then_me_rejoins",
        description="Both arbiters restart with no state while the secondary ME leads",
        expected_outcome=(
            "the secondary matching engine keeps leading with its peer's vote while both arbiters "
            "are down and after they return; the primary stays a follower throughout"
        ),
        me_ha=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            # 1. Fail the primary so the secondary leads.
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            # 2. Bring the primary back. It follows, and grants its peer its vote.
            RestartStep(
                proc_name=ENGINE_LEADING_AT_START,
                ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            VerifyStep(
                expect=expect_follows(ENGINE_LEADING_AT_START),
                timeout=30.0,
                description="the restarted primary follows the secondary",
            ),
            # 3. Take both arbiters down at the same time, then bring them back. RestartStep kills
            #    and restarts one component before moving to the next, so both are killed first.
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=5.0,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready=expect_role("arbiter_primary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=1.0,
            ),
            RestartStep(
                proc_name="arbiter_secondary",
                ready_log_name="arbiter_secondary.log",
                ready=expect_started("arbiter_secondary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            # 4. The assertions. The secondary never lost its lease, and the primary never led.
            AssertAbsentStep(
                absent=expect_lease(ENGINE_FOLLOWING_AT_START, "RanOut"),
                after_secs=10.0,
                description="the secondary kept its lease while the arbiters were away and after they returned",
            ),
            AssertAbsentStep(
                absent=expect_leads(ENGINE_LEADING_AT_START),
                after_secs=0.0,
                description="the restarted primary never led",
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
            "the launcher restarts the engine that led within seconds; "
            "the engine that followed never promotes and the one that led keeps leadership"
        ),
        me_ha=True,
        # Both, because which one leads is not known until both have started; the step kills the one that leads.
        supervised=("matching_engine_primary", "matching_engine_secondary"),
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            SupervisedKillStep(
                proc_name=ENGINE_LEADING_AT_START,
                restart_timeout=30.0,
                settle_secs=2.0,
            ),
            # A follower takes over within about one lease period plus one renewal interval,
            # a few seconds. Waiting 25 leaves no room for argument about whether the secondary
            # simply had not got round to it.
            AssertAbsentStep(
                absent=expect_leads(ENGINE_FOLLOWING_AT_START),
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
            VerifyStep(
                expect=expect_session_announced_again("sequencer_primary"),
                timeout=10.0,
                description="the restarted sequencer_primary was told the member's session is still connected (BUG-0182)",
            ),
            AssertAbsentStep(
                absent=expect_leads("sequencer_secondary"),
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
                absent=expect_leads("arbiter_secondary"),
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
    # The assertion is built the other way round: prove the follower came back and rejoined
    # properly (the leader's replication connection is re-established to it), then require that
    # it never promoted. RestartStep deletes its log first, so the absence covers only what happened
    # after it returned.
    Scenario(
        number=29,
        short_name="me_follower_restart_changes_nothing",
        description="Restarting the follower ME leaves leadership untouched",
        expected_outcome=(
            "the engine that followed returns as a follower, rejoins replication, and never "
            "adopts LEADER; the engine that led keeps leading and orders are uninterrupted"
        ),
        me_ha=True,
        # Leadership never moves, so recovery orders come back to the primary.
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            RestartStep(
                proc_name=ENGINE_FOLLOWING_AT_START,
                ready_log_name=f"{ENGINE_FOLLOWING_AT_START}.log",
                ready=expect_replication_connection(ENGINE_FOLLOWING_AT_START),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            AssertAbsentStep(
                absent=expect_leads(ENGINE_FOLLOWING_AT_START),
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
                ready=expect_follows("sequencer_secondary"),
                ready_timeout=_SEQ_FOLLOWER_TIMEOUT,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            AssertAbsentStep(
                absent=expect_leads("sequencer_secondary"),
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
                ready=expect_started("arbiter_secondary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            AssertAbsentStep(
                absent=expect_leads("arbiter_secondary"),
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
            "the engine that followed promotes, the restarted one that led rejoins as follower, "
            "and when the promoted one then dies the one that led takes leadership back"
        ),
        me_ha=True,
        # The primary ends up leading again, so recovery orders return to it.
        recovery_on_primary=True,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            RestartStep(
                proc_name=ENGINE_LEADING_AT_START,
                ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            VerifyStep(
                expect=expect_follows(ENGINE_LEADING_AT_START),
                timeout=60.0,
                description="the rejoined primary is a follower before the second failure",
            ),
            # Wait for the leader's replication connection to reach the rejoined instance
            # before failing the leader. The rejoined instance is sent the book over that
            # connection, and it is also the one the leader's lease requests reach it on, so
            # until it exists the rejoined instance holds no promise to the leader and no
            # replica worth promoting. A version of this scenario without the wait raced that
            # window -- passing alone and failing inside the suite, which is worse than failing
            # outright.
            VerifyStep(
                expect=expect_replication_connection(ENGINE_LEADING_AT_START),
                timeout=60.0,
                description="the new leader's replication connection has reached the rejoined instance",
            ),
            # Now the second failure, in the other direction.
            KillStep(
                proc_name=ENGINE_FOLLOWING_AT_START,
                takeover=expect_leads(ENGINE_LEADING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
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
        me_ha=True,
        recovery_on_primary=True,
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
                takeover=expect_leads("arbiter_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready=expect_started("arbiter_primary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            # The second failure, the other way round. The restarted arbiter -- passive since
            # rejoining -- must now take over.
            KillStep(
                proc_name="arbiter_secondary",
                takeover=expect_leads("arbiter_primary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 34 — a pair of matching engines started one after the other settles on exactly one leader.
    #
    # Both engines are taken down and brought back with the SECONDARY first, and some seconds
    # between the two starts. The secondary waits a lease period plus one renewal interval
    # before asking to lead, so that the primary leads when both start together. Started this
    # far apart, the secondary may ask first and lead, and that is correct: what must hold is
    # that exactly one of them leads.
    Scenario(
        number=34,
        short_name="cold_start_tiebreak_is_deterministic",
        description="Restarting both engines secondary-first leaves exactly one leading",
        expected_outcome=(
            "with both engines restarted, the secondary first, exactly one of them leads"
        ),
        me_ha=True,
        recovery_on_primary=True,
        # Which engine leads depends on the gap between the starts, so recovery orders could
        # be confirmed on either. The assertion is about leadership alone.
        orders_after_override=0,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name=ENGINE_FOLLOWING_AT_START,
                takeover=None,
                settle_secs=2.0,
            ),
            # Secondary back first, deliberately.
            RestartStep(
                proc_name=ENGINE_FOLLOWING_AT_START,
                ready_log_name=f"{ENGINE_FOLLOWING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_FOLLOWING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            RestartStep(
                proc_name=ENGINE_LEADING_AT_START,
                ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
            ),
            OneLeaderStep(
                components=(ENGINE_LEADING_AT_START, ENGINE_FOLLOWING_AT_START),
                timeout=90.0,
                settle_secs=10.0,
                description="exactly one of the two engines leads",
            ),
        ],
    ),

    # 35 — the matching engine's version of scenario 9: no arbiters, then the leader lost.
    #
    # With both arbiters dead the matching engine leader keeps leading, renewing its lease with its
    # peer. When the leader then dies, the secondary hears from nobody and cannot tell a dead leader
    # from being cut off itself, so it must not take the lead, and it reports that the group has no
    # leader (R-0147). The secondary's promise to the leader runs out a lease period after the kill,
    # and its request goes unanswered for another, so the absence of a promotion is checked well
    # after both.
    Scenario(
        number=35,
        short_name="no_arbiters_and_engine_leader_lost",
        expects_no_new_leader=True,
        description="Both arbiters then the leading matching engine lost: the survivor does not take the lead",
        expected_outcome=(
            "with both arbiters dead and matching_engine_primary killed, matching_engine_secondary "
            "does not take the lead, and reports that the group has no leader (R-0147)"
        ),
        me_ha=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=2.0,
            ),
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=None,
                settle_secs=0.0,
            ),
            VerifyStep(
                expect=expect_lease(ENGINE_FOLLOWING_AT_START, "NoLeader"),
                timeout=25.0,
                description="the secondary reported that the group has no leader",
            ),
            AssertAbsentStep(
                absent=expect_leads(ENGINE_FOLLOWING_AT_START),
                after_secs=5.0,
                description="the secondary did not take the lead without a majority",
            ),
        ],
    ),

    # 36 — a pair of sequencers started one after the other settles on exactly one leader.
    #
    # As scenario 34 for the matching engine. Both sequencers are taken down and brought back
    # with the SECONDARY first. Either may lead; exactly one must.
    Scenario(
        number=36,
        short_name="sequencer_cold_start_tiebreak",
        description="Restarting both sequencers secondary-first leaves exactly one leading",
        expected_outcome="with both sequencers restarted, the secondary first, exactly one of them leads",
        # In-flight orders during phase 4 advance the matching engine's counter past the
        # recovery target, so the count phase then waits for a number that has already gone by.
        # Suppressed for a deterministic count, as the ME-HA scenarios do.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                takeover=expect_leads("sequencer_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="sequencer_secondary",
                takeover=None,
                settle_secs=2.0,
            ),
            RestartStep(
                proc_name="sequencer_secondary",
                ready_log_name="sequencer_secondary.log",
                ready=expect_started("sequencer_secondary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready=expect_started("sequencer_primary"),
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
                expect=expect_session_announced_again("sequencer_secondary"),
                timeout=10.0,
                description="the restarted sequencer_secondary was told the member's session is still connected (BUG-0182)",
            ),
            VerifyStep(
                expect=expect_session_announced_again("sequencer_primary"),
                timeout=10.0,
                description="the restarted sequencer_primary was told the member's session is still connected (BUG-0182)",
            ),
            OneLeaderStep(
                components=("sequencer_primary", "sequencer_secondary"),
                timeout=90.0,
                settle_secs=10.0,
                description="exactly one of the two sequencers leads",
            ),
            # Both sequencers restarted, so the connection each opens to the matching engine
            # had to be re-established before anything can be sequenced. Waiting for it is the
            # honest precondition of the order phase rather than test tidiness -- without it
            # the scenario measures reconnection time and calls it a failure. Both are checked,
            # because either may be the one that leads.
            VerifyStep(
                expect=expect_connection_made("sequencer_primary", "matching_engine"),
                timeout=60.0,
                description="the restarted primary has reached the matching engine again",
            ),
            VerifyStep(
                expect=expect_connection_made("sequencer_secondary", "matching_engine"),
                timeout=60.0,
                description="the restarted secondary has reached the matching engine again",
            ),
        ],
    ),

    # 37 — with no arbiter, the sequencer pair settles leadership between themselves.
    #
    # Both arbiters are killed and the leading sequencer is restarted. The two sequencers hold
    # two of the three votes, so they need no arbiter to agree which of them leads.
    Scenario(
        number=37,
        short_name="sequencer_degraded_promotion",
        description="With no arbiter, the sequencer pair settles leadership between themselves",
        expected_outcome=(
            "with both arbiters dead, sequencer_primary and its peer settle leadership between "
            "themselves: the restarted primary leads with its peer's vote"
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
                takeover=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=2.0,
            ),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready=expect_started("sequencer_primary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                # Longer than the usual settle: restarting a sequencer takes the gateway's
                # connection to it with it, and orders sent before that reconnects are dropped
                # rather than queued -- "primary sequencer not connected -- PDU not forwarded".
                # The venue needs a moment to knit itself back together, and measuring before
                # it has is measuring reconnection rather than the property under test.
                settle_secs=10.0,
            ),
            # With both arbiters dead, the restarted primary can lead only with its peer's vote:
            # its own vote and the peer's make a majority of three. It gets it because it kept a
            # record of leading, and the peer promised its vote to it.
            VerifyStep(
                expect=expect_lease("sequencer_primary", "Granted", "Peer"),
                timeout=90.0,
                description="the sequencer resolves leadership with its peer, needing no arbiter",
            ),
            VerifyStep(
                expect=expect_connection_made("sequencer_primary", "matching_engine"),
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
                absent=expect_leads("sequencer_secondary"),
                after_secs=45.0,
                description="the secondary did not also elect itself with no arbiter present",
            ),
        ],
    ),

    # 38 — a pair of arbiters started one after the other settles on exactly one active arbiter.
    #
    # As scenario 34, for the arbiters. Both are taken down and brought back with the SECONDARY
    # first. Either may become active; exactly one must.
    Scenario(
        number=38,
        short_name="arbiter_cold_start_tiebreak",
        me_ha=True,
        recovery_on_primary=True,
        description="Restarting both arbiters secondary-first leaves exactly one active",
        expected_outcome="with both arbiters restarted, the secondary first, exactly one of them is active",
        # In-flight orders during phase 4 advance the matching engine's counter past the
        # recovery target, so the count phase then waits for a number that has already gone by.
        # Suppressed for a deterministic count, as the ME-HA scenarios do.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="arbiter_primary",
                takeover=expect_leads("arbiter_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=2.0,
            ),
            RestartStep(
                proc_name="arbiter_secondary",
                ready_log_name="arbiter_secondary.log",
                ready=expect_started("arbiter_secondary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready=expect_started("arbiter_primary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            OneLeaderStep(
                components=("arbiter_primary", "arbiter_secondary"),
                timeout=90.0,
                settle_secs=10.0,
                description="exactly one of the two arbiters is active",
            ),
        ],
    ),

    # 39 — an arbiter alone, with no peer and no witness, does not become active.
    #
    # An arbiter is active only while a majority of the two arbiters and the witness grants it a
    # lease. Restarted with its peer and the witness both dead, it holds only its own vote, so it
    # must not become active, and it reports that the pool has no active arbiter. The sequencer
    # leader is unaffected: it renews its lease with its peer, and orders keep flowing.
    Scenario(
        number=39,
        short_name="arbiter_alone_does_not_become_active",
        me_ha=True,
        recovery_on_primary=True,
        description="An arbiter restarted with no peer and no witness does not become active",
        expected_outcome=(
            "with the witness and its peer dead, a restarted arbiter_primary cannot gather a "
            "majority and does not become active; sequencer_primary keeps leading"
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
                takeover=None,
                settle_secs=1.0,
            ),
            KillStep(
                proc_name="arbiter_secondary",
                takeover=None,
                settle_secs=1.0,
            ),
            RestartStep(
                proc_name="arbiter_primary",
                ready_log_name="arbiter_primary.log",
                ready=expect_started("arbiter_primary"),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=SETTLE_AFTER_RESTART,
            ),
            VerifyStep(
                expect=expect_lease("arbiter_primary", "NoLeader"),
                timeout=30.0,
                description="the lone arbiter reported that the pool has no active arbiter",
            ),
            AssertAbsentStep(
                absent=expect_leads("arbiter_primary"),
                after_secs=5.0,
                description="the lone arbiter did not become active with only its own vote",
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
        # The binary gateway too, so that its refusals while the venue is not accepting orders are
        # checked against the same outage (BUG-0100).
        binary_gateway=True,
        steps=[],
    ),

    # 53 -- an order the venue took while it had no matching engine at all.
    #
    # This is the measurement BUG-0064 was found by, turned into a scenario. Both engines are
    # taken away, an order is placed, and the sequencer does what it is designed to do: commits
    # it to the write-ahead log and defers forwarding it, because there is nothing to forward
    # it to. The member is told nothing, which is correct so far -- an engine is expected back.
    #
    # Then one engine starts COLD, and that is the case the venue got wrong. A promoted
    # follower reports where its replica reached and is sent everything after it, so a routine
    # failover recovers the deferred order and always did. An engine that starts from nothing
    # used to skip the catch-up entirely, on the reasoning that it had no predecessor to take
    # over from -- and the order sat in the log, never applied, never answered, while the
    # sequencer logged that it had been recovered.
    #
    # What makes this worth asserting from the CLIENT's own messages rather than from a log is
    # that the failure is silence. Every venue-side line looks healthy: the order is
    # WAL-committed, the engine starts, the sequencer reports recovery. Only the member can see
    # that nothing ever came back.
    #
    # The report must also be marked PossResend. The engine cannot tell a record some earlier
    # engine already reported from one no engine ever saw, so it reports every record in the
    # catch-up and marks them all; the mark is what lets a member that already had the report
    # discard it instead of reading a second event. Asserting the mark is asserting that the
    # venue told the truth about what it was doing -- R-0122.
    Scenario(
        number=53,
        short_name="deferred_orders_are_answered",
        description="An order taken while no matching engine exists is answered when one starts cold",
        expected_outcome=(
            "the order is deferred and unanswered while no engine exists; once one starts the "
            "member receives its execution report, marked PossResend, and the order stays open"
        ),
        me_ha=True,
        assert_deferred_answered=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),

    # 54 -- the window BUG-0077 describes, opened deliberately.
    #
    # A restarting engine asks the sequencer for a catch-up as soon as it has somewhere to ask,
    # which can be before it knows whether its peer is leading. Scenario 24 usually misses the
    # window: the restarted instance hears its leading peer's lease renewal and grants it within
    # about a second, so it is a follower before the sequencer's order connection arrives, and the
    # catch-up is skipped.
    #
    # So the restarted instance is made to forget its lease promises, as it would after a reboot.
    # An instance that has forgotten what it promised grants nothing and asks for nothing for one
    # lease period, so for that long it cannot agree that its peer leads. The catch-up completes
    # inside that period, and the instance is current before anything can tell it its role.
    #
    # The assertions are that the restarting instance becomes current without taking the role,
    # that the reports its catch-up produced are discarded once it learns it follows, rather than
    # sent to members it will never serve, and that the venue still trades afterwards: recovery
    # orders are confirmed on the leading secondary, which they cannot be if the order routing
    # ends up pointing at an instance that will not act on them. See BUG-0077.
    Scenario(
        number=54,
        short_name="startup_catchup_while_peer_leads",
        description="A restarted engine catches up while its peer leads, before it can agree that the peer leads",
        expected_outcome=(
            "the restarted instance asks for a catch-up, becomes current, and waits without adopting "
            "leadership; once it agrees that its peer leads it discards the reports the catch-up "
            "produced; and the venue goes on trading through the leader it already had"
        ),
        me_ha=True,
        # As scenarios 16 and 24: in-flight orders would advance the promoted secondary's counter
        # during reconciliation and make the recovery count non-deterministic. The orders this
        # scenario needs are sent deliberately, between the promotion and the restart, by the
        # InterimOrdersStep below.
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            # 1. Fail the primary so the secondary is genuinely leading.
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            # 2. Orders the promoted secondary takes while the primary is down. Without these the
            #    primary's catch-up has nothing to stream -- its region is already at the head --
            #    and the reports half of this scenario tests nothing. Confirmed in the secondary's
            #    log, because the primary is dead and it is the secondary doing the matching.
            InterimOrdersStep(count_batches=1),
            # 3. Bring the primary back, knowing nothing of what it promised.
            RestartStep(
                proc_name=ENGINE_LEADING_AT_START,
                ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=0.0,
                forget_lease_promises=True,
            ),
            # 4. It asked. The restart deletes the log first, so this line is from this start and
            #    not from the one before it.
            VerifyStep(
                expect=expect_catch_up(ENGINE_LEADING_AT_START, "Started"),
                timeout=30.0,
                description="the restarted instance asks for a catch-up though it does not lead",
            ),
            # 5. And it stopped there. Current, and waiting -- never adopting the role on the
            #    strength of having caught up, which is what trap 5 of BUG-0064 was about.
            VerifyStep(
                expect=expect_catch_up(ENGINE_LEADING_AT_START, "CurrentAndWaiting"),
                timeout=30.0,
                description="it becomes current without taking the role",
                absent=expect_leads(ENGINE_LEADING_AT_START),
            ),
            # 6. Once its lease period has passed it grants its peer a renewal and follows, and the
            #    reports it produced while catching up go nowhere. It applied those records and
            #    reports none of them, because it serves nobody: the leader answered those members.
            #    It keeps them only until the WAL holds the leader's, and sends them only on taking
            #    the lead (docs/availability/matching_engine_lockstep.md, section 3). Being current
            #    and being entitled to act are separate things, and reporting is an act -- BUG-0077.
            VerifyStep(
                expect=expect_follows(ENGINE_LEADING_AT_START),
                timeout=30.0,
                description="it follows, and sends none of the reports its catch-up produced",
                absent=expect_message(ENGINE_LEADING_AT_START, "it sent the reports it kept", "KeptReportsSent", "matching_engine_test_events"),
            ),
        ],
    ),

    # 55 -- the other side of 54. Reports held back are owed to the members who placed those
    # orders the moment this instance does become the one serving them.
    #
    # 54 shows them discarded, which is the common ending: an instance that catches up and then
    # follows serves nobody, and the leader answered those members. Here the leader is taken away
    # as well, so that the instance that comes back is the one that must serve.
    #
    # It comes back knowing nothing of what it promised, as after a reboot, so for one lease
    # period it grants nothing and asks for nothing. It catches up in that time, and holds the
    # reports. Then it asks to lead, the arbiter grants it -- the arbiter's promise to the dead
    # secondary has long run out -- and on leading it releases the held reports.
    Scenario(
        number=55,
        short_name="held_reports_released_when_it_leads",
        description="Reports held while an engine was merely current are sent once it is the one serving",
        expected_outcome=(
            "the restarted instance catches up and holds its reports while it cannot yet lead; when "
            "it becomes the one serving, the reports are released and the members who placed those "
            "orders are told, rather than discarded"
        ),
        me_ha=True,
        orders_during_override=0,
        recovery_on_primary=True,
        steps=[],
        restart_steps=[],
        extra_steps=[
            # 1. Fail the primary so the secondary is genuinely leading, and the orders that
            #    follow are taken by a real leader rather than by nobody.
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            # 2. Orders the leader takes while the primary is down. Without these the catch-up
            #    that follows has nothing to stream and no reports to hold, and this scenario
            #    tests nothing.
            InterimOrdersStep(count_batches=1),
            # 3. And now the leader as well, so the instance that comes back must serve.
            KillStep(
                proc_name=ENGINE_FOLLOWING_AT_START,
                takeover=None,
                settle_secs=2.0,
            ),
            # 4. The primary comes back knowing nothing of what it promised, catches up, and
            #    waits -- holding the reports.
            RestartStep(
                proc_name=ENGINE_LEADING_AT_START,
                ready_log_name=f"{ENGINE_LEADING_AT_START}.log",
                ready=expect_engine_reachable(ENGINE_LEADING_AT_START),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=0.0,
                forget_lease_promises=True,
            ),
            VerifyStep(
                expect=expect_catch_up(ENGINE_LEADING_AT_START, "CurrentAndWaiting"),
                timeout=30.0,
                description="the restarted instance is current and holding its reports",
            ),
            # 5. It becomes the one serving, with the arbiter's grant.
            VerifyStep(
                expect=expect_leads(ENGINE_LEADING_AT_START),
                timeout=30.0,
                description="the instance that was holding becomes the one serving",
            ),
            # 6. The assertion this scenario exists for.
            VerifyStep(
                expect=expect_message(ENGINE_LEADING_AT_START, "it sent the reports it kept, now that it serves", "KeptReportsSent",
                                      "matching_engine_test_events"),
                timeout=30.0,
                description="the reports it kept are sent to the members who placed those orders",
                absent=expect_message(ENGINE_LEADING_AT_START, "its held reports discarded", "ReportsDiscarded", "matching_engine_test_events"),
            ),
        ],
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
    # The split assumed here puts each pair's primary on one machine and its
    # secondary on the other.  The machine that stops is the one the leading
    # matching engine runs on: its arbiter, its sequencer and the engine.  The
    # sequencer that leads may be on either machine, because the first election
    # of each pair is separate; when it is on the machine that stops, the
    # sequencer fails over as well.  The
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
            "the matching engine on the surviving machine takes over, the "
            "member's session is unaffected, and recovery orders are answered "
            "-- with nothing on the lost machine restarted"
        ),
        me_ha=True,
        # In-flight orders would advance the promoted secondary's counter during
        # reconciliation, as in scenario 16.
        orders_during_override=0,
        steps=[],
        extra_steps=[
            MachineKillStep(
                proc_names=(),
                machine_of=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
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
    # other instance.  It counts its lease on its own clock, so on waking it
    # finds the lease has run out and stops acting without being told.  That,
    # and not removing the node by force, is how this venue keeps a returning
    # instance from acting; the generation carried on every message remains as
    # a second defence.
    Scenario(
        number=49,
        short_name="superseded_instance_returns",
        description="A frozen leader is resumed after its peer has taken over",
        expected_outcome=(
            "the peer takes over while the leader is stopped; the leader finds on waking "
            "that its lease ran out and stops acting by itself, and the peer keeps the "
            "entitlement throughout"
        ),
        orders_during_override=0,
        steps=[],
        extra_steps=[
            IsolateStep(
                proc_name="sequencer_primary",
                takeover=expect_leads("sequencer_secondary"),
                takeover_timeout=45.0,
                stopped=expect_lease("sequencer_primary", "RanOut"),
                stopped_timeout=10.0,
                forbidden=expect_follows("sequencer_secondary"),
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
    # So this kills the engine and leaves it dead. The absence has to be real: the leading
    # sequencer measures how long it has had no matching engine to route orders to, and when
    # that passes the limit it writes a CancelBook command into the WAL. The deployed limit is
    # five minutes, which no test is going to sit out, so the scenario shortens it to a few
    # seconds and stays down past it. The restarted engine meets the command in its catch-up.
    #
    # What must then happen, per R-0117: every recovered order cancelled, by the command, the
    # member that placed each one told, and trading halted. And per R-0023, the halt stays until
    # a person lifts it -- so the venue must refuse an order sent afterwards rather than quietly
    # carrying on.
    Scenario(
        number=51,
        short_name="long_absence_cancels_and_halts",
        description="Orders a member was locked out of are cancelled, reported, and trading halts",
        expected_outcome=(
            "the leading sequencer writes a command cancelling the book; the restarted engine "
            "meets it in its catch-up, cancels every order it recovered, sends the member that "
            "placed it a report saying so, halts, and refuses the orders sent afterwards"
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
                ready=expect_engine_reachable("matching_engine", "matching_engine"),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=_ME_SETTLE,
                # Comfortably past the five seconds this scenario allows, and short enough that
                # the suite does not notice.
                down_secs=12.0,
            ),
        ],
    ),

    # 52 -- the venue's own record of what it held cannot be read, and the WAL reaches the first command of the day.
    #
    # R-0102 says such a record is not to be used. R-0123 sends the engine to the other record the venue keeps, the
    # sequencer's WAL of what it took. The engine is deterministic, so replaying that WAL from its first record gives
    # exactly the book the venue had, and the engine carries on with it: every member keeps every order, nothing is
    # cancelled, and trading does not halt. The gateway then cancels every order it believes the member has open, and
    # every cancel must be carried out.
    #
    # The region itself must survive: whatever made it unreadable is evidence, and a venue that
    # overwrites it destroys the only account of its own failure.
    Scenario(
        number=52,
        short_name="damaged_region_rebuilt_from_wal",
        description="A matching engine whose record of what it held cannot be read rebuilds it from the WAL",
        expected_outcome=(
            "the engine refuses the damaged region rather than reading it, keeps it aside, rebuilds its book from the "
            "WAL, which reaches the first command of the day, and carries on with every order, cancelling nothing"
        ),
        assert_damaged_region=True,
        assert_open_orders_survive=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[
            RestartStep(
                proc_name="matching_engine",
                ready_log_name="matching_engine.log",
                ready=expect_engine_reachable("matching_engine", "matching_engine"),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=_ME_SETTLE,
                damage_region="matching_engine_open_orders.region",
            ),
        ],
    ),

    # 56 -- a session may send only so many new orders and cancels a second.
    #
    # The limits travel the real route -- the comp_id row, the export, credentials.toml, the
    # authentication service, AuthenticationResult -- so the scenario first checks the gateway
    # names the numbers it applied, not merely that something was refused. A hop that dropped a
    # value would leave the session unlimited, and a test that only looked for refusals would
    # pass a venue that refused for some other reason.
    #
    # Then a burst of new orders, sent faster than the limit, must be split exactly: the first
    # orders up to the limit accepted and every one after refused with the limit in its text. A
    # second later one more is accepted. A burst of cancels is split the same way, the refused
    # ones answered with an OrderCancelReject that says the order is still open. Finally a comp
    # id with no limit sends the same burst and nothing is refused.
    Scenario(
        number=56,
        short_name="gateway_throttles",
        description="Each session is limited in how many new orders and cancels it may send a second",
        expected_outcome=(
            "the gateway applies the provisioned limits; a burst of new orders and a burst of cancels are each "
            "accepted up to the limit and refused beyond it with the limit in the text; a command a second later "
            "is accepted; and a comp id with no limit is never refused"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_gateway_throttles=True,
        steps=[],
    ),

    # 57 -- the binary gateway checks what it is sent, as the FIX gateway does.
    #
    # Each command is sent on a connection of its own by binary_client, and its reply is checked
    # by ClOrdID. A command the gateway refuses must be answered by the gateway itself -- an
    # OrderID of GW-ORD-n on a rejected order -- with a reason naming the field, and must never
    # reach the matching engine. A valid order and a valid cancel go through, so the refusals are
    # shown to come from the checks and not from a gateway that refuses everything. The refusals
    # while the venue is not accepting orders are checked in scenario 42.
    Scenario(
        number=57,
        short_name="binary_checks",
        description="The binary gateway refuses orders and cancels the venue cannot accept, and says why",
        expected_outcome=(
            "a valid order and a valid cancel are accepted; an order with an undefined Side, a quantity or price that "
            "is not a number, an over-long ClOrdID or an over-long Symbol is refused by the gateway with the field "
            "named; a cancel with an undefined Side is refused with an OrderCancelReject; and the matching engine's "
            "refusal of a cancel for an unknown order arrives as an OrderCancelReject with CxlRejReason 1"
        ),
        orders_during_override=0,
        orders_after_override=0,
        binary_gateway=True,
        assert_binary_checks=True,
        steps=[],
    ),

    # 58 -- the matching engine refuses an identifier longer than its book key, rather than cutting
    # it short.
    #
    # Both gateways refuse these first, so the order and the cancel are handed straight to the
    # sequencer by inject_order, as a gateway would hand them. Two orders whose identifiers differ
    # only in the 65th character must both be refused: cut short, they would be one order. Then
    # the engine is restarted and replays the same records while catching up. Each record must be
    # refused again, and the catch-up must still account for every record it was sent (R-0101):
    # a refusal placed before the record was counted would stop a healthy engine from serving.
    Scenario(
        number=58,
        short_name="engine_identifier_refusal",
        description="The matching engine refuses an over-long ClOrdID instead of cutting it short, live and while catching up",
        expected_outcome=(
            "two orders whose ClOrdIDs differ only beyond the 64th character, and a cancel naming one of them, are all "
            "refused by the matching engine; after a restart the engine refuses them again while catching up, and the "
            "catch-up still accounts for every record"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_engine_identifier_refusal=True,
        steps=[],
    ),

    # 59 -- the leading sequencer dies holding orders its follower never received.
    #
    # The leading sequencer sends each order to the matching engine before it sends the order's
    # record to its follower. If it dies between the two, the engine holds an order that no
    # surviving log holds: the follower takes the lead without the record, and gives its sequence
    # number to the next order it receives. The member is never told of the order, because its
    # report waits for the follower's acknowledgement, yet the order rests on the book and could
    # trade.
    #
    # The leader therefore holds each order until the follower acknowledges its record, and only
    # then sends it to the engine (option A of part 4.2 of the design). To show it, the primary
    # sequencer is started with libblock_sends_to_ports.so preloaded, and once the flag file is
    # created nothing more it sends to its follower arrives, so no acknowledgement can come back.
    # Three orders are sent to the leader and the leader is stopped at once, before its 100 ms wait
    # for an acknowledgement runs out; the engine must have accepted none of them. The leader is
    # then killed and the follower takes the lead, and the engine must hold no order the new
    # leader's log lacks.
    Scenario(
        number=59,
        short_name="unreplicated_orders_at_leader_death",
        description="The leading sequencer dies holding orders its follower never received",
        expected_outcome=(
            "orders the follower never acknowledged are held by the leader and never reach the matching engine, so when "
            "the leader dies the engine holds nothing the new leader's log lacks"
        ),
        orders_during_override=0,
        orders_after_override=0,
        block_leader_peer_sends=True,
        assert_engine_holds_only_logged_orders=True,
        assert_new_leader_numbers_forward=True,
        steps=[],
    ),

    # 60 -- a follower that stalls does not stop trading.
    #
    # A fault in the follower is a loss of resilience, not of service. The follower sequencer is
    # stopped with SIGSTOP, so its connection stays open but it acknowledges nothing. An order sent
    # to the leader is held for the follower's acknowledgement; within the 100 ms wait the leader
    # must treat the follower as too far behind, run as if alone and send the order to the engine,
    # saying so in its log and in its sequencer_running_alone gauge. A second order must go straight
    # through. When the follower is let run again and catches up, the leader must go back to waiting
    # for its acknowledgements, and the gauge must return to 0.
    Scenario(
        number=60,
        short_name="stalled_follower_runs_alone",
        description="A stalled follower sequencer does not stop trading; the leader runs as if alone until it catches up",
        expected_outcome=(
            "with the follower stopped, orders are accepted by the matching engine after the leader's 100 ms wait, the "
            "leader reports running as if alone and its gauge reads 1; when the follower runs again, the leader reports "
            "that it is keeping up and the gauge reads 0"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_stalled_follower_runs_alone=True,
        steps=[],
    ),

    # 61 -- a follower that restarts is sent the records it missed.
    #
    # The follower sequencer is killed and the leader takes orders without it. When the follower is
    # restarted, it and the leader find the last record their logs agree on, and the leader sends it
    # every record after that. Its log must then hold the same records, with the same contents, as
    # the leader's from where it stopped: no gap. Then the leader is killed, and the follower, which
    # takes the lead, must hold every order the matching engine accepted while it was away.
    Scenario(
        number=61,
        short_name="restarted_follower_log_repaired",
        description="A follower sequencer that restarts is sent the records it missed, and can then take over",
        expected_outcome=(
            "after the follower restarts, its log holds the same records as the leader's from where it stopped, with no gap; "
            "when the leader is then killed, the new leader's log holds every order the engine accepted while the follower was down"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_restarted_follower_log_repaired=True,
        steps=[],
    ),

    # 62 -- an old leader that rejoins discards the records its new leader does not have.
    #
    # The primary sequencer is started with libblock_sends_to_ports.so preloaded. Once the flag file
    # exists nothing it sends its follower arrives, so orders sent to it are written to its log only;
    # it is stopped at once, before its 100 ms wait could make it act on them. It is killed, the
    # follower takes the lead and takes orders of its own, numbered where the old leader's were. The
    # flag file is removed and the old leader restarted. It must discard the records the new leader
    # does not hold, and its log must end identical to the new leader's.
    Scenario(
        number=62,
        short_name="rejoining_old_leader_log_repaired",
        description="An old sequencer leader that rejoins discards the records its new leader does not have",
        expected_outcome=(
            "the restarted old leader reports discarding the records its new leader never had, none of the orders only it held "
            "remains in its log, and its log then holds the same records as the new leader's"
        ),
        orders_during_override=0,
        orders_after_override=0,
        block_leader_peer_sends=True,
        assert_rejoining_old_leader_log_repaired=True,
        steps=[],
    ),

    # 63 -- a follower that is behind does not take the lead.
    #
    # The follower sequencer is stopped with SIGSTOP. An order sent to the leader cannot wait for the
    # follower's acknowledgement, so the leader says that the follower may not lead and sends the order
    # to the matching engine only once the arbiter has echoed that statement. The leader is then killed
    # and the follower let run: it lacks the order the engine acted on, so it must not lead. The old
    # leader is restarted, leads again holding the order, and once the follower has caught up says that
    # it may lead again.
    Scenario(
        number=63,
        short_name="follower_behind_does_not_lead",
        description="A follower sequencer that lacks an order the engine acted on does not take the lead",
        expected_outcome=(
            "the order sent while the follower was stopped is accepted after the arbiter confirms the statement; after the "
            "leader is killed the follower does not lead; the restarted old leader leads holding the order, and says the "
            "follower may lead again once it has caught up"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_follower_behind_does_not_lead=True,
        steps=[],
    ),

    # 64 -- as 63, with the active arbiter restarted before the leader dies.
    #
    # The arbiter keeps the leader's statement on disk and copies it to the passive arbiter, so a
    # restart, which forgets every promise, must not forget it.
    Scenario(
        number=64,
        short_name="follower_behind_does_not_lead_across_arbiter_restart",
        description="A follower that is behind does not take the lead even after the active arbiter restarts",
        expected_outcome=(
            "as scenario 63, with the active arbiter killed and restarted after the order is accepted and before the leader "
            "is killed; the follower still does not lead"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_follower_behind_does_not_lead=True,
        restart_arbiter_before_leader_dies=True,
        steps=[],
    ),

    # 65 -- reports the leader received but never forwarded reach the member from the new leader.
    #
    # The primary sequencer is started with libblock_sends_to_ports.so preloaded. Once the flag file
    # exists, nothing it sends to a gateway arrives. A member then sends orders: the matching engine
    # accepts them and sends its reports to both sequencers, and the leader's copies go nowhere. The
    # leader is killed. The follower kept every report the engine sent it, and on taking the lead
    # forwards them all, each marked as a possible repeat.
    Scenario(
        number=65,
        short_name="kept_reports_forwarded_by_new_leader",
        description="Reports the sequencer leader never forwarded reach the member from the new leader",
        expected_outcome=(
            "the member receives no report while the leader's sends to the gateways are blocked; after the leader is killed, "
            "the member receives the acceptance of every order it sent, each marked PossResend"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_kept_reports_forwarded=True,
        steps=[],
    ),

    # 66 -- a command the new leader's log holds, which the matching engine never received, is applied.
    #
    # The secondary sequencer is started with libblock_sends_to_ports.so preloaded. Once the flag file
    # exists, nothing it sends to its peer arrives, so the follower writes and acknowledges every record
    # the leader sends it, and the leader never hears. A member sends orders; the leader holds them,
    # waiting for acknowledgements that cannot arrive, and is stopped before its 100 ms wait could make
    # it send them to the engine. It is killed. The follower holds the orders and takes the lead, and
    # every order must then be applied and answered.
    Scenario(
        number=66,
        short_name="logged_commands_reach_engine",
        description="Orders in the new sequencer leader's log that the matching engine never received are applied and answered",
        expected_outcome=(
            "the follower's log holds the orders and the engine has applied none of them when the leader is killed; once the "
            "follower leads, the engine applies each order once and the member receives its acceptance"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_logged_commands_reach_engine=True,
        steps=[],
    ),

    # 67 -- a command sent again is sequenced only if the leader's log does not already hold it.
    #
    # inject_order hands the leading sequencer an order as a gateway would. The same order is then
    # sent marked as sent again, as a gateway sends an unanswered command after a change of leader,
    # and must not be sequenced a second time. An order marked as sent again that was never sent
    # before must be sequenced. Nothing is killed: the leader checks every command marked this way.
    Scenario(
        number=67,
        short_name="sent_again_sequenced_once",
        description="A command a gateway sends again is sequenced only if the leader's log does not already hold it",
        expected_outcome=(
            "the order sent twice, the second time marked as sent again, is applied once and never refused as a duplicate; the "
            "order marked as sent again that was never sent before is applied once"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_sent_again_sequenced_once=True,
        steps=[],
    ),

    # 68 -- every order sent while the sequencers change leader is answered, and none twice.
    #
    # A member sends orders at a steady rate through the FIX gateway, starting a second before the
    # leading sequencer is killed and continuing until a few seconds after the follower has taken
    # the lead. The orders sent in between reach only the follower, which discards them while it
    # follows. The gateway keeps each command until it is answered and sends the unanswered ones
    # again when the new leader announces itself; each must then be answered, and the engine must
    # apply none of them twice.
    Scenario(
        number=68,
        short_name="orders_during_change_answered",
        description="Every order sent while the sequencers change leader is answered, and none is applied twice",
        expected_outcome=(
            "every order the member sent across the change of leader receives a report, and the matching engine accepts each at most once"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_orders_during_change_answered=True,
        steps=[],
    ),

    # 69 -- after a change of leader, a member is sent again only the reports the old leader had not forwarded.
    #
    # A member sends 1,000 orders and waits for every acceptance, then sends one more half a second
    # later, so that the leader writes a record well after the burst. The leader is killed. Its records
    # said how far it had forwarded the engine's reports, so the new leader forwards again only the few
    # it had not said it forwarded, rather than every report of the last few seconds (BUG-0116).
    Scenario(
        number=69,
        short_name="few_repeats_after_change",
        description="After a change of sequencer leader, a member is sent again only the reports the old leader had not forwarded",
        expected_outcome=(
            "the new leader forwards at most 10 kept reports, and the member receives at most 10 reports marked as possible repeats, "
            "rather than the 1,000 acceptances it already had"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_few_repeats_after_change=True,
        steps=[],
    ),

    # 70 -- a report produced while a member is disconnected is delivered when it reconnects.
    #
    # A member places an order and disconnects. While it is away, the order is cancelled, by a
    # cancel inject_order hands the leading sequencer in the member's name, so the engine produces a
    # cancel report for a session that has no connection. The member reconnects, continuing its
    # numbering, and must be sent the report without asking for it (R-0005).
    Scenario(
        number=70,
        short_name="unbound_reports_delivered",
        description="A report produced while a member is disconnected is delivered when it reconnects",
        expected_outcome=(
            "after the member reconnects it receives the report cancelling its order, produced while it was away, and the "
            "sequencer logs that it sent a report produced while the session had no connection"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_unbound_reports_delivered=True,
        steps=[],
    ),

    # 71 -- commands discarded with a closed connection to the leader are sent again when it reconnects.
    #
    # Gateway a is started allowing only 100 sends to wait on a connection, with a 16 KiB socket send
    # buffer. The leading sequencer is stopped, as a debugger or a long pause of the machine would stop
    # it, for less than its lease, so that it still leads when it runs again. A member sends orders until
    # the gateway's queue for the sequencer is full and the gateway closes the connection, discarding the
    # orders waiting in it. The gateway connects again and sends again every order still unanswered, and
    # the sequencer, running again, must accept each order exactly once.
    Scenario(
        number=71,
        short_name="commands_resent_after_reconnection",
        description="Commands discarded with a closed connection to the leading sequencer are sent again when it reconnects",
        expected_outcome=(
            "the gateway closes its connection to the stopped leader when the queue is full, connects again and sends the "
            "unanswered orders again; every order is accepted exactly once, none is refused, and the leader keeps the lead"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_commands_resent_after_reconnection=True,
        steps=[],
    ),

    # 72 -- orders reaching an engine with no route for their reports are acted on, and reported later.
    #
    # The matching engine sends its reports on connections it opens to the sequencers. Here those go
    # through a relay this script runs, which at first accepts nothing, so the engine has no connection
    # for reports while the sequencer's connection to it for orders is up. A member sends orders. Once
    # the relay lets the connections through, every order must be reported accepted.
    Scenario(
        number=72,
        short_name="engine_holds_reports_without_connection",
        description="Orders reaching an engine with no connection for reports are acted on and reported once one is established",
        expected_outcome=(
            "the engine acts on each order and holds its report while it has no connection for reports, and sends every held "
            "report once a connection is established; every order is accepted exactly once"
        ),
        orders_during_override=0,
        orders_after_override=0,
        skip_baseline_orders=True,
        assert_engine_holds_reports_without_connection=True,
        steps=[],
    ),

    # 73 -- a bounded resend asked for while no sequencer leads.
    #
    # Only the leading sequencer answers a request for a session's reports; a sequencer that does
    # not lead declines it. So a request that arrives while no instance leads is declined by every
    # sequencer that receives it, and the gateway has to ask again once a leader exists. The
    # leading sequencer is killed just before the member asks, so the request reaches only the
    # follower, which declines; when the follower takes over it announces itself, and the gateway
    # asks it again. What comes back is checked exactly as in scenario 40: the messages that
    # occupied the numbers asked for, and nothing outside them. Without asking again the resend is
    # never answered, and the session holds every live report from then on.
    Scenario(
        number=73,
        short_name="bounded_resend_with_no_leader",
        description="A member asks for a bounded range at the moment the leading sequencer dies",
        expected_outcome=(
            "the follower declines the request because it does not lead, the gateway asks again when the "
            "follower takes over, and the venue resends exactly the messages that occupied the requested numbers"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_bounded_resend=True,
        bounded_resend_with_leader_killed=True,
        steps=[],
    ),

    # 74 -- a command sent again to a follower that has just taken the lead, which holds it only because
    # its old leader replicated it.
    #
    # The new leader must recognise the command as already in its log, so it must have noted the commands
    # in the records it wrote while it followed. An order is sent and accepted, the leading sequencer is
    # killed, and once the follower leads the same order is sent to it again, marked as sent again, with a
    # second order marked as sent again that was never sent before. Checked exactly as in scenario 67.
    Scenario(
        number=74,
        short_name="sent_again_after_change_of_leader",
        description="A command sent again to a new leader is not sequenced twice when the new leader holds it from replication",
        expected_outcome=(
            "the order sent before the change of leader and sent again afterwards is applied once and never refused as a "
            "duplicate; the order marked as sent again that was never sent before is applied once"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_sent_again_sequenced_once=True,
        sent_again_after_change_of_leader=True,
        steps=[],
    ),

    # 75 -- orders that wait in the write-ahead log reach the matching engine through the reading thread.
    #
    # Both sequencers hold at most a few orders while they wait. The follower is stopped with SIGSTOP and
    # a burst of orders is sent: the leader holds the first few, writes the rest to its log without
    # holding them, and once a voter confirms that the follower may not lead, the reading thread sends
    # the matching engine the orders that waited in the log and hands it over. Every order must be
    # accepted exactly once (docs/availability/following_the_wal.md, section 5.3, part 3).
    Scenario(
        number=75,
        short_name="orders_that_waited_in_the_log",
        description="Orders that wait in the write-ahead log because the storage for held orders is full reach the matching engine once each",
        expected_outcome=(
            "the leader's storage for held orders fills; the reading thread sends the matching engine the orders that waited in "
            "the log and hands it over; the engine accepts every order of the burst exactly once"
        ),
        orders_during_override=0,
        orders_after_override=0,
        assert_orders_that_waited_in_the_log_reach_engine=True,
        steps=[],
    ),

    # 76 -- a publisher is sent only records the venue's record holds, each once.
    #
    # Both matching engine publishers run, and are sent the sequencers' records as they follow them. One
    # publisher is killed, orders are taken while it is down, and it is restarted with the write-ahead
    # log it had: it must not write any record twice, and must have no gap. Then, as in scenario 59, the
    # leading sequencer is made to hold records its follower never received, is stopped and killed, and
    # the follower takes the lead and numbers a new order where the old leader's records were. Each
    # publisher's log must then hold, under every number, the same record as the new leader's log, and
    # none the new leader does not hold (BUG-0153, docs/availability/following_the_wal.md, section 5.4).
    Scenario(
        number=76,
        short_name="publishers_follow_only_the_record",
        description="A matching engine publisher is sent only the records the venue's record holds, each once, across a restart and a change of leader",
        expected_outcome=(
            "a restarted publisher's log holds every record once with no gap; after the leading sequencer dies holding records "
            "its follower never received, each publisher's log holds the same record under each number as the new leader's log"
        ),
        orders_during_override=0,
        orders_after_override=0,
        with_publishers=True,
        block_leader_peer_sends=True,
        assert_publishers_follow_only_the_record=True,
        steps=[],
    ),

    # 77 -- a restarted sequencer that leads before it is told of the member's session holds the member's reports.
    #
    # BUG-0190. A sequencer keeps where each session is connected only in memory, and a restarted one learns it from
    # the gateways when their links to it come up. One that takes the lead before that would drop every report for the
    # member's session. Here the gateways are asked to wait 40 seconds before announcing their sessions on a link that
    # comes up, so the restarted primary leads, and the promoted matching engine cancels the book, inside that wait.
    # The primary must hold each cancel and send it when the session is announced, and the member must receive a
    # cancel for every order the engine cancelled. See docs/availability/reports_held_until_sessions_are_announced.md.
    #
    # The 40 seconds must cover the restarted primary coming up, the secondary sequencer and the leading engine being
    # killed, the primary taking the lead and the other engine being promoted and cancelling.
    Scenario(
        number=77,
        short_name="held_reports_reach_member",
        description="A restarted sequencer that leads before it is told of the member's session holds the member's reports until it is",
        expected_outcome=(
            "the restarted primary sequencer leads before the gateway announces the member's session, holds the cancels "
            "the promoted engine sends, and sends them when the session is announced; the member receives every cancel"
        ),
        me_ha=True,
        delay_session_announcements_ms=40000,
        assert_held_reports_reach_member=True,
        # No orders in flight and none after: the cancels for the resting baseline orders are the subject.
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_primary",
                takeover=expect_leads("sequencer_secondary"),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
            RestartStep(
                proc_name="sequencer_primary",
                ready_log_name="sequencer_primary.log",
                ready=expect_follows("sequencer_primary"),
                ready_timeout=_SEQ_FOLLOWER_TIMEOUT,
                resets_me_counter=False,
                settle_secs=0.0,
            ),
            KillStep(
                proc_name="sequencer_secondary",
                takeover=None,
                settle_secs=0.0,
            ),
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=0.0,
            ),
            VerifyStep(
                expect=expect_leads("sequencer_primary"),
                timeout=30.0,
                description="the restarted sequencer_primary leads",
            ),
        ],
    ),

    # 78 -- a matching engine that caught up and is waiting to learn its role catches up again before it leads.
    #
    # BUG-0198. A restarted matching_engine_primary catches up from the sequencer's WAL when the first sequencer
    # connects to it, and then waits, in the unknown role, until it has granted the leader a lease and says it follows:
    # about 10 seconds. A sequencer sends orders only to the engine that leads, so every order the leader accepts in that
    # time reaches the waiting engine only through the WAL. If the leader dies then, the waiting engine is granted the
    # lead, and it must catch up again first, or every order accepted while it waited is missing from its book and no
    # member is told.
    #
    # The engines are named by their configuration here, not by the role each had at the start, because the window is
    # the configured primary's: a restarted matching_engine_secondary follows at once (BUG-0195), so it never waits in
    # the unknown role. matching_engine_primary is killed whichever role it had. If it led, the secondary takes the
    # lead; if it followed, the secondary led already, and its lead from the start meets the expectation. The
    # primary's record of the lease promises it made is deleted while it is dead, as a reboot would lose it: a primary
    # that followed and kept that record resumes following at once, before any sequencer connects, and never waits.
    # The primary is restarted, and once it reports that it is caught up and waiting, 1,000 orders are sent and each one's
    # New report is received by the member. The secondary is then killed, before the primary has said it follows, and
    # the primary takes the lead. The question is then asked the member's way, as scenario 50 asks it: the session is
    # dropped, the gateway cancels every order it believes is open, and the primary must accept each cancel. A cancel
    # refused as naming an order the engine does not hold is an order lost. Before that, the primary must report
    # resuming as leader, which it does only at the end of a catch-up to lead, holding exactly as many orders as the
    # member was told are open.
    #
    # The venue is set to keep the book on promotion. When the primary leads at the start, the secondary is promoted
    # when the primary is killed, and set to cancel, it would cancel the baseline orders and tell the member, while the
    # primary's region still holds them, so the primary would lead holding orders the member was told were cancelled.
    # That is BUG-0199, a different defect, and the scenario would then pass or fail according to which engine won the
    # first election. The primary itself keeps its book either way when it leads, because it is starting, not being
    # promoted from follower.
    #
    # Two checks make sure the scenario made the window it exists for: the primary had not followed when the secondary
    # was killed, and it never followed before it led.
    Scenario(
        number=78,
        short_name="waiting_engine_catches_up_before_leading",
        description="A matching engine caught up and waiting to learn its role catches up again before it takes the lead",
        expected_outcome=(
            "the restarted matching_engine_primary is granted the lead while caught up and waiting, catches up again "
            "first, and holds every order the secondary accepted meanwhile, so every cancel the gateway sends is accepted"
        ),
        me_ha=True,
        recovery_on_primary=True,
        # So that the first promotion cancels nothing: see above.
        open_orders_on_promotion="keep",
        assert_waiting_engine_catches_up=True,
        assert_open_orders_survive=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=_waiting_engine_steps(),
    ),

    # 79 -- a matching engine restarted after its peer cancelled the book on promotion leads holding the cancelled orders.
    #
    # BUG-0199. The steps are scenario 78's, with the venue set to cancel the book on promotion, as every environment
    # is, and with the primary started first and leading, so that killing it promotes the secondary. The secondary
    # cancels the 1,000 baseline orders it inherits, by a CancelBook command in the WAL, and tells the member. The
    # primary's region still holds them; the restarted primary meets the command in its catch-up and cancels them too.
    # It then follows the WAL while it waits, applying the 1,000 orders sent meanwhile, so taking the lead is a promotion
    # too, and the venue set to cancel has it cancel those as well, telling the member. When it takes the lead it must hold
    # exactly the orders the member was told are open: none, and in particular none of the 1,000 cancelled first. With
    # nothing open, there is no order whose cancel by the gateway could be checked.
    Scenario(
        number=79,
        short_name="restarted_engine_holds_no_cancelled_order",
        description="A matching engine restarted after its peer cancelled the book on promotion leads holding none of the cancelled orders",
        expected_outcome=(
            "the restarted matching_engine_primary takes the lead holding none of the orders the promoted secondary cancelled, "
            "and cancels those accepted while it waited as a promotion does, so its book is exactly what the member was told is open"
        ),
        me_ha=True,
        recovery_on_primary=True,
        engine_started_first="matching_engine_primary",
        assert_waiting_engine_catches_up=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=_waiting_engine_steps(),
    ),

    # 80 -- a matching engine catching up from far back finishes the catch-up when the second sequencer connects.
    #
    # BUG-0200. The secondary engine is started first and leads. 200,000 baseline orders make the WAL long. The follower
    # sequencer is killed, so that only the leading sequencer connects to the primary engine when it restarts. The
    # primary engine is then killed, its region damaged so that it holds nothing it can use, and restarted without its
    # record of the lease promises it made, so that it does not follow at once but catches up from the start of the WAL
    # when the leading sequencer connects. The follower sequencer is restarted while that stream is under way, and connects to the engine in the
    # middle of it. The engine must ask on the new connection only, and finish the catch-up already under way: no
    # Incomplete, and Completed within 10 seconds, less than the 15 seconds after which an engine asks again. Which
    # sequencer connects first after an engine restarts is otherwise chance, and only the leader's connection first
    # makes the window.
    Scenario(
        number=80,
        short_name="catch_up_survives_second_connection",
        description="A matching engine catching up from far back finishes the catch-up when the second sequencer connects",
        expected_outcome=(
            "the restarted matching_engine_primary asks the second sequencer alone when it connects, and the catch-up "
            "already under way finishes without the stream going backwards"
        ),
        me_ha=True,
        recovery_on_primary=True,
        engine_started_first="matching_engine_secondary",
        orders_before_override=200,
        assert_catch_up_survives_second_connection=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            KillStep(
                proc_name="sequencer_secondary",
                takeover=None,
                settle_secs=0.0,
            ),
            KillStep(
                proc_name="matching_engine_primary",
                takeover=None,
                settle_secs=0.0,
            ),
            RestartStep(
                proc_name="matching_engine_primary",
                ready_log_name="matching_engine_primary.log",
                ready=expect_catch_up("matching_engine_primary", "Started"),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=False,
                settle_secs=0.0,
                damage_region="matching_engine_open_orders.region",
                forget_lease_promises=True,
            ),
            RestartStep(
                proc_name="sequencer_secondary",
                ready_log_name="sequencer_secondary.log",
                ready=expect_follows("sequencer_secondary"),
                ready_timeout=_SEQ_FOLLOWER_TIMEOUT,
                resets_me_counter=False,
                settle_secs=0.0,
            ),
        ],
    ),

    # 81 -- reports for a gateway the leading sequencer has no connection for reports to are held, and sent once it has.
    #
    # BUG-0202. A sequencer sends reports to a gateway on a connection it makes to the gateway, apart from the gateway's
    # connection to it for orders. Here both sequencers' connections for reports to fix_order_gateway_a go through a
    # relay this script runs, which at first accepts nothing, so the leading sequencer has no connection for reports to
    # the gateway while the gateway can send it orders. A member sends 50 orders, which are sequenced and answered by the
    # engine. The sequencer must hold their reports, say so, and send none until the relay lets the connection through;
    # then every order must be reported to the member exactly once.
    Scenario(
        number=81,
        short_name="reports_held_for_unconnected_gateway",
        description="Reports for a gateway the leading sequencer has no connection for reports to are held, and sent once it has",
        expected_outcome=(
            "the leading sequencer holds the reports for the gateway while it has no connection for reports to it, drops none, "
            "and sends every one once the connection is made; every order is reported to the member exactly once"
        ),
        orders_during_override=0,
        orders_after_override=0,
        skip_baseline_orders=True,
        assert_reports_held_for_unconnected_gateway=True,
        steps=[],
    ),

    # 82 -- two engines configured to apply commands differently: the leader bars its peer.
    #
    # matching_engine_secondary holds at most _SMALLER_REGION_CAPACITY open orders, fewer than the primary, so once one
    # of them were full the two would refuse different orders. They exchange their settings when they connect. The engine
    # that leads says, in its lease statements, that its peer may not lead. The leader is killed: the peer, barred by the
    # arbiter's record of that statement, must not take the lead. The killed engine is restarted, takes the lead again,
    # and bars its peer again.
    Scenario(
        number=82,
        short_name="engines_with_different_settings",
        me_ha=True,
        description="Two matching engines configured differently: the one that leads says the other may not lead",
        expected_outcome=(
            "the leading engine reports that its peer's settings differ and that it has barred it; once it is killed the peer "
            "does not take the lead; restarted, it leads again and bars its peer again"
        ),
        orders_during_override=0,
        orders_after_override=0,
        skip_baseline_orders=True,
        expects_no_new_leader=True,
        assert_engines_with_different_settings=True,
        steps=[],
    ),

    # 83 -- two engines holding different identifier secrets: the leader bars its peer.
    #
    # matching_engine_secondary is started with an identifier secret of its own, so the two engines would give the same
    # report different ExecIDs, and a member would be told of one event under two names after a failover. The engines
    # exchange a fingerprint of their secrets when they connect, and the rest is scenario 82: the leader bars its peer,
    # the peer does not take the lead when the leader is killed, and the restarted leader bars it again.
    Scenario(
        number=83,
        short_name="engines_with_different_secrets",
        me_ha=True,
        description="Two matching engines holding different identifier secrets: the one that leads says the other may not lead",
        expected_outcome=(
            "the leading engine reports that its peer's identifier secret differs and that it has barred it; once it is killed "
            "the peer does not take the lead; restarted, it leads again and bars its peer again"
        ),
        orders_during_override=0,
        orders_after_override=0,
        skip_baseline_orders=True,
        expects_no_new_leader=True,
        assert_engines_with_different_settings=True,
        different_identifier_secret=True,
        steps=[],
    ),

    # 84 -- a following engine down for longer than the absence limit, while its peer trades, cancels nothing.
    #
    # BUG-0146. R-0117 is about the venue: the orders are cancelled and trading halts only when no matching engine could
    # trade for longer than the limit. An engine that is down while its peer leads is a loss of resilience and nothing
    # more. The following engine is killed with the baseline orders resting, left down for twelve seconds against a
    # limit of five, and restarted. The leading engine traded throughout, so the leading sequencer always had an engine
    # to route to and writes no command; the restarted engine must neither cancel its book nor halt, and neither must
    # the engine that leads. The orders sent afterwards are accepted as usual.
    Scenario(
        number=84,
        short_name="follower_down_past_absence_limit_cancels_nothing",
        description="A following matching engine down for longer than the absence limit, while its peer trades, cancels nothing and does not halt",
        expected_outcome=(
            "the following engine returns after twelve seconds against a limit of five, rejoins as a follower, and neither it "
            "nor the engine that leads cancels the book or halts; orders sent afterwards are accepted"
        ),
        me_ha=True,
        recovery_on_primary=True,
        absence_limit_seconds=5,
        orders_during_override=0,
        steps=[],
        restart_steps=[],
        extra_steps=[
            RestartStep(
                proc_name=ENGINE_FOLLOWING_AT_START,
                ready_log_name=f"{ENGINE_FOLLOWING_AT_START}.log",
                ready=expect_replication_connection(ENGINE_FOLLOWING_AT_START),
                ready_timeout=30.0,
                resets_me_counter=False,
                settle_secs=_ME_SETTLE,
                down_secs=12.0,
            ),
            AssertAbsentStep(
                absent=expect_message(ENGINE_FOLLOWING_AT_START, "its book cancelled", "BookCancelled", "matching_engine_test_events"),
                after_secs=10.0,
                description="the restarted follower cancelled nothing",
            ),
            AssertAbsentStep(
                absent=expect_message(ENGINE_FOLLOWING_AT_START, "trading halted", "Halted", "matching_engine_test_events"),
                after_secs=1.0,
                description="the restarted follower did not halt",
            ),
            AssertAbsentStep(
                absent=expect_message(ENGINE_LEADING_AT_START, "its book cancelled", "BookCancelled", "matching_engine_test_events"),
                after_secs=1.0,
                description="the engine that leads cancelled nothing",
            ),
            AssertAbsentStep(
                absent=expect_message(ENGINE_LEADING_AT_START, "trading halted", "Halted", "matching_engine_test_events"),
                after_secs=1.0,
                description="the engine that leads did not halt",
            ),
        ],
    ),

    # 85 -- no matching engine for longer than the venue recovers by itself.
    #
    # docs/availability/far_back_catch_up.md, section 5. While no engine can trade, the venue cannot answer members, so
    # it states the longest recovery it does by itself, and past that it halts trading and leaves a person to decide
    # when trading resumes, rather than leaving members' orders unanswered for as long as the recovery takes. The engine
    # is killed and kept down past a limit shortened to two seconds, well short of the absence limit, so that the halt
    # comes from this rule and nothing is cancelled. The leading sequencer must write a TechnicalEvent that halts
    # trading, and tell the gateways to refuse orders, saying why; the restarted engine meets the halt in its catch-up,
    # halts, and refuses an order sent afterwards.
    Scenario(
        number=85,
        short_name="recovery_limit_halts_trading",
        description="With no matching engine for longer than the venue recovers by itself, trading halts and orders are refused saying so",
        expected_outcome=(
            "the leading sequencer writes a TechnicalEvent that halts trading and tells the gateways the venue is halted; "
            "the restarted engine halts in its catch-up, cancels nothing, and refuses the order sent afterwards"
        ),
        automatic_recovery_limit_seconds=2,
        assert_recovery_halt=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
        restart_steps=[
            RestartStep(
                proc_name="matching_engine",
                ready_log_name="matching_engine.log",
                ready=expect_engine_reachable("matching_engine", "matching_engine"),
                ready_timeout=_ME_READY_TIMEOUT,
                resets_me_counter=True,
                settle_secs=_ME_SETTLE,
                # Past the two seconds this scenario allows, and short of the five minutes after which the book is
                # cancelled.
                down_secs=8.0,
            ),
        ],
    ),

    # 86 -- a failover finishes well within the longest recovery the venue does by itself.
    #
    # The limit deployed is ten seconds. The engine that leads is killed, and the other takes over within a lease period
    # and a short catch-up. No halt may be written for the recovery limit, and orders must flow afterwards, as they do in
    # scenario 16, here with the deployed limit in force.
    Scenario(
        number=86,
        short_name="failover_within_recovery_limit",
        description="A matching engine failover finishes within the longest recovery the venue does by itself, and nothing halts",
        expected_outcome=(
            "the engine that followed takes the lead within the ten seconds the venue recovers by itself, no halt is written "
            "for the recovery limit, and recovery orders are processed"
        ),
        me_ha=True,
        automatic_recovery_limit_seconds=10,
        assert_no_recovery_halt=True,
        orders_during_override=0,
        steps=[
            KillStep(
                proc_name=ENGINE_LEADING_AT_START,
                takeover=expect_leads(ENGINE_FOLLOWING_AT_START),
                settle_secs=SETTLE_AFTER_FAILOVER,
            ),
        ],
    ),

    # 87 -- the follower's copy of the book across a break in the link between the matching engines.
    #
    # BUG-0187, and the scenario phase 8 of the recovery plan requires before that phase starts. The leading engine sends
    # the follower a BookUpdate for each order it adds to or removes from its book. The link between the two is broken
    # while the member places five orders and cancels three, then mended, and one more order is placed, whose update
    # reaches the follower and makes it take its copy to be complete up to that order. The leading engine is then
    # killed, and the follower, promoted with open_orders_on_promotion = "keep", must hold exactly the orders the member
    # was told are open: a cancel of each of them is carried out, and a cancel of each order the member was told is
    # cancelled is refused. Both engines apply the WAL themselves, so the break costs the follower nothing: it is sent
    # every record it missed when the link is mended (docs/availability/matching_engine_lockstep.md).
    Scenario(
        number=87,
        short_name="engine_link_break_keeps_the_book",
        description="A break in the link between the matching engines leaves the promoted engine holding exactly the orders the member was told are open",
        expected_outcome=(
            "after orders are placed and cancelled while the link between the engines is down, and the leading engine is "
            "killed, the promoted engine carries out a cancel of every order the member was told is open and refuses a "
            "cancel of every order the member was told is cancelled"
        ),
        me_ha=True,
        open_orders_on_promotion="keep",
        assert_book_survives_peer_link_break=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
    ),

    # 88 -- the reports a leading engine never had written are sent by the engine that takes the lead.
    #
    # docs/availability/matching_engine_lockstep.md, section 3. A follower engine applies every command but sends none of
    # their reports; it keeps them until the WAL holds the leader's. The leading engine's sends of reports to the
    # sequencers are blocked, so it applies a thousand orders whose reports never reach the WAL, and it is then killed.
    # The member has been sent nothing for those orders. The engine that takes the lead must send the reports it kept of
    # every command after the last one the WAL holds every report of, so that the member is sent a New report for each.
    Scenario(
        number=88,
        short_name="kept_reports_sent_on_promotion",
        description="The reports a leading engine never had written are sent by the engine that takes the lead",
        expected_outcome=(
            "after the leading engine applies a thousand orders whose reports never reach the sequencers and is killed, the "
            "engine that takes the lead sends the reports it kept, and the member is sent a New report for every one"
        ),
        me_ha=True,
        open_orders_on_promotion="keep",
        assert_kept_reports_sent_on_promotion=True,
        orders_during_override=0,
        orders_after_override=0,
        steps=[],
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
        time.sleep(POLL_INTERVAL)

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


def provision_throttle_limits(comp_id: str, place: int, amend: int, cancel: int) -> None:
    """Set a comp id's limits on new orders, amends and cancels per second in the database.

    Same psql route, and the same reason, as provision_cancel_on_disconnect: the values must
    travel the real path to the gateway, so that a hop which drops them fails the scenario.
    """
    statement = (
        f"UPDATE pubsub_comp_id "
        f"SET max_place_per_second = {place}, max_amend_per_second = {amend}, max_cancel_per_second = {cancel} "
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
        die(f"could not provision throttle limits for '{comp_id}' "
            f"(is the database running and migrated to v4?):\n{result.stderr.strip()}")
    log(f"  {comp_id}: throttle limits set to new orders={place} amends={amend} cancels={cancel} per second")


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
    # The comp id the throttle scenario limits. It IS in the database, so its limits arrive with
    # the export and are kept when its SCRAM material is rewritten here.
    ensure_fix8_credentials(creds_file, _THROTTLED_COMP_ID, FIX8_PASSWORD)
    # The binary client's comp id, for the scenarios that drive the binary gateway.
    ensure_fix8_credentials(creds_file, _BINARY_COMP_ID, FIX8_PASSWORD)


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


_CLIENT_FIELD = re.compile(r"^\s+\S+(?: \[[^\]]*\])? \((\d+)\): (.*)$")


class ClientReports:
    """
    The execution reports one FIX8 test client received, read from its standard output while it runs.

    f8test prints each message it receives as a header block, a body block and a trailer block, one
    "FieldName (tag): value" line per field, and the trailer ends with the CheckSum (tag 10). Only messages whose
    CheckSum line has been written are taken, so a message half written when the file is read is taken on the next read.

    This is what the member was told, which is the strongest check of an order: it was sequenced, acted on by the
    matching engine, reported, routed back and delivered. The reports a member can be sent twice (a report sent again
    after a change of leader, marked PossDupFlag) are counted once, by the first New report for each ClOrdID.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self._position = 0
        self._pending = ""
        self._current: dict[str, str] = {}
        # In the order received, each execution report's fields that the checks use, by tag: ClOrdID (11), ExecType
        # (150), OrderID (37), PossDupFlag (43), OrigClOrdID (41) and OrdRejReason (103).
        self.reports: list[dict[str, str]] = []
        # Each ClOrdID that has had a New report, mapped to the position in reports of its first.
        self.first_new: dict[str, int] = {}
        # How many orders the test has told this client to send (send_orders). The client numbers its ClOrdIDs ord1,
        # ord2, ... in the order it sends them, from a starting number its configuration decides, so the orders of one
        # step are the next numbers after those already sent.
        self.orders_sent = 0

    def read(self) -> None:
        """Take every complete message written since the last read."""
        if not self.path.is_file():
            return
        with open(self.path, "r", encoding="utf-8", errors="replace") as handle:
            handle.seek(self._position)
            text = handle.read()
            self._position = handle.tell()
        text = self._pending + text
        last_newline = text.rfind("\n")
        self._pending = text[last_newline + 1:]
        for line in text[:last_newline + 1].splitlines():
            if line.startswith('header ("header")'):
                self._current = {}
                continue
            match = _CLIENT_FIELD.match(line)
            if not match:
                continue
            tag, raw = match.groups()
            enum_value = re.match(r"^.*\((.+)\)$", raw.strip())
            self._current[tag] = enum_value.group(1) if enum_value else raw.strip()
            if tag == "10":
                self._finish(self._current)
                self._current = {}

    def _finish(self, fields: dict[str, str]) -> None:
        if fields.get("35") != "8":
            return
        cl_ord_id = fields.get("11", "")
        exec_type = fields.get("150", "")
        self.reports.append({tag: fields[tag] for tag in ("11", "150", "37", "43", "41", "103") if tag in fields})
        if exec_type == "0" and cl_ord_id not in self.first_new:
            self.first_new[cl_ord_id] = len(self.reports) - 1

    def first_number(self) -> int | None:
        """The number of the lowest-numbered ClOrdID this client has had any report for, which is its first order's: the
        test waits for its first orders to be reported before it sends more. None before any report."""
        self.read()
        numbers = [int(report["11"][3:]) for report in self.reports if report.get("11", "").startswith("ord") and report["11"][3:].isdigit()]
        return min(numbers) if numbers else None

    def new_among(self, first_offset: int, count: int) -> int:
        """How many of the count orders sent after the first first_offset have had a New report."""
        start = self.first_number()
        if start is None:
            return 0
        return sum(1 for number in range(start + first_offset, start + first_offset + count) if f"ord{number}" in self.first_new)

    def mark(self) -> int:
        """A position in the reports: pass it as since to consider only the reports received after now."""
        self.read()
        return len(self.reports)

    def new_orders_since(self, since: int) -> int:
        """How many orders had their first New report at or after since."""
        self.read()
        return sum(1 for position in self.first_new.values() if position >= since)

    def open_orders(self) -> set[str]:
        """The ClOrdIDs of the orders this client was told are open: each has had a New report and no Canceled report. A
        Canceled report names the order it cancelled by OrigClOrdID when the member asked for the cancel, and by ClOrdID
        when the venue cancelled the order itself."""
        self.read()
        cancelled = {report.get("41") or report.get("11", "") for report in self.reports if report.get("150") == "4"}
        return set(self.first_new) - cancelled

    def count(self, exec_type: str, since: int = 0, **wanted: str) -> int:
        """How many execution reports of exec_type (the FIX ExecType value, such as "4" for Canceled) arrived at or after since,
        with each other field given by tag (for instance ord_rej_reason="2") equal to the value given."""
        self.read()
        tags = {"cl_ord_id": "11", "orig_cl_ord_id": "41", "ord_rej_reason": "103"}
        return sum(1 for report in self.reports[since:]
                   if report.get("150") == exec_type and all(report.get(tags[name]) == value for name, value in wanted.items()))


# What each running FIX8 test client has received, by its process id, for the clients send_burst starts.
_CLIENT_REPORTS: dict[int, ClientReports] = {}
# How many clients send_burst has started without being told where to put their output, to name each one's file.
_CLIENT_SESSIONS_STARTED = 0


def client_reports(f8proc: subprocess.Popen) -> ClientReports:
    """The reports the FIX8 test client f8proc has received."""
    return _CLIENT_REPORTS[f8proc.pid]


def await_new_orders_since(f8proc: subprocess.Popen, count: int, since: int, timeout: float) -> tuple[bool, float]:
    """Wait up to timeout seconds for count orders, any orders, to have their first New report at the client at or after
    position since in its reports. Returns whether they did, and how long it took."""
    reports = client_reports(f8proc)
    started = time.monotonic()
    deadline = started + timeout
    while reports.new_orders_since(since) < count:
        if time.monotonic() >= deadline:
            return False, time.monotonic() - started
        time.sleep(POLL_INTERVAL)
    return True, time.monotonic() - started


# How many orders one command to the FIX8 test client sends: "T" sends a burst of a thousand, "n" a single order.
_ORDERS_PER_COMMAND = {b"T": 1000, b"n": 1}


def send_orders(f8proc: subprocess.Popen, command: bytes, times: int) -> int:
    """Tell the FIX8 test client to run command times, and count the orders it sends.

    Returns how many orders it had been told to send before these, which await_new_orders takes to know which ClOrdIDs
    these are."""
    reports = client_reports(f8proc)
    sent_before = reports.orders_sent
    # The command key alone, with no newline. f8test's menu reads its input a key at a time and takes every key as a
    # command (MyMenu::process), so a newline is a key it does not know, and it prints "Command not found" for it into
    # the output the reports go to, at whatever point it has reached, even in the middle of a field. In a run of 100,000
    # orders that split 100 lines, and four reports were not counted.
    for _ in range(times):
        f8proc.stdin.write(command)
    f8proc.stdin.flush()
    reports.orders_sent += times * _ORDERS_PER_COMMAND[command]
    return sent_before


def await_new_orders(f8proc: subprocess.Popen, count: int, sent_before: int, timeout: float) -> tuple[bool, float]:
    """Wait up to timeout seconds for each of count orders, the next sent after the first sent_before, to have a New
    report at the client.

    Named by their ClOrdIDs rather than counted after a moment in time, so reports still arriving for orders sent earlier
    cannot stand in for these. Returns whether they did, and how long it took."""
    reports = client_reports(f8proc)
    started = time.monotonic()
    deadline = started + timeout
    while reports.new_among(sent_before, count) < count:
        if time.monotonic() >= deadline:
            return False, time.monotonic() - started
        time.sleep(POLL_INTERVAL)
    return True, time.monotonic() - started


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


def set_installed_recovery_limit(prefix: Path, seconds: int) -> int:
    """Set recovery.automatic_recovery_limit_seconds in every deployed sequencer config, as set_installed_absence_limit
    does for the absence limit, and for the same reasons: written on every launch, so that a scenario that shortened it
    cannot leave it short for the next."""
    changed = 0
    pattern = re.compile(r"^(automatic_recovery_limit_seconds\s*=\s*)\d+", re.M)
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


def set_installed_absence_limit(prefix: Path, seconds: int) -> int:
    """Set absence.absence_limit_seconds in every deployed sequencer config.

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


def set_installed_promotion_policy(prefix: Path, policy: str) -> int:
    """Set order_book.open_orders_on_promotion in every deployed matching engine config.

    What a promoted engine does with the book it inherits is a stated policy rather than a
    behaviour of the code, so a scenario that promotes one has to know which answer the venue
    is configured to give before it can assert anything about the result.

    Written on every launch rather than restored afterwards, for the same reason
    set_installed_absence_limit is.
    """
    changed = 0
    pattern = re.compile(r'^(open_orders_on_promotion\s*=\s*)"[a-z]*"', re.M)
    for toml_path in sorted((prefix / "etc").rglob("*.toml")):
        try:
            text = toml_path.read_text(errors="replace")
        except OSError:
            continue
        result, count = pattern.subn(lambda m: m.group(1) + '"' + policy + '"', text)
        if count and result != text:
            toml_path.write_text(result)
            changed += 1
    return changed


_ENGINE_COMPONENTS = ("matching_engine", "matching_engine_primary", "matching_engine_secondary")
_SEQUENCER_COMPONENTS = ("sequencer_primary", "sequencer_secondary")


def replay_check_applies() -> tuple[bool, str, list[str]]:
    """Whether wal_replay_check can be relied on for the scenario just run, why not if it cannot, and the engines that led.

    It can unless an engine cancelled every order by a decision of its own, sending reports. Every
    report's ExecID is made from the position in the WAL of the command that caused it
    (docs/availability/deterministic_matching_engine.md, section 3), so the state machine it replays names each report
    as whichever engine sent it did, however often the engines restarted or changed roles. Every cancel of the book
    and every halt is a command in the WAL, and is replayed; a cancel by an engine's own decision would be in the WAL
    with no command there to replay, and is looked for so that one cannot pass unnoticed. Worked out from what the
    engines said, not from the scenario's description, so that it cannot disagree with what happened."""
    engine_module = _TEST_EVENTS.modules["matching_engine_test_events"]
    for event in _TEST_EVENTS.events:
        if (event.component in _ENGINE_COMPONENTS and isinstance(event.message, engine_module.BookCancelled) and not event.message.by_command
                and event.message.cancel_reports_sent > 0):
            return False, (f"{event.component} cancelled every order by its own decision, which is not a command in the WAL, "
                           f"and sent {event.message.cancel_reports_sent} cancel report(s)"), []
    engines_that_led = sorted({event.component for event in _TEST_EVENTS.events if event.component in _ENGINE_COMPONENTS
                               and leads(_TEST_EVENTS.messages, event.component)(event)})
    if not engines_that_led:
        return False, "no matching engine led", []
    return True, "", engines_that_led


def run_wal_replay_check(prefix: Path, launched_configs: dict[str, Path]) -> tuple[str, str, str, int] | None:
    """Runs wal_replay_check on the WAL of the sequencer that led last, if the scenario is one it can be relied on for,
    and fails the scenario if the replay differs. Run after every process has stopped, so that nothing is written to the
    WAL while it is read.

    The replay is given the region capacity and the identifier secret of the engines that led, from the configuration
    each was started with. Where engines that led were configured differently, there is no one engine to replay, and
    the scenario is one about engines configured differently, so it is not run.

    Returns, when it ran and passed, the sequencer whose WAL it replayed, the region capacity and the secret file it was
    given, and how many reports the replay produced, for run_engines_on_the_wal; otherwise None."""
    applies, detail, engines = replay_check_applies()
    if not applies:
        log(f"=== WAL replay check: not run -- {detail} ===")
        return None
    leaders = [event.component for event in _TEST_EVENTS.events
               if event.component in _SEQUENCER_COMPONENTS and leads(_TEST_EVENTS.messages, event.component)(event)]
    if not leaders:
        die("WAL replay check: no sequencer said it led, so there is no WAL to replay.")
    sequencer = leaders[-1]
    sequencer_config = prefix / "etc" / "sequencer" / f"{sequencer}.toml"
    wal_dir = sequencer_config.parent / installed_toml_section_value(sequencer_config, "wal", "directory")
    settings = set()
    for engine in engines:
        engine_config = launched_configs.get(engine)
        if engine_config is None:
            die(f"WAL replay check: {engine} led, but this run did not start it, so its configuration is not known.")
        settings.add((installed_toml_section_value(engine_config, "order_book", "region_capacity"),
                      installed_toml_section_value(engine_config, "identifiers", "secret_file")))
    if len(settings) != 1:
        log(f"=== WAL replay check: not run -- the engines that led ({', '.join(engines)}) were configured differently ===")
        return None
    region_capacity, secret_file = next(iter(settings))
    work_dir = prefix / "var"
    log(f"=== WAL replay check: {sequencer}'s WAL, with {', '.join(engines)} having led ===")
    result = subprocess.run([str(prefix / "bin" / "wal_replay_check"), "--wal", str(wal_dir), "--region-capacity", region_capacity,
                             "--secret-file", secret_file, "--work-dir", str(work_dir)], capture_output=True, text=True, check=False, timeout=600)
    for line in (result.stdout + result.stderr).splitlines():
        log(f"  {line}")
    if result.returncode != 0:
        die(f"WAL replay check: a fresh matching engine given {sequencer}'s WAL did not produce the reports the WAL holds "
            f"(wal_replay_check exit {result.returncode}). The engine is not deterministic, or the WAL does not hold what it sent.")
    produced = re.search(r"the replay produced (\d+) report\(s\)", result.stdout)
    if produced is None:
        die("WAL replay check: wal_replay_check did not say how many reports the replay produced")
    return sequencer, region_capacity, secret_file, int(produced.group(1))


def config_with_values(config: Path, values: dict[tuple[str, str], str], copy: Path) -> Path:
    """A copy of a configuration, written to copy, with each (section, key) in values set to the text given.

    Only a key already in its section is changed, and every one must be found, so that a renamed setting fails here
    rather than leaving the copy as the original."""
    lines = config.read_text().splitlines()
    section = ""
    found: set[tuple[str, str]] = set()
    for index, line in enumerate(lines):
        header = re.match(r"^\[([^\]]+)\]\s*$", line)
        if header:
            section = header.group(1)
            continue
        assignment = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if assignment and (section, assignment.group(1)) in values:
            lines[index] = f"{assignment.group(1)} = {values[(section, assignment.group(1))]}"
            found.add((section, assignment.group(1)))
    missing = set(values) - found
    if missing:
        die(f"config_with_values: {config} has no {', '.join(f'[{s}] {k}' for s, k in sorted(missing))} to change")
    copy.write_text("\n".join(lines) + "\n")
    return copy


def run_engines_on_the_wal(prefix: Path, launched_configs: dict[str, Path], bin_dir: Path, log_dir: Path,
                           replayed: tuple[str, str, str, int]) -> None:
    """The second half of phase 7's finish condition: both matching engines, run on the same WAL from the start of the
    day, one after the other, each catching up from an empty book, give the same reports as each other and as the WAL
    (section 7.2 of docs/availability/deterministic_matching_engine.md).

    A copy of the WAL wal_replay_check was run on is given to a sequencer with high availability off and no absence
    limit to speak of, so that it writes nothing of its own. Each engine is started on it in turn with its own
    configuration, high availability off, and a region that cannot be read, so that it asks for every record from the
    first and rebuilds its book from the WAL, sending a report for every command, which the sequencer appends to the
    copy (R-0123). When both have run, wal_replay_check is run on the copy and requires every report the replay produces
    to be among those the two runs added, once from each, as the replay produced it. An engine configured differently
    from the ones that led is not run, and the one that is runs twice instead, so that two runs are compared all the
    same."""
    sequencer, region_capacity, secret_file, produced = replayed
    engines = [engine for engine in ("matching_engine_primary", "matching_engine_secondary", "matching_engine") if engine in launched_configs
               and (installed_toml_section_value(launched_configs[engine], "order_book", "region_capacity"),
                    installed_toml_section_value(launched_configs[engine], "identifiers", "secret_file")) == (region_capacity, secret_file)]
    if not engines:
        die("engines on the WAL: no engine this run started is configured as the engines that led were")
    runs = engines if len(engines) >= 2 else engines * 2
    work = prefix / "var" / "engines_on_the_wal"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    sequencer_config = launched_configs[sequencer]
    wal_copy = work / "wal"
    shutil.copytree(sequencer_config.parent / installed_toml_section_value(sequencer_config, "wal", "directory"), wal_copy)
    _, damage, last_record = wal_problems(wal_copy)
    if damage:
        die(f"engines on the WAL: the copy of {sequencer}'s WAL is damaged: {damage}")
    engine_port = installed_toml_section_value(sequencer_config, "matching_engine", "port")
    log(f"=== Engines on the WAL: {', '.join(runs)}, one after the other, each catching up on a copy of {sequencer}'s "
        f"{last_record} records from an empty book ===")
    started = time.monotonic()
    processes: list[tuple[str, subprocess.Popen]] = []
    try:
        # Every report sent again, so that each engine's reports can be compared with the replay's: by default an engine
        # catching up leaves out the reports the WAL already holds (docs/availability/far_back_catch_up.md, section 2.6).
        config = config_with_values(sequencer_config, {("ha", "ha_enabled"): "false", ("wal", "directory"): f'"{wal_copy}"',
                                                       ("absence", "absence_limit_seconds"): "86400",
                                                       ("recovery", "automatic_recovery_limit_seconds"): "86400",
                                                       ("catch_up", "send_every_report"): "true"}, work / f"{sequencer}.toml")
        since = _TEST_EVENTS.mark()
        processes.append((sequencer, launch_app(sequencer, "sequencer", config, bin_dir, log_dir)))
        if _TEST_EVENTS.wait_for(leads(_TEST_EVENTS.messages, sequencer), timeout=15.0, since=since) is None:
            die(f"engines on the WAL: {sequencer}, started on the copy of its WAL, did not lead within 15s")
        for run, engine in enumerate(runs, start=1):
            region = work / f"run_{run}.region"
            region.write_bytes(b"\xDE\xAD\xBE\xEF" * 16)
            # The engine's own configuration, but sending its reports to the sequencer directly: no relay runs here.
            config = config_with_values(config_without_report_relay(launched_configs[engine]),
                                        {("ha", "enabled"): "false", ("order_book", "region_path"): f'"{region}"',
                                         ("network", "listen_port"): engine_port}, work / f"run_{run}_{engine}.toml")
            since = _TEST_EVENTS.mark()
            process = launch_app(engine, "matching_engine", config, bin_dir, log_dir)
            processes.append((engine, process))
            rebuilt = engine_event(engine, "RegionRebuilt", since, 120.0)
            if rebuilt is None:
                die(f"engines on the WAL: {engine} did not rebuild its book from the copy of the WAL within 120s")
            expected = last_record + produced * run
            deadline = time.monotonic() + 60.0
            records = 0
            while time.monotonic() < deadline:
                _, _, records = wal_problems(wal_copy)
                if records >= expected:
                    break
                time.sleep(POLL_INTERVAL)
            if records < expected:
                die(f"engines on the WAL: after run {run}, by {engine}, the copy holds {records} records, not the {expected} expected: "
                    f"{last_record} of the scenario's and {produced} reports from each run so far")
            log(f"  run {run}: {engine} rebuilt its book through seq_no={rebuilt.through_seq_no} holding {rebuilt.book_size} order(s), "
                f"and sent {produced} report(s)")
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=30)
            if Path(f"/proc/{process.pid}").exists():
                die(f"engines on the WAL: {engine} (PID {process.pid}) is still running after it was stopped")
            region.unlink(missing_ok=True)
            Path(str(region) + ".unusable").unlink(missing_ok=True)
    finally:
        for name, process in processes:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    result = subprocess.run([str(prefix / "bin" / "wal_replay_check"), "--wal", str(wal_copy), "--region-capacity", region_capacity,
                             "--secret-file", secret_file, "--work-dir", str(work), "--engine-reports-after", str(last_record),
                             "--engine-runs", str(len(runs))], capture_output=True, text=True, check=False, timeout=600)
    for line in (result.stdout + result.stderr).splitlines():
        log(f"  {line}")
    if result.returncode != 0:
        die(f"engines on the WAL: the engines run on the same WAL did not give the reports the replay gives (wal_replay_check exit "
            f"{result.returncode}). An engine catching up is not deterministic, or not as the engine that served was.")
    log(f"  engines on the WAL: {len(runs)} runs agree with each other and with the WAL ({time.monotonic() - started:.0f}s)")
    shutil.rmtree(work, ignore_errors=True)


def check_write_ahead_logs_in_order(prefix: Path) -> None:
    """Fail the scenario if either sequencer's write-ahead log holds a record out of order, or is damaged.

    Both logs are read whole, as they stand on disk after the scenario (wal_problems).
    """
    for name in ("sequencer_primary", "sequencer_secondary"):
        config = prefix / "etc" / "sequencer" / f"{name}.toml"
        wal_dir = config.parent / installed_toml_section_value(config, "wal", "directory")
        out_of_order, damage, records = wal_problems(wal_dir)
        if damage:
            die(f"{name}'s write-ahead log in {wal_dir} is damaged: {damage}. Records after the damage would be lost, and every "
                "later scenario starts from it.")
        if out_of_order:
            earlier, later = out_of_order[0]
            die(f"{name}'s write-ahead log in {wal_dir} holds {len(out_of_order)} record(s) out of order, the first record {later} "
                f"directly after record {earlier}. It holds records twice or with a gap, and every later scenario starts from it "
                "(BUG-0147).")
        log(f"  write-ahead logs: {name}'s {records} records are numbered one after another and undamaged -- OK")


def wait_for_fix_logon(gateway: str, since: int, timeout: float) -> str:
    """
    Wait for the outcome of a FIX logon attempt at gateway, from its test events at or after since.
    Returns 'ok', 'degraded', 'auth_failed', 'sig_mismatch', 'refused' for any other refusal, or 'timeout'.

    'degraded' is a session that established only because the gateway stopped waiting for the
    sequencer to report its numbering. It is reported separately from 'ok' because the two are
    indistinguishable from the member's side at this instant and diverge later, when the
    member sees a sequence number below the one it expects and drops the session.
    """
    events = _TEST_EVENTS.modules["fix_order_gateway_test_events"]
    established = message_of(events.SessionEstablished, gateway)
    refused = message_of(events.LogonRefused, gateway)
    found = _TEST_EVENTS.wait_for(lambda event: established(event) or refused(event), timeout=timeout, since=since)
    if found is None:
        return "timeout"
    if isinstance(found.message, events.SessionEstablished):
        return "ok" if found.message.numbering_confirmed else "degraded"
    if found.message.reason == events.LogonRefusal.AuthenticationFailed:
        return "auth_failed"
    if found.message.reason == events.LogonRefusal.SignatureMismatch:
        return "sig_mismatch"
    return "refused"


# How many sends gateway a may hold waiting on one connection in scenario 71, and its socket send buffer:
# small enough that a member's orders fill the queue for a stopped sequencer within a second. The
# smallest limit a gateway accepts, because it must hold several batches of a resend.
_SMALL_SEND_QUEUE  = 1024
_SMALL_SEND_BUFFER = 16384


def config_with_small_send_queue(config: Path) -> Path:
    """A copy of a gateway's configuration that allows only _SMALL_SEND_QUEUE sends to wait on a connection.

    Written beside the original, so that paths in it relative to its directory still resolve, and used
    instead of it for one scenario. The original is not touched, so nothing has to be put back if the
    scenario fails part-way."""
    text = config.read_text()
    if "\n[reactor]\n" not in text:
        die(f"config_with_small_send_queue: {config} has no [reactor] section to add the limits to")
    text = text.replace("\n[reactor]\n", "\n[reactor]\n"
                        f"connection_waiting_sends_maximum = {_SMALL_SEND_QUEUE}\n"
                        f"socket_send_buffer_size = {_SMALL_SEND_BUFFER}\n", 1)
    copy = config.with_name(config.stem + "_small_send_queue.toml")
    copy.write_text(text)
    return copy


# Scenario 75: how many orders each sequencer holds at most, so few that a burst fills the storage.
_FEW_HELD_ORDERS = 8


def config_with_few_held_orders(config: Path) -> Path:
    """A copy of a sequencer's configuration that holds at most _FEW_HELD_ORDERS orders while it waits.

    Written beside the original and used instead of it for one scenario; the original is not touched."""
    text = config.read_text()
    line = next((candidate for candidate in text.splitlines() if candidate.startswith("orders_held_at_most =")), None)
    if line is None:
        die(f"config_with_few_held_orders: {config} has no orders_held_at_most to change")
    text = text.replace(line + "\n", f"orders_held_at_most = {_FEW_HELD_ORDERS}\n", 1)
    copy = config.with_name(config.stem + "_few_held_orders.toml")
    copy.write_text(text)
    return copy


# Scenario 82: how many open orders matching_engine_secondary holds at most, fewer than the deployed configuration's.
_SMALLER_REGION_CAPACITY = 1000000


def config_with_smaller_region(config: Path) -> Path:
    """A copy of a matching engine's configuration whose open-order region holds _SMALLER_REGION_CAPACITY orders.

    Written beside the original and used instead of it for one scenario; the original is not touched."""
    text = config.read_text()
    line = next((candidate for candidate in text.splitlines() if candidate.startswith("region_capacity =")), None)
    if line is None:
        die(f"config_with_smaller_region: {config} has no region_capacity to change")
    if line == f"region_capacity = {_SMALLER_REGION_CAPACITY}":
        die(f"config_with_smaller_region: {config} already holds {_SMALLER_REGION_CAPACITY} orders, so the copy would not differ")
    text = text.replace(line + "\n", f"region_capacity = {_SMALLER_REGION_CAPACITY}\n", 1)
    copy = config.with_name(config.stem + "_smaller_region.toml")
    copy.write_text(text)
    return copy


def config_with_other_identifier_secret(config: Path) -> Path:
    """A copy of a matching engine's configuration whose identifier secret is a new one, written beside it.

    Written beside the original and used instead of it for one scenario; neither the original nor the secret it names
    is touched."""
    text = config.read_text()
    line = next((candidate for candidate in text.splitlines() if candidate.startswith("secret_file =")), None)
    if line is None:
        die(f"config_with_other_identifier_secret: {config} has no secret_file to change")
    secret_path = config.with_name(config.stem + "_other_identifier_secret.key")
    secret_path.write_text(secrets.token_hex(32) + "\n")
    secret_path.chmod(0o600)
    text = text.replace(line + "\n", f'secret_file = "{secret_path}"\n', 1)
    copy = config.with_name(config.stem + "_other_identifier_secret.toml")
    copy.write_text(text)
    return copy


# Scenario 81: the relay port for the sequencers' connection for reports to fix_order_gateway_a, and the port the
# gateway listens on for it.
_GATEWAY_REPORT_RELAY_PORTS = {11931: 11010}


def config_with_gateway_report_relay(config: Path) -> Path:
    """A copy of a sequencer's configuration whose connection for reports to fix_order_gateway_a goes to the relay port."""
    text = config.read_text()
    for relay_port, gateway_port in _GATEWAY_REPORT_RELAY_PORTS.items():
        pattern = re.compile(rf"^(port\s*=\s*){gateway_port}$", re.M)
        if len(pattern.findall(text)) != 1:
            die(f"config_with_gateway_report_relay: {config} does not have exactly one 'port = {gateway_port}' to send through the relay")
        text = pattern.sub(rf"\g<1>{relay_port}", text)
    copy = config.with_name(config.stem + "_gateway_report_relay.toml")
    copy.write_text(text)
    return copy


# Scenario 72: the relay port for each of the matching engine's connections for reports, and the
# sequencer's report listener it passes traffic to.
_REPORT_RELAY_PORTS = {11921: 11021, 11922: 11022}


def config_with_report_relay(config: Path) -> Path:
    """A copy of the matching engine's configuration whose connections for reports go to the relay ports."""
    text = config.read_text()
    for relay_port, sequencer_port in _REPORT_RELAY_PORTS.items():
        if f"port = {sequencer_port}\n" not in text:
            die(f"config_with_report_relay: {config} has no 'port = {sequencer_port}' to send through the relay")
        text = text.replace(f"port = {sequencer_port}\n", f"port = {relay_port}\n", 1)
    copy = config.with_name(config.stem + "_report_relay.toml")
    copy.write_text(text)
    return copy


def config_without_report_relay(config: Path) -> Path:
    """The configuration as config_with_report_relay found it: its connections for reports go to the sequencers' report
    listeners again, since the relay runs only during scenario 72. A configuration that does not send through the relay
    is returned as it is."""
    text = config.read_text()
    if not any(f"port = {relay_port}\n" in text for relay_port in _REPORT_RELAY_PORTS):
        return config
    for relay_port, sequencer_port in _REPORT_RELAY_PORTS.items():
        text = text.replace(f"port = {relay_port}\n", f"port = {sequencer_port}\n", 1)
    copy = config.with_name(config.stem + "_without_relay.toml")
    copy.write_text(text)
    return copy


class PortRelay:
    """Accepts connections on local ports and passes bytes both ways to other local ports.

    Nothing listens on the relay ports until start(), so a connection attempted before then is
    refused, as it would be by a process that is not running. Runs on one thread of its own, which
    stop() ends."""

    def __init__(self, mapping: dict[int, int]) -> None:
        self.mapping = mapping
        self.selector = selectors.DefaultSelector()
        self.stopping = False
        self.thread: threading.Thread | None = None
        self.accepted = 0

    def start(self) -> None:
        for relay_port, target_port in self.mapping.items():
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", relay_port))
            listener.listen(8)
            listener.setblocking(False)
            self.selector.register(listener, selectors.EVENT_READ, ("listen", target_port))
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self) -> None:
        while not self.stopping:
            for key, _ in self.selector.select(timeout=0.1):
                kind, other = key.data
                if kind == "listen":
                    accepted, _ = key.fileobj.accept()
                    target = socket.create_connection(("127.0.0.1", other))
                    accepted.setblocking(False)
                    target.setblocking(False)
                    self.selector.register(accepted, selectors.EVENT_READ, ("pipe", target))
                    self.selector.register(target, selectors.EVENT_READ, ("pipe", accepted))
                    self.accepted += 1
                    continue
                try:
                    data = key.fileobj.recv(65536)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    data = b""
                if not data:
                    for sock in (key.fileobj, other):
                        try:
                            self.selector.unregister(sock)
                        except (KeyError, ValueError):
                            pass
                        sock.close()
                    continue
                other.setblocking(True)
                other.sendall(data)
                other.setblocking(False)

    def stop(self) -> None:
        self.stopping = True
        if self.thread is not None:
            self.thread.join(timeout=2.0)
        for key in list(self.selector.get_map().values()):
            key.fileobj.close()
        self.selector.close()


def process_is_stopped(pid: int) -> bool:
    """Whether a process is stopped by a signal, read from its state in /proc rather than assumed."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    # The state is the field after the command name, which is in brackets and may contain spaces.
    return stat[stat.rindex(")") + 2] in ("T", "t")


# The test event listener of the scenario being run (docs/availability/test_events.md). Each component that sends test
# events is started with its address, by launch_app.
_TEST_EVENTS: TestEventListener | None = None

# The install directory of the scenario being run, for checks that read a deployed configuration. Set by run_scenario.
_INSTALL_PREFIX = Path()

# The comp ids the matching engine reports the outcome of orders for, in the scenario being run (_REPORT_ORDERS_OF, and
# the member's comp id where the scenario needs it). Set by run_scenario before anything is started.
_REPORT_ORDERS_NOW: tuple = _REPORT_ORDERS_OF
# How long the gateways of the scenario being run wait before announcing their sessions on a sequencer link that has
# come up; set from the scenario before anything is started.
_ANNOUNCEMENT_DELAY_MS_NOW: int = 0

# The components that send test events so far. Others are added as their checks move onto test events.
_TEST_EVENT_COMPONENTS = ("sequencer", "matching_engine", "arbiter", "fix_order_gateway", "binary_order_gateway")


def config_with_test_events(config: Path, listener: TestEventListener, name: str, report_orders_of: tuple = (),
                            comp_ids_without_session: tuple = (), delay_session_announcements_ms: int = 0) -> Path:
    """A copy of a component's configuration with a [test_events] section naming the listener, and the name this test
    knows the component by, which its threads give when they introduce themselves. report_orders_of lists the comp ids
    whose orders a matching engine reports the outcome of. comp_ids_without_session lists the comp ids whose reports a
    sequencer is to expect to reach no session. delay_session_announcements_ms is how long a gateway waits before it
    announces its sessions on a sequencer link that has come up.

    Written beside the original, so that the paths in it still resolve, and used instead of it. Named after the
    component, not the original file, because two components can be started from one file."""
    text = config.read_text()
    if "[test_events]" in text:
        die(f"config_with_test_events: {config} already has a [test_events] section")
    text = text.rstrip("\n") + f"\n\n[test_events]\nhost = \"{listener.host}\"\nport = {listener.port}\ncomponent = \"{name}\"\n"
    if report_orders_of:
        text += "report_orders_of = [" + ", ".join(f'"{comp_id}"' for comp_id in report_orders_of) + "]\n"
    if comp_ids_without_session:
        text += "comp_ids_without_session = [" + ", ".join(f'"{comp_id}"' for comp_id in comp_ids_without_session) + "]\n"
    if delay_session_announcements_ms > 0:
        text += f"delay_session_announcements_ms = {delay_session_announcements_ms}\n"
    copy = config.with_name(f"{name}_test_events.toml")
    copy.write_text(text)
    return copy


def launch_app(name: str, bin_name: str, config: Path,
               bin_dir: Path, log_dir: Path, run_dir: Path | None = None,
               extra_environment: dict[str, str] | None = None) -> subprocess.Popen:
    """Start one component, optionally under scripts/launch.py.

    With run_dir given the component is supervised: launch.py restarts it if it dies, and the
    Popen returned is the LAUNCHER, not the component. Anything that needs to kill the
    component itself must read run_dir/<name>.pid, which launch.py keeps pointed at the child.

    Supervision is what makes the "restart inside the grace period" case testable at all. A
    harness that killed and restarted the process itself would be simulating a supervisor
    rather than exercising one, and the thing under test is whether a real restart beats the
    peer's promotion timeout.

    extra_environment is added to this process's environment only, for the test libraries that
    are loaded into one process with LD_PRELOAD.
    """
    if not config.is_file():
        die(f"config not found: {config}")
    if _TEST_EVENTS is not None and bin_name in _TEST_EVENT_COMPONENTS:
        # inject_order's comp id reaches the sequencer with no gateway holding a session for it, so its reports are
        # expected to reach no session.
        config = config_with_test_events(config, _TEST_EVENTS, name, _REPORT_ORDERS_NOW if bin_name == "matching_engine" else (),
                                         _COMP_IDS_WITHOUT_SESSION if bin_name == "sequencer" else (),
                                         _ANNOUNCEMENT_DELAY_MS_NOW if bin_name in ("fix_order_gateway", "binary_order_gateway") else 0)
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
            env={**os.environ, **extra_environment} if extra_environment else None,
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


def binary_gateway_listen_port(prefix: Path) -> int:
    """Read binary_order_gateway_a's client listen port from its deployed configuration."""
    config = prefix / "etc" / "binary_order_gateway" / "binary_order_gateway_a.toml"
    if not config.is_file():
        die(f"binary_order_gateway_a config not found: {config}")
    for line in config.read_text().splitlines():
        match = re.match(r"\s*listen_port\s*=\s*(\d+)", line)
        if match:
            return int(match.group(1))
    die(f"no listen_port in {config}")
    return 0  # unreachable; die() exits


def count_lines_with_all(log_path: Path, *markers: str, from_byte: int = 0) -> int:
    """Count the lines after from_byte that contain every one of the markers."""
    if not log_path.is_file():
        return 0
    with open(log_path, "r", errors="replace") as handle:
        handle.seek(from_byte)
        return sum(1 for line in handle if all(marker in line for marker in markers))


# The write-ahead log's entry layout, as WalWriter writes it: a 24-byte header of magic (u32),
# payload_size (u32), record_id (i64) and eight reserved bytes, then the payload and a CRC32.
_WAL_ENTRY_MAGIC = 0xFEEDFACE
_WAL_ENTRY_HEADER = struct.Struct("<IIqQ")
# What a write-ahead log entry's contents begin with: the wall-clock time in nanoseconds, the leader's
# epoch, and the message type (WalEntryContents.hpp).
_WAL_CONTENTS_HEADER = struct.Struct("<qih")


def wal_record_ids(wal_dir: Path, after: tuple[int, int] = (-1, -1)) -> list[tuple[tuple[int, int], int]]:
    """The sequence numbers in a sequencer's write-ahead log, in the order the entries are written.

    Returns ((segment, offset), record_id) for each entry at or after the position `after`,
    reading each segment from its start and stopping at the first entry whose magic number is
    wrong, which is where a segment's written part ends. The checksum is not checked: this reads
    the numbering, and WalReader decides what is valid.
    """
    entries: list[tuple[tuple[int, int], int]] = []
    for segment_path in sorted(wal_dir.glob("wal_*.log")):
        segment = int(segment_path.stem.split("_")[1])
        if segment < after[0]:
            continue
        data = segment_path.read_bytes()
        offset = 0
        while offset + _WAL_ENTRY_HEADER.size <= len(data):
            magic, size, record_id, _ = _WAL_ENTRY_HEADER.unpack_from(data, offset)
            if magic != _WAL_ENTRY_MAGIC:
                break
            if (segment, offset) >= after:
                entries.append(((segment, offset), record_id))
            offset += _WAL_ENTRY_HEADER.size + size + 4
    return entries


def wal_records_after(wal_dir: Path, after_seq_no: int, from_position: tuple[int, int] = (-1, -1)) -> dict[int, int]:
    """Each record numbered above after_seq_no in a sequencer's write-ahead log, mapped to the checksum stored with it.

    A follower writes the bytes its leader sends it unchanged, so the same record in two logs carries
    the same checksum, and two logs whose maps are equal hold the same records. Reading starts at the
    segment of from_position, which a caller passes to avoid reading a long log from its start.
    """
    records: dict[int, int] = {}
    for segment_path in sorted(wal_dir.glob("wal_*.log")):
        segment = int(segment_path.stem.split("_")[1])
        if segment < from_position[0]:
            continue
        data = segment_path.read_bytes()
        offset = 0
        while offset + _WAL_ENTRY_HEADER.size <= len(data):
            magic, size, record_id, _ = _WAL_ENTRY_HEADER.unpack_from(data, offset)
            if magic != _WAL_ENTRY_MAGIC:
                break
            checksum_at = offset + _WAL_ENTRY_HEADER.size + size
            if record_id > after_seq_no and checksum_at + 4 <= len(data):
                records[record_id] = struct.unpack_from("<I", data, checksum_at)[0]
            offset = checksum_at + 4
    return records


def compare_logs_after(label: str, leader_dir: Path, leader_from: tuple[int, int], follower_dir: Path, follower_from: tuple[int, int],
                       after_seq_no: int) -> int:
    """Fail unless the two logs hold the same records, with the same contents, numbered above after_seq_no, without a gap."""
    leader = wal_records_after(leader_dir, after_seq_no, leader_from)
    follower = wal_records_after(follower_dir, after_seq_no, follower_from)
    if not leader:
        die(f"{label}: the leader's log holds no record above {after_seq_no}, so there is nothing to compare.")
    expected = list(range(after_seq_no + 1, max(leader) + 1))
    if sorted(leader) != expected:
        die(f"{label}: the leader's own log has a gap above record {after_seq_no}.")
    missing = [seq for seq in expected if seq not in follower]
    if missing:
        die(f"{label}: the follower's log lacks {len(missing)} of the leader's {len(expected)} records above {after_seq_no}, "
            f"the first {missing[0]}. Its log has a gap where it missed records.")
    different = [seq for seq in expected if follower[seq] != leader[seq]]
    if different:
        die(f"{label}: {len(different)} records hold different contents in the two logs, the first record {different[0]}.")
    extra = sorted(seq for seq in follower if seq > max(leader))
    if extra:
        die(f"{label}: the follower's log holds {len(extra)} records the leader's does not, from record {extra[0]}.")
    return len(expected)


def check_new_leader_numbers_forward(wal_dir: Path, from_position: tuple[int, int]) -> None:
    """Fail unless every record the secondary wrote after from_position is numbered above the one before it."""
    written = wal_record_ids(wal_dir, from_position)
    if len(written) < 2:
        die(f"numbering: the secondary's log in {wal_dir} holds {len(written)} record(s) from the point it had reached "
            "before the kill, so nothing numbered after the change of leader can be checked.")
    backwards = numbering_goes_backwards(wal_dir, from_position)
    if backwards:
        die(f"numbering: after taking the lead, the secondary numbered records with numbers its log already held: "
            f"{len(backwards)} place(s) where the number goes backwards, the first from {backwards[0][0]} to {backwards[0][1]}. "
            "Anything that reads the log by number misreads it from there (BUG-0105).")
    log(f"  numbering: the {len(written)} records the secondary's log holds from before the kill onwards are numbered "
        f"in ascending order, {written[0][1]} to {written[-1][1]} -- OK")


def wal_end_position(wal_dir: Path) -> tuple[int, int]:
    """The position of the last entry in a sequencer's write-ahead log, or (-1, -1) if it has none."""
    entries = wal_record_ids(wal_dir)
    return entries[-1][0] if entries else (-1, -1)


def numbering_goes_backwards(wal_dir: Path, from_position: tuple[int, int]) -> list[tuple[int, int]]:
    """Each place, from the entry at from_position onwards, where a record's number is not above the one before."""
    ids = [record_id for _, record_id in wal_record_ids(wal_dir, from_position)]
    return [(earlier, later) for earlier, later in zip(ids, ids[1:]) if later <= earlier]


def wal_entries(wal_dir: Path):
    """Each entry in a write-ahead log, in the order written: (record number, wall-clock time in nanoseconds, contents).

    The contents are the bytes the writer gave the log, after the entry's own header of wall-clock time,
    leader epoch and message type. Read as wal_record_ids reads, segment by segment, stopping at the first
    entry whose magic number is wrong. A generator, so that a long log is never held in memory whole.
    """
    for segment_path in sorted(wal_dir.glob("wal_*.log")):
        data = segment_path.read_bytes()
        offset = 0
        while offset + _WAL_ENTRY_HEADER.size <= len(data):
            magic, size, record_id, _ = _WAL_ENTRY_HEADER.unpack_from(data, offset)
            if magic != _WAL_ENTRY_MAGIC:
                break
            payload_at = offset + _WAL_ENTRY_HEADER.size
            if payload_at + size > len(data) or size < _WAL_CONTENTS_HEADER.size:
                break
            wall_time_ns, _, _ = _WAL_CONTENTS_HEADER.unpack_from(data, payload_at)
            yield record_id, wall_time_ns, data[payload_at + _WAL_CONTENTS_HEADER.size:payload_at + size]
            offset = payload_at + size + 4


def wal_entry_size_if_valid(data: bytes, offset: int) -> int:
    """The size in bytes of the entry at offset, header, payload and checksum together, if it is a whole entry whose
    checksum is right; otherwise 0. The checksum is the CRC-32 of the header and the payload, as WalWriter computes it."""
    if offset + _WAL_ENTRY_HEADER.size > len(data):
        return 0
    magic, size, _, _ = _WAL_ENTRY_HEADER.unpack_from(data, offset)
    end = offset + _WAL_ENTRY_HEADER.size + size + 4
    if magic != _WAL_ENTRY_MAGIC or end > len(data):
        return 0
    stored = struct.unpack_from("<I", data, end - 4)[0]
    return end - offset if zlib.crc32(data[offset:end - 4]) == stored else 0


def wal_problems(wal_dir: Path) -> tuple[list[tuple[int, int]], str, int]:
    """What is wrong with a sequencer's write-ahead log as it stands on disk: (records out of order, damage, records read).

    Read as WalReader reads it. Each segment is read from its start, entry by entry, until an entry that is not whole or
    whose checksum is wrong. Unwritten space, and the unfinished entry a process that was killed while writing leaves at
    the end of the log, stop the reading there and are not damage. An entry that is not valid is damage when a valid entry
    follows it, later in its segment or in a later segment, because the records after it would be lost.

    Records out of order are each place where a record's number is not one more than the number of the record before it,
    given as (the record before, the record). The first record may have any number, because the start of a log can be
    deleted once it is no longer needed.
    """
    out_of_order: list[tuple[int, int]] = []
    previous = None
    records = 0
    stopped_at = ""  # where the last segment read stopped at bytes that were neither a valid entry nor unwritten space
    magic_bytes = struct.pack("<I", _WAL_ENTRY_MAGIC)
    for segment_path in sorted(wal_dir.glob("wal_*.log")):
        data = segment_path.read_bytes()
        offset = 0
        if stopped_at and wal_entry_size_if_valid(data, 0):
            return out_of_order, f"{stopped_at} is not a valid entry, and {segment_path.name} holds valid entries after it", records
        stopped_at = ""
        while True:
            size = wal_entry_size_if_valid(data, offset)
            if size == 0:
                break
            record_id = _WAL_ENTRY_HEADER.unpack_from(data, offset)[2]
            if previous is not None and record_id != previous + 1:
                out_of_order.append((previous, record_id))
            previous = record_id
            records += 1
            offset += size
        if offset < len(data) and any(data[offset:offset + _WAL_ENTRY_HEADER.size]):
            later = data.find(magic_bytes, offset + 1)
            while later != -1 and not wal_entry_size_if_valid(data, later):
                later = data.find(magic_bytes, later + 1)
            if later != -1:
                return out_of_order, (f"the entry at byte {offset} of {segment_path.name} is not valid, and a valid entry follows "
                                      f"it at byte {later}"), records
            stopped_at = f"the entry at byte {offset} of {segment_path.name}"
    return out_of_order, "", records


def publisher_wal_directory(prefix: Path, which: str) -> Path:
    """Where a matching engine publisher keeps its write-ahead log, from its installed configuration."""
    config = prefix / "etc" / "matching_engine_publisher" / f"matching_engine_publisher_{which}.toml"
    return config.parent / installed_toml_section_value(config, "wal", "directory")


def publisher_last_record(publisher_dir: Path) -> int:
    """The number of the last record a publisher's write-ahead log holds, or 0 if it holds none."""
    entries = wal_record_ids(publisher_dir)
    return entries[-1][1] if entries else 0


def wait_for_publisher_to_hold(label: str, publisher_dir: Path, seq_no: int, timeout: float) -> None:
    """Wait until a publisher's write-ahead log holds record seq_no or a later one; fail if it does not in time."""
    deadline = time.monotonic() + timeout
    started = time.monotonic()
    while publisher_last_record(publisher_dir) < seq_no:
        if time.monotonic() > deadline:
            die(f"{label}: the publisher's log in {publisher_dir} reached record {publisher_last_record(publisher_dir)}, not {seq_no}, "
                f"within {timeout:.0f}s.")
        time.sleep(POLL_INTERVAL)
    log(f"  {label}: the publisher holds record {seq_no} ({time.monotonic() - started:.1f}s)")


def check_publisher_holds_the_record(label: str, publisher_dir: Path, sequencer_dir: Path) -> tuple[int, int]:
    """Fail unless a publisher's log holds each record once, without a gap, each the same as the sequencer's record of that number.

    A publisher writes, under each number, the message the sequencer's record carries, inside the
    sequencer's envelope, and the envelope's wall-clock time. So the publisher's record is the same as the
    sequencer's when the two times are equal and the publisher's contents appear within the sequencer's.
    Returns the first and last numbers the publisher holds.
    """
    numbers: list[int] = []
    sequencer = wal_entries(sequencer_dir)
    current = next(sequencer, None)
    for record_id, wall_time_ns, contents in wal_entries(publisher_dir):
        if numbers and record_id == numbers[-1]:
            die(f"{label}: the publisher's log in {publisher_dir} holds record {record_id} twice, one after the other. It was "
                "sent the same record by both sequencers, or by one twice, and wrote it each time (BUG-0153).")
        if numbers and record_id <= numbers[-1]:
            die(f"{label}: the publisher's log in {publisher_dir} goes back from record {numbers[-1]} to record {record_id}, so it "
                "holds that number twice (BUG-0153).")
        if numbers and record_id != numbers[-1] + 1:
            die(f"{label}: the publisher's log in {publisher_dir} jumps from record {numbers[-1]} to record {record_id}: it lacks "
                f"{record_id - numbers[-1] - 1} record(s).")
        numbers.append(record_id)
        while current is not None and current[0] < record_id:
            current = next(sequencer, None)
        if current is None or current[0] != record_id:
            die(f"{label}: the publisher's log holds record {record_id}, which the sequencer's log in {sequencer_dir} does not "
                "hold. The publisher was sent a record the venue's record does not hold (BUG-0153).")
        if current[1] != wall_time_ns or contents not in current[2]:
            die(f"{label}: record {record_id} in the publisher's log is not the record the sequencer's log in {sequencer_dir} "
                "holds under that number. The publisher was sent a record the venue's record does not hold, and the new "
                "leader gave its number to another (BUG-0153).")
    if not numbers:
        die(f"{label}: the publisher's log in {publisher_dir} holds no record, so there is nothing to compare.")
    log(f"  {label}: the publisher's log holds records {numbers[0]} to {numbers[-1]}, each once and without a gap, each the same "
        "as the sequencer's record of that number -- OK")
    return numbers[0], numbers[-1]


def installed_toml_section_value(config: Path, section: str, key: str) -> str:
    """Read one setting from a named section of a deployed configuration file, as text.

    installed_toml_int matches a key wherever it first appears, which is wrong for a key such as
    listen_port that several sections of one file have. Quotes around a string are removed.
    """
    in_section = False
    for line in config.read_text().splitlines():
        header = re.match(r"\s*\[+([^\]]+)\]+\s*$", line)
        if header:
            in_section = header.group(1).strip() == section
            continue
        if in_section:
            match = re.match(rf"\s*{re.escape(key)}\s*=\s*(\"[^\"]*\"|\S+)", line)
            if match:
                return match.group(1).strip('"')
    die(f"no {key} in section [{section}] of {config}")
    return ""  # unreachable; die() exits


def blocked_peer_environment(prefix: Path, flag: Path) -> dict[str, str]:
    """The environment that makes libblock_sends_to_ports.so block a sequencer's sends to its peer.

    Both sequencers' peer listening ports are blocked. Each sequencer listens on its own and
    connects to the other's, so between them the two ports cover every connection between the
    pair, whichever side opened it.
    """
    library = prefix / "lib" / "libblock_sends_to_ports.so"
    if not library.is_file():
        die(f"{library} is not installed; it is built with the sequencer and installed by devsetup.sh")
    ports = [installed_toml_section_value(prefix / "etc" / "sequencer" / f"sequencer_{which}.toml", "peer", "listen_port")
             for which in ("primary", "secondary")]
    return {"LD_PRELOAD": str(library), "PUBSUB_TEST_BLOCK_PORTS": ",".join(ports), "PUBSUB_TEST_BLOCK_FLAG": str(flag),
            "PUBSUB_TEST_BLOCK_CONFIRMATION": str(block_confirmation(flag))}


def block_confirmation(flag: Path) -> Path:
    """The file libblock_sends_to_ports.so creates when it begins blocking, beside the flag file that tells it to."""
    return flag.with_suffix(".blocking")


def await_blocking(flag: Path, timeout: float) -> bool:
    """Whether libblock_sends_to_ports.so has begun blocking, waiting up to timeout seconds for its confirmation file.

    The library notices the flag file at the first send after it appears, so the confirmation file appears only once the
    process has sent something more.
    """
    confirmation = block_confirmation(flag)
    deadline = time.monotonic() + timeout
    while not confirmation.exists():
        if time.monotonic() >= deadline:
            return False
        time.sleep(POLL_INTERVAL)
    return True


def broken_engine_link_environment(prefix: Path, flag: Path) -> dict[str, str]:
    """The environment that makes libblock_sends_to_ports.so break a matching engine's link to its peer while flag exists.

    Both engines' book_replication listening ports are named. Each engine listens on its own and connects to the other's,
    so the two ports cover every connection between the pair, whichever side opened it. In the library's "break" mode a
    send on them fails as a send on a reset connection does, so the engine closes the connection, and once the flag is
    removed the engine's next connection works.
    """
    library = prefix / "lib" / "libblock_sends_to_ports.so"
    if not library.is_file():
        die(f"{library} is not installed; it is built with the sequencer and installed by devsetup.sh")
    ports = [installed_toml_section_value(prefix / "etc" / "matching_engine" / f"matching_engine_{which}.toml", "book_replication", "listen_port")
             for which in ("primary", "secondary")]
    return {"LD_PRELOAD": str(library), "PUBSUB_TEST_BLOCK_PORTS": ",".join(ports), "PUBSUB_TEST_BLOCK_FLAG": str(flag),
            "PUBSUB_TEST_BLOCK_CONFIRMATION": str(block_confirmation(flag)), "PUBSUB_TEST_BLOCK_MODE": "break"}


def blocked_engine_reports_environment(prefix: Path, flag: Path, engine: str) -> dict[str, str]:
    """The environment that makes libblock_sends_to_ports.so discard a matching engine's reports to the sequencers.

    The engine connects to each sequencer's port for reports, named in [sequencer_er] and [sequencer_er_secondary] of
    its configuration, so blocking those two ports discards every report it sends once the flag exists.
    """
    library = prefix / "lib" / "libblock_sends_to_ports.so"
    if not library.is_file():
        die(f"{library} is not installed; it is built with the sequencer and installed by devsetup.sh")
    config = prefix / "etc" / "matching_engine" / f"{engine}.toml"
    ports = [installed_toml_section_value(config, section, "port") for section in ("sequencer_er", "sequencer_er_secondary")]
    return {"LD_PRELOAD": str(library), "PUBSUB_TEST_BLOCK_PORTS": ",".join(ports), "PUBSUB_TEST_BLOCK_FLAG": str(flag),
            "PUBSUB_TEST_BLOCK_CONFIRMATION": str(block_confirmation(flag))}


def blocked_gateway_environment(prefix: Path, flag: Path) -> dict[str, str]:
    """The environment that makes libblock_sends_to_ports.so block the primary sequencer's sends to the gateways.

    The sequencer opens a connection to each gateway listed in a [[gateway]] table of its
    configuration and sends reports on it, so blocking each listed port blocks every report it sends.
    """
    library = prefix / "lib" / "libblock_sends_to_ports.so"
    if not library.is_file():
        die(f"{library} is not installed; it is built with the sequencer and installed by devsetup.sh")
    config = prefix / "etc" / "sequencer" / "sequencer_primary.toml"
    ports: list[str] = []
    in_gateway_table = False
    for line in config.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            in_gateway_table = stripped == "[[gateway]]"
            continue
        match = re.match(r"port\s*=\s*(\d+)", stripped)
        if in_gateway_table and match:
            ports.append(match.group(1))
    if not ports:
        die(f"no [[gateway]] ports in {config}, so there is nothing to block")
    return {"LD_PRELOAD": str(library), "PUBSUB_TEST_BLOCK_PORTS": ",".join(ports), "PUBSUB_TEST_BLOCK_FLAG": str(flag),
            "PUBSUB_TEST_BLOCK_CONFIRMATION": str(block_confirmation(flag))}


def installed_toml_int(config: Path, key: str) -> int:
    """Read one integer setting from a deployed configuration file, by its key alone."""
    for line in config.read_text().splitlines():
        match = re.match(rf"\s*{re.escape(key)}\s*=\s*(\d+)", line)
        if match:
            return int(match.group(1))
    die(f"no {key} in {config}")
    return 0  # unreachable; die() exits


# One reply as binary_client prints it. Text comes last because it may contain spaces.
_BINARY_REPORT_LINE = re.compile(
    r"^ExecutionReport seq=(?P<seq>\S+) ClOrdID=(?P<cl_ord_id>\S*) OrderID=(?P<order_id>\S*) OrdStatus=(?P<ord_status>\S+) "
    r"ExecType=(?P<exec_type>\S+) Symbol=(?P<symbol>\S*) Text=(?P<text>.*)$")
_BINARY_CANCEL_REJECT_LINE = re.compile(
    r"^OrderCancelReject seq=(?P<seq>\S+) ClOrdID=(?P<cl_ord_id>\S*) OrigClOrdID=(?P<orig_cl_ord_id>\S*) OrdStatus=(?P<ord_status>\S+) "
    r"CxlRejReason=(?P<cxl_rej_reason>\S+) Text=(?P<text>.*)$")


def run_binary_client(bin_dir: Path, port: int, *arguments: str) -> tuple[int, list[dict[str, str]], str]:
    """Run binary_client once as the binary test comp id, and parse the replies it printed.

    Returns its exit status, one dictionary for each ExecutionReport or OrderCancelReject it
    printed (with "kind" naming which), and its whole output for a failure message. Exit status 3
    means it gave up waiting for a reply.
    """
    command = [str(bin_dir / "binary_client"), "--host", "127.0.0.1", "--port", str(port),
               "--comp-id", _BINARY_COMP_ID, "--password", FIX8_PASSWORD,
               "--reply-timeout", str(_BINARY_REPLY_TIMEOUT), *arguments]
    result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=_BINARY_REPLY_TIMEOUT * 4)
    replies: list[dict[str, str]] = []
    for line in result.stdout.splitlines():
        report = _BINARY_REPORT_LINE.match(line)
        if report:
            replies.append({"kind": "ExecutionReport", **report.groupdict()})
            continue
        reject = _BINARY_CANCEL_REJECT_LINE.match(line)
        if reject:
            replies.append({"kind": "OrderCancelReject", **reject.groupdict()})
    return result.returncode, replies, result.stdout + result.stderr


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
    previous scenario's orders resting on the book, and its baseline orders, which reuse the same
    ClOrdIDs, are refused as duplicates of orders still open.

    A scenario's premise is a venue whose book is empty and whose numbering starts at one. That
    is a thing the harness has to arrange deliberately, the same way it deletes the clients'
    session store, because in a deployment finding the previous process's region is the whole
    point.

    The engine creates the region again at startup when it finds none.
    """
    var_dir = venue_state_dir(prefix)
    if not var_dir.is_dir():
        return
    for path in sorted(var_dir.glob("*open_orders.region")):
        try:
            path.unlink()
        except OSError as error:
            die(f"could not clear the open-order region at {path}: {error}")


def clear_write_ahead_logs(prefix: Path) -> None:
    """Delete the write-ahead logs of the sequencers and the publishers before a scenario, as a new trading day starts
    with none (V12).

    Left in place, each scenario after the first starts on a WAL holding every earlier scenario's records. An engine
    that catches up from the start of the WAL then applies all of their orders and reports each to the member again,
    under the same comp id and the same ClOrdIDs as the scenario's own, and what the scenario counts is not what it
    sent (BUG-0201). The paths are the ones the deployment configures, read by devenv.clear_durable_state, which also
    clears the lease promise records and the arbiters' statements that belong to the day the WAL held.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import devenv  # pylint: disable=import-outside-toplevel
    devenv.clear_durable_state(devenv.load_env(devenv._DEFAULT_ENV_FILE), prefix, fresh_book=False, fresh_wal=True)  # pylint: disable=protected-access


def venue_state_dir(prefix: Path) -> Path:
    """The directory holding the matching engines' open-order regions.

    Read from the deployed matching engine configuration rather than assumed. deploy.py keeps the
    regions off the filesystem that holds the write-ahead logs and the small state files, because a
    region being created there delays every sync of those files (BUG-0107).
    """
    config = prefix / "etc" / "matching_engine" / "matching_engine_primary.toml"
    if config.is_file():
        match = re.search(r'^region_path\s*=\s*"([^"]+)"', config.read_text(), re.MULTILINE)
        if match:
            return Path(match.group(1)).parent
    return prefix / "var"


def small_state_dir(prefix: Path) -> Path:
    """The directory holding the matching engines' epoch files and the arbiters' lease promise records.

    deploy.py puts these with the write-ahead logs when PUBSUB_WAL_ROOT names a device, and in the
    install's var directory otherwise, so it is read from the deployed arbiter configuration.
    """
    config = prefix / "etc" / "arbiter" / "arbiter_primary.toml"
    if config.is_file():
        match = re.search(r'^promise_file\s*=\s*"([^"]+)"', config.read_text(), re.MULTILINE)
        if match:
            return Path(match.group(1)).parent
    return prefix / "var"


def clear_lease_promise_records(prefix: Path) -> None:
    """Delete the records of lease promises kept by the sequencers, matching engines and arbiters before a run.

    Each of those instances writes to disk the promise of its vote it has made to its peer, or that
    its vote is its own while it leads, so that a process restarted by its supervisor carries on
    from the record instead of waiting out a lease period. A record stays current for about ten
    seconds after the process that wrote it stops, and scenarios start closer together than that.
    Left in place, an instance that led at the end of one scenario would ask to lead at once at the
    start of the next, and the secondary could lead where the scenario expects the primary to.

    A scenario's premise is a venue started from cold, in which the primary is preferred. Without a
    record an instance waits one lease period before voting, as it does after a reboot.

    The same pattern also removes each arbiter's record of the statements component leaders have made
    about whether their peers may lead, which is named after its promise record with
    ".component_statements" added. Left in place, a statement from one scenario would have the arbiter
    refuse an instance in the next.
    """
    candidates = []
    for state_dir in {small_state_dir(prefix), venue_state_dir(prefix)}:
        if state_dir.is_dir():
            candidates += list(state_dir.glob("*lease_promise*"))
    # A sequencer keeps its record in its write-ahead log directory, which is not under var when
    # PUBSUB_WAL_ROOT puts the logs on a disk of their own, so the directory is read from the
    # deployed configuration.
    for name in ("sequencer_primary.toml", "sequencer_secondary.toml"):
        config = prefix / "etc" / "sequencer" / name
        if config.is_file():
            match = re.search(r'^\[wal\][^\[]*?^directory\s*=\s*"([^"]+)"', config.read_text(), re.MULTILINE | re.DOTALL)
            if match:
                candidates.append(Path(match.group(1)) / "lease_promise.state")
    for path in sorted(candidates):
        try:
            path.unlink(missing_ok=True)
        except OSError as error:
            die(f"could not clear the lease promise record at {path}: {error}")


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
    nobody else, the gateway's own record of it being below the deployed log level. When it is
    not given, the output still goes to a file of its own in the log directory, because every
    order the client sends is confirmed by the report it receives (client_reports).

    next_expected_receive starts the client believing the venue's numbering stands at that
    value. Setting it below where the venue has actually reached manufactures a gap the member
    detects for itself and asks about -- see _RESEND_GAP_MESSAGES.
    """
    logon_from = _TEST_EVENTS.mark()
    if client_output is None:
        global _CLIENT_SESSIONS_STARTED  # pylint: disable=global-statement
        _CLIENT_SESSIONS_STARTED += 1
        client_output = gw_log.parent / f"f8test_session_{_CLIENT_SESSIONS_STARTED}.txt"
    stdout_target = client_output.open("w")
    # Line-buffered, so that each report the client prints reaches the file as soon as it is printed. Writing to a file,
    # its output would otherwise be buffered in blocks, and the last few reports of a burst would sit in the buffer
    # until the client next printed something, while the test waited for them.
    command = ["stdbuf", "-oL", "-eL", str(FIX8_BIN), "-c", config, "-N", "GW1"]
    if next_expected_receive is not None:
        command += ["-R", str(next_expected_receive)]
    proc = subprocess.Popen(
        command,
        cwd=str(FIX8_DIR),
        stdin=subprocess.PIPE,
        stdout=stdout_target,
        stderr=subprocess.STDOUT,
    )
    stdout_target.close()  # the client holds its own copy
    _CLIENT_REPORTS[proc.pid] = ClientReports(client_output)
    log(f"  f8test PID {proc.pid}: waiting up to {FIX8_LOGON_WAIT:.0f}s for FIX logon, output in {client_output.name} ...")
    outcome = wait_for_fix_logon(gw_log.stem, logon_from, FIX8_LOGON_WAIT)
    if outcome == "auth_failed":
        stop_f8test(proc)
        die("FIX logon failed: authentication rejected — check credentials.toml has an entry for this comp_id")
    if outcome == "sig_mismatch":
        stop_f8test(proc)
        die("FIX logon failed: ServerSignature mismatch — auth service identity could not be verified")
    if outcome == "refused":
        stop_f8test(proc)
        die("FIX logon failed: the gateway refused it -- the comp id is provisioned for another gateway instance, or its "
            "throttle limits are out of range")
    if outcome == "degraded":
        stop_f8test(proc)
        die("FIX logon completed only via the gateway's sequence-state fallback — the session was opened "
            "at a number the sequencer never confirmed. The logon raced the gateway's sequencer links "
            "and the retry on connect did not close the window; see docs/bug_list.md, BUG-0019")
    if outcome == "timeout":
        stop_f8test(proc)
        die(f"FIX logon timed out after {FIX8_LOGON_WAIT:.0f}s — gateway or auth service may not be ready")
    send_orders(proc, b"T", count)
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


def kill_summary_line(outcome: str | None, label: str, elapsed: float) -> str:
    """
    One line of the result summary for a kill, isolate or machine-kill step.

    A step whose assertion is not a promotion says so.  Reporting "elected leader" because
    some marker matched is how a summary comes to claim what the run never established --
    which is the shape of half the entries in docs/bug_list.md.
    """
    if outcome is None:
        return f"  killed  : {label} — no disruption"
    if outcome == _DEFAULT_KILL_OUTCOME:
        return f"  failover: {label} {outcome} in {elapsed:.1f}s"
    return f"  outcome : {label} {outcome} in {elapsed:.1f}s"


def with_engines_resolved(step):
    """step, with ENGINE_LEADING_AT_START or ENGINE_FOLLOWING_AT_START in its process name, and in the Quill log it waits
    on, replaced by the engine each stands for in this scenario; or, for a machine named by the engine it runs, the
    processes on that machine."""
    if isinstance(step, MachineKillStep) and step.machine_of is not None:
        engine = resolved(step.machine_of)
        side = "primary" if engine == "matching_engine_primary" else "secondary"
        return step._replace(proc_names=(f"arbiter_{side}", f"sequencer_{side}", engine), machine_of=None)
    if not hasattr(step, "proc_name"):
        return step
    actual = resolved(step.proc_name)
    if actual == step.proc_name:
        return step
    changes = {"proc_name": actual}
    if hasattr(step, "ready_log_name"):
        changes["ready_log_name"] = f"{actual}.log"
    return step._replace(**changes)


def do_kill_step(
    step: KillStep,
    proc_by_name: dict[str, subprocess.Popen],
    failover_timeout: float,
) -> tuple[str | None, str, float]:
    """
    Execute one KillStep: SIGKILL the target process, then either:
      - wait for the test events the step says must follow (if any), or
      - settle briefly without waiting for anything.

    Returns (outcome, label, elapsed_seconds).
      outcome: the phrase describing what was established; label is what the step expected.
               None → nothing was expected to follow; label is the killed process.
      elapsed_seconds: how long the outcome took, or 0.0 where none was awaited.
    """
    proc = proc_by_name.get(step.proc_name)
    if proc is None or proc.poll() is not None:
        die(f"{step.proc_name} is not running — cannot kill")

    # Only events that arrive after the kill can show what the kill caused.
    since = _TEST_EVENTS.mark()

    log(f"  SIGKILL → {step.proc_name} (PID {proc.pid})")
    proc.kill()
    proc.wait()
    if Path(f"/proc/{proc.pid}").exists():
        die(f"{step.proc_name} (PID {proc.pid}) is still running after SIGKILL")
    log(f"  {step.proc_name} confirmed dead")

    if step.takeover is None:
        log(f"  No failover expected; settling {step.settle_secs:.0f}s ...")
        time.sleep(step.settle_secs)
        return None, step.proc_name, 0.0

    log(f"  Waiting for: {step.takeover.what} (timeout {failover_timeout:.0f}s) ...")
    found, elapsed = await_expectation(step.takeover, failover_timeout, since)
    outcome = step.outcome_label if step.outcome_label else _DEFAULT_KILL_OUTCOME
    if not found:
        die(f"not seen within {failover_timeout:.0f}s of killing {step.proc_name}: {step.takeover.what}")

    log(f"  {step.takeover.what} ({elapsed:.1f}s after kill)")
    log(f"  Settling {step.settle_secs:.0f}s for connections to stabilise ...")
    time.sleep(step.settle_secs)
    return outcome, step.takeover.what, elapsed


def do_machine_kill_step(
    step: MachineKillStep,
    proc_by_name: dict[str, subprocess.Popen],
) -> tuple[str | None, str, float]:
    """
    Kill every named process without waiting between them, then poll for the
    survivors taking over.  Nothing is restarted: a machine that has stopped
    does not come back on its own, and a test that restarts these processes is
    testing something else.
    """
    label = ", ".join(step.proc_names)
    since = _TEST_EVENTS.mark()

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
        if Path(f"/proc/{proc.pid}").exists():
            die(f"machine kill: {name} (PID {proc.pid}) is still running after SIGKILL")
    log(f"  {len(killed)} process(es) confirmed dead, and none will be restarted")

    if step.takeover is None:
        time.sleep(step.settle_secs)
        return _DEFAULT_KILL_OUTCOME, label, 0.0

    log(f"  Waiting for: {step.takeover.what} (timeout {step.ready_timeout:.0f}s) ...")
    found, elapsed = await_expectation(step.takeover, step.ready_timeout, since)
    if not found:
        die(f"machine kill: nothing on the surviving machine took over within {step.ready_timeout:.0f}s")
    log(f"  the surviving machine took over ({elapsed:.1f}s)")
    time.sleep(step.settle_secs)
    return _DEFAULT_KILL_OUTCOME, label, elapsed


def do_isolate_step(
    step: IsolateStep,
    proc_by_name: dict[str, subprocess.Popen],
) -> tuple[str | None, str, float]:
    """Freeze the named process, wait for its peer to take over, then let it run again."""
    proc = proc_by_name.get(step.proc_name)
    if proc is None or proc.poll() is not None:
        die(f"isolate: '{step.proc_name}' is not running")

    takeover_from = _TEST_EVENTS.mark()
    log(f"  SIGSTOP -> {step.proc_name} (PID {proc.pid}): alive, holding its sockets, renewing nothing")
    os.kill(proc.pid, signal.SIGSTOP)

    log(f"  Waiting for: {step.takeover.what} (timeout {step.takeover_timeout:.0f}s) ...")
    found, elapsed = await_expectation(step.takeover, step.takeover_timeout, takeover_from)
    if not found:
        os.kill(proc.pid, signal.SIGCONT)
        die(f"isolate: the peer did not take over within {step.takeover_timeout:.0f}s")
    log(f"  the peer took over ({elapsed:.1f}s) while {step.proc_name} still believed it was entitled to act")

    resumed_from = _TEST_EVENTS.mark()
    log(f"  SIGCONT -> {step.proc_name}: its lease ran out while it was stopped, which it finds on waking")
    os.kill(proc.pid, signal.SIGCONT)

    log(f"  Waiting for {step.proc_name} to stop acting by itself: {step.stopped.what} (timeout {step.stopped_timeout:.0f}s) ...")
    found, stopped_elapsed = await_expectation(step.stopped, step.stopped_timeout, resumed_from)
    if not found:
        die(f"isolate: {step.proc_name} did not report that its lease had run out within {step.stopped_timeout:.0f}s of waking")
    log(f"  {step.proc_name} stopped acting by itself ({stopped_elapsed:.1f}s)")

    time.sleep(step.settle_secs)

    # And the entitlement must not have gone back.
    if expectation_seen(step.forbidden, resumed_from):
        die(f"isolate: the survivor gave the entitlement back to a superseded instance: {step.forbidden.what}")
    log(f"  the survivor kept the entitlement -- checked every event it sent since the resume, up to a checkpoint: not seen: {step.forbidden.what}")
    return _DEFAULT_KILL_OUTCOME, step.proc_name, elapsed


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
    Kill the named process (if still running), delete its Quill log so that Quill
    starts a fresh one, relaunch it, and wait until it is ready.

    Updates proc_by_name and app_procs in place.
    Returns elapsed seconds from relaunch to readiness.
    """
    if step.ready is None:
        die(f"restart step for {step.proc_name}: it gives no test events that say the process is ready")
    proc = proc_by_name.get(step.proc_name)
    if proc is not None and proc.poll() is None:
        log(f"  SIGKILL → {step.proc_name} (PID {proc.pid})")
        proc.kill()
        proc.wait()
        if Path(f"/proc/{proc.pid}").exists():
            die(f"{step.proc_name} (PID {proc.pid}) is still running after SIGKILL")
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

    if step.forget_lease_promises:
        config = bin_dir.parent / "etc" / "matching_engine" / f"{step.proc_name}.toml"
        match = re.search(r'^epoch_state_file\s*=\s*"([^"]+)"', config.read_text(), re.MULTILINE) if config.is_file() else None
        if match is None:
            die(f"forget_lease_promises: no epoch_state_file in {config}, so the record cannot be found")
        record = Path(match.group(1) + ".lease_promise")
        if not record.is_file():
            die(f"forget_lease_promises: {record} does not exist, so there is nothing to forget and the "
                "scenario would pass without testing anything")
        record.unlink()
        log(f"  Deleted {record.name}, so {step.proc_name} restarts knowing nothing of what it promised")

    if step.down_secs > 0:
        log(f"  Leaving {step.proc_name} down for {step.down_secs:.0f}s, so that the absence is real")
        time.sleep(step.down_secs)

    # Delete the old Quill log before restarting, so that Quill writes a fresh file for the new process.
    (log_dir / step.ready_log_name).unlink(missing_ok=True)

    launch_entry = next((e for e in launch_table if e[0] == step.proc_name), None)
    if launch_entry is None:
        die(f"No launch table entry for '{step.proc_name}'")
    _, bin_name, config = launch_entry

    # Only events from the new process can show that it is ready.
    since = _TEST_EVENTS.mark()
    log(f"  Restarting {step.proc_name} ...")
    new_proc = launch_app(step.proc_name, bin_name, config, bin_dir, log_dir)
    proc_by_name[step.proc_name] = new_proc
    for i, (name, _) in enumerate(app_procs):
        if name == step.proc_name:
            app_procs[i] = (step.proc_name, new_proc)
            break
    else:
        app_procs.append((step.proc_name, new_proc))

    log(f"  Waiting for: {step.ready.what} (timeout {step.ready_timeout:.0f}s) ...")
    found, elapsed = await_expectation(step.ready, step.ready_timeout, since)
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

def keep_logs_of_failed_scenario(scenario: Scenario, args) -> Path | None:
    """
    Copy the Quill logs and the test clients' output of a scenario that has just failed to a directory of their own,
    tmp/ha_test_failures/<time>-scenario-<number>-<short name> under the project, and return it; None if there was
    nothing to copy.

    The next scenario deletes these files before it starts, so without the copy the evidence of a failure in a run of
    every scenario is gone by the time the run ends. Called after the scenario's processes have been stopped, so the
    logs are complete.
    """
    project_root = Path(__file__).resolve().parent.parent
    raw_prefix = args.prefix
    log_dir = Path(raw_prefix if Path(raw_prefix).is_absolute() else project_root / raw_prefix) / "log"
    if not log_dir.is_dir():
        return None
    destination = project_root / "tmp" / "ha_test_failures" / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}-scenario-{scenario.number}-{scenario.short_name}"
    shutil.copytree(log_dir, destination)
    return destination


def run_scenario(scenario: Scenario, args) -> bool:
    """
    Run one scenario end-to-end.  Returns True on PASS, False on FAIL.
    Always prints a RESULT: PASS / RESULT: FAIL summary block.
    """
    # Numbering faults recorded by a test FIX client in an earlier scenario belong to that scenario.
    raw_client_module = sys.modules.get("fix_raw_client")
    if raw_client_module is not None:
        raw_client_module.FixRawClient.all_receive_numbering_faults.clear()
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

    # The matching engine started first and required to lead before the other starts, or empty for the usual start.
    engine_first = "matching_engine_secondary" if args.secondary_engine_first else scenario.engine_started_first
    engine_second = "matching_engine_primary" if engine_first == "matching_engine_secondary" else "matching_engine_secondary"
    start_order_source = "--secondary-engine-first" if args.secondary_engine_first else f"scenario {scenario.number}"

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
    # And the test clients' output from an earlier scenario, so that every such file in the log directory is this one's.
    stale_logs.extend(log_dir.glob("f8test_session_*.txt"))
    for stale in stale_logs:
        stale.unlink(missing_ok=True)

    # ── header ────────────────────────────────────────────────────────────────
    def _step_label(s) -> str:
        if isinstance(s, KillStep):
            return s.proc_name + (" [failover expected]" if s.takeover is not None else "")
        if isinstance(s, RestartStep):
            return f"RESTART:{s.proc_name}"
        if isinstance(s, IsolateStep):
            return f"ISOLATE:{s.proc_name}"
        if isinstance(s, MachineKillStep):
            return f"MACHINE:the one {s.machine_of} runs on" if s.machine_of is not None else "MACHINE:" + "+".join(s.proc_names)
        if isinstance(s, InterimOrdersStep):
            return f"({s.count_batches * 1000} interim orders)"
        if isinstance(s, (VerifyStep, OneLeaderStep, AssertAbsentStep)):
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
    orders_before = scenario.orders_before_override if scenario.orders_before_override is not None else args.orders_before
    log(f"  orders before    : {orders_before * 1000}")
    log(f"  orders during    : {orders_during * 1000}  (in flight during kill)")
    log(f"  orders after     : {orders_after * 1000}")
    log(f"  ready timeout    : {args.ready_timeout:.0f}s")
    log(f"  failover timeout : {args.failover_timeout:.0f}s  (per step)")
    log(f"  recovery timeout : {args.recovery_timeout:.0f}s")
    if engine_first:
        log(f"  engine start     : {engine_first} first, and it must lead before {engine_second} starts")
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
    if scenario.binary_gateway:
        gateway_a_index = next(index for index, entry in enumerate(launch_table)
                               if entry[0] == "fix_order_gateway_a")
        launch_table.insert(gateway_a_index + 1,
                            ("binary_order_gateway_a", "binary_order_gateway",
                             etc_dir / "binary_order_gateway" / "binary_order_gateway_a.toml"))

    # With --secondary-engine-first, the engine configured as secondary is started before the one configured as
    # primary. That is the state a venue is in after any failover of the matching engine, when the secondary leads,
    # and a scenario must pass from it as well as from the usual start (BUG-0195). The launch table already starts the
    # primary first, so a scenario that wants the primary to lead needs no change of order here, only the wait below.
    if engine_first == "matching_engine_secondary":
        primary_index = next(index for index, entry in enumerate(launch_table) if entry[0] == "matching_engine_primary")
        secondary_index = next(index for index, entry in enumerate(launch_table) if entry[0] == "matching_engine_secondary")
        launch_table[primary_index], launch_table[secondary_index] = launch_table[secondary_index], launch_table[primary_index]

    if scenario.with_publishers:
        for which in ("primary", "secondary"):
            publisher_config = etc_dir / "matching_engine_publisher" / f"matching_engine_publisher_{which}.toml"
            launch_table.append((f"matching_engine_publisher_{which}", "matching_engine_publisher", publisher_config))

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
    # The recovery limit, on every launch for the same reason.
    set_installed_recovery_limit(prefix, scenario.automatic_recovery_limit_seconds)
    # The promotion policy, on every launch and for the same reason: a scenario that set it
    # one way must not leave a later one asserting against a policy it did not choose.
    set_installed_promotion_policy(prefix, scenario.open_orders_on_promotion)

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

        # And the venue's own memory of what it was holding and of what it sequenced, for the same reason: a
        # scenario that began with the previous one's open orders, or its records, would be testing something
        # nobody wrote. See clear_open_order_regions and clear_write_ahead_logs.
        clear_open_order_regions(prefix)
        clear_write_ahead_logs(prefix)
        clear_lease_promise_records(prefix)

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

        # Throttle limits are provisioned for every scenario, not only the one that tests them:
        # the baseline comp id must never be limited, or a scenario's baseline burst would be
        # refused for a reason it is not about, and a value another run left behind would decide
        # which.
        log("=== Provisioning throttle limits ===")
        provision_throttle_limits(FIX8_COMP_ID, 0, 0, 0)
        provision_throttle_limits(_THROTTLED_COMP_ID, _THROTTLE_PLACE_LIMIT, _THROTTLE_AMEND_LIMIT, _THROTTLE_CANCEL_LIMIT)

        if scenario.assert_session_provisioning:
            log("=== Provisioning gateway instances for the test comp id ===")
            provision_gateway_pinning(FIX8_COMP_ID, _PROVISIONED_PRIMARY_INSTANCE,
                                      _PROVISIONED_BACKUP_INSTANCE)

        log("=== Exporting credentials ===")
        creds_file = etc_dir / "authentication_service" / "credentials.toml"
        export_credentials(project_root, creds_file)
        log("")

        if scenario.with_publishers:
            # Empty, so that everything a publisher's log holds was sent to it in this scenario, and can
            # be compared with the sequencers' logs record by record.
            for which in ("primary", "secondary"):
                publisher_wal = publisher_wal_directory(prefix, which)
                removed = [path for path in publisher_wal.glob("*") if path.is_file()]
                for path in removed:
                    path.unlink()
                log(f"  matching_engine_publisher_{which}: emptied its write-ahead log in {publisher_wal} ({len(removed)} file(s))")

        # ── Phase 1: start all processes ──────────────────────────────────────
        global _TEST_EVENTS, _REPORT_ORDERS_NOW, _INSTALL_PREFIX, _ANNOUNCEMENT_DELAY_MS_NOW  # pylint: disable=global-statement
        _INSTALL_PREFIX = prefix
        _ANNOUNCEMENT_DELAY_MS_NOW = scenario.delay_session_announcements_ms
        # Until phase 2 has seen which matching engine leads, the primary is taken to.
        _ENGINES_AT_START.update({ENGINE_LEADING_AT_START: "matching_engine_primary", ENGINE_FOLLOWING_AT_START: "matching_engine_secondary"})
        # The member's orders are reported by the engine only in the scenarios whose checks no member can make: that an
        # order was applied exactly once, acted on while its reports were held back on purpose, or cancelled (or not)
        # after the member had gone.
        member_outcomes = (scenario.assert_kept_reports_forwarded or scenario.assert_logged_commands_reach_engine
                           or scenario.assert_orders_during_change_answered or scenario.assert_engine_holds_reports_without_connection
                           or scenario.assert_open_orders_survive or scenario.assert_inflight_recovery
                           or scenario.assert_unbound_reports_delivered)
        _REPORT_ORDERS_NOW = _REPORT_ORDERS_OF + ((FIX8_COMP_ID,) if member_outcomes else ())
        # The previous scenario's listener; its components have all been stopped by now.
        if _TEST_EVENTS is not None:
            _TEST_EVENTS.close()
        _TEST_EVENTS = TestEventListener(prefix / "lib" / "python")
        log(f"  test events: listening on {_TEST_EVENTS.host}:{_TEST_EVENTS.port}")
        log("=== Phase 1: starting all processes ===")
        block_flag = prefix / "var" / "block_sends_to_ports.flag"
        block_flag.unlink(missing_ok=True)
        block_confirmation(block_flag).unlink(missing_ok=True)
        # The configuration each process was started with, which a scenario's own changes may have replaced, so that a
        # scenario can restart a process as it was started.
        launched_configs: dict[str, Path] = {}
        for name, bin_name, config in launch_table:
            log(f"  Starting {name} ...")
            extra_environment = None
            if scenario.block_leader_peer_sends and name == "sequencer_primary":
                extra_environment = blocked_peer_environment(prefix, block_flag)
            if scenario.assert_kept_reports_forwarded and name == "sequencer_primary":
                extra_environment = blocked_gateway_environment(prefix, block_flag)
            if scenario.assert_logged_commands_reach_engine and name == "sequencer_secondary":
                extra_environment = blocked_peer_environment(prefix, block_flag)
            # The engine that leads at the start is named only once a lease settles it, after the launch, so it is taken to
            # be the one started first, which must lead before the other starts, and otherwise the primary.
            leading_at_launch = engine_first or "matching_engine_primary"
            if scenario.assert_book_survives_peer_link_break and name == leading_at_launch:
                extra_environment = broken_engine_link_environment(prefix, block_flag)
            if scenario.assert_kept_reports_sent_on_promotion and name == leading_at_launch:
                extra_environment = blocked_engine_reports_environment(prefix, block_flag, name)
            if scenario.assert_reports_held_for_unconnected_gateway and name in ("sequencer_primary", "sequencer_secondary"):
                config = config_with_gateway_report_relay(config)
                log(f"  {name} sends reports to fix_order_gateway_a through a relay on port {sorted(_GATEWAY_REPORT_RELAY_PORTS)}, which accepts nothing yet")
            if scenario.assert_engines_with_different_settings and scenario.different_identifier_secret and name == "matching_engine_secondary":
                config = config_with_other_identifier_secret(config)
                log(f"  {name} holds an identifier secret of its own, not matching_engine_primary's")
            elif scenario.assert_engines_with_different_settings and name == "matching_engine_secondary":
                config = config_with_smaller_region(config)
                log(f"  {name} holds at most {_SMALLER_REGION_CAPACITY:,} open orders, fewer than matching_engine_primary")
            if scenario.assert_engine_holds_reports_without_connection and name == "matching_engine":
                config = config_with_report_relay(config)
                log(f"  {name} sends its reports through a relay on ports {sorted(_REPORT_RELAY_PORTS)}, which accepts nothing yet")
            if scenario.assert_orders_that_waited_in_the_log_reach_engine and name in ("sequencer_primary", "sequencer_secondary"):
                config = config_with_few_held_orders(config)
                log(f"  {name} holds at most {_FEW_HELD_ORDERS} orders while it waits for its follower or a voter")
            if scenario.assert_commands_resent_after_reconnection and name == "fix_order_gateway_a":
                config = config_with_small_send_queue(config)
                log(f"  {name} allows only {_SMALL_SEND_QUEUE} sends to wait on a connection, with a {_SMALL_SEND_BUFFER}-byte socket send buffer")
            if extra_environment is not None:
                log(f"  {name} has libblock_sends_to_ports.so preloaded, blocking ports "
                    f"{extra_environment['PUBSUB_TEST_BLOCK_PORTS']} once {block_flag.name} exists")
            proc = launch_app(name, bin_name, config, bin_dir, log_dir,
                              run_dir if name in scenario.supervised else None, extra_environment)
            app_procs.append((name, proc))
            proc_by_name[name] = proc
            launched_configs[name] = config
            time.sleep(STARTUP_DELAY)
            # A head start of a fixed length does not make the secondary lead: how long it takes to win its election
            # depends on the machine's load. So the primary is not started until the secondary has been seen to lead,
            # and the scenario stops if it never does, rather than going on to test the usual start under a name that
            # says otherwise.
            if engine_first and name == engine_first:
                log(f"  Waiting until {name} leads before starting {engine_second} (timeout {_ME_LEADER_TIMEOUT:.0f}s) ...")
                t0 = time.monotonic()
                if _TEST_EVENTS.wait_for(leads(_TEST_EVENTS.messages, name), timeout=_ME_LEADER_TIMEOUT) is None:
                    die(f"{start_order_source}: {name} did not lead within {_ME_LEADER_TIMEOUT:.0f}s of starting on its own")
                log(f"  {name} leads ({time.monotonic() - t0:.1f}s)")
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

        log(f"  Waiting for {seq_ready_name} to say it leads (timeout {args.ready_timeout:.0f}s) ...")
        election_started = time.monotonic()
        found = _TEST_EVENTS.wait_for(leads(_TEST_EVENTS.messages, seq_ready_name), timeout=args.ready_timeout) is not None
        elapsed = time.monotonic() - election_started
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
            found, elapsed = await_expectation(expect_engine_reachable(me_log.stem, "matching_engine"), _ME_READY_TIMEOUT, 0)
            if not found:
                die(f"matching engine not ready within {_ME_READY_TIMEOUT:.0f}s with high "
                    "availability off -- orders sent now would be deferred and lost.")
            log(f"  matching engine ready ({elapsed:.1f}s)")
        else:
            log("  Waiting for arbiter_primary to become the active arbiter ...")
            found, elapsed = await_expectation(expect_leads("arbiter_primary"), 10.0, 0)
            if found:
                log(f"  arbiter_primary: active ({elapsed:.1f}s)")
            else:
                log("  arbiter_primary: not active within 10s -- continuing, as the scenario may expect that")
        log("")

        # For the ME-HA topology, wait until the primary→secondary book
        # replication connection is established before proceeding.  The secondary
        # detects the primary's death only as the replication TCP EOF; killing the
        # primary before that connection is up (the primary retries every 2 s
        # against the secondary's listener) would leave the secondary unaware and
        # no promotion would ever fire.
        if scenario.me_ha:
            log("  Waiting for the primary engine's replication connection to the secondary ...")
            found, elapsed = await_expectation(expect_replication_connection("matching_engine_secondary"), args.ready_timeout, 0)
            if not found:
                die(
                    "ME-primary → secondary replication connection not established "
                    f"within {args.ready_timeout:.0f}s — cannot test ME failover"
                )
            log(f"  ME replication connection established ({elapsed:.1f}s)")

            # Neither engine asks to lead during its first lease period, so for some seconds after
            # starting the pair has no leader at all. A scenario that killed the primary in that
            # time would be testing a pair in which nothing had yet been decided, and a restarted
            # primary would have no lead to keep. So wait until one of the two engines leads.
            log("  Waiting until one of the two matching engines leads ...")
            deadline = time.monotonic() + _ME_LEADER_TIMEOUT
            t0 = time.monotonic()
            leader_name = None
            while leader_name is None and time.monotonic() < deadline:
                _TEST_EVENTS.check()
                for name in ("matching_engine_primary", "matching_engine_secondary"):
                    if _TEST_EVENTS.find(leads(_TEST_EVENTS.messages, name)) is not None:
                        leader_name = name
                        break
                else:
                    _TEST_EVENTS.pump(0.05)
            if leader_name is not None:
                log(f"  {leader_name} leads ({time.monotonic() - t0:.1f}s)")
                # The scenario's steps name the engines by the role each has now, not by configuration.
                follower_name = "matching_engine_secondary" if leader_name == "matching_engine_primary" else "matching_engine_primary"
                _ENGINES_AT_START.update({ENGINE_LEADING_AT_START: leader_name, ENGINE_FOLLOWING_AT_START: follower_name})
                log(f"  the steps' {ENGINE_LEADING_AT_START} is {leader_name}, and {ENGINE_FOLLOWING_AT_START} is {follower_name}")
            else:
                log(f"  neither matching engine leads within {_ME_LEADER_TIMEOUT:.0f}s -- continuing, as the scenario may expect that")
            if engine_first and leader_name != engine_first:
                die(f"{start_order_source}: {engine_first} was seen to lead before {engine_second} started, "
                    f"but once both were running the engine that leads is {leader_name}")
            # In a normal start the wait above lasts about a lease period, which is also how long a starting engine
            # waits before it votes, so both engines have settled their roles before the steps begin. With the
            # secondary started first, the wait ends as soon as the primary is running, while the primary is still in
            # that first period: it has promised nothing and has not said it follows. A scenario begun then would be
            # testing a pair whose follower has not finished starting, which no normal start produces. The same holds
            # whichever engine is started first.
            if engine_first:
                log(f"  Waiting until {engine_second} follows (timeout {_ME_LEADER_TIMEOUT:.0f}s) ...")
                t0 = time.monotonic()
                if _TEST_EVENTS.wait_for(follows(_TEST_EVENTS.messages, engine_second), timeout=_ME_LEADER_TIMEOUT) is None:
                    die(f"{start_order_source}: {engine_second} did not follow within {_ME_LEADER_TIMEOUT:.0f}s")
                log(f"  {engine_second} follows ({time.monotonic() - t0:.1f}s)")
            log("")
        elif not scenario.ha_disabled:
            # The same for a single matching engine with high availability on: it asks to lead
            # only after its first lease period, and until it leads it discards orders, which it
            # then applies from the sequencer's log without writing the acceptance lines this
            # script counts. Orders are meant to meet an engine that is already leading.
            log("  Waiting until the matching engine leads ...")
            found, elapsed = await_expectation(expect_leads(me_log.stem), _ME_LEADER_TIMEOUT, 0)
            if not found:
                die(f"the matching engine did not lead within {_ME_LEADER_TIMEOUT:.0f}s")
            log(f"  matching engine leads ({elapsed:.1f}s)")
            log("")

        # ── Phase 3: baseline orders + in-flight orders ───────────────────────
        if scenario.skip_baseline_orders:
            log("=== Phase 3: skipped — this scenario's subject is a venue that cannot trade ===")
            log("")
        before_total = 0 if scenario.skip_baseline_orders else orders_before * 1000
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
            f8proc = send_burst(orders_before, gw_log, baseline_config, baseline_output)
            log(f"  Waiting for the client to receive a New report for each of the {before_total} baseline orders ...")
            found, elapsed = await_new_orders(f8proc, before_total, 0, timeout=120.0)
            if not found:
                die("baseline orders did not complete — system is not healthy")
            log(f"  {before_total} baseline orders confirmed ({elapsed:.1f}s)")

        # Send extra T commands WITHOUT waiting, just before Phase 4, so the venue
        # is under load as the kill approaches. At the venue's current speed they are
        # all processed before the kill, so they are not orders sent during the change
        # of leader; scenario 68 tests those. Suppressed for extra_steps scenarios
        # (e.g. WAL recovery) where precise control of order counts between steps is
        # required.
        if orders_during > 0 and not scenario.extra_steps and f8proc is not None:
            log(
                f"  Sending {orders_during * 1000} orders just before Phase 4, without waiting ..."
            )
            send_orders(f8proc, b"T", orders_during)
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
            reconnect_from = _TEST_EVENTS.mark()
            stop_f8test(f8proc)
            f8proc = None
            # send_burst waits for the logon and dies on failure, so a fresh session that
            # cannot be established fails here rather than as a confusing shortfall later.
            f8proc = send_burst(0, gw_log)

            # The gateway must have told the sequencer, or the rest of this scenario tests
            # the old behaviour and would pass by accident.
            found, elapsed = await_expectation(expect_message("fix_order_gateway_a", "the new session announced", "SessionAnnounced", "fix_order_gateway_test_events", fields=lambda m: {"bound": True}), _PROVISIONING_LOGON_TIMEOUT, reconnect_from)
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
            send_orders(f8proc, b"T", _INFLIGHT_BURSTS)

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
                started = client_reports(f8proc).new_orders_since(0)
                if started >= target:
                    break
                time.sleep(POLL_INTERVAL)
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
        # sequencer_events_pre_kill is used after Phase 4 to wait for
        # sequencer_primary to re-establish its ME connection (see below).
        sequencer_events_pre_kill = _TEST_EVENTS.mark()

        # Where the secondary sequencer's log ends before anything is killed, so that the records
        # it numbers after taking the lead can be told from those an earlier run left behind.
        secondary_wal_dir = None
        secondary_wal_start = (-1, -1)
        if scenario.assert_new_leader_numbers_forward:
            secondary_config = prefix / "etc" / "sequencer" / "sequencer_secondary.toml"
            secondary_wal_dir = secondary_config.parent / installed_toml_section_value(secondary_config, "wal", "directory")
            secondary_wal_start = wal_end_position(secondary_wal_dir)

        # For the ME-HA cancel-on-failover assertion after Phase 5: remember where
        # the secondary ME log and the gateway log end before the kill, so we scan
        # only what the promotion (and its cancel-ER burst) produces.
        me_secondary_events_from = _TEST_EVENTS.mark()
        gateway_events_pre_kill = _TEST_EVENTS.mark()

        log(f"=== Phase 4: {scenario.description} ===")
        for step in effective_steps:
            step = with_engines_resolved(step)
            if isinstance(step, KillStep):
                outcome, label, elapsed = do_kill_step(step, proc_by_name, args.failover_timeout)
                kill_results.append((outcome, label, elapsed))
                phase4_results.append(("kill", outcome, label, elapsed))
            elif isinstance(step, IsolateStep):
                outcome, label, elapsed = do_isolate_step(step, proc_by_name)
                kill_results.append((outcome, label, elapsed))
                phase4_results.append(("kill", outcome, label, elapsed))
            elif isinstance(step, MachineKillStep):
                outcome, label, elapsed = do_machine_kill_step(step, proc_by_name)
                kill_results.append((outcome, label, elapsed))
                phase4_results.append(("kill", outcome, label, elapsed))
            elif isinstance(step, RestartStep):
                elapsed = do_restart_step(
                    step, proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix),
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
            elif isinstance(step, OneLeaderStep):
                log(f"  VERIFY: {step.description}")
                # Either instance leading will do: the first condition met ends the wait.
                either = Expect(" or ".join(f"{name} leads" for name in step.components),
                                tuple(expect_leads(name).conditions[0] for name in step.components))
                deadline = time.monotonic() + step.timeout
                t0 = time.monotonic()
                leaders = []
                while not leaders and time.monotonic() < deadline:
                    _TEST_EVENTS.check()
                    leaders = [resolved(c.component) for c in either.conditions if _condition_met(c, _TEST_EVENTS.since_introduction(resolved(c.component)))]
                    if not leaders:
                        _TEST_EVENTS.pump(0.05)
                if not leaders:
                    die(f"Verification failed: {step.description} -- not seen within {step.timeout:.0f}s: {either.what}")
                log(f"    {leaders[0]} leads ({time.monotonic() - t0:.1f}s); waiting {step.settle_secs:.0f}s for any second leader ...")
                time.sleep(step.settle_secs)
                leaders = [resolved(name) for name in step.components
                           if expectation_seen(expect_leads(name), _TEST_EVENTS.since_introduction(resolved(name)))]
                if len(leaders) != 1:
                    die(f"Verification failed: {step.description} -- {len(leaders)} of {[resolved(name) for name in step.components]} lead: {leaders}")
                log("    confirmed: exactly one leads, up to a checkpoint of each")
                phase4_results.append(("verify", step.description, step.settle_secs))
            elif isinstance(step, AssertAbsentStep):
                log(f"  VERIFY: {step.description}")
                if step.absent is None:
                    die(f"Verification step '{step.description}' gives no test events that must not arrive")
                log(f"    waiting {step.after_secs:.0f}s, then requiring that this has NOT happened: {step.absent.what} ...")
                time.sleep(step.after_secs)
                if expectation_seen(step.absent, since_present_processes(step.absent)):
                    die(f"Verification failed: {step.description} -- {step.absent.what}")
                log("    confirmed absent, up to a checkpoint")
                phase4_results.append(("verify", step.description, step.after_secs))
            elif isinstance(step, InterimOrdersStep):
                count = step.count_batches * 1000
                log(f"  Sending {count} interim orders ...")
                interim_sent_before = send_orders(f8proc, b"T", step.count_batches)
                log(f"  Waiting for the client to receive a New report for each of the {count} interim orders ...")
                found, elapsed = await_new_orders(f8proc, count, interim_sent_before, timeout=args.recovery_timeout)
                if not found:
                    die(
                        f"interim orders did not appear within "
                        f"{args.recovery_timeout:.0f}s"
                    )
                log(f"  {count} interim orders confirmed ({elapsed:.1f}s)")
                phase4_results.append(("interim", count))
            elif isinstance(step, VerifyStep):
                log(f"  VERIFY: {step.description}")
                if step.expect is None:
                    die(f"Verification step '{step.description}' gives no test events to wait for")
                log(f"    waiting for: {step.expect.what} (timeout {step.timeout:.0f}s) ...")
                found, elapsed = await_expectation(step.expect, step.timeout, since_present_processes(step.expect))
                if not found:
                    die(f"Verification failed: {step.description} -- not seen within {step.timeout:.0f}s: {step.expect.what}")
                log(f"    confirmed ({elapsed:.1f}s)")

                if step.absent is not None:
                    log(f"    checking that this has NOT happened: {step.absent.what} ...")
                    if expectation_seen(step.absent, since_present_processes(step.absent)):
                        die(f"Verification failed: {step.description} — {step.absent.what}, which must not have happened")
                    log("    confirmed absent, up to a checkpoint")
                phase4_results.append(("verify", step.description, elapsed))

        log_session_announcement_margins()

        # If ME was restarted, its readiness is met as soon as the first
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
            for restart_step in (s for s in effective_steps if isinstance(s, RestartStep) and s.resets_me_counter):
                restarted_engine = resolved(restart_step.proc_name)
                log(f"  Waiting for sequencer_primary to re-establish its connection to {restarted_engine} ...")
                found, elapsed = await_expectation(expect_connection_made("sequencer_primary", engine_service(restarted_engine)), 10.0,
                                                   sequencer_events_pre_kill)
                if not found:
                    die(f"sequencer_primary did not re-establish its connection to {restarted_engine} within 10s after its restart")
                log(f"  sequencer_primary: connection to {restarted_engine} restored ({elapsed:.1f}s)")

            # Connected is not leading. A restarted engine asks to lead only after it has connected,
            # and if its first request is not granted -- scenario 11 kills the primary arbiter as well,
            # so a lease takes a second request -- it leads half a second or more later. Orders sent in
            # that half second are not lost: the engine takes them from the log as it catches up. But it
            # does not log a catch-up record as an accepted order, so the count below would never reach
            # its target. Waiting for the engine to lead makes the orders arrive one at a time, as the
            # count expects.
            for restart_step in (s for s in effective_steps if isinstance(s, RestartStep) and s.resets_me_counter):
                restarted = expect_leads(restart_step.proc_name)
                found, elapsed = await_expectation(restarted, 15.0, since_present_processes(restarted))
                if not found:
                    die(f"the restarted {resolved(restart_step.proc_name)} did not take the lead within 15s")
                log(f"  {resolved(restart_step.proc_name)}: leading ({elapsed:.1f}s)")
        log("")

        # ── Phase 5: recovery orders ──────────────────────────────────────────
        # Recovery orders are confirmed at the client, by New reports. That holds however a change of leader or a restart
        # has carried the matching engine's order numbering, and whichever engine is matching.
        me_restarted = any(
            isinstance(s, RestartStep) and s.resets_me_counter
            for s in effective_steps
        )
        after_total  = orders_after * 1000

        # A scenario that kills the gateway its FIX session runs on has no session
        # left to send through, so there are no recovery orders to send or wait for.
        # Skipping is not a weaker test here: what that scenario asserts happens in
        # Phase 4 and is checked below.
        if after_total == 0:
            log("=== Phase 5: skipped — no recovery orders for this scenario ===")
            log("")
        else:
            log(f"=== Phase 5: {after_total} recovery orders, confirmed by New reports at the client ===")

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
            # Counted, not named: orders sent before these may still be on their way when these are sent, and the client
            # sends in order, so these can wait behind thousands of them. What is confirmed is that after_total further
            # orders were accepted and reported to the member from here on, which is what the matching engine's own count
            # confirmed before; it does not confirm these orders themselves when others were in flight (BUG-0181).
            recovery_from = client_reports(f8proc).mark()
            send_orders(f8proc, b"T", orders_after)

            log(f"  Waiting for the client to receive New reports for {after_total} further orders ...")
            found, elapsed = await_new_orders_since(f8proc, after_total, recovery_from, timeout=args.recovery_timeout)
            if not found:
                die(
                    f"recovery orders did not appear within {args.recovery_timeout:.0f}s"
                )
            log(f"  {after_total} recovery orders confirmed ({elapsed:.1f}s)")

        # ── ME-HA: the book a promotion resumes with ──────────────────────────
        # The matching engine does no matching: every accepted NewOrderSingle is added to
        # the book as OrdStatus=New and rests there until cancelled (see
        # MatchingEngineThread::handle_new_order_single). So at the ME-primary kill the
        # promoted secondary's replicated book is non-empty.
        #
        # What happens to that book is the venue's stated policy,
        # order_book.open_orders_on_promotion, so this asserts whichever answer the scenario
        # is configured for rather than assuming one. Both answers matter and they fail in
        # opposite directions: a venue set to cancel that quietly keeps has told a member its
        # order is gone and kept it; a venue set to keep that quietly cancels has thrown away
        # a book it could have carried.
        #
        # Either way the engine must be able to SAY what it holds -- it writes every
        # replicated order into a memory-mapped region of its own and reconciles the
        # sequencer's tail on top -- so the resumed book size is checked under both.
        #
        # Skipped when leadership never moved: there is no promotion and no book to decide
        # about.
        # Not for a scenario that takes every engine away and starts one cold, nor for one in
        # which nobody is expected to take the lead: there is no promotion in either, so there
        # is no book a promotion resumed with to ask about.
        if (scenario.me_ha and not scenario.assert_gateway_orphaned and not scenario.recovery_on_primary
                and not scenario.assert_deferred_answered and not scenario.expects_no_new_leader
                and not scenario.assert_book_survives_peer_link_break and not scenario.assert_kept_reports_sent_on_promotion):
            cancels_expected = scenario.open_orders_on_promotion == "cancel"
            # Under "keep" the promotion has already been observed by the time this runs, so it is confirming an absence:
            # the engine is asked for a checkpoint first, and a cancel it sent cannot still be on its way.
            promoted_engine = resolved(ENGINE_FOLLOWING_AT_START)
            cancelled_book = engine_event(promoted_engine, "BookCancelled", me_secondary_events_from,
                                          args.failover_timeout if cancels_expected else 0.0, settle=not cancels_expected)
            cancel_count = cancelled_book.cancel_reports_sent if cancelled_book is not None else -1
            resumed_event = engine_event(promoted_engine, "ResumedAsLeader", me_secondary_events_from, 10.0)
            if resumed_event is None:
                die(f"ME-HA: the promoted {promoted_engine} never reported resuming as leader -- "
                    "cannot confirm what book it resumed with")
            resumed = resumed_event.book_size

            if cancels_expected:
                # What it inherited has to come from the cancel's own count: the engine says what it is resuming with only
                # once the CancelBook command it asked for has been applied. The command cancels the orders placed at or
                # before the position the engine caught up to, and keeps any placed after it, so what it inherited is
                # what the book held less what was kept.
                if cancelled_book is not None and not cancelled_book.by_command:
                    die(f"ME-HA: the promoted {promoted_engine} cancelled the book by its own decision, not by a CancelBook "
                        "command in the WAL, so an engine that catches up later would not cancel it (BUG-0199).")
                inherited = cancelled_book.live_orders - cancelled_book.orders_kept if cancelled_book is not None else -1
                if inherited <= 0:
                    die(f"ME-HA: the venue is configured to cancel on promotion but the promoted "
                        f"secondary reported inheriting {inherited} order(s). The baseline orders rest "
                        "in the book and replicate, so it should have had them to cancel: nothing here "
                        "distinguishes a book correctly cancelled from a book never recovered.")
                # The count is the discriminator. Cancelling SOME of the book would leave the
                # rest unmentioned, which is the silence the policy exists to avoid, and it
                # would pass any check that only asked whether cancelling happened at all.
                if cancel_count != inherited:
                    die(f"ME-HA: the promoted secondary inherited {inherited} order(s) and sent "
                        f"{cancel_count} cancel report(s). Every order it inherited must be cancelled "
                        "and its member told: cancelling part of the book leaves the rest unmentioned, "
                        "which is worse than either answer taken whole (R-0020).")
                if resumed != cancelled_book.orders_kept:
                    die(f"ME-HA: the promoted secondary cancelled {cancel_count} order(s), kept {cancelled_book.orders_kept} "
                        f"placed after the position it caught up to, and then resumed as leader holding {resumed}. The book "
                        "it resumes with must be what the command kept, or it is serving orders it has told the member are gone.")
                log(f"  ME-HA: open_orders_on_promotion=cancel -- the promoted secondary inherited "
                    f"{inherited} order(s), cancelled all {cancel_count} of them by a CancelBook command, and resumed "
                    f"holding the {resumed} placed after it caught up -- OK")
            else:
                if cancel_count > 0:
                    die(f"ME-HA: the venue is configured to keep the book on promotion, and the "
                        f"promoted secondary cancelled {cancel_count} order(s). Its region and the "
                        "sequencer's tail say what it holds, so an order still on the reconciled "
                        "book is genuinely outstanding (R-0073).")
                if resumed <= 0:
                    die(f"ME-HA: the promoted secondary resumed as leader holding {resumed} order(s). "
                        "The baseline orders rest in the book and replicate, so it should hold them: "
                        "an empty book here means the region was not recovered or was recovered empty.")
                log(f"  ME-HA: open_orders_on_promotion=keep -- the promoted secondary resumed as "
                    f"leader holding {resumed} order(s), none cancelled -- OK")

            dropped = count_seen(expect_message("fix_order_gateway_a", "a report dropped for want of a session", "ReportDroppedNoSession",
                                                "fix_order_gateway_test_events"), gateway_events_pre_kill)
            if dropped > 0:
                die(f"ME-HA: gateway dropped {dropped} ExecutionReport(s) for a missing "
                    "gateway_session_conn_id -- ER routing via the envelope is broken")

        # ── Held reports: a restarted sequencer leads before it is told of the session ──
        # BUG-0190. The restarted primary takes the lead while the gateways are still waiting, as this scenario asks them
        # to, before announcing their sessions to it, so it does not know the member's session when the promoted engine
        # cancels the book. It must hold the cancels and send them once the gateway announces the session, and the
        # member must be told of every order the engine cancelled. Without holding, the cancels are dropped. See
        # docs/availability/reports_held_until_sessions_are_announced.md.
        if scenario.assert_held_reports_reach_member:
            sequencer_events = "sequencer_test_events"
            primary_from = _TEST_EVENTS.since_introduction("sequencer_primary")
            held = component_event("sequencer_primary", sequencer_events, "ReportsHeld", primary_from, args.failover_timeout, comp_id=FIX8_COMP_ID)
            if held is None:
                die("held reports: the restarted sequencer_primary held no report for the member's session. Either it was told of the "
                    "session before the engine cancelled the book, so the scenario did not make the window it exists for, or it "
                    "dropped the cancels")
            log(f"  held reports: sequencer_primary held reports for comp_id='{held.comp_id}' while {held.gateways_not_announced} "
                "gateway instance(s) had not announced")
            delay_seconds = scenario.delay_session_announcements_ms / 1000.0
            released = component_event("sequencer_primary", sequencer_events, "HeldReportsReleased", primary_from, delay_seconds + args.failover_timeout,
                                       comp_id=FIX8_COMP_ID)
            if released is None or released.sent <= 0:
                die(f"held reports: sequencer_primary did not send the reports it held for the member's session ({released})")
            log(f"  held reports: sequencer_primary sent {released.sent} held report(s) when the session was announced")
            cancelled_book = engine_event(resolved(ENGINE_FOLLOWING_AT_START), "BookCancelled", me_secondary_events_from, args.failover_timeout)
            if cancelled_book is None or cancelled_book.cancel_reports_sent <= 0:
                die(f"held reports: the promoted matching engine cancelled no order ({cancelled_book}), so there was nothing to hold")
            # Each cancel the engine sent must reach the member: counted at the member, so a report lost anywhere is missed.
            expected = cancelled_book.cancel_reports_sent
            deadline = time.monotonic() + args.recovery_timeout
            received = client_reports(f8proc).count("4")
            while received < expected and time.monotonic() < deadline:
                time.sleep(0.2)
                received = client_reports(f8proc).count("4")
            if received != expected:
                die(f"held reports: the promoted engine cancelled {expected} order(s) and the member received {received} cancel(s)")
            dropped = count_seen(expect_message("sequencer_primary", "a report dropped for a session connected nowhere", "ReportDroppedSessionNotConnected",
                                                sequencer_events, fields=lambda m: {"comp_id": FIX8_COMP_ID}), primary_from)
            if dropped > 0:
                die(f"held reports: sequencer_primary dropped {dropped} report(s) for the member's session as connected nowhere")
            log(f"  held reports: the member received all {received} cancel(s) -- OK")

        # ── Gateway orphan: no session handover between instances ─────────────
        # Asserts a GAP rather than a guarantee. Instance a is dead and the engine has just
        # come back from an absence longer than the venue says it may be, cancelling every
        # order it recovered -- so there is a burst of execution reports addressed to a
        # gateway instance that no longer exists.
        #
        # Three things must hold for the test to mean anything:
        #   1. the cancel burst actually happened, or the rest proves nothing;
        #   2. the sequencer held those reports for the dead instance and dropped none --
        #      it does NOT reroute them to the surviving instance of the same protocol;
        #   3. instance b saw none of it, i.e. it inherited nothing from a.
        #
        # Assertion 3 is the load-bearing one. Were b ever to pick up a's sessions,
        # the third check fails first and loudest.
        #
        # WHEN SESSION HANDOVER LANDS (steps 3b-6 of docs/availability/gateway_ha.md) THIS
        # BLOCK SHOULD FAIL. Invert it rather than deleting it: the sequencer holds nothing
        # for instance 1, and b's traffic becomes non-zero.
        if scenario.assert_gateway_orphaned:
            # Only the restarted engine's events count: those since its newest introduction.
            cancelled_book = engine_event(me_log.stem, "BookCancelled", _TEST_EVENTS.since_introduction(me_log.stem), args.failover_timeout)
            cancel_count = cancelled_book.cancel_reports_sent if cancelled_book is not None else -1
            if cancel_count <= 0:
                die("gateway orphan: the restarted engine did not cancel the orders it "
                    f"recovered (count={cancel_count}) -- no execution reports were generated "
                    "for the dead gateway, so this scenario asserted nothing. It was down for "
                    "longer than the venue says it may be unable to match, with orders open, so "
                    "every one of them should have been cancelled and reported (R-0117).")
            log(f"  gateway orphan: {cancel_count} resting order(s) cancelled after the long absence")

            # Wait for the burst to reach the sequencer rather than sampling straight
            # away. Every other me_ha scenario gets this delay for free from Phase 5's
            # recovery orders; this one skips Phase 5, and without the wait it sampled
            # about two seconds after promotion and saw nothing.
            held_expect = expect_message("sequencer_primary", "reports held for the dead FIX instance 1", "ReportsHeldForGateway", "sequencer_test_events",
                                         fields=lambda m: {"gateway_protocol": 1, "gateway_instance": 1})
            log("  gateway orphan: waiting for the cancel execution reports to reach the sequencer ...")
            found, elapsed = await_expectation(held_expect, args.failover_timeout, sequencer_events_pre_kill)
            if not found:
                die("gateway orphan: the sequencer never said it was holding execution reports "
                    "for the dead FIX instance 1. Either the reports went somewhere they should "
                    "not have, or session handover now exists -- if the latter, invert this "
                    "scenario's assertions rather than removing them. See docs/availability/gateway_ha.md.")
            drop_expect = expect_message("sequencer_primary", "a report for the dead FIX instance 1 dropped", "ReportDroppedGatewayNotConnected", "sequencer_test_events",
                                         fields=lambda m: {"gateway_protocol": 1, "gateway_instance": 1})
            dropped = count_seen(drop_expect, sequencer_events_pre_kill)
            if dropped > 0:
                die(f"gateway orphan: the sequencer dropped {dropped} execution report(s) for the dead FIX instance 1 rather than holding them (BUG-0202)")
            log(f"  gateway orphan: the sequencer is holding the execution reports for the dead instance 1, first after {elapsed:.1f}s, "
                "and dropped none -- not rerouted to b")

            # The load-bearing assertion: b was alive throughout, and the question is only whether
            # anything reached it. A gateway reports its progress every five seconds whether or not it
            # is serving anyone, so the reports' figures say whether it handled traffic, not their
            # number.
            gateway_b = "fix_order_gateway_b"
            if not _TEST_EVENTS.settle(gateway_b):
                die(f"gateway orphan: {gateway_b} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so what it reported is not known")
            progress = message_of(_TEST_EVENTS.modules["fix_order_gateway_test_events"].ProgressReported, gateway_b)
            b_reports = [event.message for event in _TEST_EVENTS.events[_TEST_EVENTS.since_introduction(gateway_b):] if progress(event)]
            if not b_reports:
                die(f"gateway orphan: {gateway_b} reported no progress at all, so whether anything reached it is not known.")
            b_traffic = [figures for figures in b_reports if figures.accounted > 0 or figures.orders_received > 0]
            if b_traffic:
                die(f"gateway orphan: instance b reported traffic it should never have seen -- "
                    f"{b_traffic[-1]}. It was sent no orders of its own, so either instance a's "
                    "reports were rerouted to it -- which nothing implements -- or session handover "
                    "now exists and this scenario's assertions need inverting, not removing.")
            log(f"  gateway orphan: instance b reported only idle progress ({len(b_reports)} reports, all zeros) -- it inherited "
                "nothing from a -- OK")

            # The member is not told in this scenario: the orders were cancelled in the book, and
            # the reports saying so are held by the sequencer for an instance that never returns.
            log(f"  gateway orphan: {cancel_count} order(s) cancelled in the book, "
                "0 cancel reports delivered to any client while instance a stays down")

        # ── A record of what it held that cannot be read ──────────────────────
        # R-0102 and R-0123. Three things must hold, and the third is the one that matters
        # most, because it is the one an implementation is most likely to get wrong in the
        # helpful direction.
        if scenario.assert_damaged_region:
            region_from = _TEST_EVENTS.since_introduction(me_log.stem)  # the restarted engine's events

            refused = engine_event(me_log.stem, "RegionUnusable", region_from, args.failover_timeout) is not None
            if not refused:
                die("damaged region: the engine did not refuse the damaged region. Reading it "
                    "the wrong way round produces orders that are wrong in ways nothing "
                    "downstream can detect, which is why it is refused outright (R-0102).")
            log("  damaged region: refused rather than read")

            # The region is the only account of whatever went wrong with it. A venue that
            # overwrites it leaves nobody able to find out.
            kept = venue_state_dir(prefix) / "matching_engine_open_orders.region.unusable"
            if not kept.is_file():
                die(f"damaged region: {kept.name} was not kept. Whatever made the region "
                    "unreadable is the evidence, and starting a fresh region over the top of it "
                    "destroys the only account of the failure.")
            log(f"  damaged region: kept aside as {kept.name}")

            # The WAL reaches the first command of the day, so the engine rebuilt from it exactly the book the venue had,
            # and carries on with every order on it, cancelling nothing (R-0123). That the member's orders are all still
            # there is checked below, by cancelling each. The other case of R-0123, a WAL that does not reach the first
            # command, cannot arise while the WAL keeps every record of the day (BUG-0209).
            rebuilt = engine_event(me_log.stem, "RegionRebuilt", region_from, args.failover_timeout)
            if rebuilt is None:
                die("damaged region: the engine did not say it rebuilt its book from the WAL. The WAL reaches the first "
                    "command of the day, so the book it holds after catching up from the first record is the venue's (R-0123).")
            log(f"  damaged region: the book was rebuilt from the WAL through seq_no={rebuilt.through_seq_no}, "
                f"holding {rebuilt.book_size} order(s)")
            halt = engine_event(me_log.stem, "Halted", region_from, 0.0, settle=True)
            if halt is not None:
                die("damaged region: the engine halted although it rebuilt the venue's book. Halting costs every member "
                    "the market and protects nothing, because the book is exactly the venue's (R-0123).")
            log("  damaged region: trading carries on")

            # Nothing is cancelled: the rebuilt book is the venue's.
            cancelled_book = engine_event(me_log.stem, "BookCancelled", region_from, 0.0, settle=True)
            cancelled = cancelled_book.cancel_reports_sent if cancelled_book is not None else -1
            if cancelled > 0:
                die(f"damaged region: the engine cancelled {cancelled} order(s), although the book it rebuilt is the venue's "
                    "(R-0123).")
            log("  damaged region: nothing cancelled -- OK")

        # ── A long absence: cancelled, reported, halted ───────────────────────
        # The engine was kept down past the period the venue says it may be absent for, with
        # orders resting. R-0117 requires all three things, and each is checked separately
        # because any one of them alone would be a worse outcome than doing nothing:
        # cancelling without reporting loses the member's position silently, reporting without
        # halting carries on as though nothing happened, and halting without cancelling leaves
        # the stale orders in place.
        # ── The longest recovery the venue does by itself ────────────────────
        if scenario.assert_recovery_halt:
            gateway_events = _TEST_EVENTS.modules["fix_order_gateway_test_events"]
            halted_said = _TEST_EVENTS.find(message_of(gateway_events.OrderAcceptanceChanged, "fix_order_gateway_a", accepting=False,
                                                       halted_for_recovery=True), since=0)
            if halted_said is None:
                die("recovery limit: the gateway was never told the venue is halted while it recovers, so it went on telling members "
                    "only that no matching engine is available, or nothing at all")
            log("  recovery limit: the gateway was told the venue is halted, and refused orders saying so meanwhile -- OK")

            restarted_from = _TEST_EVENTS.since_introduction(me_log.stem)
            engine_events = _TEST_EVENTS.modules["matching_engine_test_events"]
            halt = engine_event(me_log.stem, "Halted", restarted_from, args.failover_timeout)
            if halt is None or halt.reason != engine_events.HaltReason.RecoveryTooLong:
                die(f"recovery limit: the restarted engine did not halt for the recovery limit ({halt}). The halt is a command in "
                    "the WAL, so an engine that catches up meets it where it was written")
            cancelled_book = engine_event(me_log.stem, "BookCancelled", restarted_from, 0.0, settle=True)
            if cancelled_book is not None:
                die(f"recovery limit: the engine cancelled its book ({cancelled_book}). The recovery limit halts and cancels nothing; "
                    "only the absence limit cancels")
            log("  recovery limit: the restarted engine halted in its catch-up and cancelled nothing -- OK")

            log("=== Sending an order into the halt, which must be refused ===")
            refused_from = client_reports(f8proc).mark()
            halted_sent_before = send_orders(f8proc, b"T", 1)
            deadline = time.monotonic() + args.recovery_timeout
            refused = 0
            while time.monotonic() < deadline and not refused:
                refused = client_reports(f8proc).count("8", refused_from, ord_rej_reason="2")
                time.sleep(POLL_INTERVAL)
            if client_reports(f8proc).new_among(halted_sent_before, _ORDERS_PER_COMMAND[b"T"]):
                die("recovery limit: the member was sent a New report for an order sent while halted (R-0023)")
            if not refused:
                die("recovery limit: the venue accepted an order while halted, so it resumed trading by itself (R-0023)")
            log("  recovery limit: an order sent into the halt was refused -- OK")

        if scenario.assert_no_recovery_halt:
            gateway_events = _TEST_EVENTS.modules["fix_order_gateway_test_events"]
            if not _TEST_EVENTS.settle("fix_order_gateway_a"):
                die(f"no recovery halt: fix_order_gateway_a did not answer a checkpoint ({_TEST_EVENTS.failures[-1]})")
            halted_said = _TEST_EVENTS.find(message_of(gateway_events.OrderAcceptanceChanged, "fix_order_gateway_a", accepting=False,
                                                       halted_for_recovery=True), since=0)
            if halted_said is not None:
                die("no recovery halt: the venue halted for the recovery limit during a failover, which finishes well within it")
            log("  no recovery halt: the failover finished within the recovery limit, and nothing halted -- OK")

        if scenario.assert_absence_halt:
            restarted_from = _TEST_EVENTS.since_introduction(me_log.stem)  # the restarted engine's events

            cancelled_book = engine_event(me_log.stem, "BookCancelled", restarted_from, args.failover_timeout)
            cancelled = cancelled_book.cancel_reports_sent if cancelled_book is not None else -1
            if cancelled <= 0:
                die("long absence: the engine did not cancel the orders it recovered "
                    f"(count={cancelled}). It was down for longer than the venue says it may be "
                    "with orders open, so every one of them should have been cancelled -- their "
                    "members were locked out of them while the market moved (R-0117).")
            if not cancelled_book.by_command:
                die("long absence: the engine cancelled its book by a decision of its own, not by the command the leading "
                    "sequencer writes into the WAL. An engine that decides alone decides differently from its peer and from "
                    "an engine that catches up later (docs/availability/deterministic_matching_engine.md, section 4.2).")
            log(f"  long absence: {cancelled} order(s) cancelled, by the command in the WAL")

            halt = engine_event(me_log.stem, "Halted", restarted_from, args.failover_timeout)
            engine_events = _TEST_EVENTS.modules["matching_engine_test_events"]
            halted = halt is not None and halt.reason == engine_events.HaltReason.UnableToMatchTooLong
            if not halted:
                die("long absence: the engine cancelled the book but did not halt. Carrying on "
                    "as though nothing had happened is what the halt exists to prevent (R-0117); "
                    "lifting it is a judgement and needs a person (R-0023).")
            log("  long absence: trading halted")

            # The reports have to reach the members. A cancel nobody is told about is the
            # silent loss this whole chapter exists to prevent, and it looks identical from
            # here to a cancel that worked.
            # Counted at the member, which is where the reports have to arrive.
            member_reports = client_reports(f8proc)
            deadline = time.monotonic() + args.recovery_timeout
            while member_reports.count("4") < cancelled and time.monotonic() < deadline:
                time.sleep(POLL_INTERVAL)
            told = member_reports.count("4")
            if told < cancelled:
                die(f"long absence: the member was sent {told} cancellation report(s), but {cancelled} order(s) were cancelled. "
                    "A member that is not told its order was cancelled cannot tell that from the venue having lost it "
                    "(R-0117, R-0020).")
            log(f"  long absence: the member was sent a cancellation report for each of the {cancelled} order(s) -- OK")

            # And the halt has to mean something. An order sent now must be refused, in the
            # reply to that order, which is how a member trading into a halt finds out.
            log("=== Sending an order into the halt, which must be refused ===")
            # Seen by the member: a Rejected report (ExecType 8) whose reason is that the exchange is closed (OrdRejReason 2).
            refused_from = client_reports(f8proc).mark()
            halted_sent_before = send_orders(f8proc, b"T", 1)
            deadline = time.monotonic() + args.recovery_timeout
            refused = 0
            while time.monotonic() < deadline and not refused:
                refused = client_reports(f8proc).count("8", refused_from, ord_rej_reason="2")
                time.sleep(POLL_INTERVAL)
            if client_reports(f8proc).new_among(halted_sent_before, _ORDERS_PER_COMMAND[b"T"]):
                die("long absence: the member was sent a New report for an order sent while halted. A halt that still "
                    "takes orders is not a halt (R-0117).")
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
        # ── A second sequencer during a catch-up ──────────────────────────────
        # BUG-0200. The restarted primary begins catching up from the start of the WAL when the first sequencer connects,
        # and the WAL is long enough that the second sequencer connects before the catch-up ends. Asking again on every
        # connection then would have the leading sequencer start its stream again, and the engine would find the records
        # going backwards, report the catch-up incomplete, and ask again only after catch_up_retry_seconds. It must
        # instead finish the catch-up it began. The scenario must also show it made the window: a second connection made
        # after the catch-up had finished tests nothing.
        if scenario.assert_catch_up_survives_second_connection:
            engine = "matching_engine_primary"
            engine_events = _TEST_EVENTS.modules["matching_engine_test_events"]
            library_events = _TEST_EVENTS.messages
            since = _TEST_EVENTS.since_introduction(engine)
            started = _TEST_EVENTS.find(catch_up(engine_events, engine, engine_events.CatchUpStage.Started), since=since)
            if started is None:
                die(f"second connection: {engine} never began catching up")
            # Less than catch_up_retry_seconds, so that a catch-up that went wrong and was asked for again cannot pass.
            completed = _TEST_EVENTS.wait_for(catch_up(engine_events, engine, engine_events.CatchUpStage.Completed), timeout=10.0, since=started.index)
            incomplete = _TEST_EVENTS.find(catch_up(engine_events, engine, engine_events.CatchUpStage.Incomplete), since=since)
            if incomplete is not None:
                die(f"second connection: {engine} found its catch-up incomplete ({incomplete.message}) -- asked again on a connection "
                    "whose catch-up was under way, the leading sequencer started its stream again (BUG-0200)")
            if completed is None:
                die(f"second connection: {engine} did not finish catching up within 10s of beginning")
            # The window needs the leading sequencer's connection first, so that its stream is under way, and the
            # follower's during that stream. A follower that connects first only declines.
            if _TEST_EVENTS.find(leads(library_events, "sequencer_secondary"), since=0) is not None:
                die("second connection: sequencer_secondary led, so sequencer_primary was not the sequencer serving the catch-up")
            first = _TEST_EVENTS.find(connection_made(library_events, "sequencer_primary", engine_service(engine)), since=since)
            later = _TEST_EVENTS.find(connection_made(library_events, "sequencer_secondary", engine_service(engine)),
                                      since=_TEST_EVENTS.since_introduction("sequencer_secondary"))
            if first is None or later is None:
                die(f"second connection: both sequencers must connect to the restarted {engine} (leader: {first}, follower: {later})")
            if not (first.index < later.index and started.index < later.index < completed.index):
                die(f"second connection: the scenario did not make the window it exists for: the catch-up was events {started.index} to "
                    f"{completed.index}, the leading sequencer connected at event {first.index} and the follower at event {later.index}")
            log(f"  second connection: {later.component} connected to {engine} {later.arrived - started.arrived:.2f}s into a catch-up of "
                f"{completed.message.records_applied} record(s), which then finished {completed.arrived - started.arrived:.2f}s after it "
                "began, with the stream never going backwards -- OK")
            # BUG-0176. The catch-up runs from the first record of a WAL that already holds a report of every order, so the
            # engine must leave out all but the reports of commands the WAL did not yet hold every report of when the
            # catch-up started: a handful. Sending them all again is what flooded the leading sequencer.
            sent = completed.message.reports_sent
            left_out = completed.message.reports_left_out
            if left_out <= 0:
                die(f"far-back catch-up: {engine} left out no report, so it sent the member every report again ({sent}), which the "
                    "WAL already holds (BUG-0176)")
            if sent * 100 > left_out:
                die(f"far-back catch-up: {engine} sent {sent} report(s) again and left out {left_out}; the WAL already holds all but "
                    "those of the last few commands, so it sent many it holds (BUG-0176)")
            log(f"  far-back catch-up: {engine} left out {left_out} report(s) the WAL already holds and sent {sent}, and "
                "sequencer_primary led throughout -- OK")

        # ── An engine that waited catches up before it leads ─────────────────
        # BUG-0198 and BUG-0199. The engine that leads at the end was caught up and waiting while the other engine
        # accepted orders, so it must catch up again before it leads, and say so: ResumedAsLeader is sent only at the end
        # of a catch-up to lead. Its book must then hold exactly the orders the member was told are open. Fewer means
        # orders accepted while it waited were lost; more means it holds orders the member was told were cancelled.
        if scenario.assert_waiting_engine_catches_up:
            leader = "matching_engine_primary"
            resumed = engine_event(leader, "ResumedAsLeader", _TEST_EVENTS.since_introduction(leader), args.failover_timeout)
            if resumed is None:
                die(f"waiting engine: {leader} took the lead and never reported resuming as leader, so it did not catch up "
                    "first, and holds none of the orders the other engine accepted while it waited (BUG-0198)")
            member_open = len(client_reports(f8proc).open_orders())
            if resumed.book_size < member_open:
                die(f"waiting engine: {leader} resumed as leader holding {resumed.book_size} order(s), and the member was told "
                    f"{member_open} are open. The orders it lacks are lost, and the member was not told")
            if resumed.book_size > member_open:
                die(f"waiting engine: {leader} resumed as leader holding {resumed.book_size} order(s), and the member was told "
                    f"{member_open} are open. It holds orders the member was told were cancelled (BUG-0199)")
            log(f"  waiting engine: {leader} caught up to sequence number {resumed.through_seq_no} before leading, and holds the "
                f"{member_open} order(s) the member was told are open -- OK")

        if scenario.assert_open_orders_survive:
            gateway_from = _TEST_EVENTS.mark()
            cancels_from = _TEST_EVENTS.mark()

            log("=== Dropping the client session, so the gateway cancels what it believes is open ===")
            stop_f8test(f8proc)
            f8proc = None

            found, _ = await_expectation(expect_message("fix_order_gateway_a", "the dropped session's open orders", "SessionDisconnectedWithOrders", "fix_order_gateway_test_events"), _CANCEL_GRACE_HOLD_TIMEOUT, gateway_from)
            if not found:
                die("open orders: the gateway never reported the dropped session's open orders, so "
                    "nothing was cancelled and the question was not asked")

            # How many orders the gateway believed open and so cancelled: one answer from the engine is wanted for each.
            disconnected = component_event("fix_order_gateway_a", "fix_order_gateway_test_events", "SessionDisconnectedWithOrders", gateway_from, 0.0)
            expected = disconnected.open_orders if disconnected is not None else 0
            if expected <= 0:
                die(f"open orders: the gateway reported {expected} open order(s) for the dropped session, so nothing was "
                    "cancelled and the question was not asked")

            # Whichever answer comes back, it comes back from the engine. The member has gone, so the engine's events
            # are the only record of the answers. They are read until there is one for every order the gateway cancelled,
            # and then a checkpoint makes sure every one sent by then has been read before they are counted.
            engine = me_log.stem
            deadline = time.monotonic() + _OPEN_ORDER_CANCEL_TIMEOUT
            unknown = cancelled = 0
            while time.monotonic() < deadline:
                unknown = outcome_count(engine, cancels_from, "CancelRefusedUnknown")
                cancelled = outcome_count(engine, cancels_from, "Cancelled")
                if unknown + cancelled >= expected:
                    break
                _TEST_EVENTS.pump(0.5)
            if not _TEST_EVENTS.settle(engine):
                die(f"open orders: {engine} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]})")
            unknown = outcome_count(engine, cancels_from, "CancelRefusedUnknown")
            cancelled = outcome_count(engine, cancels_from, "Cancelled")

            if unknown:
                die(f"open orders: the matching engine refused {unknown} of the {expected} cancels the gateway sent as "
                    "orders it does not recognise. Orders the member was told are open are gone, and "
                    "neither the member nor the gateway was told -- see R-0018, and docs/bug_list.md "
                    "BUG-0064")
            if cancelled != expected:
                die(f"open orders: the gateway cancelled {expected} order(s), and within {_OPEN_ORDER_CANCEL_TIMEOUT:.0f}s "
                    f"the engine cancelled {cancelled} and refused none -- the other cancels never reached it, so the "
                    "question was not asked of every order")
            log(f"  the engine cancelled all {cancelled} order(s) the gateway believed open and refused none -- OK")

        # ── Cancel-on-disconnect grace period (step 3b) ───────────────────────
        # The gateway is alive throughout; only the client session goes away. Everything
        # here reads the gateway's own log, because the behaviour under test is entirely
        # the gateway's: what it does with a dead session's resting orders, and when.
        if scenario.assert_cancel_grace:
            gateway_from = _TEST_EVENTS.mark()

            log("=== Dropping the client session (gateway stays up) ===")
            stop_f8test(f8proc)
            f8proc = None

            # 1. It must say it is holding, not cancelling. This is the line that
            #    distinguishes the new behaviour from the old one.
            found, elapsed = await_expectation(expect_message("fix_order_gateway_a", "the dropped session's orders held for the grace period", "SessionDisconnectedWithOrders", "fix_order_gateway_test_events", fields=lambda m: {"cancel_enabled": True}), _CANCEL_GRACE_HOLD_TIMEOUT, gateway_from)
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
            gateway_events = _TEST_EVENTS.modules["fix_order_gateway_test_events"]
            hold = _TEST_EVENTS.find(message_of(gateway_events.SessionDisconnectedWithOrders, "fix_order_gateway_a", cancel_enabled=True), since=gateway_from)
            held = hold.message.grace_seconds if hold is not None else None
            if held is None:
                die("cancel grace: the gateway's report of holding the orders was not found")
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
            drained = count_seen(expect_message("fix_order_gateway_a", "cancels sent", "CancelsSent", "fix_order_gateway_test_events"), gateway_from)
            if drained > 0:
                die(f"cancel grace: the gateway completed {drained} cancel drain(s) "
                    f"within {_CANCEL_GRACE_QUIET_PERIOD:.0f}s of the disconnect. The grace period "
                    "is not being honoured -- a member whose connection blipped would come back flat.")
            log(f"  cancel grace: nothing cancelled {_CANCEL_GRACE_QUIET_PERIOD:.0f}s after the drop -- OK")

            # 3. The point of the whole feature: the same comp id comes back inside the
            #    window and its orders are released without a cancel ever being sent.
            log("=== Reconnecting the same comp id inside the grace window ===")
            f8proc = send_burst(0, gw_log)
            found, elapsed = await_expectation(expect_message("fix_order_gateway_a", "the held orders reclaimed", "SessionReclaimed", "fix_order_gateway_test_events"), _CANCEL_GRACE_HOLD_TIMEOUT, gateway_from)
            if not found:
                die("cancel grace: the reconnecting comp id did not reclaim its held orders. "
                    "The grace entry is matched on comp id -- check the reconnecting session "
                    "authenticated with the same one.")
            log(f"  cancel grace: orders reclaimed on reconnect, none cancelled ({elapsed:.1f}s)")

            drained = count_seen(expect_message("fix_order_gateway_a", "cancels sent", "CancelsSent", "fix_order_gateway_test_events"), gateway_from)
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
            found, elapsed = await_expectation(expect_leads("sequencer_secondary"), _HA_OFF_LEAD_TIMEOUT, 0)
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
                if await_expectation(expect_leads(name), _HA_OFF_LEAD_TIMEOUT, 0)[0]:
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
        if scenario.assert_deferred_answered:
            log("=== An order taken while the venue has no matching engine at all ===")
            stop_f8test(f8proc)
            f8proc = None
            time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            gateway_port = gateway_listen_port(prefix, "a")

            # Both engines, because one of them would otherwise take over and the order would
            # be recovered by a promotion -- which has always worked and is not what this asks.
            for engine in ("matching_engine_primary", "matching_engine_secondary"):
                engine_proc = proc_by_name.get(engine)
                if engine_proc is None or engine_proc.poll() is not None:
                    die(f"deferred orders: {engine} is not running, so taking every engine away "
                        "is not what this scenario would be doing and it would prove nothing.")
                log(f"  SIGKILL -> {engine} (PID {engine_proc.pid})")
                engine_proc.kill()
                engine_proc.wait()
            log("  the venue now has no matching engine at all")

            member = FixRawClient("127.0.0.1", gateway_port, FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("deferred orders: the raw client could not log on. A test that cannot log on "
                    "looks identical to a venue that will not answer, so this is checked first.")

            def deferred_report_for(cl_ord_id: str, timeout: float) -> dict[int, str] | None:
                """The ExecutionReport for this order, or None. Matched on ClOrdID, because
                other reports are in flight and taking the next one to arrive would read
                whichever happened to be first."""
                deadline = time.monotonic() + timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return None
                    report = member.receive_until("8", timeout=min(1.0, remaining))
                    if report is not None and report.get(11) == cl_ord_id:
                        return report

            deferred_cl_ord_id = "deferred-answered-1"
            member.new_order_single(deferred_cl_ord_id)
            if deferred_report_for(deferred_cl_ord_id, _REFUSAL_DEFERRED_SILENCE) is not None:
                member.close()
                die("deferred orders: the order was answered while no matching engine existed. "
                    "Only an engine sends that report, so either one is still alive and this "
                    "scenario is testing nothing, or the answer came from somewhere it must not.")
            log("  the order is deferred and the member is told nothing -- correct so far")

            # Cold: this instance was killed, so it starts from its own region and has no
            # predecessor to take over from. That is the case that used to skip the catch-up.
            log("  starting matching_engine_primary from nothing")
            do_restart_step(
                RestartStep(
                    proc_name="matching_engine_primary",
                    ready_log_name="matching_engine_primary.log",
                    ready=expect_engine_reachable("matching_engine_primary", "matching_engine"),
                    ready_timeout=_ME_READY_TIMEOUT,
                    resets_me_counter=False,
                    settle_secs=_ME_SETTLE,
                ),
                proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix),
            )

            # Everything the member is handed for this order, not merely the first thing. The
            # venue may legitimately say more than one thing about it -- accepted out of the
            # catch-up, and then whatever the promotion policy does with the book it inherited
            # -- and a test that read one report would be blind to which.
            answers: list[dict[int, str]] = []
            answer_deadline = time.monotonic() + _DEFERRED_ANSWER_TIMEOUT
            while time.monotonic() < answer_deadline:
                remaining = answer_deadline - time.monotonic()
                report = member.receive_until("8", timeout=min(1.0, remaining))
                if report is not None and report.get(11) == deferred_cl_ord_id:
                    answers.append(report)
                    # Accepted then cancelled is the whole story under the cancel policy, and
                    # accepted alone is the whole story under keep. Either way a terminal
                    # status ends it and there is nothing further to wait for.
                    if report.get(39) in ("4", "8"):
                        break

            if not answers:
                member.close()
                die("deferred orders: the venue took this order, committed it to the log, and "
                    "never answered it -- the engine started and applied nothing. This is "
                    "BUG-0064 and the surviving half of BUG-0009: an order the venue accepted "
                    "and cannot account for, which the member cannot cancel because cancelling "
                    "needs the same engine.")

            for answer in answers:
                log(f"    report: OrdStatus={answer.get(39)} ExecType={answer.get(150)} "
                    f"PossResend={answer.get(97, 'absent')} OrderID={answer.get(37)}")

            # The catch-up's own report: the engine applying a record it was sent and saying so.
            # This is the one that did not exist before, and the one the member needs -- nothing
            # else in the venue ever told it this order was taken.
            accepted = [a for a in answers if a.get(39) == "0"]
            if not accepted:
                member.close()
                die("deferred orders: the member was answered but never told the order was "
                    "accepted. The engine applied the record from the catch-up, so it holds the "
                    "order; a member that is only ever told about the end of an order it was "
                    "never told the venue had cannot reconcile its day.")
            if accepted[0].get(97) != "Y":
                member.close()
                die("deferred orders: the acceptance arrived without PossResend. The engine "
                    "cannot tell this record from one an earlier engine already reported, so "
                    "every report it sends out of a catch-up must say so -- unmarked, a report "
                    "the member already had reads as a second event (R-0122).")
            log("  the member is told the order was accepted, marked PossResend -- OK")

            # And the order is still open, whatever order_book.open_orders_on_promotion says.
            # That setting speaks about a book a promoted instance inherits from a peer; this
            # instance was not promoted, it started, and what it holds it read from its own
            # region and caught up on top of. Cancelling here would be R-0018 broken by a
            # setting that is not about restarts -- the same mistake as reading the absence
            # limit as a promotion gate.
            if answers[-1].get(39) != "0":
                member.close()
                die("deferred orders: the member's last word on this order was OrdStatus="
                    f"{answers[-1].get(39)}. The engine started rather than being promoted, so "
                    "the order it recovered stays open (R-0018): the promotion policy is about "
                    "a book taken over from a peer and this instance took over from nobody.")
            log("  and the order is still open, because this engine started rather than being "
                "promoted -- OK")
            member.close()

        # ── The follower's copy of the book across a break in the link between the engines ──
        # BUG-0187. Each order and cancel is answered before the next is sent, and every one of them names its ClOrdID, so
        # what the member was told is known exactly: the orders it was told are open are those placed and not cancelled.
        # The break is proved rather than assumed: the library confirms it failed a send, and the leading engine reports
        # its connection to its peer lost, and then made again, before the last order is placed.
        if scenario.assert_book_survives_peer_link_break:
            log("=== Breaking the link between the matching engines while orders are placed and cancelled ===")
            leader = resolved(ENGINE_LEADING_AT_START)
            follower = resolved(ENGINE_FOLLOWING_AT_START)
            library_events = _TEST_EVENTS.messages
            # Dropping the FIX8 session cancels the orders it holds; those cancels reach the follower before the break.
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
            time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("link between the engines: the raw client could not log on")

            def answer_to(cl_ord_id: str) -> dict[int, str] | None:
                """The ExecutionReport or OrderCancelReject that answers cl_ord_id, or None if none comes."""
                deadline = time.monotonic() + _RAW_REPLY_TIMEOUT
                while time.monotonic() < deadline:
                    reply = member.receive_until("8", "9", timeout=min(1.0, max(0.01, deadline - time.monotonic())))
                    if reply is not None and reply.get(11) == cl_ord_id:
                        return reply
                return None

            def place(cl_ord_id: str) -> None:
                member.new_order_single(cl_ord_id)
                reply = answer_to(cl_ord_id)
                if reply is None or reply.get(35) != "8" or reply.get(39) != "0":
                    die(f"link between the engines: order {cl_ord_id} was not accepted ({reply})")

            def cancel(cl_ord_id: str, attempt: str) -> dict[int, str] | None:
                request = f"{cl_ord_id}-cancel-{attempt}"
                member.order_cancel_request(request, cl_ord_id)
                return answer_to(request)

            before = [f"link-before-{i}" for i in range(1, 11)]
            during = [f"link-during-{i}" for i in range(1, 6)]
            after = ["link-after-1"]
            for cl_ord_id in before:
                place(cl_ord_id)
            # Long enough for the updates of those orders to reach the follower while the link is up.
            time.sleep(1.0)

            broke_from = _TEST_EVENTS.mark()
            block_flag.touch()
            place(during[0])
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                die(f"link between the engines: {leader} sent nothing on the link to its peer after an order was placed, so the "
                    "link was never broken (the engine that is not configured as primary sends no book update, BUG-0188)")
            for cl_ord_id in during[1:]:
                place(cl_ord_id)
            cancelled = before[:3]
            for cl_ord_id in cancelled:
                reply = cancel(cl_ord_id, "during")
                if reply is None or reply.get(35) != "8" or reply.get(39) != "4":
                    die(f"link between the engines: the cancel of {cl_ord_id} during the break was not carried out ({reply})")
            lost = _TEST_EVENTS.wait_for(lambda event: (isinstance(event.message, library_events.ConnectionChanged) and not event.message.made
                                                        and event.message.service == "me_peer_replication" and event.component == leader),
                                         timeout=_RAW_REPLY_TIMEOUT, since=broke_from)
            if lost is None:
                die(f"link between the engines: {leader} never reported its connection to its peer lost, so the link did not break")

            block_flag.unlink()
            block_confirmation(block_flag).unlink(missing_ok=True)
            remade = _TEST_EVENTS.wait_for(connection_made(library_events, leader, "me_peer_replication"), timeout=30.0, since=lost.index)
            if remade is None:
                die(f"link between the engines: {leader} did not connect to its peer again within 30s of the break ending")
            log(f"  link between the engines: broken for {remade.arrived - lost.arrived:.2f}s, while {len(during)} order(s) were "
                f"placed and {len(cancelled)} cancelled -- OK")
            for cl_ord_id in after:
                place(cl_ord_id)
            # Long enough for that order's update to reach the follower over the mended link.
            time.sleep(1.0)

            kill_mark = _TEST_EVENTS.mark()
            leader_proc = proc_by_name.get(leader)
            if leader_proc is None or leader_proc.poll() is not None:
                die(f"link between the engines: {leader} is not running, so there is no leader to kill")
            log(f"  SIGKILL -> {leader} (PID {leader_proc.pid})")
            leader_proc.kill()
            leader_proc.wait()
            if leader_proc.poll() is None:
                die(f"link between the engines: {leader} is still running after SIGKILL")
            resumed = engine_event(follower, "ResumedAsLeader", kill_mark, args.failover_timeout)
            if resumed is None:
                die(f"link between the engines: {follower} never reported resuming as leader after {leader} was killed")

            told_open = before[3:] + during + after
            wrong: list[str] = []
            for cl_ord_id in told_open:
                reply = cancel(cl_ord_id, "after")
                if reply is None or reply.get(35) != "8" or reply.get(39) != "4":
                    wrong.append(f"{cl_ord_id}, which the member was told is open, could not be cancelled")
            for cl_ord_id in cancelled:
                reply = cancel(cl_ord_id, "again")
                if reply is None or reply.get(35) != "9":
                    wrong.append(f"{cl_ord_id}, which the member was told is cancelled, was still on the book")
            member.close()
            if wrong:
                die(f"link between the engines: the promoted {follower} resumed holding {resumed.book_size} order(s), and the member "
                    f"was told {len(told_open)} of the orders placed in this check are open. " + "; ".join(wrong) + ". The changes made while the link was down were "
                    "lost (BUG-0187)")
            log(f"  link between the engines: the promoted {follower} holds exactly the {len(told_open)} order(s), of those placed in this "
                "check, that the member was told are open -- OK")

        # ── Kept reports: the engine that takes the lead sends what the leader never had written ──
        # docs/availability/matching_engine_lockstep.md, section 3. The orders are named by their ClOrdIDs, so a New report
        # sent before the block, for an earlier order, cannot stand in for one of these.
        if scenario.assert_kept_reports_sent_on_promotion:
            log("=== Blocking the leading engine's reports, sending orders, and killing it ===")
            leader = resolved(ENGINE_LEADING_AT_START)
            follower = resolved(ENGINE_FOLLOWING_AT_START)
            block_flag.touch()
            reports_before = client_reports(f8proc).mark()
            blocked_sent_before = send_orders(f8proc, b"T", 1)
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                die(f"kept reports: {leader} sent no report after the block began, so its reports were never blocked")
            # Long enough for the leader to apply them and for the follower to be sent them as a follower of the WAL.
            time.sleep(3.0)
            early = client_reports(f8proc).new_orders_since(reports_before)
            if early > 0:
                die(f"kept reports: the member was sent {early} New report(s) for orders sent after the leader's reports were blocked, "
                    "so something other than the engine that takes the lead answered them, and the scenario tests nothing")
            kill_mark = _TEST_EVENTS.mark()
            leader_proc = proc_by_name.get(leader)
            if leader_proc is None or leader_proc.poll() is not None:
                die(f"kept reports: {leader} is not running, so there is no leader to kill")
            log(f"  SIGKILL -> {leader} (PID {leader_proc.pid})")
            leader_proc.kill()
            leader_proc.wait()
            if leader_proc.poll() is None:
                die(f"kept reports: {leader} is still running after SIGKILL")
            sent = engine_event(follower, "KeptReportsSent", kill_mark, args.failover_timeout)
            if sent is None:
                die(f"kept reports: {follower} never said it sent the reports it kept, so it did not take the lead after following")
            answered, took = await_new_orders(f8proc, _ORDERS_PER_COMMAND[b"T"], blocked_sent_before, args.recovery_timeout)
            if not answered:
                die(f"kept reports: the member was not sent a New report for each of the {_ORDERS_PER_COMMAND[b'T']} orders the dead leader "
                    f"applied without its reports reaching the WAL; {follower} sent {sent.count} kept report(s) of commands after "
                    f"{sent.reported_through}")
            log(f"  kept reports: {follower} sent {sent.count} report(s) it kept of commands after {sent.reported_through}, and the member "
                f"was sent a New report for every one of the {_ORDERS_PER_COMMAND[b'T']} orders within {took:.2f}s -- OK")

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
            # The refusal must be an OrderCancelReject, which refuses the request, and not a rejected
            # ExecutionReport, which in FIX says the order itself was rejected (R-0151, BUG-0099).
            # A member told its order was rejected believes it has no order in the market while
            # the order still rests. The reply must not report the order as rejected either.
            if reply.get(35) != "9":
                die(f"order refusal: the cancel was refused with MsgType={reply.get(35)}, not an "
                    "OrderCancelReject (35=9). A rejected ExecutionReport tells a FIX member its order "
                    "was rejected, when what was refused is the request and the order is untouched.")
            if reply.get(39) == "8":
                die("order refusal: the OrderCancelReject reports the order as rejected (39=8). The "
                    "gateway did not touch the order, so it must report it as open.")
            if reply.get(434) != "1":
                die(f"order refusal: CxlRejResponseTo={reply.get(434)}, expected 1 (a request to cancel)")
            log(f"  order refusal: cancels are refused too, with an OrderCancelReject, OrdStatus={reply.get(39)}, "
                f"Text='{reply.get(58, '')}' -- OK")

            # 3a. The binary gateway refuses on the same outage, with the same texts (BUG-0100). It
            #     learns the venue has stopped accepting orders from the same OrderAcceptance message
            #     the FIX gateway does.
            if scenario.binary_gateway:
                binary_port = binary_gateway_listen_port(prefix)
                binary_order_id = f"bin-refuse-{datetime.now().strftime('%H%M%S')}"
                _, replies, output = run_binary_client(bin_dir, binary_port, "--orders", "1", "--cl-ord-id", binary_order_id)
                answer = next((r for r in replies if r["cl_ord_id"] == binary_order_id), None)
                expected = "Venue is not accepting orders: no matching engine available"
                if answer is None or answer["ord_status"] != "Rejected" or answer["text"] != expected:
                    die(f"order refusal: the binary gateway answered an order during the outage with {answer}; expected a "
                        f"rejected ExecutionReport with Text='{expected}'. Before BUG-0100 it passed the order on and the "
                        f"member was told nothing.\n{output}")
                _, replies, output = run_binary_client(bin_dir, binary_port, "--cancel", binary_order_id)
                answer = next((r for r in replies if r["cl_ord_id"] == f"{binary_order_id}-CANCEL"), None)
                expected = "Venue cannot process cancels: no matching engine available. The order is unchanged"
                if answer is None or answer["kind"] != "OrderCancelReject" or answer["ord_status"] != "New" or answer["text"] != expected:
                    die(f"order refusal: the binary gateway answered a cancel during the outage with {answer}; expected an "
                        f"OrderCancelReject, OrdStatus New, Text='{expected}'.\n{output}")
                log("  order refusal: the binary gateway refuses orders and cancels on the same outage, with the same texts -- OK")

            # 4. The health line keeps reporting while nothing progresses.
            #
            # Before BUG-0009 step 2 this line was emitted once per N orders accounted, so when
            # accounting stalled the reporting stalled with it. It went quiet for 2m19s during the
            # incident. A line driven by progress cannot report the absence of progress.
            quiet_from = _TEST_EVENTS.mark()
            log(f"  watching the gateway for {_REFUSAL_QUIET_WATCH:.0f}s with nothing in flight ...")
            time.sleep(_REFUSAL_QUIET_WATCH)
            if not _TEST_EVENTS.settle("fix_order_gateway_a"):
                die(f"order refusal: fix_order_gateway_a did not answer a checkpoint ({_TEST_EVENTS.failures[-1]})")
            progress = message_of(_TEST_EVENTS.modules["fix_order_gateway_test_events"].ProgressReported, "fix_order_gateway_a")
            quiet_reports = [event.message for event in _TEST_EVENTS.events[quiet_from:] if progress(event)]
            if len(quiet_reports) < 2:
                die(f"order refusal: only {len(quiet_reports)} progress report(s) in "
                    f"{_REFUSAL_QUIET_WATCH:.0f}s of a degraded venue. The report has to be driven "
                    "by a clock, not by progress, or it goes silent exactly when it matters.")
            latest = quiet_reports[-1]
            if latest.refused < 1:
                die(f"order refusal: the progress report says refused={latest.refused} "
                    "after orders were demonstrably refused. The count an operator would watch "
                    "does not reflect what the members were told.")
            if latest.awaiting < 1:
                die("order refusal: awaiting=0 while orders deferred before the refusal began are "
                    "still unprocessed. Deferred and refused are different states and the report "
                    "must not fold them together -- only one of them is somebody's problem later.")
            log(f"  order refusal: {len(quiet_reports)} progress reports while nothing progressed, "
                f"awaiting={latest.awaiting} refused={latest.refused} refused_cancels={latest.refused_cancels} -- OK")

            # 5. Acceptance resumes on its own.
            #
            # No operator action. Requiring one was considered and rejected: this bug is precisely
            # a case where nobody was watching, and a recovery that depends on the watching which
            # has already failed is not safer.
            log("  restarting the matching engine -- nothing else is done")
            do_restart_step(_me_restart_step(), proc_by_name, app_procs, launch_table,
                            bin_dir, log_dir, venue_state_dir(prefix))
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

            # 6. The matching engine's own refusal of a cancel is an OrderCancelReject too.
            #
            # With an engine running again, a cancel naming an order the venue never held is
            # refused by the engine, not the gateway. The engine says so with a rejected report,
            # which the gateway must turn into an OrderCancelReject with CxlRejReason 1, Unknown
            # order (R-0151, BUG-0099).
            member.order_cancel_request("unknown-cancel", "never-placed")
            # Reports for the orders deferred earlier arrive now that an engine is running again,
            # so the reply is recognised by the ClOrdID it answers, not by being the first to come.
            reply = None
            reply_deadline = time.monotonic() + _RAW_REPLY_TIMEOUT
            while reply is None and time.monotonic() < reply_deadline:
                candidate = member.receive_until("8", "9", timeout=max(0.1, reply_deadline - time.monotonic()))
                if candidate is not None and candidate.get(11) == "unknown-cancel":
                    reply = candidate
            if reply is None:
                member.close()
                die("order refusal: a cancel for an order the venue never held got no answer at all.")
            if reply.get(35) != "9" or reply.get(434) != "1" or reply.get(102) != "1":
                member.close()
                die(f"order refusal: the engine's refusal of a cancel for an unknown order came back as "
                    f"MsgType={reply.get(35)} CxlRejResponseTo={reply.get(434)} CxlRejReason={reply.get(102)}; "
                    "expected an OrderCancelReject (35=9) with 434=1 and 102=1, Unknown order.")
            log("  order refusal: the engine's refusal of a cancel for an unknown order is an "
                "OrderCancelReject with CxlRejReason 1 -- OK")
            member.close()

        # ── The matching engine's own refusal of over-long identifiers ────────
        if scenario.assert_engine_identifier_refusal:
            log("=== The matching engine refuses over-long identifiers (BUG-0101) ===")
            sequencer_config = prefix / "etc" / "sequencer" / "sequencer_primary.toml"
            order_port = installed_toml_int(sequencer_config, "listen_port")
            shared_prefix = f"inj{datetime.now().strftime('%H%M%S')}".ljust(_MAX_CL_ORD_ID_LENGTH, "X")
            first_id = shared_prefix + "A"
            second_id = shared_prefix + "B"

            def inject(*arguments: str) -> None:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", str(order_port), *arguments],
                                        capture_output=True, text=True, check=False, timeout=30)
                if result.returncode != 0:
                    die(f"engine refusal: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            engine = me_log.stem
            cancel_id = f"inj-cancel-{shared_prefix[:12]}"
            live_from = _TEST_EVENTS.mark()
            inject("--cl-ord-id", first_id)
            inject("--cl-ord-id", second_id)
            inject("--cl-ord-id", cancel_id, "--cancel", first_id)

            # The cancel was sent last, so once it is refused the orders before it have been dealt with.
            await_order_outcome(engine, cancel_id, "CancelRefusedTooLong", _RAW_REPLY_TIMEOUT, live_from)
            orders_refused = sum(settled_order_outcomes(engine, live_from, cl_ord_id, "RefusedTooLong", during_catch_up=False)
                                 for cl_ord_id in (first_id, second_id))
            cancels_refused = order_outcomes(engine, live_from, cancel_id, "CancelRefusedTooLong", during_catch_up=False)
            if orders_refused != 2 or cancels_refused != 1:
                die(f"engine refusal: the matching engine refused {orders_refused} of the 2 over-long orders and "
                    f"{cancels_refused} of the 1 over-long cancel. Cut short to {_MAX_CL_ORD_ID_LENGTH} characters, the "
                    "two orders would be one, and the second would be refused as a duplicate of the first.")
            if any(order_outcomes(engine, live_from, cl_ord_id, outcome) for cl_ord_id in (first_id, second_id)
                   for outcome in ("Accepted", "RefusedDuplicate")):
                die("engine refusal: the matching engine accepted an over-long order, or treated the second as a "
                    "duplicate of the first, which means it cut the identifiers short (BUG-0101).")
            log("  engine refusal: both over-long orders and the over-long cancel were refused by the matching engine -- OK")

            # The same records again, while the restarted engine catches up. The refusals were not
            # published, so the engine is sent them again from the write-ahead log.
            log("  restarting the matching engine, which must replay and refuse the same records")
            do_restart_step(_me_restart_step(), proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix))
            # Only the restarted engine's events count: those since its newest introduction.
            caught_up = expect_catch_up(me_log.stem, "Completed")
            found, _ = await_expectation(caught_up, _ME_READY_TIMEOUT * 2, since_present_processes(caught_up))
            if not found:
                die("engine refusal: the restarted matching engine never completed its catch-up. A refusal that stops "
                    "the catch-up's count would leave a healthy engine unable to serve (R-0101).")
            if expectation_seen(expect_catch_up(me_log.stem, "Incomplete"), since_present_processes(caught_up)):
                die("engine refusal: the restarted engine reported its catch-up incomplete, so a refused record was not "
                    "counted (R-0101).")
            restarted_from = _TEST_EVENTS.since_introduction(engine)
            replayed_orders = sum(settled_order_outcomes(engine, restarted_from, cl_ord_id, "RefusedTooLong", during_catch_up=True)
                                  for cl_ord_id in (first_id, second_id))
            replayed_cancels = order_outcomes(engine, restarted_from, cancel_id, "CancelRefusedTooLong", during_catch_up=True)
            if replayed_orders != 2 or replayed_cancels != 1:
                die(f"engine refusal: while catching up, the engine refused {replayed_orders} of the 2 over-long orders "
                    f"and {replayed_cancels} of the 1 over-long cancel it replayed.")
            log("  engine refusal: after a restart the engine refused the same records again while catching up, and "
                "the catch-up still accounted for every record -- OK")

        # ── Unreplicated orders ───────────────────────────────────────────────
        if scenario.assert_engine_holds_only_logged_orders:
            log("=== The leading sequencer dies holding orders its follower never received (BUG-0103) ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("unreplicated orders: sequencer_primary is not leading, so stopping what it sends to its peer would "
                    "not test the death of a leader.")
            sequencer_etc = prefix / "etc" / "sequencer"
            sequencer_configs = {which: sequencer_etc / f"sequencer_{which}.toml" for which in ("primary", "secondary")}
            order_ports = {which: installed_toml_section_value(config, "network", "listen_port")
                           for which, config in sequencer_configs.items()}
            # Relative to the configuration's directory, which is where launch_app runs each process.
            wal_dirs = {which: config.parent / installed_toml_section_value(config, "wal", "directory")
                        for which, config in sequencer_configs.items()}

            def inject_to(port: str, cl_ord_id: str) -> None:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", port, "--cl-ord-id", cl_ord_id],
                                        capture_output=True, text=True, check=False, timeout=30)
                if result.returncode != 0:
                    die(f"unreplicated orders: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            def times_in_log(wal_dir: Path, cl_ord_id: str) -> int:
                """How many times a ClOrdID appears in a sequencer's write-ahead log files, as raw bytes."""
                needle = cl_ord_id.encode()
                return sum(path.read_bytes().count(needle) for path in wal_dir.rglob("*") if path.is_file())

            def engine_accepted(cl_ord_id: str, since: int) -> bool:
                return settled_order_outcomes(me_log.stem, since, cl_ord_id, "Accepted") > 0

            run_tag = datetime.now().strftime("%H%M%S")
            unreplicated = [f"unrep-{run_tag}-{number}" for number in (1, 2, 3)]  # as inject_order --count names them

            # 1. Stop everything the leader sends to its follower from arriving, and confirm the
            #    library says so, rather than assuming the flag file had its effect.
            block_flag.touch()
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                die("unreplicated orders: libblock_sends_to_ports.so never confirmed that it had begun blocking, so the "
                    "follower may still be receiving the leader's records and nothing below would mean anything.")
            log("  the leader's sends to its follower are now discarded, and the library has confirmed it -- OK")

            # 2. Three orders to the leader in one go, and the leader stopped the moment they are
            #    sent. With nothing reaching the follower, the leader holds them waiting for an
            #    acknowledgement that cannot come; after 100 ms it would treat the follower as behind
            #    and run as if alone, sending them to the engine, which is the accepted exposure of a
            #    leader running alone. Stopping it first keeps the orders held, which is the case
            #    option A exists for. A stopped process cannot fire its timer.
            primary = proc_by_name["sequencer_primary"]
            accepted_from = _TEST_EVENTS.mark()
            run_tag_base = f"unrep-{run_tag}"
            injector = subprocess.Popen([str(bin_dir / "inject_order"), "--port", order_ports["primary"], "--cl-ord-id", run_tag_base,
                                         "--count", str(len(unreplicated))], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            sent_line = injector.stdout.readline() if injector.stdout is not None else ""
            os.kill(primary.pid, signal.SIGSTOP)
            injector.wait(timeout=30)
            if not sent_line.startswith("sent "):
                die(f"unreplicated orders: inject_order did not report sending the orders: {sent_line!r}")
            with open(f"/proc/{primary.pid}/stat", encoding="ascii") as stat:
                state = stat.read().rsplit(")", 1)[1].split()[0]
            if state != "T":
                die(f"unreplicated orders: sequencer_primary is in state {state!r} after SIGSTOP, not stopped.")
            log(f"  {len(unreplicated)} orders sent to the leader, and the leader stopped straight after: {sent_line.strip()}")
            # The gateway's other copy, which the follower discards as a follower does.
            for cl_ord_id in unreplicated:
                inject_to(order_ports["secondary"], cl_ord_id)
            time.sleep(1.0)
            accepted_early = [c for c in unreplicated if engine_accepted(c, accepted_from)]
            if accepted_early:
                die(f"unreplicated orders: the matching engine accepted {accepted_early} although the follower never acknowledged "
                    "them. The leader must hold an order until the follower holds it (option A, BUG-0103).")
            log("  the matching engine has accepted none of them: the leader holds them until the follower acknowledges -- OK")

            # 3. The leader dies, and the follower takes the lead.
            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if primary.poll() is None or Path(f"/proc/{primary.pid}").exists():
                die(f"unreplicated orders: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            log("  sequencer_primary confirmed dead")
            found, elapsed = await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)
            if not found:
                die(f"unreplicated orders: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            log(f"  sequencer_secondary took the lead {elapsed:.1f}s after the kill")

            # 4. The old leader's log holds the orders. This is what makes the next check mean
            #    something: it shows that searching a log's files for a ClOrdID finds it when it is there.
            missing_from_old = [c for c in unreplicated if times_in_log(wal_dirs["primary"], c) == 0]
            if missing_from_old:
                die(f"unreplicated orders: the old leader's log in {wal_dirs['primary']} does not hold {missing_from_old}, "
                    "which it wrote before sending them to the engine. The search of the log files is not finding "
                    "what is there, so its finding nothing in the new leader's log would prove nothing.")
            log(f"  the old leader's log holds all {len(unreplicated)} orders, so the search of a log finds them -- OK")

            # 5. The engine goes on taking orders from the new leader.
            after_takeover = f"unrep-{run_tag}-after"
            after_from = _TEST_EVENTS.mark()
            inject_to(order_ports["secondary"], after_takeover)
            if not await_order_outcome(me_log.stem, after_takeover, "Accepted", _RAW_REPLY_TIMEOUT, after_from):
                die("unreplicated orders: the matching engine did not accept an order from the new leader.")
            log("  the matching engine accepted an order from the new leader")
            check_new_leader_numbers_forward(secondary_wal_dir, secondary_wal_start)

            # 6. The requirement: the engine holds no order the new leader's log does not hold.
            orphaned = [c for c in unreplicated if engine_accepted(c, accepted_from) and times_in_log(wal_dirs["secondary"], c) == 0]
            if orphaned:
                die(f"unreplicated orders: the matching engine accepted {orphaned} from the old leader, and the new leader's "
                    f"log in {wal_dirs['secondary']} does not hold them. They rest on the book with no record in any "
                    "surviving log, no member has been told of them, and the new leader has numbered later orders "
                    "without them (BUG-0103).")
            log("  the engine holds no order the new leader's log lacks. The three orders are in neither; a gateway sending again the "
                "commands it holds unanswered is what recovers them (part 4.3 of the design) -- OK")

        # ── Publishers follow only the record ─────────────────────────────────
        if scenario.assert_publishers_follow_only_the_record:
            log("=== A publisher is sent only the records the venue's record holds, each once (BUG-0153) ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("publishers: sequencer_primary is not leading, so the scenario's change of leader would not happen as written.")
            sequencer_etc = prefix / "etc" / "sequencer"
            sequencer_configs = {which: sequencer_etc / f"sequencer_{which}.toml" for which in ("primary", "secondary")}
            order_ports = {which: installed_toml_section_value(config, "network", "listen_port")
                           for which, config in sequencer_configs.items()}
            # Relative to the configuration's directory, which is where launch_app runs each process.
            wal_dirs = {which: config.parent / installed_toml_section_value(config, "wal", "directory")
                        for which, config in sequencer_configs.items()}
            publisher_dirs = {which: publisher_wal_directory(prefix, which) for which in ("primary", "secondary")}
            publisher_catch_up_timeout = 180.0
            # Time for anything sent twice, or sent late, to arrive after a publisher has reached a record.
            settle_seconds = 3.0

            def inject_orders_to(port: str, cl_ord_id: str, count: int = 1) -> None:
                command = [str(bin_dir / "inject_order"), "--port", port, "--cl-ord-id", cl_ord_id]
                if count > 1:
                    command += ["--count", str(count)]
                result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=30)
                if result.returncode != 0:
                    die(f"publishers: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            def last_record_of(which: str) -> int:
                entries = wal_record_ids(wal_dirs[which])
                return entries[-1][1] if entries else 0

            run_tag = datetime.now().strftime("%H%M%S")

            # 1. Both publishers catch up with everything the baseline orders produced.
            leader_last = last_record_of("primary")
            for which in ("primary", "secondary"):
                wait_for_publisher_to_hold(f"matching_engine_publisher_{which}", publisher_dirs[which], leader_last, publisher_catch_up_timeout)

            # 2. One publisher is killed, orders are taken while it is down, and it is restarted with its log.
            publisher_name = "matching_engine_publisher_primary"
            publisher = proc_by_name[publisher_name]
            log(f"  SIGKILL -> {publisher_name} (PID {publisher.pid})")
            publisher.kill()
            publisher.wait()
            if publisher.poll() is None or Path(f"/proc/{publisher.pid}").exists():
                die(f"publishers: {publisher_name} (PID {publisher.pid}) is still running after SIGKILL.")
            log(f"  {publisher_name} confirmed dead, holding records through {publisher_last_record(publisher_dirs['primary'])}")
            while_down = f"pubdown-{run_tag}"
            accepted_from = _TEST_EVENTS.mark()
            inject_orders_to(order_ports["primary"], while_down, count=3)
            for number in (1, 2, 3):
                if not await_order_outcome(me_log.stem, f"{while_down}-{number}", "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                    die(f"publishers: the matching engine did not accept {while_down}-{number} while the publisher was down.")
            log("  3 orders taken while the publisher was down")
            restarted = launch_app(publisher_name, "matching_engine_publisher",
                                   prefix / "etc" / "matching_engine_publisher" / "matching_engine_publisher_primary.toml", bin_dir, log_dir)
            app_procs.append((publisher_name, restarted))
            proc_by_name[publisher_name] = restarted
            leader_last = last_record_of("primary")
            wait_for_publisher_to_hold(f"{publisher_name} after its restart", publisher_dirs["primary"], leader_last, publisher_catch_up_timeout)
            time.sleep(settle_seconds)
            check_publisher_holds_the_record(f"{publisher_name} after its restart", publisher_dirs["primary"], wal_dirs["primary"])

            # 3. The leader holds records its follower never received, and dies; as in scenario 59.
            block_flag.touch()
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                die("publishers: libblock_sends_to_ports.so never confirmed that it had begun blocking, so the follower may still be "
                    "receiving the leader's records and nothing below would mean anything.")
            log("  the leader's sends to its follower are now discarded, and the library has confirmed it -- OK")
            primary = proc_by_name["sequencer_primary"]
            unreplicated_base = f"pubunrep-{run_tag}"
            injector = subprocess.Popen([str(bin_dir / "inject_order"), "--port", order_ports["primary"], "--cl-ord-id", unreplicated_base,
                                         "--count", "3"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            sent_line = injector.stdout.readline() if injector.stdout is not None else ""
            os.kill(primary.pid, signal.SIGSTOP)
            injector.wait(timeout=30)
            if not sent_line.startswith("sent "):
                die(f"publishers: inject_order did not report sending the orders: {sent_line!r}")
            log(f"  3 orders sent to the leader, and the leader stopped straight after: {sent_line.strip()}")
            for number in (1, 2, 3):
                inject_orders_to(order_ports["secondary"], f"{unreplicated_base}-{number}")
            time.sleep(1.0)
            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if primary.poll() is None or Path(f"/proc/{primary.pid}").exists():
                die(f"publishers: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            log("  sequencer_primary confirmed dead")
            found, elapsed = await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)
            if not found:
                die(f"publishers: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            log(f"  sequencer_secondary took the lead {elapsed:.1f}s after the kill")

            # 4. The new leader numbers a new order where the old leader's unacknowledged records were.
            after_takeover = f"pubafter-{run_tag}"
            after_from = _TEST_EVENTS.mark()
            inject_orders_to(order_ports["secondary"], after_takeover)
            if not await_order_outcome(me_log.stem, after_takeover, "Accepted", _RAW_REPLY_TIMEOUT, after_from):
                die("publishers: the matching engine did not accept an order from the new leader.")
            log("  the matching engine accepted an order from the new leader")

            # 5. The requirement: each publisher's log holds, under each number, the new leader's record.
            new_leader_last = last_record_of("secondary")
            for which in ("primary", "secondary"):
                label = f"matching_engine_publisher_{which} after the change of leader"
                wait_for_publisher_to_hold(label, publisher_dirs[which], new_leader_last, publisher_catch_up_timeout)
            time.sleep(settle_seconds)
            for which in ("primary", "secondary"):
                check_publisher_holds_the_record(f"matching_engine_publisher_{which} after the change of leader", publisher_dirs[which],
                                                 wal_dirs["secondary"])

        # ── Stalled follower ──────────────────────────────────────────────────
        if scenario.assert_stalled_follower_runs_alone:
            log("=== A stalled follower sequencer does not stop trading ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("stalled follower: sequencer_primary is not leading, so stopping sequencer_secondary would not stop a follower.")
            sequencer_etc = prefix / "etc" / "sequencer"
            primary_config = sequencer_etc / "sequencer_primary.toml"
            order_port = installed_toml_section_value(primary_config, "network", "listen_port")
            metrics_port = installed_toml_section_value(primary_config, "metrics", "listen_port")

            def running_alone_gauge() -> str:
                with urllib.request.urlopen(f"http://127.0.0.1:{metrics_port}/metrics", timeout=5) as response:
                    for line in response.read().decode().splitlines():
                        if line.startswith("sequencer_running_alone{") or line.startswith("sequencer_running_alone "):
                            return line.rsplit(" ", 1)[1]
                return "absent"

            def send_order(cl_ord_id: str) -> None:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_port, "--cl-ord-id", cl_ord_id],
                                        capture_output=True, text=True, check=False, timeout=30)
                if result.returncode != 0:
                    die(f"stalled follower: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            if running_alone_gauge() not in ("0", "0.0"):
                die(f"stalled follower: the leader's sequencer_running_alone gauge reads {running_alone_gauge()} before anything is "
                    "stopped; it must read 0 while the follower keeps up.")
            follower = proc_by_name["sequencer_secondary"]
            run_tag = datetime.now().strftime("%H%M%S")
            alone_from = _TEST_EVENTS.mark()
            accepted_from = alone_from
            os.kill(follower.pid, signal.SIGSTOP)
            log(f"  SIGSTOP -> sequencer_secondary (PID {follower.pid}): alive, its connection open, acknowledging nothing")
            try:
                first = f"stall-{run_tag}-1"
                send_order(first)
                if not await_expectation(expect_message("sequencer_primary", "running as if alone", "RunningAloneChanged", fields=lambda m: {"running_alone": True}), _RAW_REPLY_TIMEOUT, alone_from)[0]:
                    die("stalled follower: the leader never reported running as if alone with its follower stopped, so the order "
                        "it holds waits for an acknowledgement that cannot come and trading has stopped.")
                if not await_order_outcome(me_log.stem, first, "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                    die("stalled follower: the matching engine did not accept the order sent while the follower was stopped.")
                if running_alone_gauge() not in ("1", "1.0"):
                    die(f"stalled follower: the leader reports running as if alone but its sequencer_running_alone gauge reads "
                        f"{running_alone_gauge()}, so an operator alerting on it would not know.")
                log("  the leader ran as if alone, the engine accepted the order, and the gauge reads 1 -- OK")
                second = f"stall-{run_tag}-2"
                send_order(second)
                if not await_order_outcome(me_log.stem, second, "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                    die("stalled follower: a second order, sent while the leader was running as if alone, was not accepted.")
                log("  a second order went straight through while the leader ran as if alone -- OK")
            finally:
                os.kill(follower.pid, signal.SIGCONT)
            log(f"  SIGCONT -> sequencer_secondary (PID {follower.pid})")
            # The follower reads what the leader sent while it was stopped and acknowledges it; a
            # further order makes sure there is a new record for it to acknowledge.
            send_order(f"stall-{run_tag}-3")
            if not await_expectation(expect_message("sequencer_primary", "waiting for the follower again", "RunningAloneChanged", fields=lambda m: {"running_alone": False}), _RAW_REPLY_TIMEOUT, alone_from)[0]:
                die("stalled follower: the follower ran again but the leader never went back to waiting for its acknowledgements, "
                    "so it would go on running as if alone with a follower that is keeping up.")
            if running_alone_gauge() not in ("0", "0.0"):
                die(f"stalled follower: the leader went back to waiting for its follower but its gauge reads {running_alone_gauge()}.")
            log("  the follower caught up, the leader waits for its acknowledgements again, and the gauge reads 0 -- OK")

        # ── Orders that waited in the log ─────────────────────────────────────
        if scenario.assert_orders_that_waited_in_the_log_reach_engine:
            log("=== Orders that wait in the log reach the matching engine through the reading thread ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("orders that waited in the log: sequencer_primary is not leading, so stopping sequencer_secondary would not stop a follower.")
            primary_config = prefix / "etc" / "sequencer" / "sequencer_primary.toml"
            order_port = installed_toml_section_value(primary_config, "network", "listen_port")
            burst = 200
            tag = f"waited-{datetime.now().strftime('%H%M%S')}"
            log_from = _TEST_EVENTS.mark()
            accepted_from = _TEST_EVENTS.mark()
            follower = proc_by_name["sequencer_secondary"]
            os.kill(follower.pid, signal.SIGSTOP)
            with open(f"/proc/{follower.pid}/stat", encoding="ascii") as stat:
                state = stat.read().rsplit(")", 1)[1].split()[0]
            if state != "T":
                die(f"orders that waited in the log: sequencer_secondary is in state {state!r} after SIGSTOP, not stopped.")
            log(f"  SIGSTOP -> sequencer_secondary (PID {follower.pid}): alive, its connection open, acknowledging nothing")
            try:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_port, "--cl-ord-id", tag, "--count", str(burst)],
                                        capture_output=True, text=True, check=False, timeout=60)
                if result.returncode != 0:
                    die(f"orders that waited in the log: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")
                if not await_expectation(expect_message("sequencer_primary", "the storage for waiting orders is full", "HeldOrdersFull", "sequencer_test_events"), _RAW_REPLY_TIMEOUT, log_from)[0]:
                    die(f"orders that waited in the log: the leader's storage for held orders never filled with {burst} orders sent and "
                        f"room for {_FEW_HELD_ORDERS}, so nothing waited in the log and the scenario tests nothing.")
                log("  the leader's storage for held orders filled, and later orders wait in the log")
                if not await_expectation(expect_message("sequencer_primary", "the reading thread serves the engine", "EngineServedFromWal", "sequencer_test_events"), _RAW_REPLY_TIMEOUT, log_from)[0]:
                    die("orders that waited in the log: the leader never had the reading thread send the orders that waited in the log.")
                if not await_expectation(expect_message("sequencer_primary", "the engine handed over", "EngineHandedOver", "sequencer_test_events", fields=lambda m: {"answered_catch_up": False}), _RAW_REPLY_TIMEOUT, log_from)[0]:
                    die("orders that waited in the log: the reading thread never handed the matching engine over, so orders go on waiting.")
                log("  the reading thread sent the orders that waited in the log, and handed the engine over")
                expected = [f"{tag}-{number}" for number in range(1, burst + 1)]
                for cl_ord_id in expected:
                    if not await_order_outcome(me_log.stem, cl_ord_id, "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                        die(f"orders that waited in the log: the matching engine never accepted {cl_ord_id}.")
                repeated = [cl_ord_id for cl_ord_id in expected if settled_order_outcomes(me_log.stem, accepted_from, cl_ord_id, "Accepted") > 1]
                if repeated:
                    die(f"orders that waited in the log: the matching engine accepted {len(repeated)} order(s) more than once, the first "
                        f"{repeated[0]}.")
                log(f"  the matching engine accepted all {burst} orders, each exactly once -- OK")
            finally:
                os.kill(follower.pid, signal.SIGCONT)
            log(f"  SIGCONT -> sequencer_secondary (PID {follower.pid})")

        # ── Follower restarts ─────────────────────────────────────────────────
        if scenario.assert_restarted_follower_log_repaired:
            log("=== A follower that restarts is sent the records it missed ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("follower restarts: sequencer_primary is not leading, so killing sequencer_secondary would not kill a follower.")
            sequencer_etc = prefix / "etc" / "sequencer"
            configs = {which: sequencer_etc / f"sequencer_{which}.toml" for which in ("primary", "secondary")}
            order_ports = {which: installed_toml_section_value(config, "network", "listen_port") for which, config in configs.items()}
            wal_dirs = {which: config.parent / installed_toml_section_value(config, "wal", "directory") for which, config in configs.items()}
            run_tag = datetime.now().strftime("%H%M%S")

            def inject_orders(port: str, cl_ord_id: str, count: int = 1) -> None:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", port, "--cl-ord-id", cl_ord_id, "--count", str(count)],
                                        capture_output=True, text=True, check=False, timeout=60)
                if result.returncode != 0:
                    die(f"follower restarts: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            # Where both logs end now, with the follower keeping up.
            time.sleep(1.0)
            start_positions = {which: wal_end_position(path) for which, path in wal_dirs.items()}
            stopped_at = max(wal_records_after(wal_dirs["secondary"], 0, start_positions["secondary"]) or {0: 0})

            follower = proc_by_name["sequencer_secondary"]
            log(f"  SIGKILL -> sequencer_secondary (PID {follower.pid})")
            follower.kill()
            follower.wait()
            if Path(f"/proc/{follower.pid}").exists():
                die(f"follower restarts: sequencer_secondary (PID {follower.pid}) is still running after SIGKILL.")
            log("  sequencer_secondary confirmed dead")

            missed = f"missed-{run_tag}"
            accepted_from = _TEST_EVENTS.mark()
            inject_orders(order_ports["primary"], missed, 20)
            missed_ids = [f"{missed}-{number}" for number in range(1, 21)]
            for cl_ord_id in missed_ids:
                if not await_order_outcome(me_log.stem, cl_ord_id, "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                    die(f"follower restarts: the matching engine did not accept {cl_ord_id}, sent while the follower was down.")
            log(f"  the leader took {len(missed_ids)} orders while the follower was down, and the engine accepted them all")

            agreed_from = _TEST_EVENTS.mark()
            do_restart_step(RestartStep(proc_name="sequencer_secondary", ready_log_name="sequencer_secondary.log", ready=expect_started("sequencer_secondary"),
                                        ready_timeout=30.0, resets_me_counter=False, settle_secs=0.0),
                            proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix))
            if not await_expectation(expect_message("sequencer_primary", "the follower's WAL agrees", "WalsAgree", fields=lambda m: {"side": m.WalSide.Leader}), 30.0, agreed_from)[0]:
                die("follower restarts: the leader never reported that the restarted follower's log agrees with its own.")
            if not has_happened(expect_message("sequencer_secondary", "its WAL agrees with the leader's", "WalsAgree", fields=lambda m: {"side": m.WalSide.Follower}), 30.0):
                die("follower restarts: the restarted follower never reported that its log agrees with the leader's.")
            inject_orders(order_ports["primary"], f"after-{run_tag}")
            time.sleep(2.0)
            compared = compare_logs_after("follower restarts", wal_dirs["primary"], start_positions["primary"], wal_dirs["secondary"],
                                          start_positions["secondary"], stopped_at)
            log(f"  the restarted follower's log holds the same {compared} records as the leader's from record {stopped_at + 1}, with no gap -- OK")

            # The follower can now take over without losing anything the engine acted on.
            primary = proc_by_name["sequencer_primary"]
            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                die(f"follower restarts: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            if not await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)[0]:
                die(f"follower restarts: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            held = wal_records_after(wal_dirs["secondary"], stopped_at, start_positions["secondary"])
            leader_view = wal_records_after(wal_dirs["primary"], stopped_at, start_positions["primary"])
            lacking = [seq for seq in leader_view if seq not in held]
            if lacking:
                die(f"follower restarts: the new leader's log lacks {len(lacking)} records the old leader wrote while it was down, "
                    "including orders the matching engine accepted.")
            log("  the follower took the lead holding every record the old leader wrote while it was down, so every order the "
                "engine accepted is in the new leader's log -- OK")

        # ── Old leader rejoins ────────────────────────────────────────────────
        if scenario.assert_rejoining_old_leader_log_repaired:
            log("=== An old leader that rejoins discards the records its new leader does not have ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("old leader rejoins: sequencer_primary is not leading.")
            sequencer_etc = prefix / "etc" / "sequencer"
            configs = {which: sequencer_etc / f"sequencer_{which}.toml" for which in ("primary", "secondary")}
            order_ports = {which: installed_toml_section_value(config, "network", "listen_port") for which, config in configs.items()}
            wal_dirs = {which: config.parent / installed_toml_section_value(config, "wal", "directory") for which, config in configs.items()}
            run_tag = datetime.now().strftime("%H%M%S")

            def occurrences_in_log(wal_dir: Path, cl_ord_id: str) -> int:
                needle = cl_ord_id.encode()
                return sum(path.read_bytes().count(needle) for path in wal_dir.glob("wal_*.log"))

            time.sleep(1.0)
            start_positions = {which: wal_end_position(path) for which, path in wal_dirs.items()}
            agreed_through = max(wal_records_after(wal_dirs["secondary"], 0, start_positions["secondary"]) or {0: 0})

            block_flag.touch()
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                die("old leader rejoins: libblock_sends_to_ports.so never confirmed that it had begun blocking.")
            primary = proc_by_name["sequencer_primary"]
            only_old = f"onlyold-{run_tag}"
            injector = subprocess.Popen([str(bin_dir / "inject_order"), "--port", order_ports["primary"], "--cl-ord-id", only_old, "--count", "3"],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            sent_line = injector.stdout.readline() if injector.stdout is not None else ""
            only_old_ids = [f"{only_old}-{number}" for number in (1, 2, 3)]
            # The leader is stopped as soon as it has written the three orders, and before it can act on them.
            # "sent" means only that they are on their way, so stopping it at once raced the leader reading them.
            # It holds them at least the follower's acknowledgement timeout, 100 ms, before a voter can confirm
            # that it may act without the follower, so waiting up to 80 ms still stops it first. Only the newest
            # two segments are read, so that each look is quick.
            def written_by_leader() -> bool:
                newest = sorted(wal_dirs["primary"].glob("wal_*.log"))[-2:]
                data = b"".join(path.read_bytes() for path in newest)
                return all(cl_ord_id.encode() in data for cl_ord_id in only_old_ids)

            give_up_at = time.monotonic() + 0.080
            while not written_by_leader() and time.monotonic() < give_up_at:
                time.sleep(0.002)
            os.kill(primary.pid, signal.SIGSTOP)
            injector.wait(timeout=30)
            if not sent_line.startswith("sent "):
                die(f"old leader rejoins: inject_order did not report sending the orders: {sent_line!r}")
            time.sleep(1.0)
            if any(occurrences_in_log(wal_dirs["primary"], cl_ord_id) == 0 for cl_ord_id in only_old_ids):
                die("old leader rejoins: the leader's log does not hold the three orders sent to it, so it holds nothing its follower lacks.")
            log("  the leader wrote three orders its follower never received, and was stopped before it could act on them")

            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                die(f"old leader rejoins: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            if not await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)[0]:
                die(f"old leader rejoins: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            new_orders = f"newleader-{run_tag}"
            result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_ports["secondary"], "--cl-ord-id", new_orders, "--count", "5"],
                                    capture_output=True, text=True, check=False, timeout=60)
            if result.returncode != 0:
                die(f"old leader rejoins: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")
            log("  the follower took the lead and took five orders of its own, numbered where the old leader's three were")

            block_flag.unlink(missing_ok=True)
            discarded_from = _TEST_EVENTS.since_introduction("sequencer_primary")
            do_restart_step(RestartStep(proc_name="sequencer_primary", ready_log_name="sequencer_primary.log", ready=expect_started("sequencer_primary"),
                                        ready_timeout=30.0, resets_me_counter=False, settle_secs=0.0),
                            proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix))
            if not await_expectation(expect_message("sequencer_primary", "its WAL agrees with the leader's", "WalsAgree", fields=lambda m: {"side": m.WalSide.Follower}), 30.0, discarded_from)[0]:
                die("old leader rejoins: the restarted old leader never reported that its log agrees with the new leader's.")
            if not await_expectation(expect_message("sequencer_primary", "records the leader does not hold discarded", "RecordsDiscarded"), 5.0, discarded_from)[0]:
                die("old leader rejoins: the restarted old leader never reported discarding the records its new leader does not hold.")
            remaining = [cl_ord_id for cl_ord_id in only_old_ids if occurrences_in_log(wal_dirs["primary"], cl_ord_id) > 0]
            if remaining:
                die(f"old leader rejoins: the old leader's log still holds {remaining}, which its new leader never had.")
            log("  the old leader discarded the three records its new leader never had -- OK")
            result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_ports["secondary"], "--cl-ord-id", f"after-{run_tag}"],
                                    capture_output=True, text=True, check=False, timeout=60)
            if result.returncode != 0:
                die(f"old leader rejoins: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")
            time.sleep(2.0)
            compared = compare_logs_after("old leader rejoins", wal_dirs["secondary"], start_positions["secondary"], wal_dirs["primary"],
                                          start_positions["primary"], agreed_through)
            log(f"  the old leader's log now holds the same {compared} records as the new leader's from record {agreed_through + 1} -- OK")

        # ── Rule 11: a follower that is behind does not lead ──────────────────
        if scenario.assert_follower_behind_does_not_lead:
            log("=== A follower that is behind does not take the lead ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("rule 11: sequencer_primary is not leading.")
            sequencer_etc = prefix / "etc" / "sequencer"
            configs = {which: sequencer_etc / f"sequencer_{which}.toml" for which in ("primary", "secondary")}
            order_port = installed_toml_section_value(configs["primary"], "network", "listen_port")
            primary_wal = configs["primary"].parent / installed_toml_section_value(configs["primary"], "wal", "directory")
            run_tag = datetime.now().strftime("%H%M%S")
            behind = f"behind-{run_tag}"

            def send_order_to_leader(cl_ord_id: str) -> None:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_port, "--cl-ord-id", cl_ord_id],
                                        capture_output=True, text=True, check=False, timeout=30)
                if result.returncode != 0:
                    die(f"rule 11: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            # Let the follower catch up and the leader say it may lead, so that the statement below is a change.
            if not has_happened(expect_message("sequencer_primary", "saying the follower may lead again", "PeerMayLeadChanged", fields=lambda m: {"peer_may_lead": True}), _RAW_REPLY_TIMEOUT):
                die("rule 11: the leader never said its follower may lead, so it would never have said otherwise.")

            follower = proc_by_name["sequencer_secondary"]
            statement_from = _TEST_EVENTS.mark()
            accepted_from = _TEST_EVENTS.mark()
            os.kill(follower.pid, signal.SIGSTOP)
            log(f"  SIGSTOP -> sequencer_secondary (PID {follower.pid})")
            send_order_to_leader(behind)
            if not await_expectation(expect_message("sequencer_primary", "saying the follower may not lead", "PeerMayLeadChanged", fields=lambda m: {"peer_may_lead": False}), _RAW_REPLY_TIMEOUT, statement_from)[0]:
                die("rule 11: the leader never said the follower may not lead before acting on an order it lacks.")
            if not await_order_outcome(me_log.stem, behind, "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                die("rule 11: the matching engine never accepted the order, so the arbiter's confirmation never released it.")
            log("  the leader said the follower may not lead, the arbiter confirmed, and the engine accepted the order -- OK")

            if scenario.restart_arbiter_before_leader_dies:
                arbiter_from = _TEST_EVENTS.since_introduction("arbiter_primary")
                do_restart_step(RestartStep(proc_name="arbiter_primary", ready_log_name="arbiter_primary.log", ready=expect_started("arbiter_primary"),
                                            ready_timeout=30.0, resets_me_counter=False, settle_secs=0.0),
                                proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix))
                if not await_expectation(expect_message("arbiter_primary", "holds the statement that instance 2 may not lead", "StatementRecorded", fields=lambda m: {"instance_may_not_lead": 2}), 10.0, arbiter_from)[0]:
                    die("rule 11: the restarted arbiter did not report the recorded statement that instance 2 may not lead.")
                log("  the active arbiter restarted and still holds the statement that the follower may not lead -- OK")

            primary = proc_by_name["sequencer_primary"]
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                die(f"rule 11: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            takeover_from = _TEST_EVENTS.mark()
            os.kill(follower.pid, signal.SIGCONT)
            log(f"  SIGCONT -> sequencer_secondary (PID {follower.pid}); it lacks the order the engine acted on")
            follower_leads = leads(_TEST_EVENTS.messages, "sequencer_secondary")
            if _TEST_EVENTS.wait_for(follower_leads, timeout=2 * args.failover_timeout, since=takeover_from) is not None:
                die("rule 11: the follower took the lead although it lacks an order the matching engine acted on.")
            # Everything the follower did before answering has arrived by the time the answer does.
            if not _TEST_EVENTS.checkpoint("sequencer_secondary"):
                die(f"rule 11: the follower did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so it is not known that it did not lead.")
            if _TEST_EVENTS.find(follower_leads, since=takeover_from) is not None:
                die("rule 11: the follower took the lead although it lacks an order the matching engine acted on.")
            log(f"  the follower did not take the lead in {2 * args.failover_timeout:.0f}s, and it answered a checkpoint -- OK")

            leader_from = _TEST_EVENTS.since_introduction("sequencer_primary")
            do_restart_step(RestartStep(proc_name="sequencer_primary", ready_log_name="sequencer_primary.log", ready=expect_started("sequencer_primary"),
                                        ready_timeout=30.0, resets_me_counter=False, settle_secs=0.0),
                            proc_by_name, app_procs, launch_table, bin_dir, log_dir, venue_state_dir(prefix))
            if not has_led("sequencer_primary", args.failover_timeout * 2):
                die("rule 11: the restarted old leader did not take the lead back.")
            if behind.encode() not in b"".join(path.read_bytes() for path in primary_wal.glob("wal_*.log")):
                die("rule 11: the old leader's log does not hold the order the engine accepted.")
            log("  the old leader took the lead back, holding the order -- OK")
            if not await_expectation(expect_message("sequencer_primary", "saying the follower may lead again", "PeerMayLeadChanged", fields=lambda m: {"peer_may_lead": True}), 30.0, leader_from)[0]:
                die("rule 11: the follower caught up but the leader never said it may lead again.")
            log("  the follower caught up, and the leader says it may lead again -- OK")

        # ── Kept reports: the new leader forwards what the old one never did ──
        if scenario.assert_kept_reports_forwarded:
            log("=== Reports the leader never forwarded reach the member from the new leader ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("kept reports: sequencer_primary is not leading, so blocking its sends would not stop any report.")
            # The fix8 client logs on with the same comp id, and two sessions cannot share one.
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("kept reports: the raw client could not log on.")

            run_tag = datetime.now().strftime("%H%M%S")

            def reports_for(cl_ord_ids: list[str], timeout: float) -> dict[str, list[dict[int, str]]]:
                """Every ExecutionReport that names one of cl_ord_ids, received until the timeout runs out."""
                received: dict[str, list[dict[int, str]]] = {cl_ord_id: [] for cl_ord_id in cl_ord_ids}
                deadline = time.monotonic() + timeout
                while (remaining := deadline - time.monotonic()) > 0:
                    report = member.receive_until("8", timeout=min(1.0, remaining))
                    if report is not None and report.get(11) in received:
                        received[report[11]].append(report)
                return received

            # 1. Before anything is blocked, an order is answered, so the member's session does
            #    receive reports, and the silence in step 2 is the block and nothing else.
            before = f"kept-{run_tag}-before"
            member.new_order_single(before)
            if not reports_for([before], _RAW_REPLY_TIMEOUT)[before]:
                member.close()
                die("kept reports: an order sent before anything was blocked was never answered, so silence later would prove nothing.")
            log("  an order sent before the block is answered -- OK")

            # 2. Nothing the leader sends to a gateway arrives from now on, and the library says so.
            block_flag.touch()
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                member.close()
                die("kept reports: libblock_sends_to_ports.so never confirmed that it had begun blocking.")
            log("  the leader's sends to the gateways are now discarded, and the library has confirmed it -- OK")

            # 3. Orders the engine accepts, whose reports the leader cannot forward.
            kept = [f"kept-{run_tag}-{number}" for number in (1, 2, 3)]
            accepted_from = _TEST_EVENTS.mark()
            for cl_ord_id in kept:
                member.new_order_single(cl_ord_id)
            for cl_ord_id in kept:
                if not await_order_outcome(me_log.stem, cl_ord_id, "Accepted", _RAW_REPLY_TIMEOUT, accepted_from):
                    member.close()
                    die(f"kept reports: the matching engine never accepted {cl_ord_id}, so it sent no report to keep.")
            early = {c: r for c, r in reports_for(kept, 2.0).items() if r}
            if early:
                member.close()
                die(f"kept reports: the member was answered for {sorted(early)} while the leader's sends were blocked; only the "
                    "leader forwards reports, so the block is not doing what this scenario needs.")
            log(f"  the engine accepted all {len(kept)} orders and the member has heard nothing of them -- correct so far")

            # 4. The leader dies, and the follower takes the lead.
            primary = proc_by_name["sequencer_primary"]
            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                member.close()
                die(f"kept reports: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            log("  sequencer_primary confirmed dead")
            found, elapsed = await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)
            if not found:
                member.close()
                die(f"kept reports: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            log(f"  sequencer_secondary took the lead {elapsed:.1f}s after the kill")

            # 5. The requirement: every order's acceptance reaches the member, marked PossResend.
            received = reports_for(kept, _RAW_REPLY_TIMEOUT)
            member.close()
            for cl_ord_id, reports in received.items():
                for report in reports:
                    log(f"    {cl_ord_id}: OrdStatus={report.get(39)} ExecType={report.get(150)} PossResend={report.get(97, 'absent')}")
            unanswered = [c for c, reports in received.items() if not any(r.get(39) == "0" for r in reports)]
            if unanswered:
                die(f"kept reports: the member was never told {unanswered} were accepted. The engine accepted them and the "
                    "follower received the reports, so the new leader must forward them (section 4.4 of "
                    "docs/availability/change_of_sequencer_leader.md).")
            unmarked = [c for c, reports in received.items() if any(r.get(39) == "0" and r.get(97) != "Y" for r in reports)]
            if unmarked:
                die(f"kept reports: the acceptance of {unmarked} arrived without PossResend. The new leader cannot tell whether "
                    "the old leader forwarded it, so a member that already had it would read it as a second event (R-0122).")
            log(f"  the member was told of all {len(kept)} acceptances, each marked PossResend -- OK")

        # ── Logged commands reach the engine ──────────────────────────────────
        if scenario.assert_logged_commands_reach_engine:
            log("=== Orders the new leader's log holds, and the matching engine never received, are applied ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("logged commands: sequencer_primary is not leading.")
            secondary_config = prefix / "etc" / "sequencer" / "sequencer_secondary.toml"
            secondary_wal = secondary_config.parent / installed_toml_section_value(secondary_config, "wal", "directory")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            # Each order must leave at once. With Nagle's algorithm on, the second and third small
            # writes wait for the first to be acknowledged, up to 40 ms, and the leader is stopped
            # 20 ms after the orders are sent.
            if member.sock is not None:
                member.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("logged commands: the raw client could not log on.")
            run_tag = datetime.now().strftime("%H%M%S")

            def acceptances_for(cl_ord_ids: list[str], timeout: float) -> dict[str, list[dict[int, str]]]:
                """Every acceptance (OrdStatus=0) naming one of cl_ord_ids, received until the timeout runs out."""
                received: dict[str, list[dict[int, str]]] = {cl_ord_id: [] for cl_ord_id in cl_ord_ids}
                deadline = time.monotonic() + timeout
                while (remaining := deadline - time.monotonic()) > 0:
                    report = member.receive_until("8", timeout=min(1.0, remaining))
                    if report is not None and report.get(11) in received and report.get(39) == "0":
                        received[report[11]].append(report)
                return received

            def times_in_follower_log(cl_ord_id: str) -> int:
                needle = cl_ord_id.encode()
                return sum(path.read_bytes().count(needle) for path in secondary_wal.glob("wal_*.log"))

            # 1. Before anything is blocked, an order is answered, so the session works.
            before = f"logged-{run_tag}-before"
            member.new_order_single(before)
            if not acceptances_for([before], _RAW_REPLY_TIMEOUT)[before]:
                member.close()
                die("logged commands: an order sent before anything was blocked was never accepted.")
            log("  an order sent before the block is accepted -- OK")

            # 2. Nothing the follower sends to the leader arrives from now on, and the library says so.
            block_flag.touch()
            if not await_blocking(block_flag, _RAW_REPLY_TIMEOUT):
                member.close()
                die("logged commands: libblock_sends_to_ports.so never confirmed that it had begun blocking the follower.")
            log("  the follower's sends to the leader are now discarded, and the library has confirmed it -- OK")

            # 3. Orders the leader holds, waiting for acknowledgements that cannot arrive. It is stopped
            #    well inside its 100 ms wait, so it cannot run as if alone and send them to the engine.
            primary = proc_by_name["sequencer_primary"]
            logged = [f"logged-{run_tag}-{number}" for number in (1, 2, 3)]
            accepted_from = _TEST_EVENTS.mark()
            for cl_ord_id in logged:
                member.new_order_single(cl_ord_id)
            time.sleep(0.02)
            os.kill(primary.pid, signal.SIGSTOP)
            with open(f"/proc/{primary.pid}/stat", encoding="ascii") as stat:
                state = stat.read().rsplit(")", 1)[1].split()[0]
            if state != "T":
                member.close()
                die(f"logged commands: sequencer_primary is in state {state!r} after SIGSTOP, not stopped.")
            log(f"  {len(logged)} orders sent, and the leader stopped 20 ms later")
            time.sleep(1.0)

            # 4. The case this scenario exists for: the follower holds the orders, the engine none of them.
            missing = [c for c in logged if times_in_follower_log(c) == 0]
            if missing:
                member.close()
                die(f"logged commands: the follower's log does not hold {missing}, so this run is not the case being tested "
                    "(the leader may have been stopped before it replicated them).")
            early = [c for c in logged if settled_order_outcomes(me_log.stem, accepted_from, c, "Accepted") > 0]
            if early:
                member.close()
                die(f"logged commands: the engine already accepted {early} before the leader was killed, so this run is not the "
                    "case being tested.")
            log("  the follower's log holds all the orders and the engine has applied none of them -- correct so far")

            # 5. The leader dies, and the follower takes the lead.
            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                member.close()
                die(f"logged commands: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            log("  sequencer_primary confirmed dead")
            found, elapsed = await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)
            if not found:
                member.close()
                die(f"logged commands: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            log(f"  sequencer_secondary took the lead {elapsed:.1f}s after the kill")

            # 6. The requirement: each order applied once, and its acceptance reaches the member.
            received = acceptances_for(logged, _RAW_REPLY_TIMEOUT)
            member.close()
            unanswered = [c for c, reports in received.items() if not reports]
            if unanswered:
                die(f"logged commands: the member was never told {unanswered} were accepted. The new leader's log holds them and "
                    "nothing sent them to the matching engine (section 3.5 of docs/availability/commands_during_a_change_of_leader.md).")
            applied_twice = [c for c in logged if settled_order_outcomes(me_log.stem, accepted_from, c, "Accepted") != 1]
            if applied_twice:
                die(f"logged commands: the engine did not accept each of {applied_twice} exactly once.")
            log(f"  the engine applied each of the {len(logged)} orders once, and the member was told of each -- OK")

        # ── Commands sent again ───────────────────────────────────────────────
        if scenario.assert_sent_again_sequenced_once:
            log("=== A command sent again is sequenced only if the leader's log does not already hold it ===")
            leader_name = "sequencer_primary"
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("sent again: sequencer_primary is not leading.")
            primary_config = prefix / "etc" / "sequencer" / "sequencer_primary.toml"
            order_port = installed_toml_section_value(primary_config, "network", "listen_port")
            # How long to wait for the engine to act on an order: longer after a change of leader, when the new
            # leader first asks the engine how far it has got.
            engine_wait = _RAW_REPLY_TIMEOUT
            run_tag = datetime.now().strftime("%H%M%S")

            def inject_sent_again_test(cl_ord_id: str, *extra: str) -> None:
                result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_port, "--cl-ord-id", cl_ord_id, *extra],
                                        capture_output=True, text=True, check=False, timeout=30)
                if result.returncode != 0:
                    die(f"sent again: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")

            def accepted_by_engine(cl_ord_id: str, since: int) -> int:
                return settled_order_outcomes(me_log.stem, since, cl_ord_id, "Accepted")

            def refused_as_duplicate(cl_ord_id: str, since: int) -> int:
                return settled_order_outcomes(me_log.stem, since, cl_ord_id, "RefusedDuplicate")

            started_from = _TEST_EVENTS.mark()
            held = f"again-{run_tag}-held"
            inject_sent_again_test(held)
            if not await_order_outcome(me_log.stem, held, "Accepted", _RAW_REPLY_TIMEOUT, started_from):
                die("sent again: the matching engine never accepted the first order.")
            log("  an order is sent and accepted")

            if scenario.sent_again_after_change_of_leader:
                # The leader sends an order to the engine only once its follower has acknowledged it, so the
                # engine's acceptance means the follower holds it.
                follower_from = _TEST_EVENTS.mark()
                old_leader = proc_by_name.get("sequencer_primary")
                if old_leader is None or old_leader.poll() is not None:
                    die("sent again: sequencer_primary is not running, so it cannot be killed.")
                log(f"  SIGKILL -> sequencer_primary (PID {old_leader.pid}), which holds the order and has replicated it")
                old_leader.kill()
                old_leader.wait()
                if not await_expectation(expect_leads("sequencer_secondary"), _RESEND_TIMEOUT, follower_from)[0]:
                    die("sent again: sequencer_secondary did not take the lead after sequencer_primary was killed.")
                log("  sequencer_secondary leads; the order is in its log only because its leader replicated it")
                leader_name = "sequencer_secondary"
                secondary_config = prefix / "etc" / "sequencer" / "sequencer_secondary.toml"
                order_port = installed_toml_section_value(secondary_config, "network", "listen_port")
                engine_wait = _RESEND_TIMEOUT

            check_from = _TEST_EVENTS.mark()
            inject_sent_again_test(held, "--sent-again")
            never_sent = f"again-{run_tag}-new"
            inject_sent_again_test(never_sent, "--sent-again")
            if not await_order_outcome(me_log.stem, never_sent, "Accepted", engine_wait, started_from):
                die(f"sent again: the order {never_sent}, marked as sent again but never sent before, was not applied. Nothing ever "
                    "acted on it, so it must be sequenced as a new command.")
            time.sleep(1.0)
            if refused_as_duplicate(held, started_from) or accepted_by_engine(held, started_from) != 1:
                die(f"sent again: the order {held} was sequenced a second time when sent again -- the engine accepted it "
                    f"{accepted_by_engine(held, started_from)} time(s) and refused it as a duplicate {refused_as_duplicate(held, started_from)} time(s). "
                    "The leader's log already held it, so it must not be sequenced twice.")
            if accepted_by_engine(never_sent, started_from) != 1:
                die(f"sent again: the engine accepted {never_sent} {accepted_by_engine(never_sent, started_from)} times, not once.")
            if not await_expectation(expect_message(leader_name, "checking the end of the WAL for repeats", "CheckingLogForRepeats", "sequencer_test_events"), 1.0, check_from)[0]:
                die("sent again: the leader never built its index of the end of the log, so the check above did not run as designed.")
            log("  the order sent again was not sequenced twice, and the order sent again for the first time was applied once -- OK")

        # ── Orders during a change of leader ──────────────────────────────────
        if scenario.assert_orders_during_change_answered:
            log("=== Every order sent while the sequencers change leader is answered, and none twice ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("orders during change: sequencer_primary is not leading.")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel
            import threading  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            if member.sock is not None:
                member.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("orders during change: the raw client could not log on.")
            run_tag = datetime.now().strftime("%H%M%S")

            # Reports are read on a thread of their own while orders are sent, so that the gateway
            # is never left with a member that has stopped reading.
            answers: dict[str, list[str]] = {}
            stop_reading = threading.Event()

            def read_reports() -> None:
                while not stop_reading.is_set():
                    report = member.receive_until("8", timeout=0.5)
                    if report is not None and report.get(11, "").startswith(f"during-{run_tag}-"):
                        answers.setdefault(report[11], []).append(report.get(39, "?"))

            reader = threading.Thread(target=read_reports, name="report-reader", daemon=True)
            reader.start()

            sent: list[str] = []
            interval = 0.01  # 100 orders a second

            def send_for(seconds: float) -> None:
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    cl_ord_id = f"during-{run_tag}-{len(sent) + 1}"
                    member.new_order_single(cl_ord_id)
                    sent.append(cl_ord_id)
                    time.sleep(interval)

            accepted_from = _TEST_EVENTS.mark()
            send_for(1.0)
            primary = proc_by_name["sequencer_primary"]
            takeover_from = _TEST_EVENTS.mark()
            log(f"  {len(sent)} orders sent; SIGKILL -> sequencer_primary (PID {primary.pid}), still sending")
            primary.kill()
            sent_before_kill = len(sent)
            # Keep sending while the follower takes the lead: these are the orders at risk.
            took_over = False
            waited = 0.0
            while not took_over and waited < args.failover_timeout:
                send_for(0.5)
                waited += 0.5
                took_over = await_expectation(expect_leads("sequencer_secondary"), 0.01, takeover_from)[0]
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                stop_reading.set()
                member.close()
                die(f"orders during change: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            if not took_over:
                stop_reading.set()
                member.close()
                die(f"orders during change: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            sent_during = len(sent) - sent_before_kill
            send_for(2.0)
            log(f"  sequencer_secondary took the lead; {sent_during} orders were sent between the kill and the takeover, {len(sent)} in all")

            # Every order must be answered. Wait until it is, or the time runs out.
            deadline = time.monotonic() + 20.0
            while time.monotonic() < deadline and any(c not in answers for c in sent):
                time.sleep(0.5)
            stop_reading.set()
            reader.join(timeout=5)
            member.close()

            unanswered = [c for c in sent if c not in answers]
            log(f"  {len(sent) - len(unanswered)} of {len(sent)} orders answered")
            if unanswered:
                die(f"orders during change: {len(unanswered)} of the {len(sent)} orders were never answered, for example "
                    f"{unanswered[:5]}. The gateway must keep each command until it is answered and send it again to the new "
                    "leader (section 3 of docs/availability/commands_during_a_change_of_leader.md, BUG-0103).")
            applied_twice = [c for c in sent if settled_order_outcomes(me_log.stem, accepted_from, c, "Accepted") > 1]
            if applied_twice:
                die(f"orders during change: the matching engine accepted {applied_twice[:5]} more than once.")
            log(f"  every one of the {len(sent)} orders was answered, and none was applied twice -- OK")

        # ── Few repeats after a change of leader ──────────────────────────────
        if scenario.assert_few_repeats_after_change:
            log("=== After a change of leader, a member is sent again only the reports the old leader had not forwarded ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("few repeats: sequencer_primary is not leading.")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("few repeats: the raw client could not log on.")
            run_tag = datetime.now().strftime("%H%M%S")

            def collect(cl_ord_ids: set[str], timeout: float) -> list[dict[int, str]]:
                """Every report naming one of cl_ord_ids, until each has an acceptance or the time runs out."""
                reports: list[dict[int, str]] = []
                accepted: set[str] = set()
                deadline = time.monotonic() + timeout
                while (remaining := deadline - time.monotonic()) > 0 and accepted != cl_ord_ids:
                    report = member.receive_until("8", timeout=min(1.0, remaining))
                    if report is not None and report.get(11) in cl_ord_ids:
                        reports.append(report)
                        if report.get(39) == "0":
                            accepted.add(report[11])
                return reports

            burst = {f"repeat-{run_tag}-{number}" for number in range(1, 1001)}
            for cl_ord_id in sorted(burst):
                member.new_order_single(cl_ord_id)
            got = collect(burst, 30.0)
            if len({r[11] for r in got if r.get(39) == "0"}) != len(burst):
                member.close()
                die("few repeats: not every order of the burst was accepted.")
            time.sleep(0.5)
            last = f"repeat-{run_tag}-last"
            member.new_order_single(last)
            if not collect({last}, _RAW_REPLY_TIMEOUT):
                member.close()
                die("few repeats: the last order was not accepted.")
            log(f"  {len(burst)} orders accepted, and one more half a second later")

            primary = proc_by_name["sequencer_primary"]
            takeover_from = _TEST_EVENTS.mark()
            log(f"  SIGKILL -> sequencer_primary (PID {primary.pid})")
            primary.kill()
            primary.wait()
            if Path(f"/proc/{primary.pid}").exists():
                member.close()
                die(f"few repeats: sequencer_primary (PID {primary.pid}) is still running after SIGKILL.")
            found, elapsed = await_expectation(expect_leads("sequencer_secondary"), args.failover_timeout, takeover_from)
            if not found:
                member.close()
                die(f"few repeats: sequencer_secondary did not take the lead within {args.failover_timeout:.0f}s.")
            log(f"  sequencer_secondary took the lead {elapsed:.1f}s after the kill")

            # Everything the member is sent for these orders after the change of leader.
            repeats: list[dict[int, str]] = []
            deadline = time.monotonic() + 5.0
            while (remaining := deadline - time.monotonic()) > 0:
                report = member.receive_until("8", timeout=min(1.0, remaining))
                if report is not None and report.get(11, "").startswith(f"repeat-{run_tag}-"):
                    repeats.append(report)
            member.close()
            marked = [r for r in repeats if r.get(97) == "Y"]
            unmarked = [r for r in repeats if r.get(97) != "Y"]
            kept = component_event("sequencer_secondary", "sequencer_test_events", "KeptReportsForwarded", takeover_from, 0.0, settle=True)
            forwarded = kept.forwarded if kept is not None else -1
            log(f"  the new leader forwarded {forwarded} kept report(s); the member received {len(marked)} repeat(s) marked PossResend")
            if unmarked:
                die(f"few repeats: the member received {len(unmarked)} report(s) for these orders after the change of leader without PossResend.")
            if forwarded < 0:
                die("few repeats: the new leader did not say how many kept reports it forwarded.")
            if forwarded > 10 or len(marked) > 10:
                die(f"few repeats: the new leader forwarded {forwarded} kept report(s) and the member received {len(marked)} repeats. The old "
                    "leader's records said how far it had forwarded, so only the reports it had not forwarded should be sent again (BUG-0116).")
            log("  only the few reports the old leader had not said it forwarded were sent again -- OK")

        # ── Reports produced while unbound ────────────────────────────────────
        if scenario.assert_unbound_reports_delivered:
            log("=== A report produced while a member is disconnected is delivered when it reconnects ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("unbound reports: sequencer_primary is not leading.")
            primary_config = prefix / "etc" / "sequencer" / "sequencer_primary.toml"
            order_port = installed_toml_section_value(primary_config, "network", "listen_port")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            run_tag = datetime.now().strftime("%H%M%S")
            resting = f"unbound-{run_tag}-order"
            cancel = f"unbound-{run_tag}-cancel"

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("unbound reports: the raw client could not log on.")
            member.new_order_single(resting)
            accepted = None
            deadline = time.monotonic() + _RAW_REPLY_TIMEOUT
            while accepted is None and time.monotonic() < deadline:
                report = member.receive_until("8", timeout=1.0)
                if report is not None and report.get(11) == resting and report.get(39) == "0":
                    accepted = report
            if accepted is None:
                member.close()
                die("unbound reports: the order was not accepted.")
            next_send = member.next_send_seq_num
            unbound_from = _TEST_EVENTS.mark()
            member.close()
            if not await_expectation(expect_message("sequencer_primary", "the member's session recorded as unbound", "SessionUnboundRecorded", "sequencer_test_events", fields=lambda m: {"comp_id": FIX8_COMP_ID}), _RAW_REPLY_TIMEOUT, unbound_from)[0]:
                die("unbound reports: the sequencer never recorded the member's session as unbound.")
            log("  the member placed an order and disconnected, and its session is unbound")

            # The order is cancelled while the member is away: the engine's report has no connection to go to.
            cancelled_from = _TEST_EVENTS.mark()
            result = subprocess.run([str(bin_dir / "inject_order"), "--port", order_port, "--comp-id", FIX8_COMP_ID, "--protocol", "1",
                                     "--cl-ord-id", cancel, "--cancel", resting, "--symbol", "BHP"],
                                    capture_output=True, text=True, check=False, timeout=30)
            if result.returncode != 0:
                die(f"unbound reports: inject_order failed (exit {result.returncode}):\n{result.stdout}{result.stderr}")
            # The member is away, so the engine's event is what shows the cancel was done before it returns.
            if not await_order_outcome(me_log.stem, cancel, "Cancelled", _RAW_REPLY_TIMEOUT, cancelled_from):
                die("unbound reports: the matching engine did not cancel the order, so there is no report to deliver.")
            log("  the order was cancelled while the member was away")

            # The member comes back, continuing its numbering, and must be told without asking.
            delivered_from = _TEST_EVENTS.mark()
            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.next_send_seq_num = next_send
            member.connect()
            member.logon(reset_seq_num=False)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("unbound reports: the raw client could not log on again.")
            cancelled = None
            deadline = time.monotonic() + _RAW_REPLY_TIMEOUT
            while cancelled is None and time.monotonic() < deadline:
                report = member.receive_until("8", "9", timeout=1.0)
                if report is not None and report.get(11) == cancel:
                    cancelled = report
            member.close()
            if cancelled is None:
                die("unbound reports: the member reconnected and was never sent the report cancelling its order, produced while it "
                    "was away. R-0005 requires it to be delivered when the session binds again (BUG-0088).")
            log(f"  on reconnecting the member was sent the cancel report: OrdStatus={cancelled.get(39)} OrigClOrdID={cancelled.get(41)}")
            if not await_expectation(expect_message("sequencer_primary", "the one report produced while the member was away sent", "UndeliveredReportsSent", "sequencer_test_events", fields=lambda m: {"comp_id": FIX8_COMP_ID, "count": 1}), 2.0, delivered_from)[0]:
                die("unbound reports: the sequencer did not report sending the one report produced while the session had no connection.")
            log("  the sequencer sent exactly the one report produced while the member was away -- OK")

        # ── Commands sent again after a reconnection ──────────────────────────
        if scenario.assert_commands_resent_after_reconnection:
            log("=== Commands discarded with a closed connection to the leader are sent again when it reconnects ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("reconnection: sequencer_primary is not leading.")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("reconnection: the raw client could not log on.")

            sequencer = proc_by_name["sequencer_primary"]
            gateway_from = _TEST_EVENTS.mark()
            gateway_lost_sequencer = expect_message("fix_order_gateway_a", "its connection to the primary sequencer closed", "ConnectionChanged",
                                                    fields=lambda m: {"service": "sequencer_primary", "made": False})
            secondary_from = _TEST_EVENTS.mark()
            run_tag = datetime.now().strftime("%H%M%S")
            sent: list[str] = []
            os.kill(sequencer.pid, signal.SIGSTOP)
            stopped_at = time.monotonic()
            if not process_is_stopped(sequencer.pid):
                os.kill(sequencer.pid, signal.SIGCONT)
                die("reconnection: sequencer_primary did not stop after SIGSTOP.")
            log(f"  sequencer_primary (PID {sequencer.pid}) stopped")

            # Orders until the gateway closes its connection to the stopped sequencer, checking now and then.
            closed = False
            while len(sent) < 40000 and time.monotonic() - stopped_at < 1.0:
                for _ in range(250):
                    cl_ord_id = f"recon-{run_tag}-{len(sent) + 1}"
                    member.new_order_single(cl_ord_id)
                    sent.append(cl_ord_id)
                # The sequencer is stopped, not dead, so its socket stays open: a connection to it that closes now was closed
                # by the gateway, for not reading what it is sent.
                if await_expectation(gateway_lost_sequencer, 0.0, gateway_from)[0]:
                    closed = True
                    break
            if not closed:
                closed = await_expectation(gateway_lost_sequencer, 0.3, gateway_from)[0]
            # Running again well inside its lease, so that it still leads.
            os.kill(sequencer.pid, signal.SIGCONT)
            stopped_for = time.monotonic() - stopped_at
            if process_is_stopped(sequencer.pid):
                die("reconnection: sequencer_primary did not run again after SIGCONT.")
            log(f"  sequencer_primary running again after {stopped_for:.2f}s; {len(sent)} order(s) sent while it was stopped")
            if not closed:
                member.close()
                die(f"reconnection: {len(sent)} orders did not fill gateway a's queue for the stopped sequencer, so the connection "
                    "was never closed and nothing here was tested.")
            log("  gateway a closed its connection to the stopped sequencer when the queue was full -- OK")

            # Read the member's reports all the while from here, because gateway a's small limit on
            # waiting sends applies to the member's connection too: a member that stopped reading while
            # the reports for the first orders arrived would have its own connection closed.
            accepted: dict[str, int] = {}
            refused: list[str] = []

            def take_reports(timeout: float) -> None:
                for report in member.receive(timeout=timeout):
                    if report.get(35) == "8" and report.get(39) == "0":
                        accepted[report.get(11)] = accepted.get(report.get(11), 0) + 1
                    elif report.get(35) == "8" and report.get(39) == "8":
                        refused.append(report.get(11))

            deadline = time.monotonic() + 15.0
            resent = False
            while not resent and time.monotonic() < deadline:
                take_reports(0.1)
                resent = await_expectation(expect_connection_made("fix_order_gateway_a", "sequencer_primary"), 0.0, gateway_from)[0]
            if not resent:
                member.close()
                die("reconnection: gateway a did not connect to the primary sequencer again and send its unanswered commands again.")
            log("  gateway a connected again and sent its unanswered orders again")

            # Every order accepted, and none twice or refused; and a little longer, for a second
            # acceptance or a refusal of a duplicate arriving after the last acceptance.
            deadline = time.monotonic() + 60.0
            while len(accepted) < len(sent) and time.monotonic() < deadline:
                take_reports(0.5)
            settle_until = time.monotonic() + 2.0
            while time.monotonic() < settle_until:
                take_reports(0.2)
            # Every member connection the gateway lost before the test closes its own, on gateway a's member listener.
            member_listener = f"inbound:{gateway_listen_port(prefix, 'a')}"
            if not _TEST_EVENTS.settle("fix_order_gateway_a"):
                die(f"reconnection: gateway a did not answer a checkpoint ({_TEST_EVENTS.failures[-1]})")
            before_close = _TEST_EVENTS.mark()
            member.close()
            member_lost = message_of(_TEST_EVENTS.messages.ConnectionChanged, "fix_order_gateway_a", service=member_listener, made=False)
            if any(member_lost(event) for event in _TEST_EVENTS.events[gateway_from:before_close]):
                die("reconnection: gateway a closed the member's own connection during the test, so the reports had nowhere to go "
                    "and the test cannot tell whether the orders were sent again.")
            missing = [c for c in sent if c not in accepted]
            twice = [c for c, n in accepted.items() if n > 1]
            if missing:
                die(f"reconnection: {len(missing)} of {len(sent)} orders were never accepted, for example {missing[:3]}. They were "
                    "discarded with the closed connection and not sent again (BUG-0118).")
            if twice or refused:
                die(f"reconnection: orders were acted on twice: {len(twice)} accepted twice, {len(refused)} refused, for example "
                    f"{(twice + refused)[:3]}. A command sent again must be sequenced only if the log does not already hold it.")
            if expectation_seen(expect_leads("sequencer_secondary"), secondary_from):
                die("reconnection: sequencer_secondary took the lead, so the orders may have been sent again for the change of "
                    "leader rather than for the reconnection, which is what this scenario tests.")
            log(f"  all {len(sent)} orders accepted exactly once, none refused, and sequencer_primary kept the lead -- OK")

        # ── Reports held without a connection ─────────────────────────────────
        if scenario.assert_engine_holds_reports_without_connection:
            log("=== Orders reaching an engine with no connection for reports are acted on and reported later ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("held reports: sequencer_primary is not leading.")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("held reports: the raw client could not log on.")

            outcomes_from = _TEST_EVENTS.mark()
            run_tag = datetime.now().strftime("%H%M%S")
            sent = [f"held-{run_tag}-{number}" for number in range(1, 51)]
            for cl_ord_id in sent:
                member.new_order_single(cl_ord_id)
            if not all(await_order_outcome(me_log.stem, cl_ord_id, "Accepted", 5.0, outcomes_from) for cl_ord_id in sent):
                member.close()
                die("held reports: the matching engine dropped orders because it had no connection for reports. They are in the "
                    "sequencer's log, so the engine must act on them and report them later (BUG-0120).")
            if engine_event(me_log.stem, "ReportsHeld", outcomes_from, 5.0) is None:
                member.close()
                die("held reports: the matching engine never said it was holding reports, so it is not known to have had no "
                    "connection for reports, and nothing here was tested.")
            early = [m for m in member.receive(timeout=1.0) if m.get(35) == "8"]
            if early:
                member.close()
                die(f"held reports: {len(early)} report(s) reached the member while the engine had no connection for reports, so "
                    "the relay did not keep the connections away and nothing here was tested.")
            log(f"  {len(sent)} orders sent; the engine is acting on them and holding their reports")

            relay = PortRelay(_REPORT_RELAY_PORTS)
            relay.start()
            try:
                if engine_event(me_log.stem, "ReportsReleased", outcomes_from, 15.0) is None:
                    member.close()
                    die("held reports: once its connections for reports were let through, the engine did not send the reports it held.")
                log("  the relay let the engine's connections through, and it sent the reports it held")
                accepted: dict[str, int] = {}
                deadline = time.monotonic() + 20.0
                while len(accepted) < len(sent) and time.monotonic() < deadline:
                    for report in member.receive(timeout=0.5):
                        if report.get(35) == "8" and report.get(39) == "0":
                            accepted[report.get(11)] = accepted.get(report.get(11), 0) + 1
                for report in member.receive(timeout=1.0):
                    if report.get(35) == "8" and report.get(39) == "0":
                        accepted[report.get(11)] = accepted.get(report.get(11), 0) + 1
                member.close()
            finally:
                relay.stop()
            missing = [c for c in sent if c not in accepted]
            twice = [c for c, n in accepted.items() if n > 1]
            if missing:
                die(f"held reports: {len(missing)} of {len(sent)} orders were never reported accepted, for example {missing[:3]}.")
            if twice:
                die(f"held reports: {len(twice)} orders were reported accepted more than once, for example {twice[:3]}.")
            log(f"  all {len(sent)} orders reported accepted exactly once -- OK")

        # ── Engines whose settings differ ─────────────────────────────────────
        # Phase 7 step 2. The two engines exchange their settings when they connect, and the one that leads, finding its
        # peer's region capacity differs from its own, says through its lease statements that the peer may not lead. The
        # arbiter records the statement, so the peer is refused a lease even once the leader has gone.
        if scenario.assert_engines_with_different_settings:
            log("=== Engines whose settings differ: the leader bars its peer ===")
            engine_events = "matching_engine_test_events"
            leader = resolved(ENGINE_LEADING_AT_START)
            peer = resolved(ENGINE_FOLLOWING_AT_START)
            if not has_led(leader, _ME_LEADER_TIMEOUT):
                die(f"different settings: {leader} is not leading.")
            barred = component_event(leader, engine_events, "SettingsDifferFromPeer", _TEST_EVENTS.since_introduction(leader), 30.0, barred_peer=True)
            if barred is None:
                die(f"different settings: {leader} leads but never reported that its peer's settings differ and that it barred it.")
            if scenario.different_identifier_secret:
                if not barred.secrets_differ or barred.own_region_capacity != barred.peer_region_capacity:
                    die(f"different settings: {leader} reported secrets_differ={barred.secrets_differ} and capacities "
                        f"{barred.own_region_capacity} and {barred.peer_region_capacity}, where only the secrets were made to differ.")
                log(f"  {leader} leads, its peer holds a different identifier secret, and it has said its peer may not lead -- OK")
            else:
                capacities = (barred.own_region_capacity, barred.peer_region_capacity)
                if _SMALLER_REGION_CAPACITY not in capacities or capacities[0] == capacities[1] or barred.secrets_differ:
                    die(f"different settings: {leader} reported capacities {barred.own_region_capacity} and {barred.peer_region_capacity} and "
                        f"secrets_differ={barred.secrets_differ}, where only the capacity was made to differ, to {_SMALLER_REGION_CAPACITY} "
                        "on the secondary.")
                log(f"  {leader} leads holding up to {barred.own_region_capacity:,} orders, its peer up to {barred.peer_region_capacity:,}, "
                    "and it has said its peer may not lead -- OK")

            leader_process = proc_by_name[leader]
            log(f"  SIGKILL -> {leader} (PID {leader_process.pid})")
            leader_process.kill()
            leader_process.wait()
            if Path(f"/proc/{leader_process.pid}").exists():
                die(f"different settings: {leader} (PID {leader_process.pid}) is still running after SIGKILL.")
            takeover_from = _TEST_EVENTS.mark()
            peer_leads = leads(_TEST_EVENTS.messages, peer)
            if _TEST_EVENTS.wait_for(peer_leads, timeout=2 * args.failover_timeout, since=takeover_from) is not None:
                die(f"different settings: {peer} took the lead although the leader said it may not, its settings differing.")
            if not _TEST_EVENTS.checkpoint(peer):
                die(f"different settings: {peer} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]}), so it is not known that it did not lead.")
            if _TEST_EVENTS.find(peer_leads, since=takeover_from) is not None:
                die(f"different settings: {peer} took the lead although the leader said it may not, its settings differing.")
            log(f"  {peer} did not take the lead in {2 * args.failover_timeout:.0f}s, and it answered a checkpoint -- OK")

            # Restarted with the configuration it was started with. When matching_engine_secondary leads at the start, the
            # leader is the engine with the smaller region, and the deployed configuration would make the two alike.
            as_launched = [(name, bin_name, launched_configs.get(name, config)) for name, bin_name, config in launch_table]
            do_restart_step(RestartStep(proc_name=leader, ready_log_name=f"{leader}.log", ready=expect_started(leader),
                                        ready_timeout=30.0, resets_me_counter=False, settle_secs=0.0),
                            proc_by_name, app_procs, as_launched, bin_dir, log_dir, venue_state_dir(prefix))
            if not has_led(leader, args.failover_timeout * 2):
                die(f"different settings: the restarted {leader} did not take the lead back, so no engine leads.")
            if component_event(leader, engine_events, "SettingsDifferFromPeer", _TEST_EVENTS.since_introduction(leader), 30.0, barred_peer=True) is None:
                die(f"different settings: the restarted {leader} leads, but did not bar its peer again.")
            log(f"  the restarted {leader} leads again and has said again that its peer may not lead -- OK")

        # ── Reports held for a gateway with no connection ─────────────────────
        # BUG-0202. The leading sequencer has no connection for reports to the gateway, because the relay accepts nothing,
        # while the gateway's connection to it for orders is up, so the member's orders are sequenced and answered by the
        # engine. The sequencer must hold the reports, and send every one once the connection is made.
        if scenario.assert_reports_held_for_unconnected_gateway:
            log("=== Reports for a gateway the leading sequencer has no connection for reports to are held and sent later ===")
            if not has_led("sequencer_primary", _RAW_REPLY_TIMEOUT):
                die("gateway reports: sequencer_primary is not leading.")
            if f8proc is not None:
                stop_f8test(f8proc)
                f8proc = None
                time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            member = FixRawClient("127.0.0.1", gateway_listen_port(prefix, "a"), FIX8_COMP_ID, "GATEWAY", FIX8_PASSWORD)
            member.connect()
            member.logon(reset_seq_num=True)
            if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                member.close()
                die("gateway reports: the raw client could not log on.")

            events_from = _TEST_EVENTS.mark()
            sequencer_events = "sequencer_test_events"
            run_tag = datetime.now().strftime("%H%M%S")
            sent = [f"gwheld-{run_tag}-{number}" for number in range(1, 51)]
            for cl_ord_id in sent:
                member.new_order_single(cl_ord_id)
            held = component_event("sequencer_primary", sequencer_events, "ReportsHeldForGateway", events_from, 10.0, gateway_protocol=1, gateway_instance=1)
            if held is None:
                member.close()
                die("gateway reports: sequencer_primary never said it was holding reports for fix_order_gateway_a. Either it had a "
                    "connection for reports after all, so nothing here was tested, or it dropped the reports (BUG-0202).")
            dropped = count_seen(expect_message("sequencer_primary", "a report dropped for a gateway not connected", "ReportDroppedGatewayNotConnected",
                                                sequencer_events), events_from)
            if dropped > 0:
                member.close()
                die(f"gateway reports: sequencer_primary dropped {dropped} report(s) for fix_order_gateway_a instead of holding them (BUG-0202).")
            early = [m for m in member.receive(timeout=1.0) if m.get(35) == "8"]
            if early:
                member.close()
                die(f"gateway reports: {len(early)} report(s) reached the member while the sequencer had no connection for reports to "
                    "its gateway, so the relay did not keep the connection away and nothing here was tested.")
            log(f"  {len(sent)} orders sent; sequencer_primary is holding their reports, first seq_no={held.seq_no}")

            relay = PortRelay(_GATEWAY_REPORT_RELAY_PORTS)
            relay.start()
            try:
                released = component_event("sequencer_primary", sequencer_events, "ReportsSentToGateway", events_from, 15.0, gateway_protocol=1,
                                           gateway_instance=1)
                if released is None:
                    member.close()
                    die("gateway reports: once its connection for reports was let through, sequencer_primary did not send the reports it held.")
                if released.dropped != 0:
                    member.close()
                    die(f"gateway reports: sequencer_primary dropped {released.dropped} held report(s) for want of room.")
                log(f"  the relay let the connection through, and sequencer_primary sent the {released.sent} report(s) it held")
                accepted: dict[str, int] = {}
                deadline = time.monotonic() + 20.0
                while len(accepted) < len(sent) and time.monotonic() < deadline:
                    for report in member.receive(timeout=0.5):
                        if report.get(35) == "8" and report.get(39) == "0":
                            accepted[report.get(11)] = accepted.get(report.get(11), 0) + 1
                for report in member.receive(timeout=1.0):
                    if report.get(35) == "8" and report.get(39) == "0":
                        accepted[report.get(11)] = accepted.get(report.get(11), 0) + 1
                member.close()
            finally:
                relay.stop()
            missing = [c for c in sent if c not in accepted]
            twice = [c for c, n in accepted.items() if n > 1]
            if missing:
                die(f"gateway reports: {len(missing)} of {len(sent)} orders were never reported to the member, for example {missing[:3]}.")
            if twice:
                die(f"gateway reports: {len(twice)} orders were reported to the member more than once, for example {twice[:3]}.")
            log(f"  all {len(sent)} orders reported to the member exactly once -- OK")

        # ── Binary checks ─────────────────────────────────────────────────────
        if scenario.assert_binary_checks:
            log("=== Binary gateway checks ===")
            binary_port = binary_gateway_listen_port(prefix)
            if not has_happened(expect_connection_made("binary_order_gateway_a", "sequencer_primary"), _BINARY_READY_TIMEOUT):
                die("binary checks: the binary gateway never connected to the primary sequencer, so nothing sent to it "
                    "could reach the venue and every check below would be measuring that instead.")
            binary_config = prefix / "etc" / "binary_order_gateway" / "binary_order_gateway_a.toml"
            max_symbol_length = installed_toml_int(binary_config, "max_symbol_length")
            binary_tag = datetime.now().strftime("%H%M%S")

            def binary_reply(cl_ord_id: str, *arguments: str) -> dict[str, str]:
                """Run binary_client and return the one reply that names cl_ord_id."""
                status, replies, output = run_binary_client(bin_dir, binary_port, *arguments)
                matching = [reply for reply in replies if reply["cl_ord_id"] == cl_ord_id]
                if not matching:
                    die(f"binary checks: nothing came back naming ClOrdID '{cl_ord_id}' (binary_client exit {status}). A "
                        f"command the gateway refuses must still be answered.\n{output}")
                return matching[0]

            def binary_order(label: str, *arguments: str, cl_ord_id: str = "") -> dict[str, str]:
                # Hyphens for spaces: binary_client prints the ClOrdID as one word.
                cl_ord_id = cl_ord_id or f"bin-{binary_tag}-{label.replace(' ', '-')}"
                return binary_reply(cl_ord_id, "--orders", "1", "--cl-ord-id", cl_ord_id, *arguments)

            def expect_refused_order(label: str, expected_text: str, *arguments: str, cl_ord_id: str = "") -> None:
                reply = binary_order(label, *arguments, cl_ord_id=cl_ord_id)
                if reply["kind"] != "ExecutionReport" or reply["ord_status"] != "Rejected" or not reply["order_id"].startswith("GW-ORD-"):
                    die(f"binary checks: the order '{label}' came back as {reply}; expected a rejected ExecutionReport that "
                        "the gateway assigned (OrderID GW-ORD-n), because the order must not reach the venue.")
                if reply["text"] != expected_text:
                    die(f"binary checks: the order '{label}' was refused with Text='{reply['text']}'; expected "
                        f"'{expected_text}'.")
                log(f"  binary checks: {label} -- refused by the gateway, Text='{expected_text}' -- OK")

            # 1. A valid order reaches the matching engine, so the refusals below are the checks'
            #    doing and not a gateway that refuses everything.
            accepted = binary_order("valid")
            if accepted["ord_status"] != "New" or not re.fullmatch(r"O[0-9A-F]{16}", accepted["order_id"]):
                die(f"binary checks: a valid order came back as {accepted}; expected OrdStatus New from the matching "
                    "engine (an OrderID of the letter O and 16 hexadecimal digits).")
            log(f"  binary checks: a valid order was accepted by the matching engine, OrderID={accepted['order_id']} -- OK")

            # 2. Orders the gateway must refuse, one field wrong in each.
            expect_refused_order("undefined side", "side is missing or holds a value the protocol does not define", "--side", "Z")
            expect_refused_order("quantity not a number", "order_qty is not a decimal number", "--order-qty", "1e3")
            expect_refused_order("price not a number", "price is not a decimal number", "--price", "ten")
            expect_refused_order("over-long ClOrdID", f"cl_ord_id exceeds maximum length of {_MAX_CL_ORD_ID_LENGTH}",
                                 cl_ord_id="L" * (_MAX_CL_ORD_ID_LENGTH + 1))
            expect_refused_order("over-long Symbol", f"symbol exceeds maximum length of {max_symbol_length}",
                                 "--symbol", "S" * (max_symbol_length + 1))
            expect_refused_order("empty Symbol", "symbol is missing or holds a value the protocol does not define", "--symbol", "")

            # 3. A cancel the gateway must refuse is answered with an OrderCancelReject that says the
            #    order is still open (R-0151).
            bad_cancel_id = f"{accepted['cl_ord_id']}-CANCEL"
            reply = binary_reply(bad_cancel_id, "--cancel", accepted["cl_ord_id"], "--side", "Z")
            if (reply["kind"] != "OrderCancelReject" or reply["ord_status"] != "New" or reply["cxl_rej_reason"] != "99"
                    or reply["text"] != "side is missing or holds a value the protocol does not define"):
                die(f"binary checks: a cancel with an undefined Side came back as {reply}; expected an OrderCancelReject "
                    "with OrdStatus New, CxlRejReason 99 and the field named.")
            log("  binary checks: a cancel with an undefined Side was refused with an OrderCancelReject, OrdStatus New -- OK")

            # 4. A valid cancel of the valid order goes through.
            reply = binary_reply(bad_cancel_id, "--cancel", accepted["cl_ord_id"])
            if reply["kind"] != "ExecutionReport" or reply["ord_status"] != "Canceled":
                die(f"binary checks: cancelling the valid order came back as {reply}; expected OrdStatus Canceled.")
            log("  binary checks: a valid cancel cancelled the order -- OK")

            # 5. The matching engine's refusal of a cancel for an order it never held arrives as an
            #    OrderCancelReject with CxlRejReason 1, Unknown order.
            unknown = f"bin-{binary_tag}-never-placed"
            reply = binary_reply(f"{unknown}-CANCEL", "--cancel", unknown)
            if reply["kind"] != "OrderCancelReject" or reply["cxl_rej_reason"] != "1":
                die(f"binary checks: a cancel for an order never placed came back as {reply}; expected an "
                    "OrderCancelReject with CxlRejReason 1.")
            log("  binary checks: the engine's refusal of a cancel for an unknown order is an OrderCancelReject with "
                "CxlRejReason 1 -- OK")

        # ── Throttles ─────────────────────────────────────────────────────────
        # Nothing is killed. A comp id with small limits sends bursts over them, and the replies
        # are matched to the commands by ClOrdID, so a report for some other order cannot be
        # mistaken for the answer to this one.
        if scenario.assert_gateway_throttles:
            log("=== Throttles ===")
            stop_f8test(f8proc)
            f8proc = None
            time.sleep(_RAW_CLIENT_SETTLE)

            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from fix_raw_client import FixRawClient  # pylint: disable=import-outside-toplevel

            gateway_port = gateway_listen_port(prefix, "a")
            run_tag = datetime.now().strftime("%H%M%S")

            def replies_by_cl_ord_id(member: FixRawClient, wanted: set[str], timeout: float) -> dict[str, dict[int, str]]:
                """Collect the first ExecutionReport or OrderCancelReject answering each ClOrdID."""
                replies: dict[str, dict[int, str]] = {}
                deadline = time.monotonic() + timeout
                while len(replies) < len(wanted) and time.monotonic() < deadline:
                    reply = member.receive_until("8", "9", timeout=max(0.1, deadline - time.monotonic()))
                    if reply is not None and reply.get(11) in wanted and reply.get(11) not in replies:
                        replies[reply[11]] = reply
                return replies

            def logged_on(comp_id: str) -> FixRawClient:
                member = FixRawClient("127.0.0.1", gateway_port, comp_id, "GATEWAY", FIX8_PASSWORD)
                member.connect()
                member.logon(reset_seq_num=True)
                if member.receive_until("A", timeout=_RAW_LOGON_TIMEOUT) is None:
                    member.close()
                    die(f"throttles: '{comp_id}' could not log on. A test that cannot log on looks identical "
                        "to a venue that refuses everything, so this is checked first.")
                return member

            # 1. The limits the gateway applied, by number. Nothing else shows that each value
            #    survived the database, the export, the authentication service and
            #    AuthenticationResult: a session that arrived unlimited would refuse nothing, and
            #    a refusal for another reason would look like a throttle to a test that did not
            #    check.
            limits_from = _TEST_EVENTS.mark()
            member = logged_on(_THROTTLED_COMP_ID)
            gateway_events = _TEST_EVENTS.modules["fix_order_gateway_test_events"]
            if gateway_event("fix_order_gateway_a", "ThrottleApplied", limits_from, _THROTTLE_EVENT_TIMEOUT, comp_id=_THROTTLED_COMP_ID,
                             new_orders_per_second=_THROTTLE_PLACE_LIMIT, amends_per_second=_THROTTLE_AMEND_LIMIT,
                             cancels_per_second=_THROTTLE_CANCEL_LIMIT) is None:
                member.close()
                die(f"throttles: the gateway never applied new orders={_THROTTLE_PLACE_LIMIT} amends={_THROTTLE_AMEND_LIMIT} "
                    f"cancels={_THROTTLE_CANCEL_LIMIT} to '{_THROTTLED_COMP_ID}'. The limits did not reach it -- check, in "
                    "order: the comp_id row, credentials.toml after export, the authentication service, and "
                    "AuthenticationResult.")
            log(f"  throttles: the gateway applied new orders={_THROTTLE_PLACE_LIMIT} amends={_THROTTLE_AMEND_LIMIT} "
                f"cancels={_THROTTLE_CANCEL_LIMIT} per second -- OK")

            # 2. A burst of new orders, faster than the limit. Exactly the first orders up to the
            #    limit are accepted, and every one after is refused with the limit in the text.
            place_text = f"Throttled: at most {_THROTTLE_PLACE_LIMIT} new orders per second for this session"
            orders = [f"throttle-{run_tag}-{index}" for index in range(_THROTTLE_BURST)]
            burst_started = time.monotonic()
            for cl_ord_id in orders:
                member.new_order_single(cl_ord_id)
            burst_seconds = time.monotonic() - burst_started
            if burst_seconds >= 0.5:
                member.close()
                die(f"throttles: sending {_THROTTLE_BURST} orders took {burst_seconds:.2f}s. The burst has to arrive "
                    "well inside one second, or the window can legitimately accept more than the limit and this "
                    "check would be measuring the harness.")
            replies = replies_by_cl_ord_id(member, set(orders), _RAW_REPLY_TIMEOUT)
            if len(replies) != len(orders):
                member.close()
                die(f"throttles: {len(orders) - len(replies)} of {len(orders)} orders got no answer at all. A "
                    "refused order must be answered, or the member cannot tell refusal from loss.")
            refused = [cl_ord_id for cl_ord_id in orders if replies[cl_ord_id].get(39) == "8"]
            accepted = [cl_ord_id for cl_ord_id in orders if replies[cl_ord_id].get(39) != "8"]
            if accepted != orders[:_THROTTLE_PLACE_LIMIT]:
                member.close()
                die(f"throttles: the orders accepted were {accepted}; expected exactly the first "
                    f"{_THROTTLE_PLACE_LIMIT}, {orders[:_THROTTLE_PLACE_LIMIT]}. A limit that let more through, or "
                    "refused an order within it, is not the limit provisioned.")
            for cl_ord_id in refused:
                if replies[cl_ord_id].get(58) != place_text:
                    member.close()
                    die(f"throttles: order {cl_ord_id} was refused with Text='{replies[cl_ord_id].get(58)}'; expected "
                        f"'{place_text}'. A member must be told the limit, or it cannot tell how far to slow down.")
                # The gateway names an order it refuses itself GW-ORD-n. An engine's OrderID here
                # would mean the order reached the sequencer, which a refused order must not.
                if not replies[cl_ord_id].get(37, "").startswith("GW-ORD-"):
                    member.close()
                    die(f"throttles: order {cl_ord_id} was refused with OrderID={replies[cl_ord_id].get(37)}, which the "
                        "gateway did not assign, so the order went on to the venue (R-0150).")
            log(f"  throttles: of {_THROTTLE_BURST} new orders sent in {burst_seconds * 1000:.0f}ms, the first "
                f"{len(accepted)} were accepted and {len(refused)} refused with Text='{place_text}' -- OK")

            # 3. Refused commands do not count. Retries sent part way through the second are
            #    refused, and an order sent just over a second after the burst is accepted. A
            #    gateway that counted the retries would still be full then, and refuse it.
            time.sleep(max(0.0, burst_started + 0.6 - time.monotonic()))
            retries = [f"throttle-{run_tag}-retry-{index}" for index in range(_THROTTLE_RETRIES)]
            for cl_ord_id in retries:
                member.new_order_single(cl_ord_id)
            retry_replies = replies_by_cl_ord_id(member, set(retries), _RAW_REPLY_TIMEOUT)
            if time.monotonic() - burst_started >= 1.0:
                member.close()
                die("throttles: the retries were not answered within the second they were sent in, so they cannot "
                    "show whether refused commands count.")
            if len(retry_replies) != len(retries) or any(r.get(58) != place_text for r in retry_replies.values()):
                member.close()
                die(f"throttles: the {_THROTTLE_RETRIES} retries sent within the same second were not all refused "
                    f"with Text='{place_text}': {[(c, r.get(39), r.get(58)) for c, r in retry_replies.items()]}.")

            # And one second after the first accepted order, one more is accepted, and the gateway
            # says the run of refusals has ended, with how many there were.
            time.sleep(max(0.0, burst_started + 1.1 - time.monotonic()))
            later = f"throttle-{run_tag}-later"
            member.new_order_single(later)
            later_reply = replies_by_cl_ord_id(member, {later}, _RAW_REPLY_TIMEOUT).get(later)
            if later_reply is None or later_reply.get(39) == "8":
                member.close()
                die(f"throttles: an order sent a second after the burst was "
                    f"{'not answered' if later_reply is None else 'refused: ' + later_reply.get(58, '')}. "
                    "Every order accepted in the burst was more than a second old, so either the refused retries "
                    "were counted towards the limit (R-0149), or the limit is not measured over the last second.")
            new_order = gateway_events.ThrottledKind.NewOrder
            started = expect_message("fix_order_gateway_a", "started throttling new orders", "ThrottleStarted", "fix_order_gateway_test_events",
                                     fields=lambda _: {"comp_id": _THROTTLED_COMP_ID, "kind": new_order, "limit_per_second": _THROTTLE_PLACE_LIMIT})
            stopped = expect_message("fix_order_gateway_a", "stopped throttling new orders", "ThrottleStopped", "fix_order_gateway_test_events",
                                     fields=lambda m: {"comp_id": _THROTTLED_COMP_ID, "kind": new_order,
                                                       "refused": _THROTTLE_BURST - _THROTTLE_PLACE_LIMIT + _THROTTLE_RETRIES,
                                                       "ended_by": m.ThrottleEnd.AcceptedAgain})
            # Wait for the later of the two, then count both.
            await_expectation(stopped, _THROTTLE_EVENT_TIMEOUT, limits_from)
            for expect in (started, stopped):
                if count_seen(expect, limits_from) != 1:
                    member.close()
                    die(f"throttles: expected '{expect.what}' exactly once. The gateway says when a member starts and stops "
                        "being throttled, not each refusal, or a member sending far too fast fills its Quill log.")
            log(f"  throttles: {_THROTTLE_RETRIES} retries within the second were refused, an order a second after the "
                "burst was accepted, so refused orders did not count; and the gateway said once when the session "
                "started being throttled and once when it stopped -- OK")

            # 4. A burst of cancels for the orders now open. The first up to the cancel limit are
            #    accepted, and the rest are refused with an OrderCancelReject that says the order
            #    is still open.
            cancel_text = f"Throttled: at most {_THROTTLE_CANCEL_LIMIT} cancels per second for this session"
            open_orders = accepted + [later]
            cancels = {f"{cl_ord_id}-cancel": cl_ord_id for cl_ord_id in open_orders}
            for cancel_cl_ord_id, orig_cl_ord_id in cancels.items():
                member.order_cancel_request(cancel_cl_ord_id, orig_cl_ord_id)
            cancel_replies = replies_by_cl_ord_id(member, set(cancels), _RAW_REPLY_TIMEOUT)
            if len(cancel_replies) != len(cancels):
                member.close()
                die(f"throttles: {len(cancels) - len(cancel_replies)} of {len(cancels)} cancels got no answer.")
            cancel_order = list(cancels)
            refused_cancels = [c for c in cancel_order if cancel_replies[c].get(35) == "9"]
            accepted_cancels = [c for c in cancel_order if cancel_replies[c].get(35) != "9"]
            if accepted_cancels != cancel_order[:_THROTTLE_CANCEL_LIMIT]:
                member.close()
                die(f"throttles: the cancels accepted were {accepted_cancels}; expected exactly the first "
                    f"{_THROTTLE_CANCEL_LIMIT}, {cancel_order[:_THROTTLE_CANCEL_LIMIT]}.")
            for cancel_cl_ord_id in refused_cancels:
                reply = cancel_replies[cancel_cl_ord_id]
                if reply.get(39) != "0" or reply.get(434) != "1" or reply.get(102) != "99" or reply.get(58) != cancel_text:
                    member.close()
                    die(f"throttles: cancel {cancel_cl_ord_id} was refused with OrdStatus={reply.get(39)} "
                        f"CxlRejResponseTo={reply.get(434)} CxlRejReason={reply.get(102)} Text='{reply.get(58)}'; expected "
                        f"OrdStatus 0 (the order is open), 434=1, 102=99 and Text='{cancel_text}' (R-0150, R-0151).")
            log(f"  throttles: of {len(cancels)} cancels, the first {len(accepted_cancels)} were accepted and "
                f"{len(refused_cancels)} refused with an OrderCancelReject, OrdStatus 0, Text='{cancel_text}' -- OK")
            member.close()

            # 5. A comp id with no limit sends the same burst and nothing is refused, so the
            #    refusals above came from the limit and not from the venue refusing a burst.
            unlimited_from = _TEST_EVENTS.mark()
            member = logged_on(FIX8_COMP_ID)
            if gateway_event("fix_order_gateway_a", "ThrottleApplied", unlimited_from, _THROTTLE_EVENT_TIMEOUT, comp_id=FIX8_COMP_ID,
                             new_orders_per_second=0, amends_per_second=0, cancels_per_second=0) is None:
                member.close()
                die(f"throttles: the gateway never applied no limit (0, 0 and 0) to '{FIX8_COMP_ID}', the comp id provisioned with "
                    "no limit.")
            unlimited_orders = [f"unthrottled-{run_tag}-{index}" for index in range(_THROTTLE_BURST)]
            for cl_ord_id in unlimited_orders:
                member.new_order_single(cl_ord_id)
            unlimited_replies = replies_by_cl_ord_id(member, set(unlimited_orders), _RAW_REPLY_TIMEOUT)
            member.close()
            throttled = [c for c, r in unlimited_replies.items() if r.get(58, "").startswith("Throttled")]
            if len(unlimited_replies) != len(unlimited_orders) or throttled:
                die(f"throttles: the comp id with no limit had {len(throttled)} order(s) throttled and "
                    f"{len(unlimited_orders) - len(unlimited_replies)} unanswered, out of {len(unlimited_orders)}.")
            log(f"  throttles: a comp id with no limit sent the same burst of {_THROTTLE_BURST} and none was "
                "throttled -- OK")

        # ── A bounded resend, out of the middle of the member's history ───────
        # Nothing is killed. The member asks for a range it is entitled to ask for, and what
        # comes back is checked against its own record of what it was sent under those numbers.
        if scenario.assert_bounded_resend:

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
            sequencer_logs = {name: log_dir / f"{name}.log" for name in ("sequencer_primary", "sequencer_secondary")}
            replay_from = _TEST_EVENTS.mark()
            if scenario.bounded_resend_with_leader_killed:
                # The primary leads in this scenario because it is started first. Checked rather than
                # assumed, because killing the follower instead would test nothing.
                if not has_led("sequencer_primary", 0.0):
                    die("bounded resend: sequencer_primary never logged becoming leader, so killing it would not leave the "
                        "venue without a leader.")
                leader = proc_by_name.get("sequencer_primary")
                if leader is None or leader.poll() is not None:
                    die("bounded resend: sequencer_primary is not running, so it cannot be killed.")
                log(f"  SIGKILL -> sequencer_primary (PID {leader.pid}), just before the member asks")
                leader.kill()
                leader.wait()
                log("  sequencer_primary confirmed dead")
            for keystroke, prompt in (("R", "Enter BeginSeqNo"),
                                      (str(_BOUNDED_RESEND_BEGIN), "Enter EndSeqNo"),
                                      (str(_BOUNDED_RESEND_END), None)):
                f8proc.stdin.write(f"{keystroke}\n".encode())
                f8proc.stdin.flush()
                if prompt is not None and not _wait_for_client_prompt(bounded_output, prompt, _BOUNDED_RESEND_SETTLE):
                    die(f"bounded resend: the client never asked for '{prompt}' after being sent "
                        f"{keystroke!r}. Its menu reads keys in raw mode, so each line has to be "
                        "written only once it has asked for it.")

            started = time.monotonic()
            found = gateway_event("fix_order_gateway_a", "ResendRequested", replay_from, _RESEND_TIMEOUT, begin_seq_no=_BOUNDED_RESEND_BEGIN,
                                  end_seq_no=_BOUNDED_RESEND_END) is not None
            elapsed = time.monotonic() - started
            if not found:
                die("bounded resend: the gateway never reported reading the request with both bounds. Either "
                    "the client did not send it, or EndSeqNo is not being read -- which was "
                    "BUG-0039, and this is the test that was owed for it.")
            log(f"  bounded resend: the gateway read both bounds off the request ({elapsed:.1f}s)")

            started = time.monotonic()
            found = gateway_event("fix_order_gateway_a", "ResendCompleted", replay_from, _RESEND_TIMEOUT) is not None
            elapsed = time.monotonic() - started
            if not found:
                die("bounded resend: the gateway never finished the resend.")
            log(f"  bounded resend: the venue answered ({elapsed:.1f}s)")

            # 0. Exactly one sequencer answered, and the other declined. The gateway sends the request
            #    to both, because it does not know which one leads. When both answered, it placed the
            #    two answers' records on the member's numbers in the order they arrived, and the
            #    member was sent the wrong orders under the right numbers whenever the two
            #    interleaved -- BUG-0148, which made this scenario fail at random.
            # Each sequencer's answers and declines, counted once both have answered a checkpoint. A sequencer killed during
            # the test has none open, and everything it sent was read before its connection closed. The decline comes on a
            # request the gateway sent on another connection, so it is waited for first, as long as the answer was.
            sequencer_events = _TEST_EVENTS.modules["sequencer_test_events"]
            _TEST_EVENTS.wait_for(message_of(sequencer_events.ReplayDeclined), timeout=_BOUNDED_RESEND_SETTLE, since=replay_from)
            answered = {name: count_seen(expect_message(name, "a replay answered", "ReplayAnswered", "sequencer_test_events"), replay_from)
                        for name in sequencer_logs}
            declined = {name: count_seen(expect_message(name, "a replay declined", "ReplayDeclined", "sequencer_test_events"), replay_from)
                        for name in sequencer_logs}
            if sum(answered.values()) != 1 or sum(declined.values()) != 1:
                die(f"bounded resend: the sequencers answered {answered} and declined {declined}; exactly one answer, from "
                    "the leader, and one decline, from the follower, were expected. Two answers are mixed by the gateway "
                    "on the member's numbers (BUG-0148).")
            log(f"  bounded resend: one sequencer answered and the other declined ({answered}, {declined}) -- OK")
            if scenario.bounded_resend_with_leader_killed:
                # The decline and the answer both came from the secondary: first as the follower,
                # then as the leader it became. In between, the gateway asked again.
                if answered["sequencer_secondary"] != 1 or declined["sequencer_secondary"] != 1:
                    die(f"bounded resend: expected sequencer_secondary to decline the request and then answer it, but it "
                        f"answered {answered['sequencer_secondary']} and declined {declined['sequencer_secondary']}.")
                found = gateway_event("fix_order_gateway_a", "ReplayAskedAgain", replay_from, 0.0, settle=True) is not None
                if not found:
                    die("bounded resend: the gateway never reported asking the new leader again, so the answer did not come from "
                        "asking the new leader again.")
                log("  bounded resend: the follower declined, took over, and answered when the gateway asked again -- OK")

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
            gateway_from = _TEST_EVENTS.mark()
            client_output = log_dir / "f8test_resend_client.txt"

            # A number the venue cannot replay, with reports on both sides of it. Without this
            # the only such numbers are the Logon and heartbeat of the reconnect itself, which
            # sit at the end of the range where the replay's own completion gap-fills them --
            # so the harder case, gap-filling in place and putting the reports after it back on
            # the numbers they came from, would never run.
            log("=== Letting a heartbeat land mid-stream, then sending a few more orders ===")
            time.sleep(_RESEND_HEARTBEAT_IDLE)
            tail_sent_before = send_orders(f8proc, b"n", _RESEND_TAIL_ORDERS)
            found, elapsed = await_new_orders(f8proc, _RESEND_TAIL_ORDERS, tail_sent_before, timeout=30.0)
            if not found:
                die(f"resend: the {_RESEND_TAIL_ORDERS} orders sent after the heartbeat were not all "
                    "reported to the member as New within 30s.")
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

            started = time.monotonic()
            found = gateway_event("fix_order_gateway_a", "SessionResumed", gateway_from, _PROVISIONING_LOGON_TIMEOUT) is not None
            elapsed = time.monotonic() - started
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
            started = time.monotonic()
            requested = gateway_event("fix_order_gateway_a", "ResendRequested", gateway_from, _RESEND_TIMEOUT, begin_seq_no=asks_from)
            found, elapsed = requested is not None, time.monotonic() - started
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
            resume_at = requested.resume_at if requested is not None else None
            if resume_at is None:
                die("resend: the gateway did not say where it would resume after the replay, so "
                    "the range the member asked about cannot be bounded.")

            completed = gateway_event("fix_order_gateway_a", "ResendCompleted", gateway_from, _RESEND_TIMEOUT)
            if completed is None:
                die("resend: the gateway never finished the resend. The reports come from the "
                    "sequencer's WAL asynchronously -- check for a SessionReplayRequest that got "
                    "no SessionReplayComplete.")
            resent = completed.reports_resent
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
            filled = (completed.gap_from, completed.gap_to) if completed.gap_from != 0 else None
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
                # The range can also be closed by a gap-fill from inside the resend rather than the
                # terminating one: when a heartbeat falls due as the member's request arrives, the
                # numbers after the last report are all administrative, and the gateway gap-fills
                # them as it reaches them. So what the member expects next is the higher of one past
                # the highest number it received and the highest NewSeqNo any gap-fill gave it.
                highest = _client_highest_seq_num(client_output)
                expects = max([highest + 1 if highest is not None else 0] + [int(msg["NewSeqNo"]) for msg in gap_fills])
                if expects < resume_at:
                    die(f"resend: the venue reported no gap left, so the member should be expecting "
                        f"{resume_at}. Its highest MsgSeqNum is {highest} and the gap-fills it received "
                        f"took it to {[msg['NewSeqNo'] for msg in gap_fills]}, so it expects {expects}: "
                        "the range was not closed and the member is still short.")
                log(f"  resend: the resent reports and gap-fills closed the range, leaving the "
                    f"member expecting {resume_at} -- OK")

            # 7. The session is still there afterwards. This is the assertion the others
            #    cannot make between them: every one of them can hold while the two sides walk
            #    away from the resend disagreeing about where the numbering now stands, and
            #    nothing shows it until the venue next sends. A resend that is answered
            #    correctly message by message and then kills the session on the following
            #    heartbeat has not worked, and a test that stops at "resend complete" says it
            #    has.
            survival_from = _TEST_EVENTS.mark()
            log(f"  resend: watching the session for {_RESEND_SURVIVAL_WAIT:.0f}s, spanning a "
                "heartbeat, to see that it survives the resend ...")
            time.sleep(_RESEND_SURVIVAL_WAIT)

            unbound = count_seen(expect_message("fix_order_gateway_a", "the member's session unbound", "SessionAnnounced", "fix_order_gateway_test_events", fields=lambda m: {"comp_id": FIX8_COMP_ID, "bound": False}), survival_from)
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
        # Nothing is killed. Everything here reads the gateway's own test events, because the
        # decision under test is the gateway's alone: whether this session belongs on this
        # instance, taken once, at the moment authentication succeeds.
        if scenario.assert_session_provisioning:

            # 1. The accepted path, asserted on the NUMBERS rather than on the session
            #    having opened. A gateway that ignored provisioning entirely would open the
            #    session too, so "it logged on" discriminates nothing; the numbers only
            #    appear if the pinning survived the database -> export -> auth service ->
            #    AuthenticationResult chain intact. The baseline session opened back in
            #    Phase 3, so this reads from the gateway's start.
            provisioned = gateway_event("fix_order_gateway_a", "SessionProvisioned", _TEST_EVENTS.since_introduction("fix_order_gateway_a"), 0.0,
                                        settle=True, comp_id=FIX8_COMP_ID, primary_instance=_PROVISIONED_PRIMARY_INSTANCE,
                                        backup_instance=_PROVISIONED_BACKUP_INSTANCE, this_instance=_PROVISIONED_PRIMARY_INSTANCE)
            if provisioned is None:
                die("provisioning: the gateway never said it admitted this comp id as provisioned for primary="
                    f"{_PROVISIONED_PRIMARY_INSTANCE} backup={_PROVISIONED_BACKUP_INSTANCE}. Either the values did not reach "
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
            auth_restart_from = _TEST_EVENTS.mark()

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
            found, elapsed = await_expectation(expect_connection_made("fix_order_gateway_a", "authentication_service_primary"), args.failover_timeout, auth_restart_from)
            if not found:
                die("provisioning: the gateway did not reconnect to the restarted authentication "
                    f"service within {args.failover_timeout:.0f}s, so the next logon would be "
                    "refused for the wrong reason.")
            log(f"  provisioning: gateway reconnected to the authentication service ({elapsed:.1f}s)")

            # 3. The same comp id, the same gateway, now provisioned elsewhere: refused.
            log("=== Reconnecting the comp id now provisioned for another instance ===")
            stop_f8test(f8proc)
            f8proc = None
            logon_from = _TEST_EVENTS.mark()
            f8proc = subprocess.Popen(
                [str(FIX8_BIN), "-c", FIX8_CFG, "-N", "GW1"],
                cwd=str(FIX8_DIR),
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            gateway_events = _TEST_EVENTS.modules["fix_order_gateway_test_events"]
            started = time.monotonic()
            refused = gateway_event("fix_order_gateway_a", "LogonRefused", logon_from, _PROVISIONING_LOGON_TIMEOUT, comp_id=FIX8_COMP_ID,
                                    reason=gateway_events.LogonRefusal.NotProvisionedHere, primary_instance=_ELSEWHERE_PRIMARY_INSTANCE,
                                    backup_instance=0)
            if refused is None:
                die("provisioning: the gateway did not refuse a comp id provisioned for another "
                    f"instance, primary={_ELSEWHERE_PRIMARY_INSTANCE} with no backup. A session that can log "
                    "on anywhere makes the pinning a convention rather than a rule, and the "
                    "recovery guarantees in docs/availability/gateway_ha.md rest on it being a rule.")
            log(f"  provisioning: logon refused at the wrong instance ({time.monotonic() - started:.1f}s)")

            # And refused means refused: no session may have been established alongside it.
            established = count_seen(expect_message("fix_order_gateway_a", "a session established", "SessionEstablished",
                                                    "fix_order_gateway_test_events", fields=lambda _: {"comp_id": FIX8_COMP_ID}), logon_from)
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
            # Counted at the client, so a lower bound: an order the engine accepted whose report died with the gateway is
            # not counted, which can only make this check harder to pass.
            accepted = client_reports(f8proc).new_orders_since(0)
            if accepted <= before_total:
                die(f"in-flight: the member was told of {accepted} accepted order(s), no more than the "
                    f"{before_total} baseline -- nothing was actually in flight when the gateway "
                    "died, so this scenario would assert nothing. Raise _INFLIGHT_BURSTS.")
            inflight_accepted = accepted - before_total
            log(f"  in-flight: {inflight_accepted} of the in-flight order(s) reached the venue")

            # 2. Nothing cancelled them. The process that would have sent the cancels is the
            #    one that died, so the member's book survives its gateway -- which is the
            #    whole point of a gateway failure not being a member failure.
            # The member has no gateway at this moment, so the engine's own events are the record: the cancels it would
            # have done for the member's orders, from the start of the scenario, once it has answered a checkpoint.
            if not _TEST_EVENTS.settle(me_log.stem):
                die(f"in-flight: {me_log.stem} did not answer a checkpoint ({_TEST_EVENTS.failures[-1]})")
            cancels = outcome_count(me_log.stem, 0, "Cancelled")
            if cancels > 0:
                die(f"in-flight: {cancels} order(s) were cancelled when the gateway died. A "
                    "member's book must survive the loss of the process serving it.")
            log(f"  in-flight: none cancelled -- {inflight_accepted} order(s) still live on the book")

            # 3. The member returns on the OTHER instance.
            log("=== Reconnecting the member on instance b ===")
            stop_f8test(f8proc)
            f8proc = None
            gateway_b_from = _TEST_EVENTS.mark()
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
            started = time.monotonic()
            resumed = gateway_event("fix_order_gateway_b", "SessionResumed", gateway_b_from, _RESEND_TIMEOUT)
            found, elapsed = resumed is not None, time.monotonic() - started
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
            started = time.monotonic()
            requested = gateway_event("fix_order_gateway_b", "ResendRequested", gateway_b_from, _RESEND_TIMEOUT)
            found, elapsed = requested is not None, time.monotonic() - started
            if not found:
                die("in-flight: the member never asked for what it missed. It was sent a Logon "
                    f"numbered {logon_seq} while expecting 1, so it was short by everything the "
                    "dead instance had sent. Check the generated config carries "
                    "ignore_logon_sequence_check.")
            log(f"  in-flight: the member asked instance b for the gap ({elapsed:.1f}s)")

            started = time.monotonic()
            completed = gateway_event("fix_order_gateway_b", "ResendCompleted", gateway_b_from, _RESEND_TIMEOUT)
            found, elapsed = completed is not None, time.monotonic() - started
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
            inherited = resumed.report_numbers if resumed is not None else None
            if inherited is None or inherited == 0:
                die("in-flight: instance b inherited no record of which of this session's numbers "
                    "held execution reports. It never sent them itself, so without that record it "
                    "cannot tell a report's number from a heartbeat's, and a resend would refill "
                    "both -- see BUG-0051. The gateway reports the record on SessionSequenceUpdate "
                    "every 2s; a session that lives and dies inside one interval reports nothing.")
            resent = completed.reports_resent
            log(f"  in-flight: instance b inherited a record covering {inherited} number(s) for a "
                f"session it never served, and replayed {resent} report(s) into the range asked "
                "for -- the rest of that range is the venue's own high bias, which nobody sent -- OK")

            resume_at = requested.resume_at if requested is not None else None
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
            survival_from = _TEST_EVENTS.mark()
            log(f"  in-flight: watching the session for {_RESEND_SURVIVAL_WAIT:.0f}s, spanning a "
                "heartbeat, to see that it survives the resend ...")
            time.sleep(_RESEND_SURVIVAL_WAIT)
            unbound = count_seen(expect_message("fix_order_gateway_b", "the member's session unbound", "SessionAnnounced", "fix_order_gateway_test_events", fields=lambda m: {"comp_id": FIX8_COMP_ID, "bound": False}), survival_from)
            if unbound > 0:
                die("in-flight: the session died after instance b answered the resend. A member "
                    "that fails over to the surviving gateway and asks for what it missed must "
                    "come out of it still logged in -- losing the session at a failover is the "
                    "moment it can least afford it.")
            if f8proc.poll() is not None:
                die("in-flight: the member's client exited after the resend. Its session log "
                    "records why; a sequence-number disagreement is the likely cause.")
            log("  in-flight: the session survived the resend and the heartbeat that followed -- OK")

        if scenario.assert_new_leader_numbers_forward and not scenario.assert_engine_holds_only_logged_orders:
            check_new_leader_numbers_forward(secondary_wal_dir, secondary_wal_start)

        # Every FIX client this scenario drove itself watched the venue's message numbers. A number
        # that skipped ahead means the venue numbered messages for the member and never delivered
        # them, which a member with a real session layer would log off at, whatever the scenario
        # was checking.
        raw_client_module = sys.modules.get("fix_raw_client")
        if raw_client_module is not None and raw_client_module.FixRawClient.all_receive_numbering_faults:
            faults = raw_client_module.FixRawClient.all_receive_numbering_faults
            die("the venue's message numbering to a member was broken: " + "; ".join(faults[:5]) +
                (f"; and {len(faults) - 5} more" if len(faults) > 5 else ""))

        # A sequencer that wrote records out of order, or found its write-ahead log damaged, has left
        # a log that holds records twice, or out of order, whatever the member saw. Every later
        # scenario then starts from that log, so it is a failure of this one (BUG-0147).
        check_write_ahead_logs_in_order(prefix)

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

    # Once every process has stopped, so that nothing is written to the WAL while it is read.
    if result_pass:
        try:
            replayed = run_wal_replay_check(prefix, launched_configs)
            if replayed is not None:
                run_engines_on_the_wal(prefix, launched_configs, bin_dir, log_dir, replayed)
        except TestFailure:
            result_pass = False

    # ── result summary ─────────────────────────────────────────────────────────
    log("")
    log("=" * 60)
    if result_pass:
        log("  RESULT  : PASS")
        log(f"  scenario: {scenario.number} — {scenario.description}")
        if scenario.extra_steps:
            for entry in phase4_results:
                if entry[0] == "kill":
                    _, outcome, label, elapsed = entry
                    log(kill_summary_line(outcome, label, elapsed))
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
            for outcome, label, elapsed in kill_results:
                log(kill_summary_line(outcome, label, elapsed))
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
    parser.add_argument(
        "--secondary-engine-first", action="store_true",
        help=(
            "Start matching_engine_secondary before matching_engine_primary, and start the primary only once the "
            "secondary leads. Applies only to the scenarios that run both matching engines; with 'all', only those "
            "scenarios are run"
        ),
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
        scenarios_to_run = ([scenario for scenario in _SCENARIOS if scenario.me_ha and not scenario.engine_started_first]
                            if args.secondary_engine_first else _SCENARIOS)
    else:
        scenario = _SCENARIO_MAP.get(args.scenario)
        if scenario is None:
            parser.error(
                f"unknown scenario {args.scenario}; valid values: "
                + ", ".join(str(n) for n in valid_scenario_numbers)
            )
        if args.secondary_engine_first and not scenario.me_ha:
            parser.error(f"--secondary-engine-first applies only to scenarios that run both matching engines, and scenario {args.scenario} runs one")
        if args.secondary_engine_first and scenario.engine_started_first:
            parser.error(f"scenario {args.scenario} starts {scenario.engine_started_first} first itself, so --secondary-engine-first does not apply to it")
        scenarios_to_run = [scenario]

    overall_pass = True
    results: list[tuple[int, str, bool]] = []
    for scenario in scenarios_to_run:
        passed = run_scenario(scenario, args)
        if not passed:
            kept = keep_logs_of_failed_scenario(scenario, args)
            if kept is not None:
                log(f"  the Quill logs and client output of scenario {scenario.number} are kept in {kept}")
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
        expected = {scenario.number for scenario in scenarios_to_run if scenario.expected_failure}
        passed_count = sum(1 for num, _, p in results if p and num not in expected)
        expected_count = sum(1 for num, _, p in results if p and num in expected)
        failed_count = len(results) - passed_count - expected_count
        log("")
        log("=" * 60)
        log(f"SUMMARY: of {len(results)} scenarios, {passed_count} passed, {expected_count} failed as expected, {failed_count} failed")
        for num, name, passed in results:
            status = ("EXPECTED FAILURE" if num in expected else "PASS") if passed else "FAIL"
            log(f"  Scenario {num:2d} ({name}): {status}")
        if failed_count:
            log(f"{failed_count} scenario(s) FAILED")
        log("=" * 60)

    if not overall_pass:
        sys.exit(1)


if __name__ == "__main__":
    main()
