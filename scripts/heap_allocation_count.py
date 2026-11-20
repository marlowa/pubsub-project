#!/usr/bin/env python3
# Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
heap_allocation_count.py -- counts the calls each thread of the venue makes to the heap allocator while orders flow,
and says where each call came from. BUG-0087: whether the order path allocates from the heap, established on demand.

How it works:

  1. It clears what earlier runs left, as devenv.py start --fresh-wal --fresh-book does, and checks that nothing of the
     installation is running.
  2. It runs perf_run.py with a load profile of its own: one steady phase of orders and cancels, long enough for the
     settling time, the window and a margin. perf_run.py starts the venue, drives the orders and stops the venue.
     Every program perf_run.py starts inherits LD_PRELOAD naming the counting library,
     <install>/lib/libheap_allocation_count.so (libraries/heap_allocation_count), and is told which programs to count.
  3. Once the load has run for the settling time, it opens the counting window by creating a file, and closes it after
     the window's length by removing the file. Each counted program writes a report when the window closes.
  4. It turns each call stack's addresses into functions and source lines with addr2line, and prints, for each program
     and thread, the calls of each kind and the call stacks they came from, most frequent first.
  5. Given --expectations, it compares each thread's calls with the most that file allows for it, and fails if any
     thread made more.

Everything a component does while it starts, allocating its pools among other things, happens before the window opens
and is not counted.

The output goes to <install>/heap_count/<date_time>/: the profile, perf_run.py's output, the reports, and summary.txt.
"""
from __future__ import annotations

import argparse
import collections
import datetime
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import tomllib

_SCRIPT_DIR = Path(__file__).resolve().parent
_PROJECT_ROOT = _SCRIPT_DIR.parent

COUNTED_PROGRAMS = ["sequencer", "matching_engine", "binary_order_gateway", "fix_order_gateway", "matching_engine_publisher"]
CALL_KINDS = ["malloc", "calloc", "realloc", "aligned"]
# Frames in these files are the allocator and the C++ runtime that calls it; a stack is named by the first frame
# outside them, which is the venue's own code.
RUNTIME_FILES = ("libheap_allocation_count.so", "libc.so", "libstdc++.so", "libgcc_s.so", "ld-linux")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("install_prefix", nargs="?", default="installed", help="the install prefix (default: installed)")
    parser.add_argument("--settle", type=float, default=30.0, metavar="SECONDS",
                        help="seconds of load before the window opens (default: 30)")
    parser.add_argument("--window", type=float, default=60.0, metavar="SECONDS", help="seconds the window stays open (default: 60)")
    parser.add_argument("--rate", type=float, default=0.3, metavar="FRACTION",
                        help="the load, as a fraction of the profile's ceiling of 3,500 orders a second (default: 0.3)")
    parser.add_argument("--cancel-ratio", type=float, default=0.5, metavar="FRACTION",
                        help="the fraction of orders that are cancelled (default: 0.5)")
    parser.add_argument("--expectations", type=Path, metavar="TOML",
                        help="the most calls each thread may make in the window; any thread over it fails the run")
    parser.add_argument("--stacks", type=int, default=5, metavar="N", help="call stacks shown for each thread (default: 5)")
    parser.add_argument("--reports", type=Path, metavar="DIR",
                        help="summarise the reports of an earlier run in DIR, and compare them with --expectations, without running anything")
    return parser.parse_args()


def clear_earlier_runs() -> None:
    """Clears the WALs, regions and records an earlier run left, and checks that nothing of the installation runs."""
    devenv = [sys.executable, str(_SCRIPT_DIR / "devenv.py"), "--fresh-wal", "--fresh-book", "--no-prometheus"]
    subprocess.run(devenv + ["start"], cwd=_PROJECT_ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    subprocess.run([sys.executable, str(_SCRIPT_DIR / "devenv.py"), "stop"], cwd=_PROJECT_ROOT, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    still_running = []
    for process in Path("/proc").iterdir():
        if process.name.isdigit():
            try:
                executable = os.readlink(process / "exe")
            except OSError:
                continue
            if "/installed/bin/" in executable:
                still_running.append(f"{process.name} {executable}")
    if still_running:
        sys.exit("error: these are still running after devenv.py stop:\n  " + "\n  ".join(still_running))


def write_profile(path: Path, seconds: int, rate: float, cancel_ratio: float) -> None:
    path.write_text(f"""# Written by heap_allocation_count.py: one steady phase, long enough for the settling time, the window and a margin.

[run]
ceiling_orders_per_second = 3500
compression               = 1
chart_step_seconds        = 30
minimum_samples           = 1

[[phase]]
name         = "steady, counting heap calls"
pattern      = "steady"
duration     = "{seconds}s"
compressible = false
rate         = {rate}
cancel_ratio = {cancel_ratio}
""", encoding="utf-8")


def wait_for_line(log_path: Path, pattern: str, process: subprocess.Popen, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.is_file() and re.search(pattern, log_path.read_text(encoding="utf-8", errors="replace"), re.M):
            return
        if process.poll() is not None:
            sys.exit(f"error: perf_run.py ended before the load began; see {log_path}")
        time.sleep(1)
    sys.exit(f"error: the load did not begin within {timeout:.0f} seconds; see {log_path}")


class Report:
    """One program's report: its threads' counts, their call stacks, and its executable mappings."""

    def __init__(self, path: Path):
        self.path = path
        self.program = ""
        self.window_seconds = 0.0
        self.threads: dict[int, dict] = {}
        self.stacks: dict[int, list[tuple[int, list[int]]]] = collections.defaultdict(list)
        self.mappings: list[tuple[int, int, int, str]] = []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            words = line.split()
            if not words:
                continue
            if words[0] == "program":
                self.program = words[1]
                self.window_seconds = float(words[5])
            elif words[0] == "thread":
                values = dict(zip(words[3::2], (int(value) for value in words[4::2])))
                self.threads[int(words[1])] = {"name": words[2], **values}
            elif words[0] == "stack":
                frames = [int(word, 16) for word in words[3:] if word != "(nil)"]
                self.stacks[int(words[1])].append((int(words[2]), frames))
            elif words[0] == "map":
                # map 55d0c-55d1f r-xp 00001000 08:02 123 /path/to/file
                low, high = (int(part, 16) for part in words[1].split("-"))
                if len(words) >= 7:
                    self.mappings.append((low, high, int(words[3], 16), words[6]))

    def locate(self, address: int) -> tuple[str, int] | None:
        for low, high, offset, path in self.mappings:
            if low <= address < high:
                return path, address - low + offset
        return None


def resolve(report: Report) -> dict[int, str]:
    """Turns every address in a report into 'function at file:line', one addr2line call for each file."""
    by_file: dict[str, list[tuple[int, int]]] = collections.defaultdict(list)
    for stacks in report.stacks.values():
        for _calls, frames in stacks:
            for address in frames:
                located = report.locate(address)
                if located is not None:
                    by_file[located[0]].append((address, located[1]))
    names: dict[int, str] = {}
    for path, pairs in by_file.items():
        unique = sorted(set(pairs))
        result = subprocess.run(["addr2line", "-f", "-C", "-e", path] + [hex(offset) for _address, offset in unique],
                                capture_output=True, text=True, check=False)
        lines = result.stdout.splitlines()
        for index, (address, _offset) in enumerate(unique):
            function = lines[2 * index] if 2 * index < len(lines) else "?"
            where = lines[2 * index + 1] if 2 * index + 1 < len(lines) else "?"
            names[address] = f"{function} at {Path(where).name if where.startswith('/') else where} [{Path(path).name}]"
    return names


def is_runtime(name: str) -> bool:
    return any(runtime in name for runtime in RUNTIME_FILES)


def summarise(reports: list[Report], stacks_shown: int) -> tuple[list[str], dict[tuple[str, str], int]]:
    lines: list[str] = []
    calls_by_thread: dict[tuple[str, str], int] = {}
    for report in sorted(reports, key=lambda report: report.program):
        names = resolve(report)
        lines.append(f"== {report.program} ({report.path.name}), window {report.window_seconds:.1f} s")
        for thread_id, counts in sorted(report.threads.items(), key=lambda item: -sum(item[1].get(kind, 0) for kind in CALL_KINDS)):
            calls = sum(counts.get(kind, 0) for kind in CALL_KINDS)
            key = (report.program, counts["name"])
            calls_by_thread[key] = calls_by_thread.get(key, 0) + calls
            kinds = ", ".join(f"{kind} {counts.get(kind, 0)}" for kind in CALL_KINDS + ["free"])
            lines.append(f"  thread {counts['name']} ({thread_id}): {calls} calls -- {kinds}, {counts.get('bytes', 0)} bytes")
            for stack_calls, frames in sorted(report.stacks.get(thread_id, []), key=lambda item: -item[0])[:stacks_shown]:
                named = [names.get(address, hex(address)) for address in frames]
                first_own = next((name for name in named if not is_runtime(name)), named[0] if named else "?")
                lines.append(f"    {stack_calls:>9} from {first_own}")
                for name in named:
                    lines.append(f"                {name}")
    return lines, calls_by_thread


def compare(calls_by_thread: dict[tuple[str, str], int], expectations_path: Path) -> list[str]:
    """Each [[thread]] of the file names a program, a thread name and the most calls it may make in the window."""
    expectations = tomllib.loads(expectations_path.read_text(encoding="utf-8")).get("thread", [])
    failures = []
    for expected in expectations:
        key = (expected["program"], expected["thread"])
        made = calls_by_thread.get(key, 0)
        if made > expected["most_calls"]:
            failures.append(f"{key[0]} thread {key[1]}: {made} calls, more than the {expected['most_calls']} allowed -- {expected.get('why', '')}")
    return failures


def finish(reports_dir: Path, args: argparse.Namespace, summary_path: Path) -> int:
    """Summarises the reports in reports_dir, compares them with the expectations if given, and writes the summary."""
    reports = [Report(path) for path in sorted(reports_dir.glob("*.txt"))]
    if not reports:
        sys.exit(f"error: no program wrote a report to {reports_dir}")
    lines, calls_by_thread = summarise(reports, args.stacks)
    missing = sorted(set(COUNTED_PROGRAMS) - {report.program for report in reports})
    if missing:
        lines.append(f"no report from: {', '.join(missing)} (not started, or made no heap call after the window closed and before it exited)")
    failures = compare(calls_by_thread, args.expectations) if args.expectations else []
    lines += [f"FAIL {failure}" for failure in failures]
    lines.append("RESULT: " + ("FAIL" if failures else "PASS" if args.expectations else "measured, nothing to compare with"))
    text = "\n".join(lines) + "\n"
    summary_path.write_text(text, encoding="utf-8")
    print(text)
    return 1 if failures else 0


def main() -> int:
    args = parse_arguments()
    if args.reports is not None:
        return finish(args.reports, args, args.reports.parent / "summary_again.txt")
    install = (_PROJECT_ROOT / args.install_prefix).resolve()
    library = next((path for path in (install / "lib" / "libheap_allocation_count.so", install / "lib64" / "libheap_allocation_count.so")
                    if path.is_file()), None)
    if library is None:
        sys.exit(f"error: libheap_allocation_count.so is not installed under {install}; build and deploy first")
    output = install / "heap_count" / datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    reports_dir = output / "reports"
    reports_dir.mkdir(parents=True)
    window_file = output / "window_open"
    profile = output / "profile.toml"
    write_profile(profile, int(args.settle + args.window + 30), args.rate, args.cancel_ratio)

    print(f"clearing earlier runs; output in {output}", flush=True)
    clear_earlier_runs()

    environment = dict(os.environ)
    environment.update({"LD_PRELOAD": str(library), "HEAP_COUNT_PROGRAMS": ",".join(COUNTED_PROGRAMS),
                        "HEAP_COUNT_WINDOW_FILE": str(window_file), "HEAP_COUNT_REPORT_DIR": str(reports_dir)})
    log_path = output / "perf_run.log"
    with log_path.open("w", encoding="utf-8") as log:
        with subprocess.Popen([sys.executable, str(_SCRIPT_DIR / "perf_run.py"), "--gateway", "binary", "--clients", "4", "--profile", str(profile),
                               str(install)], cwd=_PROJECT_ROOT, stdout=log, stderr=subprocess.STDOUT, env=environment) as run:
            wait_for_line(log_path, r"^\[[0-9:]+\] --- phase ", run, timeout=300)
            print(f"load running; the window opens in {args.settle:.0f} s", flush=True)
            time.sleep(args.settle)
            window_file.touch()
            print(f"window open for {args.window:.0f} s", flush=True)
            time.sleep(args.window)
            window_file.unlink()
            print("window closed; waiting for perf_run.py to stop the venue", flush=True)
            status = run.wait()
    if status != 0:
        print(f"warning: perf_run.py exited with status {status}; see {log_path}")

    return finish(reports_dir, args, output / "summary.txt")


if __name__ == "__main__":
    sys.exit(main())
