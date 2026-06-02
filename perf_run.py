#!/usr/bin/env python3
"""
perf_run.py — start the full FIX sequencer system under perf, fire NOS orders
              via fix8, SIGTERM all processes, and produce per-process perf
              reports and flamegraph SVGs.

Usage (from the project root):
    ./perf_run.py                              # 1 client, 1 burst
    ./perf_run.py --burst=5                    # 1 client, 5 x T = 5000 orders
    ./perf_run.py --clients=3                  # 3 concurrent clients, 1 burst each
    ./perf_run.py --burst=4 --clients=2        # 2 clients x 4 bursts = 8000 orders
    ./perf_run.py build/installed --burst=2    # explicit install prefix

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
ORDER_TIMEOUT   = 120.0  # seconds to wait for ord1000 in the ME log
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
               bin_dir: Path, log_dir: Path) -> subprocess.Popen:
    if not config.is_file():
        die(f"config not found: {config}")
    log_file = log_dir / f"{name}.log"
    log(f"Starting {name} ...")
    with open(log_dir / f"{name}.stdout", "w") as stdout_fh:
        proc = subprocess.Popen(
            [str(bin_dir / bin_name), str(log_file), str(config)],
            cwd=str(log_dir),
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


def wait_for_order_completion(me_log: Path, total_orders: int, timeout: float) -> bool:
    """
    Poll the matching engine log until 'ME-ORD-<total_orders>' appears,
    indicating that all orders have been processed.  Returns True on success,
    False on timeout.  The pattern uses a negative look-ahead so that
    ME-ORD-1000 does not match ME-ORD-10000.

    Only new bytes appended since the last iteration are read, so memory
    usage stays flat regardless of how large the log file grows.

    The idle bail-out is computed dynamically from the observed throughput:
    once at least MIN_CALIBRATION_SECS have elapsed since the first ME-ORD
    entry appeared, the bail-out is set to 2× the estimated time remaining
    at the current rate.  A generous fixed timeout is used until then.
    """
    # INITIAL_IDLE_TIMEOUT: patience before any throughput rate is known.
    # Scale with the overall timeout so large runs aren't cut short early.
    INITIAL_IDLE_TIMEOUT = max(30.0, timeout * 0.10)
    MIN_CALIBRATION_SECS = 2.0    # minimum elapsed time before trusting the rate
    MIN_IDLE_TIMEOUT     = 5.0    # floor: never declare stall faster than this

    target          = f"ME-ORD-{total_orders}"
    pattern         = re.compile(re.escape(target) + r"(?!\d)")
    any_ord_pattern = re.compile(r"ME-ORD-(\d+)")

    deadline        = time.monotonic() + timeout
    last_seen       = 0
    last_change     = time.monotonic()
    rate_start_t    = None   # when the first ME-ORD entry was observed
    # rate_start_n stays 0: rate is measured as "orders since monitoring started",
    # not "orders since the first chunk".  If the first chunk already contains
    # many orders, setting rate_start_n=highest makes rate=0 and calibration
    # never fires.
    rate_start_n    = 0
    idle_timeout    = INITIAL_IDLE_TIMEOUT
    rate_calibrated = False
    file_pos        = 0      # byte offset of the next unread byte in the log

    log(f"Waiting for {target} in {me_log.name} (timeout {timeout:.0f}s) ...")

    while time.monotonic() < deadline:
        if me_log.is_file():
            with open(me_log, "r", errors="replace") as fh:
                fh.seek(file_pos)
                chunk = fh.read()
                file_pos = fh.tell()

            if pattern.search(chunk):
                return True

            matches = any_ord_pattern.findall(chunk)
            if matches:
                highest = max(int(m) for m in matches)
                now     = time.monotonic()

                if highest > last_seen:
                    last_seen   = highest
                    last_change = now
                    if rate_start_t is None:
                        rate_start_t = now

                # Recompute idle_timeout from observed throughput.
                if rate_start_t is not None:
                    elapsed = time.monotonic() - rate_start_t
                    if elapsed >= MIN_CALIBRATION_SECS:
                        rate = (last_seen - rate_start_n) / elapsed
                        if rate > 0:
                            remaining    = total_orders - last_seen
                            idle_timeout = max((remaining / rate) * 2, MIN_IDLE_TIMEOUT)
                            if not rate_calibrated:
                                rate_calibrated = True
                                log(f"  throughput ~{rate:,.0f} orders/s → "
                                    f"dynamic idle bail-out {idle_timeout:.0f}s")

            if last_seen > 0 and (time.monotonic() - last_change) >= idle_timeout:
                log(f"  STALL DETECTED: ME-ORD progress stopped at {last_seen} "
                    f"(expected {total_orders}, missing "
                    f"{total_orders - last_seen} orders) — pipeline may have "
                    f"lost orders silently. Proceeding with shutdown.")
                return False

        time.sleep(0.1)

    log(f"  TIMEOUT: last ME-ORD seen = {last_seen} / {total_orders}")
    return False


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


def run_fix8_session(me_log: Path, burst: int, clients: int) -> None:
    """
    Start `clients` concurrent f8test processes, wait for all FIX sessions to
    log on, then send `burst` 'T' commands to each (each T = 1000 NOS).
    Waits until the matching engine has processed all clients*burst*1000 orders,
    then SIGTERMs every f8test process.  f8test does not exit on stdin EOF so
    we must kill it explicitly.
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
    # ORDER_TIMEOUT is the per-1000-order budget; with large client/burst counts
    # the naive product becomes absurdly long (e.g. 15,000s for 50×10 runs).
    MAX_ORDER_TIMEOUT = 600.0  # 10 minutes hard cap
    timeout = min(ORDER_TIMEOUT * max(1, burst * clients), MAX_ORDER_TIMEOUT)
    success = wait_for_order_completion(me_log, total_orders, timeout)
    if not success:
        log(f"  WARNING: timed out waiting for ME-ORD-{total_orders} — proceeding anyway")
    else:
        log(f"  All {total_orders} orders confirmed in matching engine log")

    log("  Terminating fix8 client(s) ...")
    if success:
        # Test completed cleanly: f8test ignores SIGTERM so skip the grace period
        # and send SIGKILL immediately. There is nothing worth flushing at this
        # point and the wait would only slow down the overall run.
        for proc in procs:
            if proc.poll() is None:
                proc.kill()
        for proc in procs:
            proc.wait()
    else:
        # Test timed out or stalled: try SIGTERM first in case there is useful
        # diagnostic state to flush, then SIGKILL if that does not work.
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
    parser.add_argument("prefix", nargs="?", default="build/installed",
                        metavar="install_prefix",
                        help="Path to the cmake install prefix (default: build/installed)")
    parser.add_argument("--burst", type=int, default=1, metavar="N",
                        help="Number of T commands per fix8 session (each T = 1000 NOS). Default: 1")
    parser.add_argument("--clients", type=int, default=1, metavar="N",
                        help="Number of concurrent fix8 sessions. Default: 1")
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

    preflight(prefix)
    log_dir.mkdir(parents=True, exist_ok=True)
    perf_dir.mkdir(parents=True, exist_ok=True)

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

    # -- launch applications in dependency order (mirrors start_fix_seq_system.py)
    # witness first: arbiters connect outbound to it at startup.
    steps = [
        ("witness",                "witness",                etc_dir / "witness"               / "witness.toml"),
        ("arbiter_primary",        "arbiter",                etc_dir / "arbiter"               / "arbiter.toml"),
        ("arbiter_secondary",      "arbiter",                etc_dir / "arbiter"               / "arbiter_secondary.toml"),
        ("order_gateway", "order_gateway", etc_dir / "order_gateway" / "order_gateway.toml"),
        ("sequencer_primary",      "sequencer",              etc_dir / "sequencer"             / "sequencer.toml"),
        ("sequencer_secondary",    "sequencer",              etc_dir / "sequencer"             / "sequencer_secondary.toml"),
        ("matching_engine",        "matching_engine",        etc_dir / "matching_engine"       / "matching_engine.toml"),
    ]

    app_procs:  list[tuple[str, subprocess.Popen]] = []
    perf_procs: list[tuple[str, subprocess.Popen]] = []

    def full_shutdown() -> None:
        shutdown_processes(app_procs)
        stop_perf_procs(perf_procs)
        if app_procs:
            generate_reports([n for n, _ in app_procs], perf_dir)

    try:
        for name, bin_name, config in steps:
            proc = launch_app(name, bin_name, config, bin_dir, log_dir)
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
        run_fix8_session(me_log, args.burst, args.clients)

        log(f"Waiting {POST_ORDER_WAIT:.0f}s for pipeline to drain ...")
        time.sleep(POST_ORDER_WAIT)

        full_shutdown()

    except KeyboardInterrupt:
        log("Interrupted — shutting down cleanly ...")
        full_shutdown()
        sys.exit(130)

    log("=== Done ===")


if __name__ == "__main__":
    main()
