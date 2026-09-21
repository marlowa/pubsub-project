#!/usr/bin/env python3
"""
crash_reports.py -- notice that a venue process crashed, once, and keep the evidence.

Why this exists. A sequencer segfaulted on 2026-08-21 and nobody knew until 2026-08-28, when
somebody happened to look at `coredumpctl` for an unrelated reason. A matching engine had
segfaulted on 2026-08-08 and was still unrecorded three weeks later. Neither was hard to diagnose
once found -- both took minutes -- and the whole cost was in nobody looking. See docs/bug_list.md,
BUG-0057 and BUG-0067.

## Preserve first, warn second, then never warn again

A warning that repeats on every start stops being read, and a venue is started many times a day.
So this warns **once per crash** -- but a one-shot warning is only safe if missing it costs
nothing, which is why the order matters:

  1. Write the crash's symbolised backtrace to a file that will still be there next month.
  2. Say so, once, naming that file.
  3. Record it as seen, so the next start is silent.

**The backtrace, not the core, is the thing worth keeping.** Both of the crashes above were
diagnosed from `coredumpctl info` output and neither could have been diagnosed from its core: the
executables had been rebuilt many times over and their symbols were long gone. systemd captures the
trace at dump time, while the binary still exists, and keeps it in the journal after the core file
itself has been reclaimed -- an entry marked `missing` has lost its core and still has its stack.
A few kilobytes of text outlives a 200KB core in usefulness.

## What is deliberately filtered out

`coredumpctl` on this machine lists 93 entries, of which 84 are the unit test binary raising
signals on purpose -- `ConsoleCaptureDeathTest` and friends. Reporting those would bury the two
that matter under noise on the very first run, which is the fastest way to teach somebody to ignore
a warning. Only executables under the install prefix are considered, and test binaries are excluded
by name.

## What this cannot see

`ulimit -c` is 0 in some launch contexts, and a process that produces no core produces no
`coredumpctl` entry either. **The launcher is the primary detector**: it sees the exit status of
every child it supervises and knows a crash signal from a shutdown one, whether or not a core was
written. This is the backstop for what the launcher cannot see -- a component started by hand, or a
launcher that died with its child.

Nothing here is required for the venue to run. Absent `coredumpctl`, or a journal this user cannot
read, it reports nothing rather than failing.
"""

from __future__ import annotations

import re
import subprocess
from datetime import datetime
from pathlib import Path

# Executables that raise signals on purpose. Their cores are evidence that the tests ran, not that
# anything is wrong, and they outnumber the real crashes forty to one.
_TEST_BINARY_MARKERS = ("_tests", "_test", "gtest")

# Where the seen-mark and the preserved traces live. var/ is chosen because it already holds the
# WAL and the epoch state -- the things that must survive a redeploy. installed/etc is rewritten by
# every deployment, so a mark kept there would be lost and every crash re-reported.
#
# Per machine, which is right: a crash is a machine-local event and so is its core. The deployed
# directory structure gives every host a var/, so each keeps its own crash history beside its own
# WAL and epoch state, and nothing has to be collected centrally for this to work.
_STATE_DIR_NAME = "var"
_SEEN_FILE_NAME = "last_seen_crash"
_REPORT_DIR_NAME = "crash_reports"


class Crash:
    """One crash worth telling somebody about."""

    def __init__(self, pid: str, when: str, signal_name: str, executable: str) -> None:
        self.pid = pid
        self.when = when
        self.signal_name = signal_name
        self.executable = executable

    @property
    def name(self) -> str:
        return Path(self.executable).name

    def __repr__(self) -> str:
        return f"Crash({self.name} pid={self.pid} {self.signal_name} at {self.when})"


def _coredumpctl(*args: str) -> str | None:
    """Run coredumpctl, or None when it is absent or refuses."""
    try:
        result = subprocess.run(["coredumpctl", *args], capture_output=True, text=True,
                                check=False, timeout=20)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout


def _parse_timestamp(fields: list[str]) -> str:
    """The leading date fields of a coredumpctl list row, rejoined."""
    return " ".join(fields[:4])


def find_crashes(install_dir: Path) -> list[Crash]:
    """Crashes of this venue's own executables, newest last. Empty when it cannot tell.

    Scoped to the install prefix so that another checkout's crashes, and the machine's unrelated
    ones, are somebody else's problem.
    """
    listing = _coredumpctl("list", "--no-pager", "--no-legend")
    if not listing:
        return []

    wanted = str((install_dir / "bin").resolve())
    crashes: list[Crash] = []
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) < 10:
            continue
        executable = next((f for f in fields if f.startswith("/")), "")
        if not executable.startswith(wanted):
            continue
        if any(marker in Path(executable).name for marker in _TEST_BINARY_MARKERS):
            continue
        # Layout: <date fields...> PID UID GID SIG COREFILE EXE SIZE
        pid = fields[4]
        signal_name = next((f for f in fields if f.startswith("SIG")), "?")
        crashes.append(Crash(pid, _parse_timestamp(fields), signal_name, executable))
    return crashes


def _seen_path(install_dir: Path) -> Path:
    return install_dir / _STATE_DIR_NAME / _SEEN_FILE_NAME


def _already_seen(install_dir: Path) -> set[str]:
    try:
        return {line.strip() for line in _seen_path(install_dir).read_text().splitlines() if line.strip()}
    except OSError:
        return set()


def _mark_seen(install_dir: Path, pids: list[str]) -> None:
    """Record these as reported. Best effort: a venue must not fail to start over a marker file."""
    path = _seen_path(install_dir)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        existing = _already_seen(install_dir)
        path.write_text("\n".join(sorted(existing | set(pids))) + "\n")
    except OSError:
        pass


def preserve(install_dir: Path, crash: Crash) -> Path | None:
    """Write this crash's symbolised backtrace where it will outlive the journal. None on failure.

    Named by executable, pid and date so that a directory of these reads as a history rather than
    as a pile of hashes.
    """
    info = _coredumpctl("info", crash.pid)
    if not info:
        return None
    stamp = re.sub(r"[^0-9A-Za-z]+", "-", crash.when).strip("-")
    target_dir = install_dir / _STATE_DIR_NAME / _REPORT_DIR_NAME
    target = target_dir / f"{crash.name}-{crash.pid}-{stamp}.txt"
    try:
        target_dir.mkdir(parents=True, exist_ok=True)
        header = (f"# Preserved by crash_reports.py on {datetime.now().isoformat(timespec='seconds')}\n"
                  f"# The journal keeps this trace after the core file is reclaimed; the executable\n"
                  f"# it names will have been rebuilt long before anybody reads this.\n\n")
        target.write_text(header + info)
    except OSError:
        return None
    return target


def report_new_crashes(install_dir: Path) -> list[str]:
    """Preserve and describe crashes not reported before. Lines to print; empty when there are none.

    Marks them seen as it goes, so the next start says nothing. That is safe only because the
    backtrace has been written to disk first -- missing the message costs nothing when the evidence
    is still there.
    """
    crashes = [c for c in find_crashes(install_dir) if c.pid not in _already_seen(install_dir)]
    if not crashes:
        return []

    lines: list[str] = []
    newest = crashes[-1]
    lines.append(f"{len(crashes)} unreported crash(es) of this venue's processes:")
    for crash in crashes[-3:]:
        saved = preserve(install_dir, crash)
        where = f" -> {saved}" if saved else "  (backtrace could not be read)"
        lines.append(f"  {crash.name} {crash.signal_name} at {crash.when}{where}")
    if len(crashes) > 3:
        lines.append(f"  ... and {len(crashes) - 3} older, listed by: coredumpctl list | grep bin/")
    lines.append(f"The backtrace matters more than the core: {newest.name} has been rebuilt since, "
                 "so its symbols are gone but the trace is not.")
    lines.append("This is said once per crash. It will not be repeated on the next start.")

    _mark_seen(install_dir, [c.pid for c in crashes])
    return lines


def report(install_dir: Path) -> bool:
    """Print the lines, if any. True when something was printed."""
    lines = report_new_crashes(install_dir)
    if not lines:
        return False
    print("WARNING: a venue process crashed since this was last checked.")
    for line in lines:
        print(f"  {line}")
    return True


if __name__ == "__main__":
    import sys
    target_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "installed"
    report(target_dir)
    raise SystemExit(0)
