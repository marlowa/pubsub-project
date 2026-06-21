#!/usr/bin/env python3
from __future__ import annotations
"""
perf_run.py — start the full FIX sequencer system under perf, fire NOS orders
              via fix8, SIGTERM all processes, and produce per-process perf
              reports and flamegraph SVGs.

Usage (from the project root):
    ./perf_run.py                              # 1 client, 1 burst
    ./perf_run.py --burst=5                    # 1 client, 5 x T = 5000 orders
    ./perf_run.py --clients=3                  # 3 concurrent clients, 1 burst each
    ./perf_run.py --burst=4 --clients=2        # 2 clients x 4 bursts = 8000 orders
    ./perf_run.py installed --burst=2    # explicit install prefix

Options:
    --burst=N    Number of times the 'T' command is sent per fix8 session.
                 Each 'T' sends 1000 NOS messages.  Default: 1.
    --clients=N  Number of concurrent fix8 sessions.  All sessions start
                 simultaneously and each sends --burst T commands.  Default: 1.

Output directory:
    <prefix>/perf/<YYYYMMDD_HHMMSS>/
        <name>.perf.data     raw perf samples (one per process)
        <name>.perf.stderr   perf record stderr (for sanity checking)
        <name>.svg           flamegraph SVG (requires FlameGraph scripts)
        <name>.jpg           flamegraph JPG (requires ImageMagick convert)
        report.txt           combined perf report (--stdio, flat)
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
STARTUP_DELAY   = 1.0    # seconds between app launches
SETTLE_TIME     = 3.0    # seconds after last app before attaching perf
FIX8_LOGON_WAIT = 3.0    # seconds for fix8 to establish the FIX session
ORDER_TIMEOUT   = 180.0  # seconds to wait for ord1000 in the ME log
POST_ORDER_WAIT = 2.0    # seconds after last order before SIGTERM
CALLGRAPH        = "dwarf" # dwarf unwinds across the user/kernel boundary; resolves the
                           # otherwise-anonymous kernel stacks that dominate the gateway profile.
                           # fp would suffice for pure-userspace profiling but loses the call
                           # chain whenever a sample lands inside a syscall (epoll, recv, send).
DWARF_STACK_SIZE = 4096    # bytes per sample; default 8192 — halving saves significant RAM
PERF_MMAP_SIZE   = "16M"   # per-CPU ring-buffer cap passed to -m; prevents OOM under load
FREQ             = 99      # perf sample frequency (Hz)
# Processes to profile; set to None to profile all launched processes.
# Profiling arbiters, the witness, and both sequencers with DWARF is expensive
# and rarely useful; the hot path is gateway and ME.
PERF_TARGETS     = {"order_gateway", "matching_engine"}
SHUTDOWN_TIMEOUT = 5.0   # seconds to wait for each app to exit after SIGTERM

FIX8_DIR  = Path("/home/marlowa/mystuff/fix8_install")
FIX8_BIN  = FIX8_DIR / "bin" / "f8test"
FIX8_CFG  = "myfix_gateway_client.xml"
FLAMEGRAPH = Path("/home/marlowa/mystuff/FlameGraph")
# ──────────────────────────────────────────────────────────────────────────────


def log(msg: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def die(msg: str) -> None:
    log(f"ERROR: {msg}")
    sys.exit(1)


def set_fix_capture_enabled(config_path: Path, enabled: bool) -> None:
    """Patch the enabled flag in the order_gateway fix_capture config section."""
    text = config_path.read_text()
    patched = re.sub(r'(?m)^(enabled\s*=\s*)(true|false)',
                     lambda m: m.group(1) + ("true" if enabled else "false"),
                     text)
    config_path.write_text(patched)


def resolve_prefix(raw: str) -> Path:
    p = Path(raw).resolve()
    if not p.is_dir():
        die(f"install prefix '{raw}' does not exist or is not a directory")
    return p


def preflight(prefix: Path) -> None:
    if not FIX8_BIN.is_file() or not os.access(FIX8_BIN, os.X_OK):
        die(f"f8test not found or not executable: {FIX8_BIN}")
    if subprocess.call(["which", "perf"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        die("'perf' not found in PATH")
    for name in ("witness", "arbiter", "sequencer", "matching_engine", "order_gateway"):
        exe = prefix / "bin" / name
        if not exe.is_file() or not os.access(exe, os.X_OK):
            die(f"binary not found or not executable: {exe}")


def launch_app(name: str, bin_name: str, config: Path,
               bin_dir: Path, log_dir: Path,
               workdir: Path | None = None) -> subprocess.Popen:
    if not config.is_file():
        die(f"config not found: {config}")
    log_file = log_dir / f"{name}.log"
    log(f"Starting {name} ...")
    cwd = str(workdir) if workdir is not None else str(log_dir)
    with open(log_dir / f"{name}.stdout", "w") as stdout_fh:
        proc = subprocess.Popen(
            [str(bin_dir / bin_name), str(log_file), str(config)],
            cwd=cwd,
            stdout=stdout_fh,
            stderr=subprocess.STDOUT,
        )
    log(f"  {name} PID {proc.pid}")
    return proc


def attach_perf(name: str, pid: int, perf_dir: Path) -> subprocess.Popen:
    data_file  = perf_dir / f"{name}.perf.data"
    stderr_file = perf_dir / f"{name}.perf.stderr"
    call_graph_arg = f"{CALLGRAPH},{DWARF_STACK_SIZE}" if CALLGRAPH == "dwarf" else CALLGRAPH
    with open(stderr_file, "w") as stderr_fh:
        proc = subprocess.Popen(
            ["perf", "record", "-p", str(pid), "-o", str(data_file),
             "--call-graph", call_graph_arg, "-F", str(FREQ), "-m", PERF_MMAP_SIZE],
            stdout=subprocess.DEVNULL,
            stderr=stderr_fh,
        )
    log(f"  perf → {name} (PID {pid}) → {data_file.name}")
    return proc


def _wait_for_log_pattern(log_path: Path, label: str, target: int,
                           count_fn, timeout: float,
                           min_idle_timeout: float = 8.0,
                           stall_is_warning: bool = True) -> bool:
    """
    Generic log-polling loop used by both wait phases.

    count_fn(chunk: str) -> int  counts matching events in a new chunk.

    min_idle_timeout controls the floor for the dynamic bail-out.  The ME
    phase uses the default 8s.  The ER phase passes 120s because the initial
    calibration captures a Quill burst-flush rate, not the true sequencer→
    gateway pipeline drain rate.

    stall_is_warning controls log severity on stall.  GW-ER-SENT stall is a
    genuine failure (pass True).  ME-ORD stall is a pipeline-delay artefact —
    the remaining NOS are still in transit; the ER phase is authoritative
    (pass False, which logs an informational message instead of a warning).

    Returns True when the running total reaches `target`, False on timeout
    or stall.
    """
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
                        # Only calculate the throughput and lock in the timeout once
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
    """Phase 1: wait for the ME to accept all NOS orders."""
    target_str      = f"ME-ORD-{total_orders}"
    target_pattern  = re.compile(re.escape(target_str) + r"(?!\d)")
    any_ord_pattern = re.compile(r"ME-ORD-(\d+)")
    # Wrap the generic loop: we need the highest-seen counter rather than a
    # simple running count, so we use a closure that resets on each chunk.
    highest_seen = [0]

    def count_nos(chunk: str) -> int:
        if target_pattern.search(chunk):
            highest_seen[0] = total_orders
            return total_orders - highest_seen[0] + 1  # signal completion
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
    """Phase 2: wait for the gateway to deliver all ERs back to fix8 clients.

    Counts GW-ER-SENT lines in the gateway log.  Each line represents one
    completed NOS→ER round-trip.  This is the correct completion criterion:
    the fix8 T command fires NOS messages without waiting for ERs, so the ME
    finishing before the ERs have returned through the sequencer→gateway path
    only measures NOS intake, not round-trip throughput.

    The min_idle_timeout is set to 120s (not the default 8s) because the
    initial calibration rate is dominated by a Quill burst-flush of already-
    queued entries, not the true sequencer→gateway pipeline drain rate.  The
    actual pipeline can be 100× slower than the burst; 120s gives it time to
    drain the backlog that accumulated during the ME phase.
    """
    def count_er(chunk: str) -> int:
        return chunk.count("GW-ER-SENT")

    return _wait_for_log_pattern(gw_log, "GW-ER-SENT", total_orders, count_er, timeout,
                                  min_idle_timeout=120.0)


def shutdown_processes(named_procs: list[tuple[str, subprocess.Popen]]) -> None:
    log("Sending SIGTERM to all applications ...")
    for name, proc in named_procs:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
    for name, proc in named_procs:
        try:
            proc.wait(timeout=SHUTDOWN_TIMEOUT)
            log(f"  {name} exited")
        except subprocess.TimeoutExpired:
            log(f"  WARNING: {name} did not exit within {SHUTDOWN_TIMEOUT:.0f}s — sending SIGKILL")
            proc.kill()
            proc.wait()


def stop_perf_procs(perf_procs: list[tuple[str, subprocess.Popen]]) -> None:
    log("Waiting for perf to finish writing data ...")
    # perf record exits automatically once the monitored process dies;
    # we just need to wait for it to flush and close the data file.
    for name, proc in perf_procs:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            log(f"  WARNING: perf for {name} did not exit — killing")
            proc.kill()
            proc.wait()


def generate_reports(app_names: list[str], perf_dir: Path) -> None:
    report_path = perf_dir / "report.txt"
    log(f"Generating perf reports → {perf_dir}")

    with open(report_path, "w") as report_fh:
        for name in app_names:
            data = perf_dir / f"{name}.perf.data"
            if not data.is_file():
                log(f"  WARNING: no perf data for {name} — skipping")
                continue

            header = (
                f"{'=' * 70}\n"
                f"  {name}\n"
                f"{'=' * 70}\n"
            )
            print(header, end="")
            report_fh.write(header)

            result = subprocess.run(
                ["perf", "report", "-i", str(data), "--stdio", "--no-children"],
                capture_output=True, text=True,
            )
            print(result.stdout)
            report_fh.write(result.stdout + "\n")

            # Flamegraph SVG
            if FLAMEGRAPH.is_dir():
                svg_path = perf_dir / f"{name}.svg"
                try:
                    script = subprocess.run(
                        ["perf", "script", "-i", str(data)],
                        capture_output=True,
                    )
                    collapse = subprocess.run(
                        [str(FLAMEGRAPH / "stackcollapse-perf.pl")],
                        input=script.stdout, capture_output=True,
                    )
                    flamegraph = subprocess.run(
                        [str(FLAMEGRAPH / "flamegraph.pl")],
                        input=collapse.stdout, capture_output=True,
                    )
                    svg_path.write_bytes(flamegraph.stdout)
                    log(f"  flamegraph: {svg_path.name}")

                    # Convert SVG → JPG via ImageMagick convert
                    jpg_path = svg_path.with_suffix(".jpg")
                    convert = subprocess.run(
                        ["convert", str(svg_path), str(jpg_path)],
                        capture_output=True,
                    )
                    if convert.returncode == 0:
                        log(f"  jpg:        {jpg_path.name}")
                    else:
                        log(f"  WARNING: SVG→JPG conversion failed for {name} "
                            f"(is ImageMagick installed?)")
                except Exception as exc:  # pylint: disable=broad-except
                    log(f"  WARNING: flamegraph failed for {name}: {exc}")

    log(f"Combined text report : {report_path}")
    log(f"Per-process SVGs     : {perf_dir}/*.svg")


def run_fix8_session(me_log: Path, gw_log: Path, burst: int, clients: int) -> None:
    """
    Start `clients` concurrent f8test processes, wait for all FIX sessions to
    log on, then send `burst` 'T' commands to each (each T = 1000 NOS).

    Two-phase completion:
      Phase 1 — wait for the ME to accept all NOS orders (ME-ORD-N).
      Phase 2 — wait for the gateway to deliver all ERs back to fix8 clients
                 (GW-ER-SENT count reaches total_orders).  fix8 T commands
                 fire NOS messages without waiting for ERs, so only phase 2
                 measures true round-trip throughput.

    fix8 clients are kept alive through both phases so the gateway can deliver
    ERs.  They are killed only after phase 2 completes (or times out).
    """
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

    log(f"  Waiting {FIX8_LOGON_WAIT:.0f}s for FIX logon(s) ...")
    time.sleep(FIX8_LOGON_WAIT)

    log(f"  Sending {burst} T command(s) to each of {clients} client(s) ...")
    for i, proc in enumerate(procs):
        try:
            for _ in range(burst):
                proc.stdin.write(b"T\n")
            proc.stdin.flush()
        except BrokenPipeError:
            die(f"f8test client {i + 1} stdin pipe broke before T commands were sent")

    # Scale the timeout proportionally to total_orders, but cap at 10 minutes.
    MAX_ORDER_TIMEOUT = 600.0
    timeout = min(ORDER_TIMEOUT * max(1, burst * clients), MAX_ORDER_TIMEOUT)

    # Phase 1: ME intake.  This is a progress indicator only — the ME processes
    # NOS in pipeline order and its log may lag the ER log.  A stall here does
    # not mean orders are lost; GW-ER-SENT is the authoritative end-to-end signal.
    nos_ok = wait_for_order_completion(me_log, total_orders, timeout)
    if not nos_ok:
        log(f"  ME-ORD live count did not reach {total_orders:,} — "
            f"pipeline still draining; proceeding to ER phase (authoritative)")
    else:
        log(f"  All {total_orders:,} NOS confirmed in matching engine log")

    # Phase 2: full round-trip — wait for gateway to deliver all ERs.
    # Use the same timeout budget; after ME completion most of the remaining
    # time is pipeline drain.
    er_ok = wait_for_er_completion(gw_log, total_orders, timeout)
    if not er_ok:
        log(f"  WARNING: not all ERs delivered before timeout — {total_orders} ERs expected")

    log("  Terminating fix8 client(s) ...")
    if er_ok:
        # Both phases completed cleanly. f8test ignores SIGTERM so use SIGKILL.
        for proc in procs:
            if proc.poll() is None:
                proc.kill()
        for proc in procs:
            proc.wait()
    else:
        # Timed out or stalled: SIGTERM first, then SIGKILL if needed.
        for proc in procs:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for i, proc in enumerate(procs):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                log(f"  WARNING: client {i + 1} did not exit — killing")
                proc.kill()
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
    args = parser.parse_args()
    if args.burst < 1:
        parser.error("--burst must be >= 1")
    if args.clients < 1:
        parser.error("--clients must be >= 1")

    script_dir = Path(__file__).resolve().parent
    prefix     = resolve_prefix(str(script_dir / args.prefix)
                                if not Path(args.prefix).is_absolute()
                                else args.prefix)
    bin_dir    = prefix / "bin"
    etc_dir    = prefix / "etc"
    log_dir    = prefix / "log"
    ts         = datetime.now().strftime("%Y%m%d_%H%M%S")
    perf_dir   = prefix / "perf" / ts
    me_log     = log_dir / "matching_engine.log"
    gw_log     = log_dir / "order_gateway.log"

    gw_config = prefix / "etc" / "order_gateway" / "order_gateway.toml"

    preflight(prefix)
    log_dir.mkdir(parents=True, exist_ok=True)
    perf_dir.mkdir(parents=True, exist_ok=True)

    if args.capture:
        if not gw_config.is_file():
            die(f"order_gateway config not found: {gw_config}")
        set_fix_capture_enabled(gw_config, True)
        log("FIX capture enabled in order_gateway config")

    # Extend LD_LIBRARY_PATH so the installed shared library is found.
    lib_dir = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    log("=== perf_run ===")
    log(f"  install prefix : {prefix}")
    log(f"  perf output    : {perf_dir}")
    cg_desc = f"{CALLGRAPH},{DWARF_STACK_SIZE}" if CALLGRAPH == "dwarf" else CALLGRAPH
    targets_desc = ", ".join(sorted(PERF_TARGETS)) if PERF_TARGETS is not None else "all"
    log(f"  call-graph     : {cg_desc}  (freq={FREQ} Hz, mmap={PERF_MMAP_SIZE})")
    log(f"  perf targets   : {targets_desc}")
    log(f"  clients        : {args.clients}")
    log(f"  burst          : {args.burst}  ({args.clients * args.burst * 1000} orders total)")
    log(f"  FIX capture    : {'enabled' if args.capture else 'disabled'}")

    # -- export SCRAM credentials from the database before starting the auth service
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

    # -- launch applications in dependency order (mirrors dev.toml startup_order)
    # Each tuple: (name, binary, config, optional_workdir).
    # auth services must start first; gateway connects to them on startup.
    steps = [
        ("auth_service_primary",   "authentication_service", etc_dir / "authentication_service" / "authentication_service.toml",  etc_dir / "authentication_service"),
        ("auth_service_secondary", "authentication_service", etc_dir / "authentication_service" / "authentication_service_secondary.toml", etc_dir / "authentication_service"),
        ("witness",                "witness",                etc_dir / "witness"               / "witness.toml",                  None),
        ("arbiter_primary",        "arbiter",                etc_dir / "arbiter"               / "arbiter.toml",                  None),
        ("arbiter_secondary",      "arbiter",                etc_dir / "arbiter"               / "arbiter_secondary.toml",        None),
        ("matching_engine",        "matching_engine",        etc_dir / "matching_engine"       / "matching_engine.toml",          None),
        ("sequencer_primary",      "sequencer",              etc_dir / "sequencer"             / "sequencer.toml",                None),
        ("sequencer_secondary",    "sequencer",              etc_dir / "sequencer"             / "sequencer_secondary.toml",      None),
        ("order_gateway",          "order_gateway",          etc_dir / "order_gateway"         / "order_gateway.toml",            None),
    ]

    app_procs:  list[tuple[str, subprocess.Popen]] = []
    perf_procs: list[tuple[str, subprocess.Popen]] = []

    def full_shutdown() -> None:
        shutdown_processes(app_procs)
        stop_perf_procs(perf_procs)
        if app_procs:
            generate_reports([n for n, _ in app_procs], perf_dir)

    try:
        for name, bin_name, config, workdir in steps:
            proc = launch_app(name, bin_name, config, bin_dir, log_dir, workdir)
            app_procs.append((name, proc))
            time.sleep(STARTUP_DELAY)

        log(f"Settling for {SETTLE_TIME:.0f}s ...")
        time.sleep(SETTLE_TIME)

        # Verify all apps are still alive
        for name, proc in app_procs:
            if proc.poll() is not None:
                die(f"{name} (PID {proc.pid}) died during startup "
                    f"(exit code {proc.returncode})")

        # Attach perf to targeted processes
        log("=== Attaching perf to all processes ===")
        for name, proc in app_procs:
            if PERF_TARGETS is None or name in PERF_TARGETS:
                perf_proc = attach_perf(name, proc.pid, perf_dir)
                perf_procs.append((name, perf_proc))
        time.sleep(1)  # give perf a moment to start recording

        # Fire fix8 session(s)
        run_fix8_session(me_log, gw_log, args.burst, args.clients)

        log(f"Waiting {POST_ORDER_WAIT:.0f}s for pipeline to drain ...")
        time.sleep(POST_ORDER_WAIT)

        full_shutdown()

    except KeyboardInterrupt:
        log("Interrupted — shutting down cleanly ...")
        full_shutdown()
        if args.capture:
            set_fix_capture_enabled(gw_config, False)
        sys.exit(130)
    finally:
        if args.capture:
            set_fix_capture_enabled(gw_config, False)
            log("FIX capture disabled in order_gateway config")

    # Post-shutdown ground-truth counts.  Read after all processes have exited so
    # any in-flight log entries are flushed.  GW-ER-SENT is the authoritative
    # end-to-end signal: it confirms the full NOS→ME→sequencer→gateway→fix8 path.
    total_orders = args.clients * args.burst * 1000
    def count_in_log(path: Path, marker: str) -> int:
        try:
            return sum(1 for line in path.open(errors="replace") if marker in line)
        except FileNotFoundError:
            return 0

    me_final     = count_in_log(me_log,  "ME-ORD")
    gw_nos_recv  = count_in_log(gw_log,  "GW-NOS-RECV")
    gw_er_sent   = count_in_log(gw_log,  "GW-ER-SENT")
    gw_gap_fills = count_in_log(gw_log,  "SequenceReset-GapFill")
    me_ok        = me_final    == total_orders
    nos_ok       = gw_nos_recv == total_orders
    # ERs can exceed NOS count when the ME generates multiple fills per order
    # (e.g. partial fill + final fill). "short" is a real failure; "excess" is
    # expected and not a failure.
    er_short     = gw_er_sent  <  total_orders
    er_excess    = gw_er_sent  >  total_orders

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
        log(f"  NOS→ER match  : NO — {-er_discrepancy:,} ERs missing vs NOS count")
    if gw_gap_fills > 0:
        log(f"  Gap fills     : {gw_gap_fills:>10,}  (FIX SequenceReset-GapFill sent in response to ResendRequest)")

    # PASS criteria: gateway received every NOS and every NOS generated an ER
    # back to a client.  ME-ORD excess is expected whenever cancel-on-disconnect
    # is active (the ME logs the same ME-ORD-N marker for both accepted orders
    # and gateway-initiated cancel requests), so me_ok is informational only.
    if nos_ok and not er_short:
        if er_excess:
            log(f"=== PASS — all orders processed; {gw_er_sent - total_orders:,} extra ERs "
                f"(partial fills or HA double-forwarding) ===")
        else:
            log("=== PASS — all orders processed and every ER delivered ===")
        if not me_ok:
            log(f"  (ME-ORD excess of +{me_final - total_orders:,} reflects "
                f"gateway-initiated cancel requests processed by the ME — expected)")
    elif er_short:
        log(f"=== FAIL — {total_orders - gw_er_sent:,} ERs not delivered "
            f"(lost in pipeline or fix8 client disconnected) ===")
    elif not nos_ok:
        log(f"=== FAIL — {total_orders - gw_nos_recv:,} NOS not received by gateway ===")
    else:
        log(f"=== FAIL — unexpected state: "
            f"ME-ORD={me_final:,} NOS={gw_nos_recv:,} ER={gw_er_sent:,} ===")

    log("=== Done ===")


if __name__ == "__main__":
    main()
