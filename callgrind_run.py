#!/usr/bin/env python3
from __future__ import annotations
"""
callgrind_run.py — start the full FIX sequencer system under valgrind callgrind,
                   fire NOS orders via fix8, then SIGTERM the profiled processes
                   so callgrind flushes one profile per process.

This is the callgrind counterpart of perf_run.py, but it is NOT a drop-in copy:
perf samples an already-running native process, whereas callgrind runs the guest
in an instrumentation VM (~20-50x slower) from launch to exit.  Two consequences
shape this script:

  * Readiness is established by POLLING component logs (leader election, gateway
    operational, FIX logon) rather than by fixed sleeps -- polling self-adjusts
    to the slowdown.  See the readiness markers and READY_TIMEOUT/LOGON_TIMEOUT.
  * There are no flamegraphs.  Flamegraphs come from stack SAMPLES (perf's model);
    callgrind produces an exact weighted call graph.  Visualise it in kcachegrind
    (or qcachegrind); the text report is produced with callgrind_annotate.

Usage (from the project root):
    ./callgrind_run.py                         # 1 client, 1 burst
    ./callgrind_run.py --burst=5               # 1 client, 5 x T = 5000 orders
    ./callgrind_run.py --clients=3             # 3 concurrent clients, 1 burst each
    ./callgrind_run.py --burst=4 --clients=2   # 2 clients x 4 bursts = 8000 orders
    ./callgrind_run.py installed --burst=2     # explicit install prefix

Options:
    --burst=N     Number of times the 'T' command is sent per fix8 session.
                  Each 'T' sends 1000 NOS messages.  Default: 1.
    --clients=N   Number of concurrent fix8 sessions.  All sessions start
                  simultaneously and each sends --burst T commands.  Default: 1.

Output directory:
    <prefix>/callgrind/<YYYYMMDD_HHMMSS>/
        <name>.callgrind.out.<pid>   raw callgrind profile (open in kcachegrind)
        <name>.callgrind.stderr      valgrind stderr log (for sanity checking)
        report.txt                   combined callgrind_annotate text report

Visualise:
    kcachegrind <prefix>/callgrind/<ts>/order_gateway.callgrind.out.<pid>
"""

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

# ── fix8 authentication ───────────────────────────────────────────────────────
FIX8_COMP_ID = "CLIENT"
FIX8_PASSWORD = ""     # empty: f8test sends no Password tag

# ── tunables ──────────────────────────────────────────────────────────────────
# Callgrind runs the guest inside an instrumentation VM, typically 20-50x slower
# than native.  Unlike perf -- which samples an already-running native process --
# every component we profile is slow from launch through shutdown.  So readiness
# is established by POLLING component logs (which self-adjusts to the slowdown)
# rather than by fixed sleeps tuned for native speed.
STARTUP_DELAY     = 1.0   # seconds between app launches (staggering only)
POST_ORDER_WAIT   = 2.0   # seconds after last order before SIGTERM
LOG_POLL_INTERVAL = 0.1   # seconds between log-file polls

# Generous, callgrind-scaled upper bounds (wall-clock).  Polling returns as soon
# as the marker appears, so these are ceilings, not fixed delays.
READY_TIMEOUT = 600.0     # leader election + gateway operational under callgrind
LOGON_TIMEOUT = 300.0     # FIX SCRAM logon round-trip through the slowed gateway
ORDER_TIMEOUT = 1800.0    # order/ER completion for a burst through slowed components

# Callgrind only writes its profile when the guest exits CLEANLY.  A SIGKILL'd
# valgrind writes nothing, so give it a long window to shut down and flush.
CALLGRIND_FLUSH_TIMEOUT = 600.0  # seconds to wait for clean exit + profile dump

# Processes to profile; set to None to profile all launched processes.
CALLGRIND_TARGETS = {"order_gateway", "matching_engine_primary"}

# ── readiness markers (all substrings must appear together on one log line) ────
_SEQ_LEADER_MARKERS     = ("SequencerThread: role transition", "-> leader")
_ARB_ACTIVE_MARKERS     = ("ArbiterThread: role transition", "-> leader")
_GW_OPERATIONAL_MARKERS = ("OrderGatewayThread", "operational state")
_GW_LOGON_OK            = "authentication succeeded -- FIX session established"

FIX8_DIR  = Path("/home/marlowa/mystuff/fix8_install")
FIX8_BIN  = FIX8_DIR / "bin" / "f8test"
FIX8_CFG  = "myfix_gateway_client.xml"
# ──────────────────────────────────────────────────────────────────────────────


def log(msg: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def die(msg: str) -> None:
    log(f"ERROR: {msg}")
    sys.exit(1)


def ensure_fix8_credentials(creds_file: Path, comp_id: str, password: str,
                             logon_mode: str) -> None:
    if logon_mode == "proprietary":
        log(f"  proprietary logon mode -- skipping SCRAM credential rewrite for '{comp_id}'")
        return

    salt = secrets.token_bytes(16)
    pwd_bytes = password.encode("utf-8")
    salted = hashlib.pbkdf2_hmac("sha256", pwd_bytes, salt, 4096)
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
    kept = [b for b in blocks[1:]
            if f'comp_id    = "{comp_id}"' not in b and
               f'comp_id = "{comp_id}"' not in b]
    result = header + "".join(f"[[credential]]{b}" for b in kept) + new_block
    creds_file.write_text(result)
    log(f"  SCRAM credential for '{comp_id}' (password={password!r}) written to {creds_file.name}")


def set_fix_capture_enabled(config_path: Path, enabled: bool) -> None:
    text = config_path.read_text()
    patched = re.sub(r'(?m)^(enabled\s*=\s*)(true|false)',
                     lambda m: m.group(1) + ("true" if enabled else "false"),
                     text)
    config_path.write_text(patched)


# HA/session timeout fields to relax for a profiling run.  Under callgrind the
# profiled components run ~20-50x slower, so wall-clock timeouts that are fine at
# native speed will trip -- e.g. the arbiter/secondary declaring the slowed ME
# "dead" and failing over, or the profiled gateway aborting its own FIX logon
# before the (also slowed) SCRAM round-trip completes.  We scale only the TIMEOUT
# fields, not the heartbeat *interval* fields: frequent heartbeats plus patient
# timeouts is the most tolerant of the slowdown.  Values appear either as bare
# ints ("heartbeat_timeout_seconds = 6") or quoted durations ("logon_timeout =
# \"30s\""); both are handled, preserving any unit suffix.
_TIMEOUT_KEYS = (
    "heartbeat_timeout_seconds",
    "startup_election_timeout_seconds",
    "arbitration_timeout_seconds",
    "vote_timeout_seconds",
    "logon_timeout",
    "scram_auth_timeout",
)


def _scale_timeout_text(text: str, factor: int) -> str:
    def repl(match: "re.Match") -> str:
        prefix, open_quote, number, unit, close_quote = match.groups()
        return f"{prefix}{open_quote or ''}{int(number) * factor}{unit}{close_quote or ''}"

    for key in _TIMEOUT_KEYS:
        pattern = re.compile(rf'(?m)^(\s*{re.escape(key)}\s*=\s*)(")?(\d+)(\w*)(")?\s*$')
        text = pattern.sub(repl, text)
    return text


def relax_timeouts(config_paths: list[Path], factor: int,
                   originals: dict[Path, str]) -> None:
    """Scale every timeout field in each config by `factor`.

    The original text of each touched file is recorded in `originals` (a caller-
    owned dict) BEFORE it is rewritten, so restore_configs() can revert the exact
    pre-run configs even if this aborts part-way through.
    """
    for path in config_paths:
        if not path.is_file():
            continue
        original = path.read_text()
        originals[path] = original
        scaled = _scale_timeout_text(original, factor)
        if scaled != original:
            path.write_text(scaled)


def restore_configs(originals: dict[Path, str]) -> None:
    for path, text in originals.items():
        try:
            path.write_text(text)
        except OSError as exc:
            log(f"  WARNING: could not restore {path.name}: {exc}")


def resolve_prefix(raw: str) -> Path:
    p = Path(raw).resolve()
    if not p.is_dir():
        die(f"install prefix '{raw}' does not exist or is not a directory")
    return p


def preflight(prefix: Path) -> None:
    if not FIX8_BIN.is_file() or not os.access(FIX8_BIN, os.X_OK):
        die(f"f8test not found or not executable: {FIX8_BIN}")
    if subprocess.call(["which", "valgrind"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        die("'valgrind' not found in PATH")
    for name in ("witness", "arbiter", "sequencer", "matching_engine", "order_gateway"):
        exe = prefix / "bin" / name
        if not exe.is_file() or not os.access(exe, os.X_OK):
            die(f"binary not found or not executable: {exe}")


def launch_app_under_callgrind(name: str, bin_name: str, config: Path,
                               bin_dir: Path, log_dir: Path,
                               callgrind_dir: Path,
                               workdir: Path | None = None) -> subprocess.Popen:
    if not config.is_file():
        die(f"config not found: {config}")

    log_file = log_dir / f"{name}.log"
    stderr_file = callgrind_dir / f"{name}.callgrind.stderr"
    output_file_template = str(callgrind_dir / f"{name}.callgrind.out.%p")

    log(f"Starting {name} under Valgrind Callgrind...")
    cwd = str(workdir) if workdir is not None else str(log_dir)

    # --separate-threads=yes: this is a multithreaded reactor; without it every
    #   thread is merged into one profile, burying the per-thread costs we care
    #   about (e.g. reactor-thread control-command handling).
    # (--num-callers is a valgrind-core/memcheck option; it does nothing useful
    #  for callgrind, which records the full call graph regardless.)
    valgrind_cmd = [
        "valgrind",
        "--tool=callgrind",
        "--separate-threads=yes",
        f"--callgrind-out-file={output_file_template}",
    ]

    app_cmd = [str(bin_dir / bin_name), str(log_file), str(config)]
    full_cmd = valgrind_cmd + app_cmd

    with open(stderr_file, "w") as stderr_fh, open(log_dir / f"{name}.stdout", "w") as stdout_fh:
        proc = subprocess.Popen(
            full_cmd,
            cwd=cwd,
            stdout=stdout_fh,
            stderr=stderr_fh,
        )
    log(f"  {name} running under Callgrind (PID {proc.pid})")
    return proc


def _wait_for_log_pattern(log_path: Path, label: str, target: int,
                            count_fn, timeout: float,
                            min_idle_timeout: float = 8.0,
                            stall_is_warning: bool = True) -> bool:
    INITIAL_IDLE_TIMEOUT = max(30.0, timeout * 0.10)
    MIN_CALIBRATION_SECS = 2.0
    MIN_IDLE_TIMEOUT     = min_idle_timeout

    deadline        = time.monotonic() + timeout
    total_seen      = 0
    last_change     = time.monotonic()
    rate_start_t    = None
    idle_timeout    = INITIAL_IDLE_TIMEOUT
    rate_calibrated = False
    file_pos        = 0

    log(f"Waiting for {label} #{target:,} in {log_path.name} (timeout {timeout:.0f}s) ...")

    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as fh:
                fh.seek(file_pos)
                chunk = fh.read()
                file_pos = fh.tell()

            if chunk:
                new_count = count_fn(chunk)
                if new_count > 0:
                    now         = time.monotonic()
                    total_seen += new_count
                    last_change = now
                    if rate_start_t is None:
                        rate_start_t = now

                if total_seen >= target:
                    return True

                if rate_start_t is not None:
                    elapsed = time.monotonic() - rate_start_t
                    if elapsed >= MIN_CALIBRATION_SECS:
                        if not rate_calibrated:
                            rate = total_seen / elapsed
                            if rate > 0:
                                remaining    = target - total_seen
                                idle_timeout = max((remaining / rate) * 10, MIN_IDLE_TIMEOUT)
                                rate_calibrated = True
                                log(f"  {label} throughput ~{rate:,.0f}/s → "
                                    f"dynamic idle bail-out {idle_timeout:.0f}s")

            if total_seen > 0 and (time.monotonic() - last_change) >= idle_timeout:
                if stall_is_warning:
                    log(f"  WARNING: {label} stalled at {total_seen:,} / {target:,} "
                        f"— {target - total_seen:,} ERs not delivered before timeout.")
                else:
                    log(f"  {label} live count stalled at {total_seen:,} / {target:,} "
                        f"— {target - total_seen:,} NOS still in the sequencer pipeline; "
                        f"GW-ER-SENT is the authoritative completion signal.")
                return False

        time.sleep(0.1)

    log(f"  TIMEOUT: {label} seen = {total_seen:,} / {target:,}")
    return False


def wait_for_order_completion(me_log: Path, total_orders: int, timeout: float) -> bool:
    target_str      = f"ME-ORD-{total_orders}"
    target_pattern  = re.compile(re.escape(target_str) + r"(?!\d)")
    any_ord_pattern = re.compile(r"ME-ORD-(\d+)")
    highest_seen = [0]

    def count_nos(chunk: str) -> int:
        if target_pattern.search(chunk):
            highest_seen[0] = total_orders
            return total_orders - highest_seen[0] + 1
        matches = any_ord_pattern.findall(chunk)
        if matches:
            h = max(int(m) for m in matches)
            if h > highest_seen[0]:
                delta = h - highest_seen[0]
                highest_seen[0] = h
                return delta
        return 0

    return _wait_for_log_pattern(me_log, "ME-ORD", total_orders, count_nos, timeout,
                                 stall_is_warning=False)


def wait_for_er_completion(gw_log: Path, total_orders: int, timeout: float) -> bool:
    def count_er(chunk: str) -> int:
        return chunk.count("GW-ER-SENT")

    return _wait_for_log_pattern(gw_log, "GW-ER-SENT", total_orders, count_er, timeout,
                                 min_idle_timeout=120.0)


def poll_log_for(log_path: Path, *markers: str, timeout: float,
                 from_byte: int = 0) -> tuple[bool, float]:
    """Poll log_path for a line containing ALL markers (beyond from_byte).

    Returns (found, elapsed_seconds). Polling means this returns as soon as the
    marker appears, so a large timeout is a safe ceiling, not a fixed wait.
    The component logs are truncated on launch, so from_byte defaults to 0.
    """
    deadline = time.monotonic() + timeout
    pos = from_byte
    t0 = time.monotonic()
    while time.monotonic() < deadline:
        if log_path.is_file():
            with open(log_path, "r", errors="replace") as fh:
                fh.seek(pos)
                chunk = fh.read()
                pos = fh.tell()
            for line in chunk.splitlines():
                if all(m in line for m in markers):
                    return True, time.monotonic() - t0
        time.sleep(LOG_POLL_INTERVAL)
    return False, time.monotonic() - t0


def count_marker(log_path: Path, marker: str) -> int:
    """Count lines in log_path containing marker (0 if the file is absent)."""
    if not log_path.is_file():
        return 0
    with open(log_path, "r", errors="replace") as fh:
        return sum(1 for line in fh if marker in line)


def wait_for_system_ready(seq_log: Path, arb_log: Path, gw_log: Path) -> None:
    """Block until the HA system has elected leaders and the gateway is up.

    Uses log polling, which self-adjusts to callgrind's slowdown, instead of a
    fixed settle. Aborts (die) if any readiness marker is not seen in time.
    """
    log("Waiting for the system to become ready (polling component logs) ...")

    found, secs = poll_log_for(seq_log, *_SEQ_LEADER_MARKERS, timeout=READY_TIMEOUT)
    if not found:
        die(f"sequencer_primary did not elect a leader within {READY_TIMEOUT:.0f}s")
    log(f"  sequencer_primary: leader elected ({secs:.1f}s)")

    found, secs = poll_log_for(arb_log, *_ARB_ACTIVE_MARKERS, timeout=READY_TIMEOUT)
    if not found:
        die(f"arbiter_primary did not become active within {READY_TIMEOUT:.0f}s")
    log(f"  arbiter_primary: active ({secs:.1f}s)")

    # The gateway is profiled under callgrind, so reaching operational (listeners
    # up) is the slow step -- polling waits exactly as long as it takes.
    found, secs = poll_log_for(gw_log, *_GW_OPERATIONAL_MARKERS, timeout=READY_TIMEOUT)
    if not found:
        die(f"order_gateway did not reach operational within {READY_TIMEOUT:.0f}s "
            f"(profiled under callgrind -- consider raising READY_TIMEOUT)")
    log(f"  order_gateway: operational ({secs:.1f}s)")


def wait_for_fix_logons(gw_log: Path, sessions: int, timeout: float,
                        client_procs: list[subprocess.Popen]) -> bool:
    """Poll the gateway log until `sessions` FIX logons have succeeded.

    Bails early if a fix8 client exits before logon -- e.g. it could not connect
    because the profiled gateway was not yet listening, or the logon was rejected.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if count_marker(gw_log, _GW_LOGON_OK) >= sessions:
            return True
        if any(p.poll() is not None for p in client_procs):
            log("  ERROR: a fix8 client exited before its FIX session was established")
            return False
        time.sleep(LOG_POLL_INTERVAL)
    return False


def shutdown_processes(named_procs: list[tuple[str, subprocess.Popen]]) -> None:
    log("Sending SIGTERM (signal 15) to applications to trigger Callgrind dumps...")
    for name, proc in named_procs:
        if proc.poll() is None:
            try:
                proc.send_signal(signal.SIGTERM)
            except Exception:
                pass

    for name, proc in named_procs:
        try:
            proc.wait(timeout=CALLGRIND_FLUSH_TIMEOUT)
            log(f"  {name} exited cleanly (Callgrind profile flushed)")
        except subprocess.TimeoutExpired:
            # A SIGKILL'd valgrind writes NO profile, so this loses the data for
            # this process. Only do it as a last resort to avoid hanging forever.
            log(f"  WARNING: {name} did not exit within {CALLGRIND_FLUSH_TIMEOUT:.0f}s after "
                f"SIGTERM — sending SIGKILL. Its Callgrind profile will be LOST; raise "
                f"CALLGRIND_FLUSH_TIMEOUT if this recurs.")
            proc.kill()
            proc.wait()


def generate_reports(app_names: list[str], callgrind_dir: Path) -> None:
    report_path = callgrind_dir / "report.txt"
    log(f"Generating callgrind reports → {callgrind_dir}")

    with open(report_path, "w") as report_fh:
        for name in app_names:
            data_files = sorted(callgrind_dir.glob(f"{name}.callgrind.out.*"))
            if not data_files:
                log(f"  WARNING: no callgrind data found for {name} — skipping")
                continue

            for data in data_files:
                header = (
                    f"{'=' * 70}\n"
                    f"  {name} ({data.name})\n"
                    f"{'=' * 70}\n"
                )
                print(header, end="")
                report_fh.write(header)

                result = subprocess.run(
                    ["callgrind_annotate", "--auto=yes", str(data)],
                    capture_output=True, text=True,
                )

                print(result.stdout)
                report_fh.write(result.stdout + "\n")

    log(f"Combined text report : {report_path}")


def run_fix8_session(me_log: Path, gw_log: Path, burst: int, clients: int) -> None:
    total_orders = clients * burst * 1000
    log(f"=== Starting {clients} fix8 client(s), {burst} T burst(s) each "
        f"({total_orders} orders total) ===")

    procs: list[subprocess.Popen] = []
    for i in range(clients):
        proc = subprocess.Popen(
            [str(FIX8_BIN), "-c", FIX8_CFG, "-N", "GW1"],
            cwd=str(FIX8_DIR),
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(proc)
        log(f"  client {i + 1} of {clients}: f8test PID {proc.pid}")

    log(f"  Waiting for {clients} FIX session(s) to log on (poll, up to {LOGON_TIMEOUT:.0f}s) ...")
    if not wait_for_fix_logons(gw_log, clients, LOGON_TIMEOUT, procs):
        die(f"only {count_marker(gw_log, _GW_LOGON_OK)}/{clients} FIX session(s) "
            f"logged on within {LOGON_TIMEOUT:.0f}s — cannot send orders")
    log(f"  {clients} FIX session(s) established")

    log(f"  Sending {burst} T command(s) to each of {clients} client(s) ...")
    for i, proc in enumerate(procs):
        try:
            for _ in range(burst):
                proc.stdin.write(b"T\n")
            proc.stdin.flush()
        except BrokenPipeError:
            die(f"f8test client {i + 1} stdin pipe broke before T commands were sent")

    # A generous ceiling scaled by order count; polling (with its own idle
    # bail-out) returns as soon as the pipeline drains, so this only bounds a hang.
    timeout = ORDER_TIMEOUT * max(1, burst * clients)

    nos_ok = wait_for_order_completion(me_log, total_orders, timeout)
    if not nos_ok:
        log(f"  ME-ORD live count did not reach {total_orders:,} — "
            f"pipeline still draining; proceeding to ER phase (authoritative)")
    else:
        log(f"  All {total_orders:,} NOS confirmed in matching engine log")

    er_ok = wait_for_er_completion(gw_log, total_orders, timeout)
    if not er_ok:
        log(f"  WARNING: not all ERs delivered before timeout — {total_orders} ERs expected")

    log("  Terminating fix8 client(s) immediately with SIGKILL ...")
    for proc in procs:
        if proc.poll() is None:
            proc.kill()
    for proc in procs:
        proc.wait()
    log(f"  All {clients} fix8 client(s) stopped")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                       formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix", nargs="?", default="installed",
                        metavar="install_prefix",
                        help="Path to the cmake install prefix (default: installed)")
    parser.add_argument("--burst", type=int, default=1, metavar="N",
                        help="Number of T commands per fix8 session (each T = 1000 NOS). Default: 1")
    parser.add_argument("--clients", type=int, default=1, metavar="N",
                        help="Number of concurrent fix8 sessions. Default: 1")
    parser.add_argument("--capture", action="store_true", default=False,
                        help="Enable FIX capture (writes all wire bytes to fix_capture.bin).")
    parser.add_argument("--logon-mode", choices=["scram", "proprietary"], default="scram",
                        help="Authentication mode for fix8 clients.")
    parser.add_argument("--slowdown-factor", type=int, default=50, metavar="N",
                        help="Multiply HA/session timeouts by N for the run so callgrind's "
                             "~20-50x slowdown does not trip failover or FIX logon timeouts. "
                             "Restored afterwards. Default: 50. Use 1 to leave configs untouched.")
    args = parser.parse_args()
    if args.burst < 1:
        parser.error("--burst must be >= 1")
    if args.clients < 1:
        parser.error("--clients must be >= 1")
    if args.slowdown_factor < 1:
        parser.error("--slowdown-factor must be >= 1")

    script_dir = Path(__file__).resolve().parent
    prefix     = resolve_prefix(str(script_dir / args.prefix)
                                if not Path(args.prefix).is_absolute()
                                else args.prefix)
    bin_dir    = prefix / "bin"
    etc_dir    = prefix / "etc"
    log_dir    = prefix / "log"
    ts         = datetime.now().strftime("%Y%m%d_%H%M%S")
    callgrind_dir = prefix / "callgrind" / ts
    me_log     = log_dir / "matching_engine_primary.log"
    gw_log     = log_dir / "order_gateway.log"
    seq_log    = log_dir / "sequencer_primary.log"
    arb_log    = log_dir / "arbiter_primary.log"

    gw_config = prefix / "etc" / "order_gateway" / "order_gateway.toml"

    # Launched components whose HA/session timeouts must be relaxed so callgrind's
    # slowdown does not trip failover or the gateway's own FIX logon timeout.
    timeout_configs = [
        etc_dir / "arbiter" / "arbiter_primary.toml",
        etc_dir / "arbiter" / "arbiter_secondary.toml",
        etc_dir / "matching_engine" / "matching_engine_primary.toml",
        etc_dir / "matching_engine" / "matching_engine_secondary.toml",
        etc_dir / "sequencer" / "sequencer_primary.toml",
        etc_dir / "sequencer" / "sequencer_secondary.toml",
        gw_config,  # logon_timeout / scram_auth_timeout
    ]

    preflight(prefix)
    log_dir.mkdir(parents=True, exist_ok=True)
    callgrind_dir.mkdir(parents=True, exist_ok=True)

    if args.capture and not gw_config.is_file():
        die(f"order_gateway config not found: {gw_config}")

    lib_dir = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    log("=== callgrind_run ===")
    log(f"  install prefix : {prefix}")
    log(f"  callgrind output: {callgrind_dir}")
    targets_desc = ", ".join(sorted(CALLGRIND_TARGETS)) if CALLGRIND_TARGETS is not None else "all"
    log(f"  valgrind tool  : callgrind")
    log(f"  callgrind targets: {targets_desc}")
    log(f"  clients        : {args.clients}")
    log(f"  burst          : {args.burst}  ({args.clients * args.burst * 1000} orders total)")
    log(f"  FIX capture    : {'enabled' if args.capture else 'disabled'}")
    log(f"  logon mode     : {args.logon_mode}")
    log(f"  timeout scale  : x{args.slowdown_factor}")

    log("Exporting credentials ...")
    creds_file   = etc_dir / "authentication_service" / "credentials.toml"
    export_script = script_dir / "db" / "export_credentials.py"
    result = subprocess.run(
        [sys.executable, str(export_script),
         "--credentials-file", str(creds_file),
         "--db-host", "localhost",
         "--db-port", "5432",
         "--db-name", "pubsub",
         "--db-user", "pubsub_app"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        die(f"export_credentials.py failed:\n{result.stderr.strip()}")
    log("  credentials exported")
    ensure_fix8_credentials(creds_file, FIX8_COMP_ID, FIX8_PASSWORD, args.logon_mode)

    steps = [
        ("auth_service_a",   "authentication_service", etc_dir / "authentication_service" / "authentication_service_a.toml",  etc_dir / "authentication_service"),
        ("auth_service_b", "authentication_service", etc_dir / "authentication_service" / "authentication_service_b.toml", etc_dir / "authentication_service"),
        ("witness",            "witness",                etc_dir / "witness"                / "witness.toml",                None),
        ("arbiter_primary",        "arbiter",                etc_dir / "arbiter"                / "arbiter_primary.toml",        None),
        ("arbiter_secondary",      "arbiter",                etc_dir / "arbiter"                / "arbiter_secondary.toml",      None),
        ("matching_engine_primary", "matching_engine",        etc_dir / "matching_engine"        / "matching_engine_primary.toml", None),
        ("matching_engine_secondary", "matching_engine",      etc_dir / "matching_engine"        / "matching_engine_secondary.toml",None),
        ("sequencer_primary",      "sequencer",              etc_dir / "sequencer"              / "sequencer_primary.toml",      None),
        ("sequencer_secondary",    "sequencer",              etc_dir / "sequencer"              / "sequencer_secondary.toml",    None),
        ("order_gateway",          "order_gateway",          etc_dir / "order_gateway"          / "order_gateway.toml",          etc_dir / "order_gateway"),
    ]

    app_procs: list[tuple[str, subprocess.Popen]] = []

    # Populated by relax_timeouts() inside the try; reverted in the finally so the
    # configs are always restored, even on early failure.
    original_configs: dict[Path, str] = {}

    def full_shutdown() -> None:
        shutdown_processes(app_procs)
        # Only the profiled processes produce callgrind output; reporting the
        # unprofiled ones would just emit "no data" noise.
        profiled = [n for n, _ in app_procs
                    if CALLGRIND_TARGETS is None or n in CALLGRIND_TARGETS]
        if profiled:
            generate_reports(profiled, callgrind_dir)

    try:
        # Relax HA/session timeouts (reverted in finally); the FIX-capture toggle,
        # if requested, is applied on top and reverted by the same restore.
        relax_timeouts(timeout_configs, args.slowdown_factor, original_configs)
        if args.slowdown_factor != 1:
            log(f"Relaxed HA/session timeouts x{args.slowdown_factor} for the profiling run")
        if args.capture:
            set_fix_capture_enabled(gw_config, True)
            log("FIX capture enabled in order_gateway config")

        for name, bin_name, config, workdir in steps:
            if CALLGRIND_TARGETS is None or name in CALLGRIND_TARGETS:
                proc = launch_app_under_callgrind(
                    name, bin_name, config, bin_dir, log_dir, callgrind_dir, workdir
                )
            else:
                log_file = log_dir / f"{name}.log"
                cwd = str(workdir) if workdir is not None else str(log_dir)
                log(f"Starting {name} (unprofiled) ...")
                with open(log_dir / f"{name}.stdout", "w") as stdout_fh:
                    proc = subprocess.Popen(
                        [str(bin_dir / bin_name), str(log_file), str(config)],
                        cwd=cwd, stdout=stdout_fh, stderr=subprocess.STDOUT
                    )
                log(f"  {name} PID {proc.pid}")

            app_procs.append((name, proc))
            time.sleep(STARTUP_DELAY)

        # Fail fast if any process died immediately at launch (bad config, port
        # clash, missing lib). A live-but-not-yet-ready process is handled by the
        # readiness polling below, which self-adjusts to callgrind's slowdown.
        for name, proc in app_procs:
            if proc.poll() is not None:
                die(f"{name} (PID {proc.pid}) died during startup "
                    f"(exit code {proc.returncode})")

        wait_for_system_ready(seq_log, arb_log, gw_log)

        run_fix8_session(me_log, gw_log, args.burst, args.clients)

        log(f"Waiting {POST_ORDER_WAIT:.0f}s for pipeline to drain ...")
        time.sleep(POST_ORDER_WAIT)

        full_shutdown()

    except KeyboardInterrupt:
        log("Interrupted — shutting down cleanly ...")
        full_shutdown()
        sys.exit(130)
    except BaseException:
        log("Failure — shutting down all running processes ...")
        shutdown_processes(app_procs)
        raise
    finally:
        # Revert timeout relaxation (and any FIX-capture toggle applied on top)
        # by writing back the exact pre-run config text.
        restore_configs(original_configs)
        log("Restored component configs (timeouts + FIX capture)")

    total_orders = args.clients * args.burst * 1000
    def count_in_log(path: Path, marker: str) -> int:
        try:
            return sum(1 for line in path.open(errors="replace") if marker in line)
        except FileNotFoundError:
            return 0

    # Count only the ME order-acceptance line ("accepted NOS OrderID=ME-ORD-N").
    # A bare "ME-ORD" substring also matches the cancel-ER lines ("sent cancel ER
    # OrderID=ME-ORD-N"), which double-counts one entry per cancelled order.
    me_final     = count_in_log(me_log,  "accepted NOS")
    gw_nos_recv  = count_in_log(gw_log,  "GW-NOS-RECV")
    gw_er_sent   = count_in_log(gw_log,  "GW-ER-SENT")
    gw_gap_fills = count_in_log(gw_log,  "SequenceReset-GapFill")

    def count_status(actual: int, expected: int) -> str:
        diff = actual - expected
        if diff == 0:
            return "OK"
        if diff > 0:
            return f"EXCESS by +{diff:,}"
        return f"SHORT by {-diff:,}"

    log("=== Post-shutdown ground-truth counts ===")
    log(f"  ME-ORD        : {me_final:>10,} / {total_orders:,}  {count_status(me_final, total_orders)}")
    log(f"  GW-NOS-RECV   : {gw_nos_recv:>10,} / {total_orders:,}  {count_status(gw_nos_recv, total_orders)}")
    log(f"  GW-ER-SENT    : {gw_er_sent:>10,} / {total_orders:,}  {count_status(gw_er_sent, total_orders)}")
    er_discrepancy = gw_er_sent - gw_nos_recv
    if er_discrepancy == 0:
        log(f"  NOS→ER match  : YES — one ER per NOS")
    elif er_discrepancy > 0:
        log(f"  NOS→ER match  : {er_discrepancy:,} extra ERs (partial fills or late cancel ACKs)")
    else:
        log(f"  NOS→ER match  : NO — {-er_discrepancy:,} ERs missing vs NOS count load")
    if gw_gap_fills > 0:
        log(f"  Gap fills     : {gw_gap_fills}")


if __name__ == "__main__":
    main()
