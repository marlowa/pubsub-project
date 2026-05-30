#!/usr/bin/env python3
"""
ha_test.py — HA failover test for the pubsub_itc_fw sequencer system.

Starts the full 7-process system, confirms orders are flowing, kills a
primary component, and verifies that the secondary takes over and orders
resume.  Reports PASS/FAIL and the observed failover time.

Usage (from the project root):
    ./ha_test.py                               # kill sequencer_primary (default)
    ./ha_test.py --kill sequencer_primary      # same as above
    ./ha_test.py --kill arbiter_primary        # arbiter failover; witness casts vote
    ./ha_test.py --kill both                   # arbiter first, then sequencer
    ./ha_test.py build/installed --kill sequencer_primary

Options:
    --kill TARGET           What to kill: sequencer_primary, arbiter_primary, both.
                            "both" kills arbiter_primary first, waits for the
                            arbiter_secondary to become active, then kills
                            sequencer_primary. Default: sequencer_primary.
    --orders-before N       Bursts of 1000 NOS sent before the kill to confirm
                            the system is healthy.  Default: 1.
    --orders-after N        Bursts of 1000 NOS sent after the kill to confirm
                            recovery.  Default: 1.
    --ready-timeout SECS    Max seconds to wait for the initial leader election.
                            Default: 10.
    --failover-timeout SECS Max seconds to wait for a secondary to become leader
                            after the kill.  Default: 30.
    --recovery-timeout SECS Max seconds to wait for recovery orders to appear in
                            the ME log.  Default: 30.

Startup order (mirrors start_fix_seq_system.py):
  1. witness                -- arbiters connect outbound to it (port 7100)
  2. arbiter_primary        -- component listener 7200, peer listener 7203
  3. arbiter_secondary      -- component listener 7201, peer listener 7204
  4. sample_fix_gateway_seq -- FIX client port 9879, ER inbound port 7010
  5. sequencer_primary      -- listens on port 7001
  6. sequencer_secondary    -- listens on port 7002
  7. matching_engine        -- connects outbound to sequencer ER listener

Failover timing:
  The sequencer follower resets its peer_heartbeat_timeout (default 15 s) on
  each received heartbeat (sent every 5 s).  After a SIGKILL the TCP RST
  closes the connection immediately but the timeout keeps running.  Worst-case
  detection is 15 s; arbitration adds ~1 ms; recovery orders then flow.
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

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY     = 1.0   # seconds between app launches
FIX8_LOGON_WAIT   = 3.0   # seconds for f8test to establish the FIX session
LOG_POLL_INTERVAL = 0.05  # seconds between log polls
SHUTDOWN_TIMEOUT  = 5.0   # seconds per-process for SIGTERM grace period
POST_FAILOVER_SETTLE = 2.0  # seconds after failover confirmed before after-orders

FIX8_DIR  = Path("/home/marlowa/mystuff/fix8_install")
FIX8_BIN  = FIX8_DIR / "bin" / "f8test"
FIX8_CFG  = "myfix_gateway_client.xml"
# ──────────────────────────────────────────────────────────────────────────────

# Substrings from adopt_role() — both must appear on the same log line:
#   "SequencerThread: role transition {} -> {} (epoch={})"
#   "ArbiterThread:   role transition {} -> {} (epoch={})"
_SEQ_ROLE  = "SequencerThread: role transition"
_ARB_ROLE  = "ArbiterThread: role transition"
_TO_LEADER = "-> leader"


def log(msg: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def die(msg: str) -> None:
    log(f"FAIL: {msg}")
    sys.exit(1)


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
    Poll log_path for a log line that contains ALL of the given markers.
    Only bytes beyond from_byte are examined so stale content is ignored.
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
    Wait for ME-ORD-<target> (exact match — no trailing digit) in the ME log.
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
    Launch one f8test session, wait for FIX logon, then send count T commands.
    Each T command sends 1000 NOS messages.  Returns the Popen object.
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


def do_kill_and_failover(proc_name: str,
                         role_prefix: str,
                         secondary_log: Path,
                         proc_by_name: dict[str, subprocess.Popen],
                         failover_timeout: float) -> float:
    """
    SIGKILL proc_name, then poll secondary_log for a '-> leader' role transition.
    Returns the observed failover time in seconds, or raises SystemExit on failure.
    """
    proc = proc_by_name.get(proc_name)
    if proc is None or proc.poll() is not None:
        die(f"{proc_name} is not running — cannot kill")

    secondary_name = proc_name.replace("_primary", "_secondary")
    log_start_pos  = file_end(secondary_log)

    log(f"  SIGKILL → {proc_name} (PID {proc.pid})")
    proc.kill()
    proc.wait()
    log(f"  {proc_name} confirmed dead")

    kill_time = time.monotonic()
    log(f"  Watching {secondary_log.name} for '{role_prefix}' ... '{_TO_LEADER}' "
        f"(timeout {failover_timeout:.0f}s) ...")

    found, elapsed, _ = poll_log_for(
        secondary_log, role_prefix, _TO_LEADER,
        timeout=failover_timeout,
        from_byte=log_start_pos,
    )
    if not found:
        die(f"{secondary_name} did not become leader within {failover_timeout:.0f}s")

    log(f"  {secondary_name} is now leader ({elapsed:.1f}s after kill)")
    return elapsed


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("prefix", nargs="?", default="build/installed",
                        metavar="install_prefix",
                        help="Path to the cmake install prefix (default: build/installed)")
    parser.add_argument("--kill", default="sequencer_primary",
                        choices=["sequencer_primary", "arbiter_primary", "both"],
                        help="Component(s) to kill (default: sequencer_primary)")
    parser.add_argument("--orders-before", type=int, default=1, metavar="N",
                        help="Bursts of 1000 NOS before the kill (default: 1)")
    parser.add_argument("--orders-after", type=int, default=1, metavar="N",
                        help="Bursts of 1000 NOS after the kill (default: 1)")
    parser.add_argument("--ready-timeout", type=float, default=10.0, metavar="SECS",
                        help="Max seconds for initial leader election (default: 10)")
    parser.add_argument("--failover-timeout", type=float, default=30.0, metavar="SECS",
                        help="Max seconds per failover step (default: 30)")
    parser.add_argument("--recovery-timeout", type=float, default=30.0, metavar="SECS",
                        help="Max seconds for recovery orders to appear (default: 30)")
    args = parser.parse_args()

    for attr, flag in [("orders_before", "--orders-before"),
                       ("orders_after",  "--orders-after")]:
        if getattr(args, attr) < 1:
            parser.error(f"{flag} must be >= 1")

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

    preflight(prefix)
    log_dir.mkdir(parents=True, exist_ok=True)

    # Delete stale log files so polling always starts from byte 0.  Processes
    # overwrite (not append) their logs on each start, so any previously captured
    # file_end offset would skip past content written by the new process.
    for stale in (seq_primary_log, seq_secondary_log,
                  arb_primary_log, arb_secondary_log, me_log):
        stale.unlink(missing_ok=True)

    lib_dir  = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = (
        f"{lib_dir}:{existing}" if existing else lib_dir
    )

    # ── summary header ─────────────────────────────────────────────────────────
    log("=" * 60)
    log("  ha_test")
    log("=" * 60)
    log(f"  install prefix   : {prefix}")
    log(f"  kill target      : {args.kill}")
    log(f"  orders before    : {args.orders_before * 1000}")
    log(f"  orders after     : {args.orders_after * 1000}")
    log(f"  ready timeout    : {args.ready_timeout:.0f}s")
    log(f"  failover timeout : {args.failover_timeout:.0f}s  (per failover step)")
    log(f"  recovery timeout : {args.recovery_timeout:.0f}s")
    log("")

    steps = [
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

    # Kill tasks executed in order — "both" kills arbiter first, then sequencer,
    # so the second kill exercises the newly-elected arbiter_secondary.
    kill_tasks: list[tuple[str, str, Path]] = []
    if args.kill in ("arbiter_primary", "both"):
        kill_tasks.append(("arbiter_primary",   _ARB_ROLE, arb_secondary_log))
    if args.kill in ("sequencer_primary", "both"):
        kill_tasks.append(("sequencer_primary", _SEQ_ROLE, seq_secondary_log))

    app_procs:   list[tuple[str, subprocess.Popen]] = []
    proc_by_name: dict[str, subprocess.Popen]       = {}
    result_pass   = False
    killed_names: list[str] = []
    failover_times: list[tuple[str, float]] = []

    try:
        # ── Phase 1: startup ──────────────────────────────────────────────────
        log("=== Phase 1: starting all processes ===")
        for name, bin_name, config in steps:
            log(f"  Starting {name} ...")
            proc = launch_app(name, bin_name, config, bin_dir, log_dir)
            app_procs.append((name, proc))
            proc_by_name[name] = proc
            time.sleep(STARTUP_DELAY)
        log("")

        for name, proc in app_procs:
            if proc.poll() is not None:
                die(f"{name} (PID {proc.pid}) died during startup "
                    f"(exit code {proc.returncode})")

        # ── Phase 2: wait for leader election ─────────────────────────────────
        log("=== Phase 2: waiting for leader election ===")

        # Log files were deleted before startup so all reads begin at byte 0.
        log(f"  Polling sequencer_primary.log for leader election "
            f"(timeout {args.ready_timeout:.0f}s) ...")
        found, elapsed, _ = poll_log_for(
            seq_primary_log, _SEQ_ROLE, _TO_LEADER,
            timeout=args.ready_timeout,
            from_byte=0,
        )
        if not found:
            die(f"sequencer_primary did not elect leader within "
                f"{args.ready_timeout:.0f}s — check sequencer.toml ha_enabled flag")
        log(f"  sequencer_primary: leader elected ({elapsed:.1f}s)")

        # Arbiter election typically completes before the sequencer, but confirm.
        log("  Polling arbiter_primary.log for active role ...")
        found, elapsed, _ = poll_log_for(
            arb_primary_log, _ARB_ROLE, _TO_LEADER,
            timeout=10.0,
            from_byte=0,
        )
        if found:
            log(f"  arbiter_primary: active ({elapsed:.1f}s)")
        else:
            log("  arbiter_primary: active marker not seen within 10s "
                "(election may have completed before file position was captured — continuing)")
        log("")

        # ── Phase 3: baseline orders ──────────────────────────────────────────
        before_total  = args.orders_before * 1000
        log(f"=== Phase 3: {before_total} baseline orders ===")

        f8_before = send_burst(args.orders_before)
        log(f"  Waiting for ME-ORD-{before_total} ...")

        found, elapsed, me_pos = wait_for_me_ord(
            me_log, before_total, timeout=120.0, from_byte=0,
        )
        stop_f8test(f8_before)

        if not found:
            die("baseline orders did not complete — system is not healthy")
        log(f"  {before_total} baseline orders confirmed ({elapsed:.1f}s)")
        log("")

        # ── Phase 4: kill and failover ────────────────────────────────────────
        log(f"=== Phase 4: kill and failover ({'→ '.join(t[0] for t in kill_tasks)}) ===")

        for proc_name, role_prefix, secondary_log in kill_tasks:
            elapsed = do_kill_and_failover(
                proc_name, role_prefix, secondary_log,
                proc_by_name, args.failover_timeout,
            )
            killed_names.append(proc_name)
            failover_times.append((proc_name, elapsed))

            if len(kill_tasks) > 1 and proc_name != kill_tasks[-1][0]:
                # Pause between sequential kills so the first secondary has time
                # to fully establish itself before we stress the system again.
                log(f"  Settling {POST_FAILOVER_SETTLE:.0f}s before next kill ...")
                time.sleep(POST_FAILOVER_SETTLE)

        log(f"  Settling {POST_FAILOVER_SETTLE:.0f}s for connections to stabilise ...")
        time.sleep(POST_FAILOVER_SETTLE)
        log("")

        # ── Phase 5: recovery orders ──────────────────────────────────────────
        after_total  = args.orders_after * 1000
        after_target = before_total + after_total
        log(f"=== Phase 5: {after_total} recovery orders (ME-ORD target: {after_target}) ===")

        f8_after = send_burst(args.orders_after)
        log(f"  Waiting for ME-ORD-{after_target} ...")

        found, elapsed, _ = wait_for_me_ord(
            me_log, after_target,
            timeout=args.recovery_timeout,
            from_byte=me_pos,
        )
        stop_f8test(f8_after)

        if not found:
            die(f"recovery orders did not appear within {args.recovery_timeout:.0f}s "
                f"— secondary may not be forwarding to ME")
        log(f"  {after_total} recovery orders confirmed ({elapsed:.1f}s)")
        result_pass = True
        log("")

    except KeyboardInterrupt:
        log("Interrupted — shutting down ...")
    finally:
        shutdown_all(app_procs)

    # ── result summary ─────────────────────────────────────────────────────────
    log("")
    log("=" * 60)
    if result_pass:
        log("  RESULT  : PASS")
        log(f"  killed  : {', '.join(killed_names)}")
        for proc_name, ft in failover_times:
            secondary = proc_name.replace("_primary", "_secondary")
            log(f"  failover: {secondary} elected leader in {ft:.1f}s")
        log(f"  baseline: {before_total} orders — OK")
        log(f"  recovery: {after_total} orders — OK")
    else:
        log("  RESULT  : FAIL")
    log("=" * 60)


if __name__ == "__main__":
    main()
