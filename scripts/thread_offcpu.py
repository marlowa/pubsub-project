#!/usr/bin/env python3
"""
thread_offcpu.py -- Say why a thread stopped running, when perf cannot.

Cycle-sampling profilers see a thread only while it is ON the CPU. A thread that stalls
because it is waiting produces no samples at all -- just a gap -- so a profile of a stalling
thread looks unremarkable and says nothing about the stall. That is what happened to the
sequencer on 2026-08-31: 22 reactor stalls of up to 470 ms, and a perf profile whose busiest
symbol accounted for 3% of samples.

The measurement that distinguishes the cases is off-CPU profiling, which normally means the
sched tracepoints -- and those need root. This needs nothing: /proc/<tid>/schedstat and
/proc/<tid>/stat are world-readable and between them separate the three reasons a thread is
not running.

    field                     what it means                    what it tells you
    schedstat[0]  run    ns   time spent ON the cpu
    schedstat[1]  wait   ns   time RUNNABLE but not scheduled  -> the cpu was busy elsewhere
    stat[2]       state       R runnable, S sleeping, D uninterruptible sleep

So for a window where the wall clock advanced but run time did not:

    wait time advanced          the thread was ready and something else had the cpu.
                                A machine problem: too many processes, or no isolation.
    wait time did not advance,
    state S                     the thread was asleep -- waiting on a lock, a queue, or an
                                event that had not arrived. Idleness looks like this too,
                                which is why the caller must say when the thread was
                                supposed to be busy.
    state D                     uninterruptible sleep: the thread was in the kernel waiting
                                for I/O. On a reactor thread this is the serious one.

Polling is not free and not exact: at the default 200 Hz a stall shorter than 5 ms may fall
between samples, and the reported boundaries are accurate only to one interval. It is enough
to classify a stall of tens or hundreds of milliseconds, which is what it is for.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path


def thread_ids(pid: int) -> list[int]:
    return sorted(int(p.name) for p in Path(f"/proc/{pid}/task").iterdir())


def thread_name(pid: int, tid: int) -> str:
    try:
        return Path(f"/proc/{pid}/task/{tid}/comm").read_text().strip()
    except OSError:
        return "?"


def find_thread(pid: int, name: str) -> int | None:
    for tid in thread_ids(pid):
        if thread_name(pid, tid) == name:
            return tid
    return None


def sample(pid: int, tid: int) -> tuple[float, str, int, int] | None:
    """Wall clock, state character, cumulative run ns, cumulative runqueue-wait ns."""
    try:
        stat = Path(f"/proc/{pid}/task/{tid}/stat").read_text()
        sched = Path(f"/proc/{pid}/task/{tid}/schedstat").read_text().split()
    except OSError:
        return None
    # The comm field is parenthesised and may itself contain spaces, so the state is the
    # first field after the closing bracket rather than a fixed offset.
    state = stat[stat.rindex(")") + 2]
    return time.time(), state, int(sched[0]), int(sched[1])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True, help="process holding the thread")
    parser.add_argument("--thread", required=True, help="thread name, as in /proc/<pid>/task/<tid>/comm")
    parser.add_argument("--hz", type=int, default=200, help="samples a second (default 200)")
    parser.add_argument("--seconds", type=float, default=900, help="how long to watch (default 900)")
    parser.add_argument("--report-over-ms", type=float, default=50,
                        help="report any window off the cpu for longer than this (default 50)")
    parser.add_argument("--output", help="write every sample here as CSV, for later analysis")
    args = parser.parse_args()

    tid = find_thread(args.pid, args.thread)
    if tid is None:
        names = sorted({thread_name(args.pid, t) for t in thread_ids(args.pid)})
        print(f"error: no thread named '{args.thread}' in pid {args.pid}", file=sys.stderr)
        print(f"       threads present: {', '.join(names)}", file=sys.stderr)
        return 2

    print(f"watching {args.thread} (tid {tid}) in pid {args.pid} at {args.hz} Hz for {args.seconds:.0f}s")
    print(f"reporting any window off the cpu for more than {args.report_over_ms:.0f} ms\n")

    interval = 1.0 / args.hz
    deadline = time.time() + args.seconds
    previous = sample(args.pid, tid)
    if previous is None:
        print("error: cannot read that thread", file=sys.stderr)
        return 2

    out = open(args.output, "w") if args.output else None
    if out:
        out.write("wall,state,run_ns,wait_ns\n")

    events = []
    while time.time() < deadline:
        time.sleep(interval)
        current = sample(args.pid, tid)
        if current is None:
            print("the thread has gone")
            break
        if out:
            out.write(f"{current[0]:.6f},{current[1]},{current[2]},{current[3]}\n")

        wall_ms = (current[0] - previous[0]) * 1000
        run_ms = (current[2] - previous[2]) / 1e6
        wait_ms = (current[3] - previous[3]) / 1e6
        off_ms = wall_ms - run_ms

        if off_ms >= args.report_over_ms:
            if wait_ms >= off_ms * 0.5:
                why = f"RUNNABLE, not scheduled ({wait_ms:.0f} ms on the runqueue) -- the cpu was busy elsewhere"
            elif current[1] == "D":
                why = "UNINTERRUPTIBLE SLEEP -- in the kernel waiting for I/O"
            elif current[1] == "S":
                why = f"SLEEPING (runqueue wait only {wait_ms:.0f} ms) -- waiting on a lock, a queue, or an event"
            else:
                why = f"state {current[1]}, runqueue wait {wait_ms:.0f} ms"
            stamp = time.strftime("%H:%M:%S", time.localtime(current[0]))
            print(f"  {stamp}  off the cpu {off_ms:7.0f} ms   {why}")
            events.append((current[0], off_ms, current[1], wait_ms))
        previous = current

    if out:
        out.close()

    print(f"\n{len(events)} window(s) off the cpu for more than {args.report_over_ms:.0f} ms")
    if events:
        runnable = sum(1 for _, off, _, wait in events if wait >= off * 0.5)
        blocked_io = sum(1 for _, _, state, _ in events if state == "D")
        sleeping = len(events) - runnable - blocked_io
        print(f"  {runnable} runnable but unscheduled, {blocked_io} in uninterruptible I/O, {sleeping} sleeping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
