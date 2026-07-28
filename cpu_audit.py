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
from pathlib import Path

import cpu_layout

_SCRIPT_DIR = Path(__file__).resolve().parent
_DEFAULT_ENV_FILE = _SCRIPT_DIR / "environments" / "dev.toml"


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


def audit(layout_path: Path, run_dir: Path, verbose: bool) -> list[str]:
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

    return problems


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
    return parser.parse_args()


def main() -> None:
    """Read the layout, audit the running components, exit non-zero on a mismatch."""
    args = parse_args()

    env_path = args.env if args.env.is_absolute() else (_SCRIPT_DIR / args.env).resolve()
    if not env_path.is_file():
        sys.exit(f"error: env file not found: {env_path}")
    with open(env_path, "rb") as handle:
        env = tomllib.load(handle)

    if args.install_dir is not None:
        install_dir = args.install_dir.resolve()
    else:
        install_dir = (_SCRIPT_DIR / env["paths"]["install_dir"]).resolve()

    run_dir = install_dir / "run"
    layout_path = run_dir / "cpu_layout.toml"
    if not layout_path.is_file():
        sys.exit(f"error: no layout file at {layout_path} -- run deploy.py first")

    print("=== cpu_audit.py ===")
    print(f"  layout : {layout_path}")
    print()

    problems = audit(layout_path, run_dir, args.verbose)

    if problems:
        print(f"{len(problems)} problem(s) found:")
        for problem in problems:
            print(f"  - {problem}")
        sys.exit(1)

    print("  reality matches the declared layout")


if __name__ == "__main__":
    main()
