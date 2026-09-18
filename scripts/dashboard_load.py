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

PROTOCOLS = {"binary": BINARY_PHASES, "fix": FIX_PHASES}

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


def binary_command(client, rate, seconds, first_cl_ord_id, args):
    return [str(client),
            "--comp-id-prefix", args.binary_comp_id,
            "--password", args.binary_password,
            "--sessions", "1",
            "--orders-per-burst", str(rate),
            "--bursts", str(seconds),
            "--rate", str(rate),
            "--first-cl-ord-id", str(first_cl_ord_id),
            "--cancel-ratio", str(args.cancel_ratio)]


def fix_command(client, rate, seconds, first_cl_ord_id, args):
    return [sys.executable, str(client),
            "--comp-id", args.fix_comp_id,
            "--orders-per-burst", str(rate),
            "--bursts", str(seconds),
            "--rate", str(rate),
            "--first-cl-ord-id", str(first_cl_ord_id),
            "--cancel-ratio", str(args.cancel_ratio),
            "--drain", "0.5"]


def run_protocol(label, client, schedule, build_command, first_cl_ord_id, args, report):
    """Drive one protocol through its phases. Runs in its own thread.

    Each phase is a separate client invocation, so a session is a sequence of logons rather
    than one long-lived one. That exercises the connect path repeatedly and keeps a phase that
    goes wrong from taking the rest of the protocol's run with it.
    """
    next_id = first_cl_ord_id
    for name, seconds, rate, orders in schedule:
        started = time.monotonic()
        if rate == 0:
            report(f"  [{label:>6}] {name:<22} {seconds:>4}s  quiet")
            time.sleep(seconds)
            continue
        report(f"  [{label:>6}] {name:<22} {seconds:>4}s  {rate:>4}/s  {orders:>7} orders")
        result = subprocess.run(build_command(client, rate, seconds, next_id, args),
                                capture_output=True, text=True, check=False)
        next_id += orders + 1
        if result.returncode != 0:
            tail = ((result.stdout or "") + (result.stderr or "")).strip().splitlines()[-2:]
            report(f"  [{label:>6}] {name} FAILED (exit {result.returncode}): " + " / ".join(tail))
        # A phase that finishes early -- the client paces itself and may drift -- is held to
        # its slot, so the two protocols' timelines stay aligned and an offset spike really
        # does land against the other's trickle.
        remaining = seconds - (time.monotonic() - started)
        if remaining > 0:
            time.sleep(remaining)


def run(args):
    binary_client, fix_client = resolve_clients(args.prefix)
    schedules = {name: plan(phases, args.minutes) for name, phases in PROTOCOLS.items()}

    lock = threading.Lock()

    def report(line):
        with lock:
            print(line, flush=True)

    base = args.first_cl_ord_id if args.first_cl_ord_id is not None else int(time.time()) * 1000
    threads = [
        threading.Thread(target=run_protocol, args=("binary", binary_client, schedules["binary"],
                                                    binary_command, base, args, report)),
        threading.Thread(target=run_protocol, args=("fix", fix_client, schedules["fix"],
                                                    fix_command, base + 500_000_000, args, report)),
    ]
    started = time.time()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    print(f"\ndone: {time.time() - started:.0f}s elapsed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
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

    schedules = {name: plan(phases, args.minutes) for name, phases in PROTOCOLS.items()}
    print(f"session: {args.minutes:.1f} minutes, two protocols in parallel, "
          f"cancel ratio {args.cancel_ratio}\n")
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
