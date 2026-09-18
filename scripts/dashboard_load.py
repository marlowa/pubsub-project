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
import time
from pathlib import Path

# (name, share of the session, orders per second; 0 means send nothing at all)
#
# The shares are fractions rather than minutes so that --minutes rescales the whole day
# without the phases losing their proportions. The rates are what a chart needs rather than
# what a venue can take: the open is roughly three times the steady rate, which is visible
# on a graph, and the spike is high enough to be unmistakable.
PHASES = [
    ("pre-open quiet",      0.04,   0),
    ("the open",            0.08, 400),
    ("morning steady",      0.18, 120),
    ("morning trickle",     0.10,  15),
    ("midday lull",         0.08,   0),
    ("afternoon steady",    0.18, 120),
    ("afternoon spike",     0.05, 600),
    ("late trickle",        0.11,  20),
    ("the close",           0.14, 350),
    ("post-close quiet",    0.04,   0),
]

DEFAULT_COMP_ID_PREFIX = "LOADCLIENT"
DEFAULT_PASSWORD = "loadclientpassword"


def resolve_client(prefix):
    """The load client binary, or exit saying where it was looked for."""
    candidate = Path(prefix) / "bin" / "binary_load_client"
    if not candidate.is_file():
        raise SystemExit(f"error: {candidate} not found. Deploy first, or pass --prefix.")
    return candidate


def plan(minutes):
    """Turn the phase shares into concrete (name, seconds, rate, orders) tuples."""
    total_seconds = int(minutes * 60)
    schedule = []
    for name, share, rate in PHASES:
        seconds = max(1, int(round(total_seconds * share)))
        schedule.append((name, seconds, rate, seconds * rate))
    return schedule


def run_phase(client, name, seconds, rate, first_cl_ord_id, args):
    """Drive one phase, or sleep through it when the rate is zero."""
    if rate == 0:
        print(f"  {name:<20} {seconds:>4}s   quiet -- nothing sent")
        time.sleep(seconds)
        return 0

    # One burst per second, so the client paces steadily across the phase rather than
    # firing everything and idling. Sending flat out would make every phase look the same
    # on a chart: dominated by queueing, which is not what this script is for.
    command = [
        str(client),
        "--comp-id-prefix", args.comp_id_prefix,
        "--password", args.password,
        "--sessions", "1",
        "--orders-per-burst", str(rate),
        "--bursts", str(seconds),
        "--rate", str(rate),
        "--first-cl-ord-id", str(first_cl_ord_id),
        "--cancel-ratio", str(args.cancel_ratio),
    ]
    print(f"  {name:<20} {seconds:>4}s   {rate:>4}/s   {seconds * rate:>7} orders")
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        tail = (result.stdout or result.stderr).strip().splitlines()[-3:]
        print(f"    phase failed (exit {result.returncode}):", *tail, sep="\n      ", file=sys.stderr)
    return seconds * rate


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--minutes", type=float, default=30.0, help="length of the simulated session (default: 30)")
    parser.add_argument("--prefix", default="installed", help="install prefix holding bin/binary_load_client (default: installed)")
    parser.add_argument("--comp-id-prefix", default=DEFAULT_COMP_ID_PREFIX, help=f"SenderCompID prefix (default: {DEFAULT_COMP_ID_PREFIX})")
    parser.add_argument("--password", default=DEFAULT_PASSWORD, help="SCRAM password for the sessions")
    parser.add_argument("--first-cl-ord-id", type=int, default=None,
                        help="starting ClOrdID. Defaults to something derived from the clock, because a ClOrdID the "
                             "matching engine has already seen is rejected as a duplicate and the orders never reach the book")
    parser.add_argument("--cancel-ratio", type=float, default=0.9, metavar="F",
                        help="fraction of placed orders the client also cancels (default: 0.9). Orders that are "
                             "never cancelled rest on the book for ever, and the matching engine recovers every one "
                             "of them at the next start -- past a few minutes of absence that is an R-0117 "
                             "cancel-and-halt, and the count only grows. Set 0 to leave the book to accumulate, "
                             "which is occasionally what a test wants")
    parser.add_argument("--dry-run", action="store_true", help="print the timeline and exit without sending anything")
    args = parser.parse_args(argv)

    schedule = plan(args.minutes)
    total_orders = sum(orders for _, _, _, orders in schedule)
    total_seconds = sum(seconds for _, seconds, _, _ in schedule)

    print(f"session: {total_seconds / 60:.1f} minutes, {total_orders:,} orders across {len(schedule)} phases, "
          f"cancel ratio {args.cancel_ratio}\n")
    print(f"  {'phase':<20} {'time':>5}   {'rate':>6}   {'orders':>7}")
    for name, seconds, rate, orders in schedule:
        rate_text = "quiet" if rate == 0 else f"{rate}/s"
        print(f"  {name:<20} {seconds:>4}s   {rate_text:>6}   {orders:>7}")
    print()

    if args.dry_run:
        return 0

    client = resolve_client(args.prefix)
    next_cl_ord_id = args.first_cl_ord_id if args.first_cl_ord_id is not None else int(time.time()) * 1000

    started = time.time()
    for name, seconds, rate, _ in schedule:
        next_cl_ord_id += run_phase(client, name, seconds, rate, next_cl_ord_id, args) + 1

    print(f"\ndone: {time.time() - started:.0f}s elapsed, ClOrdIDs up to {next_cl_ord_id:,}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
