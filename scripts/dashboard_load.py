#!/usr/bin/env python3
"""Shape load against an already-running venue so the dashboards show something worth reading.

Not a perf run and not a benchmark. perf_run.py owns the venue, runs it under perf and
produces a report; this drives binary_load_client against a venue that is already up, and
its only output is the shape of the traffic. Use it when what you want to look at is a
Grafana dashboard rather than a number.

The session is built from phases, and the phases exist to put recognisable events on a
chart:

  * bursts at the open and the close, where the rate is several times the day's average
  * long steady stretches, which is what a healthy venue mostly looks like
  * trickles, where orders arrive sparsely enough that the consuming thread goes to sleep
    between them
  * genuine lulls, where nothing is sent at all

The lulls are the point, and they are why this is not simply a constant rate. A venue that
is never quiet never has to wake anything up, so the cost of waking up never appears. A
latency outlier that only happens after a quiet period is invisible under constant load and
obvious here -- and it is a real enough pattern that it is worth being able to reproduce on
demand rather than waiting for it.

Each phase is a separate binary_load_client invocation, so a session is a sequence of logons
rather than one long-lived one. That is deliberate: it exercises the connect path repeatedly,
and it means a phase that goes wrong cannot take the rest of the session with it.

Usage:

    python3 scripts/dashboard_load.py                     # 30 minutes, default shape
    python3 scripts/dashboard_load.py --minutes 5         # a quick look
    python3 scripts/dashboard_load.py --dry-run           # print the timeline and stop
"""

import argparse
import subprocess
import sys
import threading
import time
from pathlib import Path

# A sibling script rather than a package, so it is imported by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import measurement_machine  # noqa: E402  -- needs the path set above

# Each protocol gets its own shape, and the shapes are OFFSET on purpose.
#
# A mixed run exists to answer one question a single-protocol run cannot: does load on one
# gateway disturb latency on the other. They share a matching engine, a sequencer and a host,
# so the answer is not obviously no.
#
# Answering it needs the bursts to NOT coincide. If both protocols spike together, every
# latency figure rises at once and nothing can be attributed. So each protocol spikes while
# the other is running a steady trickle -- not while the other is silent, because a protocol
# that is sending nothing has no latency to disturb. The trickle is the measurement; the
# spike on the other side is the thing being measured.
#
# Shares must sum to the same total in both tables so the two timelines stay aligned.
#
# (name, share of the session, orders per second; 0 means send nothing at all)

# The binary gateway: steadier and faster, which is where members are moving.
BINARY_PHASES = [
    ("pre-open quiet",      0.04,   0),
    ("the open",            0.08, 400),
    ("morning steady",      0.16, 150),
    ("trickle (FIX spikes)",0.10,  20),   # <-- FIX bursts across this; watch binary latency
    ("midday steady",       0.12, 150),
    ("BINARY SPIKE",        0.08, 600),   # <-- watch FIX latency across this
    ("afternoon steady",    0.16, 150),
    ("late trickle",        0.08,  20),
    ("the close",           0.14, 350),
    ("post-close quiet",    0.04,   0),
]

# The FIX gateway: burstier and lower rate, the traditional shape.
FIX_PHASES = [
    ("pre-open quiet",      0.04,   0),
    ("the open",            0.08, 150),
    ("morning steady",      0.16,  60),
    ("FIX SPIKE",           0.10, 300),   # <-- watch binary latency across this
    ("midday steady",       0.12,  60),
    ("trickle (bin spikes)",0.08,  15),   # <-- binary bursts across this; watch FIX latency
    ("afternoon steady",    0.16,  60),
    ("late trickle",        0.08,  15),
    ("the close",           0.14, 200),
    ("post-close quiet",    0.04,   0),
]

# Identical shapes, run in lockstep. Matched rates are the precondition for comparing one
# protocol's latency against the other's: a p99 measured at 185 orders/s against a p99
# measured at 91 says nothing about the protocols, because the busier one is carrying twice
# the queueing. The offset tables above deliberately break that precondition -- they exist to
# answer whether one protocol's burst disturbs the other, which needs the rates to DIFFER and
# to not coincide. The two questions need two experiments; this is the comparison one.
COMPARE_PHASES = [
    ("pre-open quiet",      0.06,   0),
    ("warm-up",             0.10,  50),
    ("steady low",          0.20, 100),
    ("steady medium",       0.20, 200),
    ("quiet",               0.08,   0),
    ("steady high",         0.20, 300),
    ("wind down",           0.10,  50),
    ("post-close quiet",    0.06,   0),
]

INTERFERE = {"binary": BINARY_PHASES, "fix": FIX_PHASES}
COMPARE = {"binary": COMPARE_PHASES, "fix": COMPARE_PHASES}
MODES = {"interfere": INTERFERE, "compare": COMPARE}

DEFAULT_COMP_ID_PREFIX = "LOADCLIENT"
DEFAULT_PASSWORD = "loadclientpassword"


def resolve_clients(prefix):
    """The two load clients, or exit naming whichever is missing."""
    binary = Path(prefix) / "bin" / "binary_load_client"
    fix = Path(__file__).resolve().parent / "fix_load_client.py"
    if not binary.is_file():
        raise SystemExit(f"error: {binary} not found. Deploy first, or pass --prefix.")
    if not fix.is_file():
        raise SystemExit(f"error: {fix} not found.")
    return binary, fix


def plan(phases, minutes):
    """Turn a protocol's phase shares into concrete (name, seconds, rate, orders) tuples."""
    total_seconds = int(minutes * 60)
    return [(name, max(1, int(round(total_seconds * share))), rate,
             max(1, int(round(total_seconds * share))) * rate)
            for name, share, rate in phases]


# Orders per "T". Small enough that the interval between bursts paces the rate smoothly,
# large enough that the shaper is not writing to a pipe thousands of times a second.
BURST_SIZE = 10


def binary_command(client, args, first_cl_ord_id, peak_rate):
    """binary_load_client, held open for the whole run and driven from stdin."""
    return [str(client),
            "--comp-id-prefix", args.binary_comp_id,
            "--password", args.binary_password,
            "--sessions", "1",
            "--orders-per-burst", str(BURST_SIZE),
            "--rate", str(peak_rate),
            "--first-cl-ord-id", str(first_cl_ord_id),
            "--cancel-ratio", str(args.cancel_ratio)]


def fix_command(client, args, first_cl_ord_id, peak_rate):
    """fix_load_client, same contract: --bursts 0 means one burst per T line on stdin."""
    return [sys.executable, "-u", str(client),
            "--comp-id", args.fix_comp_id,
            "--bursts", "0",
            "--orders-per-burst", str(BURST_SIZE),
            "--rate", str(peak_rate),
            "--first-cl-ord-id", str(first_cl_ord_id),
            "--cancel-ratio", str(args.cancel_ratio),
            "--drain", "1.0"]


def run_protocol(label, command, schedule, report):
    """Drive one protocol through its phases down a single, long-lived session.

    ONE session for the whole run, not one per phase. A client that logs out leaves the
    gateway to cancel every order it still had resting -- cancel-on-disconnect, working
    exactly as intended -- and with a cancel ratio below 1 that is thousands of orders
    arriving at the matching engine as a single wall of work. Measured at 8,455 and 9,418
    orders at two phase boundaries, which showed up as 150ms and 700ms spikes in the round
    trip of BOTH gateways, because they share the engine.

    Those spikes were an artefact of the harness and contaminated the measurements the run
    exists to take. Holding the session open removes them: the only logout is at the end.

    The rate is set by how often a "T" is written, each T being BURST_SIZE orders.
    """
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
    try:
        for name, seconds, rate, orders in schedule:
            if rate == 0:
                report(f"  [{label:>6}] {name:<22} {seconds:>4}s  quiet")
                time.sleep(seconds)
                continue
            report(f"  [{label:>6}] {name:<22} {seconds:>4}s  {rate:>4}/s  {orders:>7} orders")
            interval = BURST_SIZE / rate
            deadline = time.monotonic() + seconds
            next_burst = time.monotonic()
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    report(f"  [{label:>6}] client exited early (code {process.returncode})")
                    return
                try:
                    process.stdin.write("T\n")
                    process.stdin.flush()
                except (BrokenPipeError, ValueError):
                    report(f"  [{label:>6}] client closed its input")
                    return
                next_burst += interval
                time.sleep(max(0.0, next_burst - time.monotonic()))
    finally:
        # Closing stdin ends the client's burst loop; it then drains, reports and logs out.
        try:
            process.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()


def run(args):
    binary_client, fix_client = resolve_clients(args.prefix)
    schedules = {name: plan(phases, args.minutes) for name, phases in MODES[args.mode].items()}
    lock = threading.Lock()

    def report(line):
        with lock:
            print(line, flush=True)

    base = args.first_cl_ord_id if args.first_cl_ord_id is not None else int(time.time()) * 1000
    peaks = {name: max(rate for _, _, rate, _ in schedule) for name, schedule in schedules.items()}

    threads = [
        threading.Thread(target=run_protocol, args=(
            "binary", binary_command(binary_client, args, base, peaks["binary"]),
            schedules["binary"], report)),
        threading.Thread(target=run_protocol, args=(
            "fix", fix_command(fix_client, args, base + 500_000_000, peaks["fix"]),
            schedules["fix"], report)),
    ]
    started = time.time()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    print(f"\ndone: {time.time() - started:.0f}s elapsed")
    return 0


def warn_if_the_machine_is_not_fit_to_measure_on():
    """Say so, loudly, when the processor settings make the resulting dashboard misleading.

    This script's output is a picture somebody keeps. With the processor settings at their
    defaults every latency figure on that picture is several times too large, the run completes
    normally, and nothing on the dashboard records which machine state produced it -- so the
    screenshot outlives the session and is believed. A warning here is the only thing between
    that and someone quoting a round trip six times worse than the venue's.

    The other load scripts already do this. perf_run.py refuses outright; fix_load_client.py
    warns. This one warns, because a dashboard of throughput, protocol mix and the shape of a
    trading day is still worth having on an untuned machine -- only its latency panels are not.
    """
    try:
        ready, problems = measurement_machine.describe_state()
    except Exception:  # pylint: disable=broad-except
        return  # Reporting the machine is a courtesy; never let it stop a run.
    if ready:
        return
    print("WARNING: this machine's processor settings make every latency figure meaningless:")
    for problem in problems:
        print(f"           {problem}")
    print("         Throughput, protocol mix and the shape of the day will still be right.")
    print("         The latency panels will read several times too high, and the dashboard")
    print("         does not record that, so a screenshot of them will mislead whoever sees it.")
    print("         Put it right with: sudo python3 scripts/measurement_machine.py --on")
    print()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=sorted(MODES), default="interfere",
                        help="interfere: each protocol spikes while the other trickles, so a burst on one "
                             "can be seen in the other's latency. compare: both protocols run identical "
                             "rates in lockstep, which is the only condition under which their latencies "
                             "can fairly be compared (default: interfere)")
    parser.add_argument("--minutes", type=float, default=30.0, help="length of the simulated session (default: 30)")
    parser.add_argument("--prefix", default="installed", help="install prefix holding bin/binary_load_client")
    parser.add_argument("--binary-comp-id", default=DEFAULT_COMP_ID_PREFIX, help="comp id prefix for the binary client")
    parser.add_argument("--binary-password", default=DEFAULT_PASSWORD, help="SCRAM password for the binary sessions")
    parser.add_argument("--fix-comp-id", default="CLIENT", help="SenderCompID for the FIX client")
    parser.add_argument("--cancel-ratio", type=float, default=0.9,
                        help="fraction of placed orders also cancelled (default: 0.9). Orders never cancelled rest "
                             "on the book for ever and are recovered at the next start, which past a few minutes "
                             "of absence is an R-0117 cancel-and-halt. Set 0 to let the book accumulate")
    parser.add_argument("--first-cl-ord-id", type=int, default=None,
                        help="starting ClOrdID. Defaults to something derived from the clock, because a ClOrdID the "
                             "matching engine has already seen is rejected as a duplicate")
    parser.add_argument("--dry-run", action="store_true", help="print both timelines and exit without sending anything")
    args = parser.parse_args(argv)

    # Before anything else, so it is read rather than scrolled past at the end of a long run.
    warn_if_the_machine_is_not_fit_to_measure_on()

    schedules = {name: plan(phases, args.minutes) for name, phases in MODES[args.mode].items()}
    print(f"session: {args.minutes:.1f} minutes, mode {args.mode}, two protocols in parallel, "
          f"cancel ratio {args.cancel_ratio}\n")
    if args.mode == "compare":
        print("  both protocols run IDENTICAL rates: latency may be compared directly.\n")
    else:
        print("  rates differ by design: latencies are NOT comparable between protocols in this\n"
              "  mode -- use --mode compare for that. This mode answers whether one protocol's\n"
              "  burst disturbs the other.\n")
    for label, schedule in schedules.items():
        total = sum(orders for _, _, _, orders in schedule)
        print(f"  {label} — {total:,} orders")
        for name, seconds, rate, orders in schedule:
            rate_text = "quiet" if rate == 0 else f"{rate}/s"
            print(f"    {name:<22} {seconds:>4}s   {rate_text:>6}   {orders:>7}")
        print()

    if args.dry_run:
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
