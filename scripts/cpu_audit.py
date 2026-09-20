#!/usr/bin/env python3
"""
cpu_audit.py -- check what is actually pinned against what the layout says.

The declared layout says where every thread on this machine should run.  Nothing
enforces it: an affinity mask is advisory, any thread may call
sched_setaffinity() on itself at any time, and some NUMA-aware thread pools in
third-party libraries do exactly that.  A mask overridden that way is invisible
in the logs -- the component reports the pinning it performed, quite truthfully,
and something later moves the thread.

So the layout is checked against reality rather than assumed.  This reads
run/cpu_layout.toml, walks the running components through their PID files, reads
every thread's real mask from /proc/<pid>/task/<tid>/status, and reports:

  * a thread on a hot-path core that has no business being there -- the failure
    that matters, because it means a latency-critical thread is sharing;
  * a thread of an admitted component that is not on the core it was allocated;
  * a Quill backend that is not on its allocated background core;
  * any process not masked to the background tier at all.

A mask, though, answers "where may this thread run", and a measurement is spoiled
by where threads *did* run.  The two differ by a long way: on a machine without
isolcpus most threads carry an unrestricted mask, so a mask-only report names
well over a thousand threads as occupying the hot-path cores when almost none of
them ever execute there.  A report that is permanently that red is one nobody
reads.

So placement is sampled as well.  Over a short window every candidate thread is
read twice or more from /proc/<pid>/task/<tid>/stat, which carries both the CPU
time it has consumed and the processor it last ran on.  A thread is reported as
having run on a hot-path core only when its consumed time advanced across an
interval *and* it was on that core at the end of it -- consumed time alone says
nothing about where, and the processor field alone can be stale for a thread
that has been asleep since yesterday.  Sampling under-reports by construction: a
thread may visit a core between two samples and be gone by the next.  Nothing
here calls an unobserved core quiet, only unobserved.

Exit status is 0 when reality matches the layout and 1 when it does not, so this
can gate a performance run rather than being read by eye afterwards.

Usage:
  ./cpu_audit.py [--install-dir PATH] [--env PATH] [--verbose]
                 [--sample-seconds N] [--sample-interval N] [--strict]
"""

from __future__ import annotations

try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ImportError:
        import sys
        sys.exit("error: Python 3.11+ or the 'tomli' package is required to parse TOML")

import argparse
import os
import sys
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

import cpu_layout

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_DEFAULT_ENV_FILE = _PROJECT_ROOT / "environments" / "dev.toml"


def read_thread_affinity(pid: int, tid: int) -> list[int] | None:
    """Read one thread's CPU affinity from /proc, or None if it has gone."""
    status_path = Path(f"/proc/{pid}/task/{tid}/status")
    try:
        for line in status_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("Cpus_allowed_list:"):
                return cpu_layout.parse_cpu_list(line.split(":", 1)[1])
    except (OSError, ValueError):
        return None
    return None


def read_thread_name(pid: int, tid: int) -> str:
    """The kernel's short name for a thread, or '?' if it has gone."""
    try:
        return Path(f"/proc/{pid}/task/{tid}/comm").read_text(encoding="utf-8").strip()
    except OSError:
        return "?"


def list_threads(pid: int) -> list[int]:
    """Every thread id in a process, or empty if it has gone."""
    try:
        return sorted(int(entry.name) for entry in Path(f"/proc/{pid}/task").iterdir())
    except OSError:
        return []


def read_running_components(run_dir: Path) -> dict[str, int]:
    """Map component name to PID for everything with a live PID file."""
    running: dict[str, int] = {}
    for pid_file in sorted(run_dir.glob("*.pid")):
        try:
            pid = int(pid_file.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            continue
        if Path(f"/proc/{pid}").is_dir():
            running[pid_file.stem] = pid
    return running


def is_kernel_thread(pid: int) -> bool:
    """Kernel threads have an empty cmdline; userspace processes do not."""
    try:
        return len(Path(f"/proc/{pid}/cmdline").read_bytes()) == 0
    except OSError:
        return True


# Field offsets into /proc/<pid>/task/<tid>/stat.
#
# The fields are documented in proc(5), which numbers them from 1.  The second
# field is the thread name in parentheses, and a thread may name itself anything
# at all, including something containing spaces and parentheses -- "kworker/u128:3+
# dm_vblank_control_workqueue" is a real example from this machine.  Splitting the
# line from the left would therefore put every later field at an offset that
# depends on the name, so the line is split on the LAST occurrence of ") " and the
# fields are counted from what follows it.  That makes the first available field
# proc(5)'s field 3, so proc(5) field N is at index N - 3 in what remains.
#
# The two fields this file depends on are the consumed processor time, which says
# whether a thread ran at all between two readings, and the processor, which says
# where it last ran.  Neither is any use without the other: see sample_placement.
_STAT_STATE = 0        # proc(5) field 3, the run state: R, S, D and so on
_STAT_UTIME = 11       # proc(5) field 14, ticks spent in user mode
_STAT_STIME = 12       # proc(5) field 15, ticks spent in kernel mode
_STAT_PROCESSOR = 36   # proc(5) field 39, the processor this thread last ran on


def parse_isolated_cores(cmdline: str) -> list[int]:
    """Cores the kernel was told to keep out of general scheduling, from a boot line.

    Separate from reading /proc/cmdline so it can be tested on the forms that
    actually appear. isolcpus accepts flag words before the list, as in
    `isolcpus=domain,managed_irq,2-15`, and dropping them is the difference
    between reading the isolated set and reading nothing at all.
    """
    for word in cmdline.split():
        if word.startswith("isolcpus="):
            ranges = [part for part in word.split("=", 1)[1].split(",") if part and part[0].isdigit()]
            try:
                return cpu_layout.parse_cpu_list(",".join(ranges))
            except ValueError:
                return []
    return []


def read_isolated_cores() -> list[int]:
    """The isolated cores of the running kernel, or none if the boot line cannot be read."""
    try:
        return parse_isolated_cores(Path("/proc/cmdline").read_text(encoding="utf-8"))
    except OSError:
        return []


def read_thread_activity(pid: int, tid: int) -> tuple[int, int] | None:
    """One thread's consumed CPU time in ticks and the processor it last ran on.

    Returns None if the thread has gone, which happens constantly and is not an
    error: threads come and go while this is being read.
    """
    try:
        raw = Path(f"/proc/{pid}/task/{tid}/stat").read_text(encoding="utf-8")
        fields = raw.rsplit(") ", 1)[1].split()
        return (int(fields[_STAT_UTIME]) + int(fields[_STAT_STIME]),
                int(fields[_STAT_PROCESSOR]))
    except (OSError, IndexError, ValueError):
        return None


@dataclass
class Placement:
    """One thread, where its mask allows it to run, and where it was seen to run.

    Both halves are needed to say anything useful.  The mask decides whether a
    thread is worth watching at all, and what was observed decides whether it
    actually took any of a hot-path core.
    """

    # How the thread is named in the report: the process name, the thread name if
    # it differs from the process name, and the process id.
    label: str

    # The deployment component this thread belongs to, or None for every thread on
    # the machine that is not part of the venue.  The distinction decides which
    # question is being asked of the thread: whether it ran where the layout put
    # it, or whether it had any business on a hot-path core in the first place.
    component: str | None

    # One of "irq", "kernel" or "userspace", which is to say what could be done
    # about this thread if it were found somewhere unwelcome.  See classify().
    kind: str

    # The hot-path cores this thread's affinity mask permits it to run on.  A
    # thread with none of them is not sampled at all.
    permitted: list[int] = field(default_factory=list)

    # Processor time in ticks that this thread consumed while the window was open.
    # Zero means the thread was asleep throughout, and therefore that nothing at
    # all is known about where it would have run.
    ticks: int = 0

    # How many sampling intervals the thread was seen running on each processor.
    # A count is evidence of having run there; the absence of one is not evidence
    # of not having, because a thread can come and go between two readings.
    cores: Counter = field(default_factory=Counter)

    @property
    def ran(self) -> bool:
        """True when this thread consumed any processor time at all during the window."""
        return self.ticks > 0


def sample_placement(targets: dict[tuple[int, int], Placement], seconds: float,
                     interval: float, read=read_thread_activity) -> None:
    """Fill in where each target thread ran, by repeated reads of /proc.

    Attribution is per interval rather than per sample: a thread counts as having
    run on a core when its consumed CPU time advanced between two consecutive
    reads and it was sitting on that core at the second of them.  Either signal
    alone is misleading -- consumed time says when but not where, and the
    processor field of a sleeping thread records where it last ran, however long
    ago that was.

    At least two reads always happen, so a window of zero still yields the
    start-to-end comparison; more intervals catch more of what a thread did.
    """
    previous: dict[tuple[int, int], tuple[int, int]] = {}
    deadline = time.monotonic() + seconds
    passes = 0
    while True:
        passes += 1
        for key, placement in targets.items():
            reading = read(*key)
            if reading is None:
                continue
            ticks, processor = reading
            was = previous.get(key)
            if was is not None and ticks > was[0]:
                placement.ticks += ticks - was[0]
                placement.cores[processor] += 1
            previous[key] = reading
        if passes >= 2 and time.monotonic() >= deadline:
            return
        time.sleep(min(interval, max(0.0, deadline - time.monotonic())))


def confine_to_background(background: list[int]) -> bool:
    """Keep this audit off the cores it is auditing, so it cannot become the contamination.

    Sampling is a poll, and a poll that runs on a hot-path core competes with the
    threads whose placement it is trying to establish.
    """
    try:
        os.sched_setaffinity(0, set(background))
        return True
    except (OSError, ValueError):
        return False


def classify(kernel: bool, thread_name: str) -> str:
    """What can be done about a thread found on a hot-path core.

      kernel    per-CPU housekeeping (cpuhp/N, migration/N, ksoftirqd/N,
                kworker/N:*). One set exists on every core by construction and
                cannot be moved. Reported for completeness, never a failure.
      irq       interrupt handler threads. These *are* steerable, independently
                of isolcpus, by rewriting the IRQ's affinity -- so an interrupt
                landing on a hot-path core is worth knowing about.
      userspace anything else. Avoidable, and the reason --strict exists.
    """
    if not kernel:
        return "userspace"
    return "irq" if thread_name.startswith("irq/") else "kernel"


def survey_candidates(hot_path_cores: set[int],
                      deployment_pids: set[int]) -> dict[tuple[int, int], Placement]:
    """Every thread outside the deployment whose mask permits a hot-path core.

    Pinning a thread to a core reserves the core *for* it; it does not reserve
    the core *from* anything else.  Nothing but `isolcpus` stops an unrelated
    process being scheduled there, so the layout being internally consistent is
    not the same as the hot-path cores being quiet -- and it is the second that
    a latency measurement actually depends on.

    The mask is the filter and not the finding: a thread that may run on a
    hot-path core is a candidate for having done so, nothing more.  Which of
    them actually did is settled by sampling them.
    """
    candidates: dict[tuple[int, int], Placement] = {}

    for process_dir in Path("/proc").iterdir():
        if not process_dir.name.isdigit():
            continue
        pid = int(process_dir.name)
        if pid in deployment_pids:
            continue

        kernel = is_kernel_thread(pid)
        try:
            process_name = (process_dir / "comm").read_text(encoding="utf-8").strip()
            tasks = list((process_dir / "task").iterdir())
        except OSError:
            continue

        for task in tasks:
            try:
                thread_id = int(task.name)
            except ValueError:
                continue
            mask = read_thread_affinity(pid, thread_id)
            if mask is None or not set(mask) & hot_path_cores:
                continue

            thread_name = read_thread_name(pid, thread_id)
            label = thread_name if thread_name == process_name else f"{process_name}/{thread_name}"
            candidates[(pid, thread_id)] = Placement(
                label=f"{label} (pid {pid})",
                component=None,
                kind=classify(kernel, thread_name),
                permitted=sorted(set(mask) & hot_path_cores))

    return candidates


def audit(layout_path: Path, run_dir: Path, args: argparse.Namespace,
          metrics_only: set[str]) -> list[str]:
    """Compare every running thread's real mask, and where it really ran, against the layout.

    `metrics_only` names the processes the env file marks as observing the venue
    rather than taking part in it. deploy.py allocates them nothing, so holding
    them to the layout would fail every run on a machine where they are up. They
    are not waved through either: with no entry to be measured against, they are
    treated as any other unrelated process and reported if they turn up on a
    hot-path core.
    """
    with open(layout_path, "rb") as handle:  # binary: tomllib requires it
        layout = tomllib.load(handle)

    machine = layout["machine"]
    background = set(cpu_layout.parse_cpu_list(machine["background_cores"]))
    components = layout.get("components", {})

    # Every core allocated to some component, and to which one.  A thread found
    # on one of these that does not belong to that component is the finding this
    # audit exists for.
    hot_path_owner: dict[int, str] = {}
    for name, entry in components.items():
        for core in cpu_layout.parse_cpu_list(entry.get("hot_path_cores", "")):
            hot_path_owner[core] = name

    running = read_running_components(run_dir)
    if not running:
        return [f"no running components found via PID files in {run_dir}"]

    problems: list[str] = []
    verbose = args.verbose
    deployment_threads: dict[tuple[int, int], Placement] = {}

    for name, pid in sorted(running.items()):
        if name in metrics_only:
            if verbose:
                print(f"  --  {name} (pid {pid}) observes the venue rather than taking part; "
                      f"the layout allocates it nothing")
            continue
        entry = components.get(name)
        if entry is None:
            problems.append(f"{name} (pid {pid}) is running but has no entry in the layout")
            continue

        own_cores = cpu_layout.parse_cpu_list(entry.get("hot_path_cores", ""))
        backend_core = entry.get("quill_backend_core")
        unclaimed_own_cores = set(own_cores)

        for tid in list_threads(pid):
            mask = read_thread_affinity(pid, tid)
            if mask is None:
                continue
            thread_name = read_thread_name(pid, tid)
            where = f"{name}/{thread_name} (pid {pid} tid {tid})"
            deployment_threads[(pid, tid)] = Placement(
                label=where, component=name, kind="userspace",
                permitted=sorted(set(mask) & set(hot_path_owner)))

            # A thread pinned to exactly one core is claiming that core.
            if len(mask) == 1:
                core = mask[0]
                owner = hot_path_owner.get(core)
                if owner is not None and owner != name:
                    problems.append(
                        f"{where} is on CPU {core}, which is allocated to {owner}")
                elif owner == name:
                    unclaimed_own_cores.discard(core)
                elif backend_core is not None and core == backend_core:
                    pass
                elif core not in background:
                    problems.append(
                        f"{where} is pinned to CPU {core}, which is in neither tier")
                elif verbose:
                    print(f"  ok  {where} on background CPU {core}")
                continue

            # Any wider mask must stay inside the background tier.
            outside = sorted(set(mask) - background)
            if outside:
                stray = [core for core in outside if core in hot_path_owner]
                if stray:
                    owners = ", ".join(
                        f"CPU {core} ({hot_path_owner[core]})" for core in stray)
                    problems.append(
                        f"{where} may run on hot-path cores it does not own: {owners}")
                else:
                    problems.append(
                        f"{where} may run on {cpu_layout.format_cpu_list(outside)}, "
                        f"outside the background tier")
            elif verbose:
                print(f"  ok  {where} on background {cpu_layout.format_cpu_list(mask)}")

        if unclaimed_own_cores:
            problems.append(
                f"{name} was allocated CPU(s) "
                f"{cpu_layout.format_cpu_list(sorted(unclaimed_own_cores))} "
                f"but no thread is pinned there")

    venue = {name: pid for name, pid in running.items() if name not in metrics_only}
    problems.extend(examine_placement(hot_path_owner, background, deployment_threads, venue, args))
    return problems


def examine_placement(hot_path_owner: dict[int, str], background: set[int],
                      deployment_threads: dict[tuple[int, int], Placement],
                      venue: dict[str, int], args: argparse.Namespace) -> list[str]:
    """Sample where threads really ran, and report what was found there."""
    hot_path_cores = set(hot_path_owner)
    candidates = survey_candidates(hot_path_cores, set(venue.values()))
    targets = dict(candidates)
    targets.update(deployment_threads)

    if not confine_to_background(sorted(background)):
        print("  NOTE: could not confine this audit to the background tier, so its own")
        print("  polling may be part of what it measures")

    print(f"  sampling {len(targets)} thread(s) for {args.sample_seconds:g}s "
          f"every {args.sample_interval:g}s")
    sample_placement(targets, args.sample_seconds, args.sample_interval)

    problems = report_own_placement(hot_path_owner, deployment_threads, args.verbose)
    problems.extend(report_hot_path_occupancy(hot_path_owner, candidates, args.strict))
    return problems


def report_own_placement(hot_path_owner: dict[int, str],
                         deployment_threads: dict[tuple[int, int], Placement],
                         verbose: bool) -> list[str]:
    """Check the deployment's own threads ran where the layout allocated them.

    A thread that consumed no CPU during the window is reported as unobserved
    and never as correct: an idle component proves nothing about its placement,
    and saying otherwise would turn a measurement that did not happen into a
    pass.
    """
    problems: list[str] = []
    unobserved = 0

    for placement in deployment_threads.values():
        if not placement.ran:
            unobserved += 1
            continue
        trespass = {core: count for core, count in placement.cores.items()
                    if core in hot_path_owner and hot_path_owner[core] != placement.component}
        if trespass:
            where = ", ".join(f"CPU {core} ({hot_path_owner[core]}, {count} interval(s))"
                              for core, count in sorted(trespass.items()))
            problems.append(f"{placement.label} was observed running on {where}")
        elif verbose:
            seen = cpu_layout.format_cpu_list(sorted(placement.cores))
            print(f"  ran {placement.label} on CPU {seen}")

    if unobserved and verbose:
        print(f"  {unobserved} deployment thread(s) consumed no CPU during the window, "
              f"so their placement was not observed either way")
    return problems


def report_hot_path_occupancy(hot_path_owner: dict[int, str],
                              candidates: dict[tuple[int, int], Placement],
                              strict: bool) -> list[str]:
    """Print who else ran on the hot-path cores, and who else may; fail on the first if strict.

    Two questions, kept apart because they have very different answers.  What
    *ran* there is the contamination a measurement actually suffered.  What *may*
    run there is the risk it was exposed to, which on a machine without isolcpus
    is almost every thread on it -- true, unactionable, and not worth failing a
    check over.
    """
    hot_path_cores = set(hot_path_owner)
    isolated = read_isolated_cores()

    observed: list[tuple[Placement, dict[int, int]]] = []
    for placement in candidates.values():
        on_hot_path = {core: count for core, count in placement.cores.items()
                       if core in hot_path_cores}
        if on_hot_path:
            observed.append((placement, on_hot_path))

    print()
    print("  Hot-path core occupancy by threads outside the deployment")
    print("  (an affinity mask reserves a core *for* a thread, not *from* others;")
    if isolated:
        covered = "all" if hot_path_cores <= set(isolated) else "some"
        print(f"   only isolcpus does that, and this machine isolates "
              f"{cpu_layout.format_cpu_list(sorted(isolated))} -- {covered} of the hot path)")
    else:
        print("   only isolcpus does that, and this boot does not use it)")

    report_observed(observed)
    report_permitted(candidates)

    if strict and any(placement.kind == "userspace" for placement, _ in observed):
        count = sum(1 for placement, _ in observed if placement.kind == "userspace")
        return [f"{count} unrelated userspace thread(s) were observed running on hot-path "
                f"cores -- this machine is not quiet enough for a latency measurement "
                f"(see isolcpus in docs/framework/cpu_pinning.md)"]
    return []


def report_observed(observed: list[tuple[Placement, dict[int, int]]]) -> None:
    """The threads actually seen executing on a hot-path core during the window."""
    if not observed:
        print()
        print("  Observed: no thread outside the deployment was seen running on a hot-path")
        print("  core during the window. Sampling under-reports, so this means unobserved")
        print("  rather than proven absent -- lengthen --sample-seconds to look harder.")
        return

    by_kind: dict[str, list[tuple[Placement, dict[int, int]]]] = {
        "userspace": [], "irq": [], "kernel": []}
    for entry in observed:
        by_kind[entry[0].kind].append(entry)

    print()
    print(f"  Observed running on hot-path cores: {len(observed)} thread(s)")
    for kind, heading in (("userspace", "unrelated userspace"),
                          ("irq", "interrupt handler"),
                          ("kernel", "per-CPU kernel")):
        entries = by_kind[kind]
        if not entries:
            continue
        print(f"\n    {len(entries)} {heading} thread(s):")
        # Heaviest first: the intervals a thread was seen running are a better
        # measure of how much of the core it took than the number of threads.
        for placement, cores in sorted(entries, key=lambda e: -sum(e[1].values()))[:12]:
            where = ", ".join(f"CPU {core} x{count}" for core, count in sorted(cores.items()))
            print(f"      {placement.label}: {where}")
        if len(entries) > 12:
            print(f"      ... and {len(entries) - 12} more")


def report_permitted(candidates: dict[tuple[int, int], Placement]) -> None:
    """The threads whose mask allows a hot-path core, whether or not they used it."""
    by_kind: dict[str, list[Placement]] = {"userspace": [], "irq": [], "kernel": []}
    for placement in candidates.values():
        by_kind[placement.kind].append(placement)

    if not candidates:
        print("\n  Permitted: no other thread on this machine may run on a hot-path core")
        return

    print(f"\n  Permitted to run there, in total: {len(candidates)} thread(s)")

    if by_kind["irq"]:
        print(f"\n    {len(by_kind['irq'])} interrupt handler(s) -- these are steerable:")
        for placement in sorted(by_kind["irq"], key=lambda p: p.permitted):
            ran = "ran" if placement.ran else "not seen running"
            print(f"      CPU {cpu_layout.format_cpu_list(placement.permitted)}: "
                  f"{placement.label} ({ran})")

    if by_kind["userspace"]:
        # Ranked by thread count: a browser with 150 threads is a far bigger
        # contaminant than a daemon with one, and they all span the same cores.
        weights: Counter = Counter()
        seen_running: Counter = Counter()
        for placement in by_kind["userspace"]:
            name = placement.label.rsplit(" (pid", 1)[0].split("/")[0]
            weights[name] += 1
            if placement.cores:
                seen_running[name] += 1
        print(f"\n    {len(by_kind['userspace'])} unrelated userspace thread(s), "
              f"from {len(weights)} process name(s). Heaviest first:")
        for name, count in weights.most_common(12):
            print(f"      {count:>5} threads  {name:<28} {seen_running[name]} seen on a hot-path core")

        if "irqbalance" in weights:
            print("\n      NOTE: irqbalance is running. It moves interrupt affinity around at will,")
            print("      so any hand-steering of the IRQs above will be undone. Stop or restrict it")
            print("      before relying on IRQ placement for a measurement.")

    if by_kind["kernel"]:
        print(f"\n    {len(by_kind['kernel'])} per-CPU kernel thread(s) "
              f"(cpuhp, migration, ksoftirqd, kworker).")
        print("      One set exists on every core by construction and cannot be moved.")


def parse_args() -> argparse.Namespace:
    """Command-line arguments."""
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--env", type=Path, default=_DEFAULT_ENV_FILE, metavar="PATH",
                        help=f"environment TOML (default: {_DEFAULT_ENV_FILE})")
    parser.add_argument("--install-dir", type=Path, default=None, metavar="PATH",
                        help="install directory (default: paths.install_dir from the env TOML)")
    parser.add_argument("--verbose", action="store_true",
                        help="also report threads that are correctly placed")
    parser.add_argument("--sample-seconds", type=float, default=2.0, metavar="N",
                        help="how long to sample real placement for (default: 2.0). "
                             "0 takes two readings back to back, which sees only what is "
                             "running at that instant")
    parser.add_argument("--sample-interval", type=float, default=0.2, metavar="N",
                        help="seconds between readings while sampling (default: 0.2). "
                             "Shorter catches more and costs more; the audit confines itself "
                             "to the background tier so the cost does not land on the hot path")
    parser.add_argument("--strict", action="store_true",
                        help="fail when unrelated userspace threads are observed running on "
                             "hot-path cores. Threads merely permitted there are reported but "
                             "never fail, since without isolcpus that is nearly every thread")
    return parser.parse_args()


def main() -> None:
    """Read the layout, audit the running components, exit non-zero on a mismatch."""
    args = parse_args()

    env_path = args.env if args.env.is_absolute() else (_PROJECT_ROOT / args.env).resolve()
    if not env_path.is_file():
        sys.exit(f"error: env file not found: {env_path}")
    with open(env_path, "rb") as handle:
        env = tomllib.load(handle)

    if args.install_dir is not None:
        install_dir = args.install_dir.resolve()
    else:
        install_dir = (_PROJECT_ROOT / env["paths"]["install_dir"]).resolve()

    run_dir = install_dir / "run"
    layout_path = run_dir / "cpu_layout.toml"
    if not layout_path.is_file():
        sys.exit(f"error: no layout file at {layout_path} -- run deploy.py first")

    print("=== cpu_audit.py ===")
    print(f"  layout : {layout_path}")
    print()

    metrics_only = {name for name, entry in env.get("components", {}).items()
                    if isinstance(entry, dict) and entry.get("metrics_only")}

    problems = audit(layout_path, run_dir, args, metrics_only)

    print()
    if problems:
        print(f"{len(problems)} problem(s) found:")
        for problem in problems:
            print(f"  - {problem}")
        sys.exit(1)

    print("  reality matches the declared layout")


if __name__ == "__main__":
    main()
