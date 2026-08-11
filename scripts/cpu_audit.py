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

Exit status is 0 when reality matches the layout and 1 when it does not, so this
can gate a performance run rather than being read by eye afterwards.

Usage:
  ./cpu_audit.py [--install-dir PATH] [--env PATH] [--verbose]
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
import sys
from dataclasses import dataclass
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


@dataclass
class Occupant:
    """A thread found to be permitted on a hot-path core it does not own."""

    label: str
    cores: list[int]
    kind: str  # "irq", "kernel" or "userspace"


def survey_hot_path_occupancy(hot_path_owner: dict[int, str],
                              deployment_pids: set[int]) -> list[Occupant]:
    """Find every thread on the machine allowed to run on a hot-path core.

    Pinning a thread to a core reserves the core *for* it; it does not reserve
    the core *from* anything else.  Nothing but `isolcpus` stops an unrelated
    process being scheduled there, so the layout being internally consistent is
    not the same as the hot-path cores being quiet -- and it is the second that
    a latency measurement actually depends on.

    Threads are classified by what can be done about them:

      kernel    per-CPU housekeeping (cpuhp/N, migration/N, ksoftirqd/N,
                kworker/N:*). One set exists on every core by construction and
                cannot be moved. Reported for completeness, never a failure.
      irq       interrupt handler threads. These *are* steerable, independently
                of isolcpus, by rewriting the IRQ's affinity -- so an interrupt
                landing on a hot-path core is worth knowing about.
      userspace anything else. Avoidable, and the reason --strict exists.
    """
    hot_path_cores = set(hot_path_owner)
    occupants: list[Occupant] = []

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
            if mask is None:
                continue
            overlap = sorted(set(mask) & hot_path_cores)
            if not overlap:
                continue

            thread_name = read_thread_name(pid, thread_id)
            if kernel:
                kind = "irq" if thread_name.startswith("irq/") else "kernel"
            else:
                kind = "userspace"
            label = thread_name if thread_name == process_name else f"{process_name}/{thread_name}"
            occupants.append(Occupant(f"{label} (pid {pid})", overlap, kind))

    return occupants


def audit(layout_path: Path, run_dir: Path, verbose: bool, strict: bool) -> list[str]:
    """Compare every running thread's real mask against the layout."""
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

    for name, pid in sorted(running.items()):
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

    problems.extend(report_hot_path_occupancy(hot_path_owner, set(running.values()), strict))
    return problems


def report_hot_path_occupancy(hot_path_owner: dict[int, str], deployment_pids: set[int],
                              strict: bool) -> list[str]:
    """Print who else can run on the hot-path cores; return failures if strict.

    Printed rather than returned by default because on a development workstation
    without `isolcpus` this is never empty -- a check that is permanently red is
    a check that gets ignored. It is reported so the contamination is visible
    when a measurement is being taken, and --strict turns the avoidable part of
    it into a failure for a machine that is supposed to be quiet.
    """
    occupants = survey_hot_path_occupancy(hot_path_owner, deployment_pids)
    if not occupants:
        print("  no other thread on this machine may run on a hot-path core")
        return []

    by_kind: dict[str, list[Occupant]] = {"userspace": [], "irq": [], "kernel": []}
    for occupant in occupants:
        by_kind[occupant.kind].append(occupant)

    print()
    print("  Hot-path core occupancy by threads outside the deployment")
    print("  (an affinity mask reserves a core *for* a thread, not *from* others;")
    print("   only isolcpus does that, and this machine does not use it)")

    if by_kind["irq"]:
        count = len(by_kind["irq"])
        print(f"\n  {count} interrupt handler(s) on hot-path cores -- these are steerable:")
        for occupant in sorted(by_kind["irq"], key=lambda o: o.cores):
            print(f"    CPU {cpu_layout.format_cpu_list(occupant.cores)}: {occupant.label}")

    if by_kind["userspace"]:
        # Ranked by thread count: a browser with 150 threads is a far bigger
        # contaminant than a daemon with one, and they all span the same cores.
        weights: dict[str, int] = {}
        for occupant in by_kind["userspace"]:
            name = occupant.label.rsplit(" (pid", 1)[0].split("/")[0]
            weights[name] = weights.get(name, 0) + 1
        count = len(by_kind["userspace"])
        print(f"\n  {count} unrelated userspace thread(s), from {len(weights)} process name(s).")
        print("    Heaviest first:")
        for name, count in sorted(weights.items(), key=lambda item: -item[1])[:12]:
            print(f"    {count:>5} threads  {name}")

        if "irqbalance" in weights:
            print("\n    NOTE: irqbalance is running. It moves interrupt affinity around at will,")
            print("    so any hand-steering of the IRQs above will be undone. Stop or restrict it")
            print("    before relying on IRQ placement for a measurement.")

    if by_kind["kernel"]:
        count = len(by_kind["kernel"])
        print(f"\n  {count} per-CPU kernel thread(s) (cpuhp, migration, ksoftirqd, kworker).")
        print("    One set exists on every core by construction and cannot be moved.")

    if strict and by_kind["userspace"]:
        return [f"{len(by_kind['userspace'])} unrelated userspace thread(s) may run on "
                f"hot-path cores -- this machine is not quiet enough for a latency "
                f"measurement (see isolcpus in docs/design/cpu_pinning.md)"]
    return []


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
    parser.add_argument("--strict", action="store_true",
                        help="fail when unrelated userspace threads may run on hot-path cores "
                             "(expected to fail on a workstation without isolcpus)")
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

    problems = audit(layout_path, run_dir, args.verbose, args.strict)

    print()
    if problems:
        print(f"{len(problems)} problem(s) found:")
        for problem in problems:
            print(f"  - {problem}")
        sys.exit(1)

    print("  reality matches the declared layout")


if __name__ == "__main__":
    main()
