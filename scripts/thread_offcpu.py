#!/usr/bin/env python3
"""
thread_offcpu.py -- Find out why a thread stopped running, when a profiler cannot.

THE PROBLEM. A component that normally answers in microseconds occasionally takes hundreds
of milliseconds. A sampling profiler is the usual next step and it cannot help, because it
observes a thread only while that thread is RUNNING on a cpu. A thread stalled because it is
waiting produces no samples at all, just a gap, so the profile of a stalling thread looks
unremarkable and says nothing about the stall.

That inverts the usual reading: a profile with no obvious hot spot is not evidence that
nothing is wrong. For a stall it is the expected result.

WHAT THIS DOES INSTEAD. There are three reasons a thread is not running, with three different
causes and three different remedies, and the operating system distinguishes them:

    what is seen                          what it means                where the fault lies
    state S, and little runqueue time     waiting on a lock, a queue,  the application or its
                                          or an event that has not     framework -- this is
                                          arrived                      the lock hypothesis
    state D                               in the kernel, waiting for   below the application
                                          I/O or memory management     entirely
    runnable, runqueue time climbing      ready to run; something      the machine: process
                                          else had the cpu             count, pinning

The distinction settles the question usually asked first. **A contended mutex or futex shows
as state S.** A thread found in state D is not blocked on a user-space lock whatever the
framework does inside, and an investigation can be redirected on the strength of one letter.

WHAT IT NEEDS. Three files under /proc, all readable by whoever owns the process:

    /proc/<pid>/task/<tid>/stat        field 3 is the thread state character
    /proc/<pid>/task/<tid>/schedstat   ns on the cpu; ns runnable but not scheduled
    /proc/<pid>/task/<tid>/wchan       the kernel function the thread is sleeping in

No source, no symbols, no debug build, no restart, and no privileges beyond owning the
process. It therefore works against software that cannot be rebuilt or inspected.

wchan is the most useful of the three and the most often overlooked: for a thread in state D
it names the function being waited in, which frequently identifies the mechanism outright
rather than merely its category.

EXAMPLES

    # watch every thread of a process for two minutes
    thread_offcpu.py --pid 1234 --seconds 120

    # one named thread, finer sampling, report anything over 10 ms
    thread_offcpu.py --pid 1234 --thread SequencerThread --hz 500 --min-stall-ms 10

    # keep the samples for later analysis
    thread_offcpu.py --pid 1234 --output samples.csv

READING THE RESULT. Two tables. The first lists each stall with its duration, the thread it
happened on and why the thread was not running. The second counts the kernel functions seen
during uninterruptible sleep, which is the one that usually names the cause.

CAUTIONS.

  * Polling has a floor. At the default 200 Hz a stall shorter than about 5 ms can fall
    between samples, and boundaries are accurate only to one interval. Good for tens or
    hundreds of milliseconds; useless below a few.
  * Idleness looks like sleeping. An event-driven thread with nothing to do sits in state S
    quite legitimately, which is why --min-stall-ms exists and why a result matters only for
    a period when the thread was supposed to be busy.
  * Watching many threads at a high rate costs cpu on the machine being measured. Name one
    thread when it is known.
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import Counter
from pathlib import Path

# Reasons a thread is not running, as this reports them.
RUNNABLE = "runnable, waiting for a cpu"
BLOCKED_IO = "uninterruptible sleep (kernel I/O)"
SLEEPING = "sleeping (lock, queue, or event)"


class Thread:
    """One thread being watched, and what has been seen of it."""

    def __init__(self, pid: int, tid: int, name: str):
        self.pid = pid
        self.tid = tid
        self.name = name
        self.stat_path = Path(f"/proc/{pid}/task/{tid}/stat")
        self.sched_path = Path(f"/proc/{pid}/task/{tid}/schedstat")
        self.wchan_path = Path(f"/proc/{pid}/task/{tid}/wchan")
        self.previous: tuple[float, str, int, int] | None = None
        self.window_start: float | None = None
        self.window_wait_ms = 0.0
        self.window_states: Counter = Counter()
        self.window_wchans: Counter = Counter()
        self.gone = False

    def read(self) -> tuple[float, str, int, int] | None:
        """Wall clock, state character, cumulative run ns, cumulative runqueue-wait ns."""
        try:
            stat = self.stat_path.read_text()
            sched = self.sched_path.read_text().split()
        except OSError:
            # The thread has ended. Only these two are required, so only these end the watch.
            self.gone = True
            return None
        # comm is parenthesised and may itself contain spaces and brackets, so the state is
        # found from the LAST closing bracket rather than at a fixed offset.
        return time.time(), stat[stat.rindex(")") + 2], int(sched[0]), int(sched[1])

    def where(self) -> str:
        """The kernel function this thread is sleeping in, or "" if it cannot be told.

        Best effort by design. wchan reads as "0" for a running thread and can be denied
        outright depending on how the machine is configured. A probe that treats one optional
        field as fatal exits on its first sample and then reports, with complete confidence,
        that nothing happened.
        """
        try:
            value = self.wchan_path.read_text().strip()
            return value if value and value != "0" else ""
        except OSError:
            return ""


class Stall:
    """One window in which a thread was not running."""

    def __init__(self, thread: str, start: float, duration_ms: float, why: str,
                 wchans: Counter, states: Counter):
        self.thread = thread
        self.start = start
        self.duration_ms = duration_ms
        self.why = why
        self.wchans = wchans
        self.states = states


def writable_file_mappings(pid: int) -> list[str]:
    """Files this process has mapped writable, read from outside it.

    Worth showing beside an I/O stall, because a write to a memory-mapped file does not look
    like I/O in the code that does it -- it is a memcpy -- and yet the first write to each
    page must be backed by a real block, which can wait. A process found stalling in the
    kernel while holding a writable file mapping is a strong hint at where to look, and this
    needs no source: the mapping is visible from the outside whatever built the program.
    """
    mapped = []
    try:
        for line in Path(f"/proc/{pid}/maps").read_text().splitlines():
            parts = line.split(maxsplit=5)
            if len(parts) < 6:
                continue
            perms, path = parts[1], parts[5].strip()
            # Writable, backed by a real path, and not the program or its libraries -- those
            # are mapped writable for relocations and are never the thing being written to.
            if "w" in perms and path.startswith("/") and not path.endswith(".so") \
                    and ".so." not in path and "/dev/shm/sem." not in path:
                mapped.append(path)
    except OSError:
        return []
    return sorted(set(mapped))


def io_counters(pid: int) -> tuple[int, int] | None:
    """Bytes this process has actually caused to be read from and written to storage."""
    try:
        values = {}
        for line in Path(f"/proc/{pid}/io").read_text().splitlines():
            key, _, value = line.partition(":")
            values[key.strip()] = int(value)
        return values.get("read_bytes", 0), values.get("write_bytes", 0)
    except (OSError, ValueError):
        return None


def discover(pid: int, wanted: str | None) -> list[Thread]:
    task_dir = Path(f"/proc/{pid}/task")
    if not task_dir.is_dir():
        raise SystemExit(f"error: no process {pid}, or it cannot be read by this user")
    threads = []
    for entry in sorted(task_dir.iterdir(), key=lambda p: int(p.name)):
        try:
            name = entry.joinpath("comm").read_text().strip()
        except OSError:
            continue
        if wanted is None or name == wanted:
            threads.append(Thread(pid, int(entry.name), name))
    if not threads:
        present = sorted({t.name for t in discover(pid, None)})
        raise SystemExit(f"error: no thread named '{wanted}' in pid {pid}\n"
                         f"       threads present: {', '.join(present)}")
    return threads


def classify(off_ms: float, wait_ms: float, states: Counter) -> str:
    """Why the thread was not running, from what was seen across the whole window."""
    # Runqueue time is the least ambiguous signal: it means the thread was READY and did not
    # get a cpu, which is a machine problem rather than anything the code did.
    if wait_ms >= off_ms * 0.5:
        return RUNNABLE
    # A window can span more than one state. Uninterruptible sleep anywhere in it is the
    # finding, because nothing else produces it and it is never ordinary idleness.
    if states.get("D", 0):
        return BLOCKED_IO
    return SLEEPING


def watch(threads: list[Thread], hz: int, seconds: float, min_stall_ms: float,
          out) -> tuple[list[Stall], int, dict[str, float]]:
    interval = 1.0 / hz
    deadline = time.time() + seconds
    stalls: list[Stall] = []
    samples = 0

    # How much cpu each thread actually used, so the reader can tell a thread that was busy
    # and stalled from one that was simply idle. Without it the two are indistinguishable:
    # an event-driven thread with nothing to do sleeps exactly like one waiting on a lock.
    started_at = time.time()
    first_run_ns: dict[str, int] = {}

    for thread in threads:
        thread.previous = thread.read()
        if thread.previous:
            first_run_ns[thread.name] = thread.previous[2]

    while time.time() < deadline:
        time.sleep(interval)
        alive = False
        for thread in threads:
            if thread.gone:
                continue
            current = thread.read()
            if current is None:
                continue
            alive = True
            samples += 1
            previous = thread.previous
            thread.previous = current
            if previous is None:
                continue

            wall_ms = (current[0] - previous[0]) * 1000
            run_ms = (current[2] - previous[2]) / 1e6
            wait_ms = (current[3] - previous[3]) / 1e6
            off = run_ms < wall_ms * 0.2   # barely ran during this interval

            wchan = thread.where() if current[1] == "D" else ""
            if out:
                out.write(f"{current[0]:.6f},{thread.name},{current[1]},{current[2]},{current[3]},{wchan}\n")

            if off:
                # Consecutive off-cpu samples are ONE stall. Testing each sample against the
                # threshold instead is the mistake that makes a 400 ms stall invisible: spread
                # over eighty samples it shows five milliseconds each and trips nothing.
                if thread.window_start is None:
                    thread.window_start = previous[0]
                    thread.window_wait_ms = 0.0
                    thread.window_states = Counter()
                    thread.window_wchans = Counter()
                thread.window_wait_ms += wait_ms
                thread.window_states[current[1]] += 1
                if wchan:
                    thread.window_wchans[wchan] += 1
            elif thread.window_start is not None:
                duration = (previous[0] - thread.window_start) * 1000
                if duration >= min_stall_ms:
                    stalls.append(Stall(thread.name, thread.window_start, duration,
                                        classify(duration, thread.window_wait_ms, thread.window_states),
                                        thread.window_wchans, thread.window_states))
                thread.window_start = None
        if not alive:
            print("  every watched thread has ended")
            break

    busy = {}
    elapsed = time.time() - started_at
    for thread in threads:
        if thread.previous and thread.name in first_run_ns and elapsed > 0:
            used_ns = thread.previous[2] - first_run_ns[thread.name]
            busy[thread.name] = 100.0 * (used_ns / 1e9) / elapsed
    return stalls, samples, busy


def report(stalls: list[Stall], samples: int, seconds: float, min_stall_ms: float,
           busy: dict[str, float]) -> None:
    print(f"\n{samples:,} samples taken over {seconds:.0f}s")

    if busy:
        print("\n  cpu used by each watched thread over that period:")
        for name, percent in sorted(busy.items(), key=lambda kv: -kv[1]):
            note = ""
            if percent < 10:
                note = "   <- mostly idle: its sleeping is probably having nothing to do"
            print(f"    {name[:28]:<30}{percent:5.1f}%{note}")

    if not stalls:
        # Said explicitly, because "nothing was found" and "the probe never ran" produce the
        # same empty output otherwise, and the second is easy to mistake for the first.
        print(f"\nNo thread was off the cpu for longer than {min_stall_ms:.0f} ms.")
        busiest = max(busy.values()) if busy else 0.0
        if busiest > 80:
            # A finding rather than a failure, and the only one of the three outcomes that
            # supports the explanation usually reached for first.
            print("\n  A watched thread stayed on the cpu throughout, at "
                  f"{busiest:.0f}%. If latency excursions are")
            print("  happening anyway, they are INSIDE that loop rather than a wait for")
            print("  something: a lock taken and released, work that is genuinely long, or")
            print("  a cache or TLB effect. Nothing here is waiting on the kernel, so the")
            print("  next step is a cycle profile -- which for a running thread does work.")
        else:
            print("If a stall was expected here, check that the load was actually running, and")
            print("that --min-stall-ms is below the size of stall being looked for.")
        return

    print(f"\n{len(stalls)} stall(s) over {min_stall_ms:.0f} ms\n")
    print(f"  {'when':<13}{'thread':<20}{'off cpu':>10}   why")
    print(f"  {'-'*13}{'-'*20}{'-'*10}   {'-'*44}")
    for stall in sorted(stalls, key=lambda s: -s.duration_ms)[:25]:
        when = time.strftime("%H:%M:%S", time.localtime(stall.start))
        print(f"  {when:<13}{stall.thread[:19]:<20}{stall.duration_ms:>7.0f} ms   {stall.why}")
    if len(stalls) > 25:
        print(f"  ... and {len(stalls) - 25} more")

    worst = max(stalls, key=lambda s: s.duration_ms)
    total = sum(s.duration_ms for s in stalls)
    print(f"\n  longest {worst.duration_ms:.0f} ms, total {total/1000:.2f} s off the cpu")

    by_reason = Counter(s.why for s in stalls)
    print("\n  by reason:")
    for why, n in by_reason.most_common():
        print(f"    {n:>5}  {why}")

    wchans: Counter = Counter()
    for stall in stalls:
        wchans.update(stall.wchans)
    if wchans:
        print("\n  kernel functions waited in, while in uninterruptible sleep:")
        print("  (this is usually the answer -- it names the mechanism, not just its category)")
        for name, n in wchans.most_common(12):
            print(f"    {n:>6}  {name}")
    elif by_reason.get(BLOCKED_IO):
        print("\n  uninterruptible sleep was seen but wchan named nothing.")
        print("  kernel.kptr_restrict may be 1; as root, sysctl -w kernel.kptr_restrict=0")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True, help="process to watch")
    parser.add_argument("--thread", help="one thread by name (see /proc/<pid>/task/<tid>/comm); default is all of them")
    parser.add_argument("--hz", type=int, default=200, help="samples a second per thread (default 200)")
    parser.add_argument("--seconds", type=float, default=120, help="how long to watch (default 120)")
    parser.add_argument("--min-stall-ms", type=float, default=40,
                        help="ignore anything off the cpu for less than this (default 40)")
    parser.add_argument("--output", help="write every sample to this file as CSV")
    args = parser.parse_args()

    threads = discover(args.pid, args.thread)
    names = ", ".join(sorted({t.name for t in threads}))
    print(f"watching {len(threads)} thread(s) of pid {args.pid} at {args.hz} Hz for {args.seconds:.0f}s")
    print(f"  threads: {names}")
    print(f"  reporting anything off the cpu for more than {args.min_stall_ms:.0f} ms")
    if len(threads) > 8:
        print(f"  NOTE: {len(threads)} threads at {args.hz} Hz is {len(threads) * args.hz:,} reads a second")
        print("        on the machine being measured. Use --thread when the thread is known.")

    out = None
    if args.output:
        out = open(args.output, "w")
        out.write("wall,thread,state,run_ns,wait_ns,wchan\n")
    try:
        stalls, samples, busy = watch(threads, args.hz, args.seconds, args.min_stall_ms, out)
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 1
    finally:
        if out:
            out.close()

    report(stalls, samples, args.seconds, args.min_stall_ms, busy)

    # Shown after a stall in the kernel, because it is the likeliest place to look next and
    # is readable whatever built the program.
    if any(s.why == BLOCKED_IO for s in stalls):
        mapped = writable_file_mappings(args.pid)
        counters = io_counters(args.pid)
        print("\n  this process was blocked in the kernel. What it has mapped writable:")
        if mapped:
            for path in mapped[:10]:
                print(f"    {path}")
            print("\n    A write to a mapped file is a memcpy in the code and can still wait:")
            print("    the first write to each page needs a real block behind it, and on a")
            print("    journalling filesystem that can wait for a transaction to commit.")
        else:
            print("    (none -- so a mapped file is not the explanation here)")
        if counters:
            print(f"\n    storage bytes: {counters[0]:,} read, {counters[1]:,} written")
    if args.output:
        print(f"\n  samples written to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
