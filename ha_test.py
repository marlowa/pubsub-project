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

Options:
    --scenario N|all      Scenario number, or 'all' to run every scenario in
                          order (required).  All scenarios must pass for the
                          command to exit with code 0.
    install_prefix        Path to cmake install prefix (default: build/installed)
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
  1. witness                -- arbiters connect outbound to it (port 7100)
  2. arbiter_primary        -- component listener 7200, peer listener 7203
  3. arbiter_secondary      -- component listener 7201, peer listener 7204
  4. sample_fix_gateway_seq -- FIX client port 9879, ER inbound port 7010
  5. sequencer_primary      -- listens on port 7001
  6. sequencer_secondary    -- listens on port 7002
  7. matching_engine        -- connects outbound to sequencer ER listeners 7021/7022

Failover timing:
  Both the sequencer and arbiter followers arm a 15 s peer_heartbeat_timeout
  when they adopt the follower/passive role.  Each received heartbeat (sent
  every 5 s) resets the timer.  After a SIGKILL the TCP RST closes all peer
  connections immediately; the running timeout fires at its remaining value
  (worst case 15 s).
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import NamedTuple

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY          = 1.0   # seconds between app launches
FIX8_LOGON_WAIT        = 3.0   # seconds for f8test to establish a FIX session
LOG_POLL_INTERVAL      = 0.05  # seconds between log-file polls
SHUTDOWN_TIMEOUT       = 5.0   # seconds per-process for SIGTERM grace period
SETTLE_AFTER_FAILOVER  = 2.0   # seconds after failover confirmed (let conns stabilise)
SETTLE_AFTER_KILL      = 1.0   # seconds after a kill where no failover is expected

FIX8_DIR = Path("/home/marlowa/mystuff/fix8_install")
FIX8_BIN = FIX8_DIR / "bin" / "f8test"
FIX8_CFG = "myfix_gateway_client.xml"
# ──────────────────────────────────────────────────────────────────────────────

# Substrings that appear together on adopt_role() log lines:
#   "SequencerThread: role transition {from} -> {to} (epoch={n})"
#   "ArbiterThread:   role transition {from} -> {to} (epoch={n})"
_SEQ_ROLE  = "SequencerThread: role transition"
_ARB_ROLE  = "ArbiterThread: role transition"
_TO_LEADER = "-> leader"

# Readiness marker for a restarted matching engine.
#
# We wait for the sequencer's inbound ORDER connection to the ME to be
# re-established, not just the ME's outbound ER connection.  The sequencer
# connects outbound to ME's order listener (port configured in matching_engine
# service registry); after a ME restart the sequencer's OutboundConnectionManager
# retries after 2 s.  Only once this connection is up can the sequencer forward
# sequenced orders to ME.  Waiting for "sequencer order connection established"
# ensures both directions are ready before Phase 5 sends recovery orders.
_ME_READY_MARKERS = (
    "MatchingEngineThread: sequencer order connection",
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
    """
    proc_name: str
    secondary_log_name: str | None
    role_prefix: str | None
    settle_secs: float


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


class Scenario(NamedTuple):
    number: int
    short_name: str
    description: str
    expected_outcome: str
    steps: list           # list[KillStep]
    restart_steps: list = []  # list[RestartStep]


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
                 "matching_engine", "sample_fix_gateway_seq"):
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


def launch_app(name: str, bin_name: str, config: Path,
               bin_dir: Path, log_dir: Path) -> subprocess.Popen:
    if not config.is_file():
        die(f"config not found: {config}")
    with open(log_dir / f"{name}.stdout", "w") as stdout_fh:
        proc = subprocess.Popen(
            [str(bin_dir / bin_name), str(log_dir / f"{name}.log"), str(config)],
            cwd=str(log_dir),
            stdout=stdout_fh,
            stderr=subprocess.STDOUT,
        )
    log(f"  {name} — PID {proc.pid}")
    return proc


def send_burst(count: int) -> subprocess.Popen:
    """
    Launch one f8test session, wait for FIX logon, then send `count` T commands.
    Each T command sends 1000 NOS.  Returns the Popen object.
    """
    proc = subprocess.Popen(
        [str(FIX8_BIN), "-c", FIX8_CFG, "-N", "GW1"],
        cwd=str(FIX8_DIR),
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    log(f"  f8test PID {proc.pid}: waiting {FIX8_LOGON_WAIT:.0f}s for FIX logon ...")
    time.sleep(FIX8_LOGON_WAIT)
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
    secondary_name = step.proc_name.replace("_primary", "_secondary")
    secondary_log  = log_dir / step.secondary_log_name

    log(
        f"  Watching {step.secondary_log_name} for "
        f"'{step.role_prefix}' ... '{_TO_LEADER}' "
        f"(timeout {failover_timeout:.0f}s) ..."
    )
    found, elapsed, _ = poll_log_for(
        secondary_log, step.role_prefix, _TO_LEADER,
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
    script_dir = Path(__file__).resolve().parent
    raw_prefix = args.prefix
    prefix = resolve_prefix(
        str(script_dir / raw_prefix)
        if not Path(raw_prefix).is_absolute()
        else raw_prefix
    )

    bin_dir = prefix / "bin"
    etc_dir = prefix / "etc"
    log_dir = prefix / "log"

    me_log            = log_dir / "matching_engine.log"
    seq_primary_log   = log_dir / "sequencer_primary.log"
    seq_secondary_log = log_dir / "sequencer_secondary.log"
    arb_primary_log   = log_dir / "arbiter_primary.log"
    arb_secondary_log = log_dir / "arbiter_secondary.log"

    log_dir.mkdir(parents=True, exist_ok=True)

    # Delete stale log files so all polling begins at byte 0.  Processes
    # overwrite (not append) their logs on each start, so any pre-existing EOF
    # offset would skip past content written by the new run.
    for stale in (seq_primary_log, seq_secondary_log,
                  arb_primary_log, arb_secondary_log, me_log):
        stale.unlink(missing_ok=True)

    # ── header ────────────────────────────────────────────────────────────────
    kill_seq_parts = [
        s.proc_name + (" [failover expected]" if s.secondary_log_name else "")
        for s in scenario.steps
    ]
    kill_seq_parts += [f"RESTART:{rs.proc_name}" for rs in scenario.restart_steps]
    kill_seq = " → ".join(kill_seq_parts) if kill_seq_parts else "(none)"

    log("=" * 60)
    log(f"  ha_test  —  Scenario {scenario.number}: {scenario.description}")
    log("=" * 60)
    log(f"  install prefix   : {prefix}")
    log(f"  kill sequence    : {kill_seq}")
    log(f"  expected outcome : {scenario.expected_outcome}")
    log(f"  orders before    : {args.orders_before * 1000}")
    log(f"  orders during    : {args.orders_during * 1000}  (in flight during kill)")
    log(f"  orders after     : {args.orders_after * 1000}")
    log(f"  ready timeout    : {args.ready_timeout:.0f}s")
    log(f"  failover timeout : {args.failover_timeout:.0f}s  (per step)")
    log(f"  recovery timeout : {args.recovery_timeout:.0f}s")
    log("")

    # Process launch table (name, binary, config).
    launch_table = [
        ("witness",                "witness",
         etc_dir / "witness"                / "witness.toml"),
        ("arbiter_primary",        "arbiter",
         etc_dir / "arbiter"                / "arbiter.toml"),
        ("arbiter_secondary",      "arbiter",
         etc_dir / "arbiter"                / "arbiter_secondary.toml"),
        ("sample_fix_gateway_seq", "sample_fix_gateway_seq",
         etc_dir / "sample_fix_gateway_seq" / "sample_fix_gateway_seq.toml"),
        ("sequencer_primary",      "sequencer",
         etc_dir / "sequencer"              / "sequencer.toml"),
        ("sequencer_secondary",    "sequencer",
         etc_dir / "sequencer"              / "sequencer_secondary.toml"),
        ("matching_engine",        "matching_engine",
         etc_dir / "matching_engine"        / "matching_engine.toml"),
    ]

    app_procs:      list[tuple[str, subprocess.Popen]] = []
    proc_by_name:   dict[str, subprocess.Popen]        = {}
    f8proc:         subprocess.Popen | None             = None
    result_pass     = False
    kill_results:   list[tuple[bool, str, float]]      = []
    restart_results: list[tuple[str, float]]           = []
    before_total    = 0
    after_total     = 0

    try:
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

        # ── Phase 3: baseline orders + in-flight orders ───────────────────────
        before_total = args.orders_before * 1000
        log(f"=== Phase 3: {before_total} baseline orders ===")

        f8proc = send_burst(args.orders_before)
        log(f"  Waiting for ME-ORD-{before_total} ...")
        found, elapsed, me_pos = wait_for_me_ord(
            me_log, before_total, timeout=120.0, from_byte=0,
        )
        if not found:
            die("baseline orders did not complete — system is not healthy")
        log(f"  {before_total} baseline orders confirmed ({elapsed:.1f}s)")

        # Send extra T commands WITHOUT waiting — these orders will be in flight
        # during Phase 4 so the kill happens while the system is under load.
        # Some may be lost during failover; Phase 5 adjusts the target
        # dynamically based on the actual ME count at that point.
        if args.orders_during > 0:
            log(
                f"  Sending {args.orders_during * 1000} in-flight orders "
                f"(will span Phase 4 kill) ..."
            )
            for _ in range(args.orders_during):
                f8proc.stdin.write(b"T\n")
            f8proc.stdin.flush()
        log("")

        # ── Phase 4: execute kill / restart scenario ──────────────────────────
        # f8proc stays alive so the FIX session remains established.
        log(f"=== Phase 4: {scenario.description} ===")
        for step in scenario.steps:
            failover_occurred, label, elapsed = do_kill_step(
                step, proc_by_name, log_dir, args.failover_timeout,
            )
            kill_results.append((failover_occurred, label, elapsed))

        for step in scenario.restart_steps:
            elapsed = do_restart_step(
                step, proc_by_name, app_procs, launch_table, bin_dir, log_dir,
            )
            restart_results.append((step.proc_name, elapsed))
        log("")

        # ── Phase 5: recovery orders ──────────────────────────────────────────
        # Re-use the established FIX session from Phase 3.
        #
        # Target calculation:
        #   ME restart   → ME log and counter reset; target = after_total,
        #                  scan from byte 0.
        #   orders_during> 0 → some in-flight orders may have been processed
        #                  (or lost) during Phase 4; scan from me_pos to find
        #                  the actual current ME-ORD count, then add after_total.
        #   otherwise    → simple cumulative: before_total + after_total.
        me_restarted = any(rs.resets_me_counter for rs in scenario.restart_steps)
        after_total  = args.orders_after * 1000
        if me_restarted:
            after_target = after_total
            me_log_from  = 0
        elif args.orders_during > 0:
            # If all during-orders were discarded by the follower (correct HA
            # behaviour), find_last_me_ord returns 0.  Fall back to before_total
            # so the target stays ahead of me_pos.
            current_me_ord = find_last_me_ord(me_log, from_byte=me_pos) or before_total
            after_target   = current_me_ord + after_total
            me_log_from    = me_pos
        else:
            after_target = before_total + after_total
            me_log_from  = me_pos

        log(
            f"=== Phase 5: {after_total} recovery orders "
            f"(ME-ORD target: {after_target}) ==="
        )

        log(f"  Sending {after_total} recovery orders via existing FIX session ...")
        for _ in range(args.orders_after):
            f8proc.stdin.write(b"T\n")
        f8proc.stdin.flush()

        log(f"  Waiting for ME-ORD-{after_target} ...")
        found, elapsed, _ = wait_for_me_ord(
            me_log, after_target,
            timeout=args.recovery_timeout,
            from_byte=me_log_from,
        )
        if not found:
            die(
                f"recovery orders did not appear within {args.recovery_timeout:.0f}s"
            )
        log(f"  {after_total} recovery orders confirmed ({elapsed:.1f}s)")
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

    # ── result summary ─────────────────────────────────────────────────────────
    log("")
    log("=" * 60)
    if result_pass:
        log("  RESULT  : PASS")
        log(f"  scenario: {scenario.number} — {scenario.description}")
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
        "prefix", nargs="?", default="build/installed",
        metavar="install_prefix",
        help="Path to the cmake install prefix (default: build/installed)",
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

    script_dir = Path(__file__).resolve().parent
    raw_prefix = args.prefix
    prefix = resolve_prefix(
        str(script_dir / raw_prefix)
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
