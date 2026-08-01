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
# fix8's f8test never sends tag 554 (Password), so it authenticates with an
# empty password.  FIX8_COMP_ID is the SenderCompID in myfix_gateway_client.xml.
# ensure_fix8_credentials() rewrites the credential in credentials.toml before
# the auth service starts, bypassing whatever the database holds.
FIX8_COMP_ID = "CLIENT"
FIX8_PASSWORD = ""       # empty: f8test sends no Password tag

# ── tunables ──────────────────────────────────────────────────────────────────
STARTUP_DELAY   = 1.0    # seconds between app launches
SETTLE_TIME     = 3.0    # seconds after last app before attaching perf
FIX8_LOGON_WAIT = 3.0    # seconds for fix8 to establish the FIX session
ORDER_TIMEOUT   = 180.0  # seconds to wait for ord1000 in the ME log
POST_ORDER_WAIT = 15.0   # seconds after last order before SIGTERM; long enough for
                         # the cancel-on-disconnect drain of a large book to finish
CALLGRAPH        = "dwarf" # dwarf unwinds across the user/kernel boundary; resolves the
                           # otherwise-anonymous kernel stacks that dominate the gateway profile.
                           # fp would suffice for pure-userspace profiling but loses the call
                           # chain whenever a sample lands inside a syscall (epoll, recv, send).
DWARF_STACK_SIZE = 4096    # bytes per sample; default 8192 — halving saves significant RAM
PERF_MMAP_SIZE   = "16M"   # per-CPU ring-buffer cap passed to -m; prevents OOM under load
FREQ             = 99      # perf sample frequency (Hz)
# Only the gateway instance under test and the matching engine are profiled.  Profiling
# arbiters, the witness, and both sequencers with DWARF is expensive and rarely useful;
# the hot path is gateway and ME.  The set is built in main() from --gateway and
# --gateway-instance, because which gateway is the target is a per-run choice.

# The binary gateway's client listener host, and the burst size both load tools use.
# 1000 is fix8's "T" command, so --burst means the same thing for either gateway.  The
# port is not a constant: it is read from the chosen instance's deployed configuration,
# because the instances listen on different ports and a constant would drift.
BINARY_GATEWAY_HOST = "127.0.0.1"
ORDERS_PER_BURST    = 1000

# The binary gateway authenticates with SCRAM, so the load client needs credentials.
# These are provisioned into credentials.toml at the start of each run.
BINARY_LOAD_COMP_ID_PREFIX = "LOADCLIENT"
BINARY_LOAD_PASSWORD       = "loadclientpassword"
SHUTDOWN_TIMEOUT = 5.0   # seconds to wait for each app to exit after SIGTERM
MAX_ORDER_TIMEOUT = 120.0  # hard cap on order/ER completion wait (2 minutes)

# The gateway logs one of these per ER it discards because the target client
# session already disconnected. Such ERs are "accounted for" but never delivered.
_ER_DROPPED_MARKER = "client already disconnected -- dropping"

# CONTRACT WITH THE GATEWAYS -- both fix_order_gateway and binary_order_gateway emit this marker, at
# Info, every 1000 accounted-for execution reports (see report_order_progress() in
# FixOrderGatewayThread.hpp, which carries the matching warning). It is how this script knows a
# run has finished: the count is the authoritative end-to-end signal that every order made
# the full NOS -> ME -> sequencer -> gateway -> client round trip.
#
# If a gateway stops emitting it, or renames it, or changes the field, nothing fails loudly:
# the run just waits until it times out and reports a stall that reads like a pipeline fault.
# The per-order GW-NOS-RECV / GW-ER-SENT lines are at Debug and must not be relied on here --
# counting them cost around a third of the gateway's CPU, which made the gateway comparison
# measure logging rather than protocol.
_PROGRESS_MARKER = "GW-PROGRESS"
_PROGRESS_ACCOUNTED_RE = re.compile(r"GW-PROGRESS accounted=(\d+)")

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


def write_scram_credential(creds_file: Path, comp_id: str, password: str) -> None:
    """Rewrite the SCRAM credential for comp_id in credentials.toml.

    Called after export_credentials.py so a perf run always authenticates successfully
    regardless of what the database holds for the comp ids the load tools use.
    """
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
    # Split on [[credential]] blocks; drop any existing block for this comp_id.
    blocks = re.split(r"\[\[credential\]\]", existing)
    header = blocks[0]
    kept = [b for b in blocks[1:]
            if f'comp_id    = "{comp_id}"' not in b and
               f'comp_id = "{comp_id}"' not in b]
    result = header + "".join(f"[[credential]]{b}" for b in kept) + new_block
    creds_file.write_text(result)
    log(f"  SCRAM credential for '{comp_id}' (password={password!r}) written to {creds_file.name}")


# ── Gateway instances ─────────────────────────────────────────────────────────
# Each gateway protocol runs as several instances (fix_order_gateway_a,
# fix_order_gateway_b, and the same pair for binary) so that losing one does not take
# every session with it. They listen on different ports, so driving a chosen instance
# means pointing the load generator at that port.
#
# The port is always read from the instance's own deployed TOML rather than kept as a
# constant here, because a constant drifts from the deployment silently and the failure
# looks like the gateway being down.
#
# For FIX there is a second reason: f8test takes its target from an XML session config
# that lives in the fix8 installation, outside this repo. Rather than requiring a
# hand-maintained file per instance, the port read here is written into a generated copy
# of that config.

def gateway_component(gateway: str, instance: str) -> str:
    """Deployed component name for a protocol and instance, e.g. 'fix_order_gateway_a'."""
    return f"{'binary' if gateway == 'binary' else 'fix'}_order_gateway_{instance}"


def gateway_listen_port(prefix: Path, gateway: str, instance: str) -> int:
    """Read an instance's client listen port from its deployed configuration."""
    component = gateway_component(gateway, instance)
    directory = "binary_order_gateway" if gateway == "binary" else "fix_order_gateway"
    config = prefix / "etc" / directory / f"{component}.toml"
    if not config.is_file():
        die(f"{component} config not found: {config}\n"
            f"       is instance '{instance}' deployed in this environment?")
    for line in config.read_text().splitlines():
        match = re.match(r"\s*listen_port\s*=\s*(\d+)", line)
        if match:
            return int(match.group(1))
    die(f"no listen_port in {config}")
    return 0  # unreachable; die() exits


def fix8_config_for_instance(prefix: Path, instance: str) -> str:
    """Return the name of an f8test config aimed at the given gateway instance.

    Instance 'a' uses the stock config unchanged, so the common case behaves exactly as
    before. Any other instance gets a generated copy with the port rewritten, written
    beside the original because f8test resolves the name relative to its own directory.
    """
    if instance == "a":
        return FIX8_CFG

    port = gateway_listen_port(prefix, "fix", instance)
    source = FIX8_DIR / FIX8_CFG
    if not source.is_file():
        die(f"fix8 session config not found: {source}")

    generated_name = f"myfix_gateway_client_{instance}.xml"
    rewritten, count = re.subn(r'port="\d+"', f'port="{port}"', source.read_text(), count=1)
    if count == 0:
        die(f"no port attribute to rewrite in {source}")
    (FIX8_DIR / generated_name).write_text(rewritten)
    log(f"fix8 config for instance {instance}: {generated_name} -> port {port}")
    return generated_name


def ensure_fix8_credentials(creds_file: Path, comp_id: str, password: str,
                            logon_mode: str) -> None:
    """Provision the credential f8test authenticates with.

    Skipped when logon_mode == 'proprietary' (no SCRAM involved).
    """
    if logon_mode == "proprietary":
        log(f"  proprietary logon mode -- skipping SCRAM credential rewrite for '{comp_id}'")
        return
    write_scram_credential(creds_file, comp_id, password)


def ensure_binary_load_credentials(creds_file: Path, clients: int) -> None:
    """Provision a credential for each comp id binary_load_client will log on with.

    The binary gateway authenticates every session, and the load client gives each of its
    sessions a distinct comp id because the gateway refuses a duplicate. So a run needs one
    credential per session, not the single one f8test needs, and the names have to match
    exactly what the client derives from its prefix.
    """
    for comp_id in binary_load_comp_ids(clients):
        write_scram_credential(creds_file, comp_id, BINARY_LOAD_PASSWORD)
    log(f"  {clients} SCRAM credential(s) written for the binary load client")


def binary_load_comp_ids(clients: int) -> list[str]:
    """The comp ids binary_load_client uses -- must mirror its own naming exactly.

    A single session keeps the bare prefix; more than one appends an index. Getting this
    wrong provisions credentials nobody asks for and refuses the logons that do arrive.
    """
    if clients == 1:
        return [BINARY_LOAD_COMP_ID_PREFIX]
    return [f"{BINARY_LOAD_COMP_ID_PREFIX}-{index + 1}" for index in range(clients)]


def set_fix_capture_enabled(config_path: Path, enabled: bool) -> None:
    """Patch the enabled flag in the fix_order_gateway fix_capture config section."""
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
    for name in ("witness", "arbiter", "sequencer", "matching_engine", "fix_order_gateway"):
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
                           stall_is_warning: bool = True,
                           done_predicate=None) -> bool:
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

            # Optional early completion: e.g. once every client has disconnected,
            # no ER can be delivered, so waiting for the target is pointless.
            if done_predicate is not None and done_predicate():
                log(f"  {label}: all client sessions gone — {total_seen:,}/{target:,} accounted, "
                    f"remaining ERs are undeliverable; completing")
                return True

            if chunk:
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


def wait_for_er_completion(gw_log: Path, total_orders: int, timeout: float, clients: int) -> bool:
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
    # An ER is "accounted for" once the gateway either sends it to the client or
    # drops it because that client already disconnected. Counting only GW-ER-SENT
    # hangs forever when clients disconnect mid-stream: their ERs are dropped, so
    # the delivered count can never reach total_orders.
    established = [0]
    lost        = [0]
    lost_re     = re.compile(r"FIX client connection \d+ lost")

    # The gateway reports a CUMULATIVE total, but _wait_for_log_pattern accumulates whatever
    # this returns, so hand back the increase since the last chunk rather than the total --
    # returning the total would count every progress line's whole history again and finish
    # the run long before the orders had.
    highest = [0]

    def count_er(chunk: str) -> int:
        established[0] += chunk.count("FIX session established")
        lost[0]       += len(lost_re.findall(chunk))
        previous = highest[0]
        for match in _PROGRESS_ACCOUNTED_RE.finditer(chunk):
            highest[0] = max(highest[0], int(match.group(1)))
        return highest[0] - previous

    def all_clients_gone() -> bool:
        # Only once every client has logged on AND every one has since dropped:
        # no session remains, so any further ER can only be dropped. Requiring
        # `clients` on both sides avoids a false trigger during ramp-up (one
        # client connecting+dropping before the others have connected).
        return established[0] >= clients and lost[0] >= clients

    return _wait_for_log_pattern(gw_log, "GW-ER (sent+dropped)", total_orders, count_er, timeout,
                                  min_idle_timeout=120.0, done_predicate=all_clients_gone)


def terminate_clients(procs: list[subprocess.Popen], clients: int) -> None:
    """SIGKILL and reap all fix8 clients (f8test ignores SIGTERM).

    Safe to call from a finally: this guarantees the clients are never orphaned,
    even when the session aborts via die() or Ctrl-C mid-wait.
    """
    for proc in procs:
        if proc.poll() is None:
            proc.kill()
    for proc in procs:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
    log(f"  All {clients} fix8 client(s) stopped")


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


def run_binary_load_session(bin_dir: Path, output_dir: Path, burst: int, clients: int, rate: int,
                            port: int) -> None:
    """
    Drive the binary order gateway with binary_load_client, the counterpart of f8test.

    Simpler than the fix8 path because the client owns both ends of the exchange: it
    knows exactly how many orders it sent and how many reports came back, so completion
    is the process exiting rather than a log-line count reaching a total. It also reports
    per-order round-trip latency, which the FIX path cannot measure from outside.

    Deliberately NOT mirrored on the FIX side: the binary gateway does not log a line per
    execution report. Adding one would import the FIX gateway's per-ER logging cost into
    the very measurement meant to compare them.
    """
    total_orders = clients * burst * ORDERS_PER_BURST
    log(f"=== Starting binary_load_client, {clients} session(s), {burst} burst(s) each "
        f"({total_orders} orders total) ===")

    command = [
        str(bin_dir / "binary_load_client"),
        "--host", BINARY_GATEWAY_HOST,
        "--port", str(port),
        "--comp-id-prefix", BINARY_LOAD_COMP_ID_PREFIX,
        "--password", BINARY_LOAD_PASSWORD,
        "--sessions", str(clients),
        "--orders-per-burst", str(ORDERS_PER_BURST),
        "--bursts", str(burst),
    ]
    if rate > 0:
        command += ["--rate", str(rate)]
        log(f"  rate limited to {rate} orders/s per session "
            f"({rate * clients} offered venue-wide) -- measuring service latency")
    else:
        log("  no rate limit -- measuring peak throughput; reported latency is "
            "queueing-dominated")

    timeout = min(ORDER_TIMEOUT * max(1, burst * clients), MAX_ORDER_TIMEOUT)
    try:
        result = subprocess.run(command, timeout=timeout, check=False,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except subprocess.TimeoutExpired:
        die(f"binary_load_client did not finish within {timeout:.0f}s")

    # Echo it and keep a copy. The client's throughput and latency figures are the whole
    # point of the run, and they used to exist only on the console -- so a run whose output
    # was not teed left its perf artefacts behind but none of its measurements.
    summary_file = output_dir / "binary_load_client.txt"
    summary_file.write_text(result.stdout or "")
    for line in (result.stdout or "").splitlines():
        log(f"  {line}")
    log(f"  load client output saved to {summary_file}")

    if result.returncode != 0:
        die(f"binary_load_client reported failure (exit {result.returncode}) -- "
            f"not every order was acknowledged")
    log("  binary_load_client completed: every order acknowledged")


def run_fix8_session(me_log: Path, gw_log: Path, burst: int, clients: int, fix8_config: str) -> None:
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
            [str(FIX8_BIN), "-c", fix8_config, "-N", "GW1"],
            cwd=str(FIX8_DIR),
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(proc)
        log(f"  client {i + 1} of {clients}: f8test PID {proc.pid}")

    # The clients are launched; from here everything runs under try/finally so a
    # die() or Ctrl-C mid-wait never orphans a fix8 client.
    try:
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

        # Phase 2: full round-trip — wait for gateway to account for all ERs
        # (delivered or dropped for a disconnected client). Same timeout budget.
        er_ok = wait_for_er_completion(gw_log, total_orders, timeout, clients)
        if not er_ok:
            log(f"  WARNING: not all ERs accounted for before timeout — {total_orders} expected")
    finally:
        log("  Terminating fix8 client(s) with SIGKILL ...")
        terminate_clients(procs, clients)


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
    parser.add_argument("--gateway", choices=["fix", "binary"], default="fix",
                        help="Which gateway to load and profile.  'fix' (default): the ASCII "
                             "FIX gateway, driven by fix8's f8test.  'binary': the binary "
                             "gateway, driven by binary_load_client.  Both send the same "
                             "orders into the same book, so the two runs are comparable.")
    parser.add_argument("--gateway-instance", choices=["a", "b"], default="a",
                        help="Which instance of the chosen gateway to drive and profile "
                             "(default: a). Every instance of both protocols is launched "
                             "either way, so the process set and therefore the machine's load "
                             "is the same whichever is measured. The load generator is pointed "
                             "at the chosen instance's listen port, read from its deployed "
                             "configuration.")
    parser.add_argument("--rate", type=int, default=0,
                        help="Orders per second per session (binary gateway only).  Omit for a "
                             "throughput test, which offers load faster than the pipeline "
                             "drains so the latency figures are queueing-dominated.  Set a "
                             "sustainable rate to measure service latency.")
    parser.add_argument("--logon-mode", choices=["scram", "proprietary"], default="scram",
                        help="Authentication mode for fix8 clients.  'scram' (default): "
                             "standard SCRAM-SHA-256 with an empty password (fix8 sends no "
                             "tag 554).  'proprietary': skip SCRAM credential rewrite, used "
                             "when testing on RHEL8 with the proprietary logon path.")
    args = parser.parse_args()
    if args.burst < 1:
        parser.error("--burst must be >= 1")
    if args.clients < 1:
        parser.error("--clients must be >= 1")
    if args.rate and args.gateway != "binary":
        parser.error("--rate applies only to --gateway binary; f8test has no rate control")
    if args.capture and args.gateway != "fix":
        parser.error("--capture is FIX wire capture and applies only to --gateway fix")

    script_dir = Path(__file__).resolve().parent
    prefix     = resolve_prefix(str(script_dir / args.prefix)
                                if not Path(args.prefix).is_absolute()
                                else args.prefix)
    bin_dir    = prefix / "bin"
    etc_dir    = prefix / "etc"
    log_dir    = prefix / "log"
    ts         = datetime.now().strftime("%Y%m%d_%H%M%S")
    perf_dir   = prefix / "perf" / ts
    me_log     = log_dir / "matching_engine_primary.log"
    gw_log     = log_dir / f"fix_order_gateway_{args.gateway_instance}.log"

    gw_config = prefix / "etc" / "fix_order_gateway" / f"fix_order_gateway_{args.gateway_instance}.toml"

    preflight(prefix)
    log_dir.mkdir(parents=True, exist_ok=True)
    perf_dir.mkdir(parents=True, exist_ok=True)

    if args.capture:
        if not gw_config.is_file():
            die(f"fix_order_gateway config not found: {gw_config}")
        set_fix_capture_enabled(gw_config, True)
        log("FIX capture enabled in fix_order_gateway config")

    # Extend LD_LIBRARY_PATH so the installed shared library is found.
    lib_dir = str(prefix / "lib")
    existing = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = f"{lib_dir}:{existing}" if existing else lib_dir

    log("=== perf_run ===")
    log(f"  install prefix : {prefix}")
    log(f"  perf output    : {perf_dir}")
    cg_desc = f"{CALLGRAPH},{DWARF_STACK_SIZE}" if CALLGRAPH == "dwarf" else CALLGRAPH
    targets_desc = ("matching_engine_primary, "
                    + gateway_component(args.gateway, args.gateway_instance))
    log(f"  call-graph     : {cg_desc}  (freq={FREQ} Hz, mmap={PERF_MMAP_SIZE})")
    log(f"  perf targets   : {targets_desc}")
    log(f"  gateway        : {args.gateway}")
    log(f"  clients        : {args.clients}")
    log(f"  burst          : {args.burst}  ({args.clients * args.burst * 1000} orders total)")
    log(f"  FIX capture    : {'enabled' if args.capture else 'disabled'}")
    log(f"  logon mode     : {args.logon_mode}")

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
    if args.gateway == "binary":
        ensure_binary_load_credentials(creds_file, args.clients)
    else:
        ensure_fix8_credentials(creds_file, FIX8_COMP_ID, FIX8_PASSWORD, args.logon_mode)

    # -- launch applications in dependency order (mirrors dev.toml startup_order)
    # Each tuple: (name, binary, config, optional_workdir).
    # auth services must start first; gateway connects to them on startup.
    steps = [
        ("auth_service_a",   "authentication_service", etc_dir / "authentication_service" / "authentication_service_a.toml",  etc_dir / "authentication_service"),
        ("auth_service_b", "authentication_service", etc_dir / "authentication_service" / "authentication_service_b.toml", etc_dir / "authentication_service"),
        ("witness",                "witness",                etc_dir / "witness"               / "witness.toml",                  None),
        ("arbiter_primary",        "arbiter",                etc_dir / "arbiter"               / "arbiter_primary.toml",                  None),
        ("arbiter_secondary",      "arbiter",                etc_dir / "arbiter"               / "arbiter_secondary.toml",        None),
        ("matching_engine_primary", "matching_engine",       etc_dir / "matching_engine"       / "matching_engine_primary.toml",  None),
        ("matching_engine_secondary", "matching_engine",    etc_dir / "matching_engine"       / "matching_engine_secondary.toml",None),
        ("sequencer_primary",      "sequencer",              etc_dir / "sequencer"             / "sequencer_primary.toml",                None),
        ("sequencer_secondary",    "sequencer",              etc_dir / "sequencer"             / "sequencer_secondary.toml",      None),
        ("fix_order_gateway_a",    "fix_order_gateway",      etc_dir / "fix_order_gateway"      / "fix_order_gateway_a.toml",      etc_dir / "fix_order_gateway"),
        ("fix_order_gateway_b",    "fix_order_gateway",      etc_dir / "fix_order_gateway"      / "fix_order_gateway_b.toml",      etc_dir / "fix_order_gateway"),
        ("binary_order_gateway_a", "binary_order_gateway",   etc_dir / "binary_order_gateway"   / "binary_order_gateway_a.toml",   etc_dir / "binary_order_gateway"),
        ("binary_order_gateway_b", "binary_order_gateway",   etc_dir / "binary_order_gateway"   / "binary_order_gateway_b.toml",   etc_dir / "binary_order_gateway"),
    ]

    # Every gateway instance of both protocols runs whichever one is under test, so the
    # process set -- and therefore the machine's load -- is the same in every run and the
    # results are comparable. The idle ones do nothing but hold their listeners open.
    perf_targets = {"matching_engine_primary",
                    gateway_component(args.gateway, args.gateway_instance)}

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
            if name in perf_targets:
                perf_proc = attach_perf(name, proc.pid, perf_dir)
                perf_procs.append((name, perf_proc))
        time.sleep(1)  # give perf a moment to start recording

        # Drive load through whichever gateway is under test.
        if args.gateway == "binary":
            run_binary_load_session(bin_dir, perf_dir, args.burst, args.clients, args.rate,
                                    gateway_listen_port(prefix, "binary", args.gateway_instance))
        else:
            run_fix8_session(me_log, gw_log, args.burst, args.clients,
                             fix8_config_for_instance(prefix, args.gateway_instance))

        log(f"Waiting {POST_ORDER_WAIT:.0f}s for pipeline to drain ...")
        time.sleep(POST_ORDER_WAIT)

        full_shutdown()

    except KeyboardInterrupt:
        log("Interrupted — shutting down cleanly ...")
        full_shutdown()
        if args.capture:
            set_fix_capture_enabled(gw_config, False)
        sys.exit(130)
    except BaseException:
        # Covers die() → sys.exit(), broken-pipe errors, and any other
        # unexpected exception.  Always clean up running processes so that
        # a failed run does not leave orphaned binaries behind.
        log("Failure — shutting down all running processes ...")
        shutdown_processes(app_procs)
        stop_perf_procs(perf_procs)
        raise
    finally:
        if args.capture:
            set_fix_capture_enabled(gw_config, False)
            log("FIX capture disabled in fix_order_gateway config")

    # Post-shutdown ground-truth counts.  Read after all processes have exited so
    # any in-flight log entries are flushed.  GW-ER-SENT is the authoritative
    # end-to-end signal: it confirms the full NOS→ME→sequencer→gateway→fix8 path.
    total_orders = args.clients * args.burst * 1000
    def count_in_log(path: Path, marker: str) -> int:
        try:
            return sum(1 for line in path.open(errors="replace") if marker in line)
        except FileNotFoundError:
            return 0

    # Count the ME order-acceptance line only ("accepted NOS OrderID=ME-ORD-N").
    # A bare "ME-ORD" substring also matches the cancel-ER lines, double-counting.
    me_final      = count_in_log(me_log,  "accepted NOS")

    if args.gateway == "binary":
        # The binary gateway logs no line per execution report, deliberately: doing so
        # would import the FIX gateway's per-ER logging cost into the measurement meant to
        # compare the two. binary_load_client counted its own sends and receipts and has
        # already failed the run if they did not match, so the ME count is the only
        # independent check worth making here.
        log("=== Post-shutdown ground-truth counts ===")
        log(f"  ME-ORD        : {me_final:>10,} / {total_orders:,}  "
            f"{'OK' if me_final == total_orders else 'MISMATCH'}")
        log("  ER delivery   : verified by binary_load_client (it exits non-zero on any shortfall)")
        if me_final != total_orders:
            log("=== FAIL — the matching engine did not accept every order ===")
            sys.exit(1)
        log("=== PASS — all orders processed ===")
        log("=== Done ===")
        return

    # Read from the cumulative GW-PROGRESS totals rather than by counting per-order lines,
    # which are at Debug and therefore absent from a normal run. See the contract note beside
    # _PROGRESS_MARKER above.
    def latest_progress(path: Path) -> tuple[int, int, int, int]:
        accounted = sent = dropped = received = 0
        try:
            for line in path.open(errors="replace"):
                if _PROGRESS_MARKER not in line:
                    continue
                fields = dict(pair.split("=", 1) for pair in line.split() if "=" in pair)
                accounted = max(accounted, int(fields.get("accounted", 0)))
                sent = max(sent, int(fields.get("sent", 0)))
                dropped = max(dropped, int(fields.get("dropped", 0)))
                received = max(received, int(fields.get("nos_received", 0)))
        except FileNotFoundError:
            pass
        return accounted, sent, dropped, received

    _, gw_er_sent, gw_er_dropped, gw_nos_recv = latest_progress(gw_log)
    gw_gap_fills  = count_in_log(gw_log,  "SequenceReset-GapFill")
    nos_ok        = gw_nos_recv == total_orders
    # Every order generates an ER; the gateway either delivers it (GW-ER-SENT) or
    # drops it because the client already disconnected. "short" (fewer accounted
    # for than orders) is a real pipeline loss; a low delivered-count alone is
    # not, since dropped ERs are expected when clients leave mid-stream.
    er_accounted  = gw_er_sent + gw_er_dropped
    er_short      = er_accounted < total_orders
    er_excess     = er_accounted > total_orders

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
    if gw_er_dropped > 0:
        log(f"  GW-ER-DROPPED : {gw_er_dropped:>10,}            (client disconnected before delivery)")
    er_discrepancy = er_accounted - gw_nos_recv
    if er_discrepancy == 0:
        log(f"  NOS→ER match  : YES — one ER (sent or dropped) per NOS")
    elif er_discrepancy > 0:
        log(f"  NOS→ER match  : {er_discrepancy:,} extra ERs (partial fills or late cancel ACKs)")
    else:
        log(f"  NOS→ER match  : NO — {-er_discrepancy:,} ERs unaccounted for vs NOS")
    if gw_gap_fills > 0:
        log(f"  Gap fills     : {gw_gap_fills:>10,}  (FIX SequenceReset-GapFill sent in response to ResendRequest)")

    # PASS: the gateway received every NOS and every ER was accounted for --
    # delivered, or dropped because its client had already disconnected. ERs that
    # are neither (er_short) are genuinely lost in the sequencer->gateway pipeline.
    if nos_ok and not er_short:
        if gw_er_dropped > 0:
            log(f"=== PASS — all orders processed; {gw_er_sent:,} ERs delivered, "
                f"{gw_er_dropped:,} dropped for disconnected clients ===")
        elif er_excess:
            log(f"=== PASS — all orders processed; {er_accounted - total_orders:,} extra ERs "
                f"(partial fills or HA double-forwarding) ===")
        else:
            log("=== PASS — all orders processed and every ER delivered ===")
    elif er_short:
        log(f"=== FAIL — {total_orders - er_accounted:,} ERs unaccounted for "
            f"(lost in the sequencer→gateway pipeline) ===")
    elif not nos_ok:
        log(f"=== FAIL — {total_orders - gw_nos_recv:,} NOS not received by gateway ===")
    else:
        log(f"=== FAIL — unexpected state: "
            f"ME-ORD={me_final:,} NOS={gw_nos_recv:,} ER={gw_er_sent:,} ===")

    log("=== Done ===")


if __name__ == "__main__":
    main()
