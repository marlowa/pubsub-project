#!/usr/bin/env python3
"""
pinning_report.py -- report which processes have pinned threads, and what looks wrong.

A thread may be restricted to particular processors by taskset, by a service manager, by a
container's cpuset, or by the program calling sched_setaffinity() on itself. Where that is done
with a hand-written mask per thread it is easy to get wrong, and wrong in ways that produce no
error and no log line: the process starts, runs, and is quietly slower than intended.

This walks every process the caller can see, prints which threads are restricted to which
processors, and then names the things that look like mistakes:

  * two threads of one process pinned to the same processor, so they share it
  * a thread left unrestricted while its fellows are pinned, which is a mask that was missed
  * a mask naming a processor the machine does not have online
  * a restricted thread still allowed on several processors, so it can migrate between them
  * threads pinned to two processors of the SAME physical core, which share execution resources
  * two different processes pinned to the same processor or the same physical core

None of these is read from a configuration file. They are read from the running processes, so
what is reported is what the machine is actually doing, whatever the configuration intended.

WHAT THIS CANNOT TELL YOU. An affinity mask says where a thread MAY run, not where it DID.
Nor does pinning reserve a processor FROM anything else: it reserves it FOR a thread, and
unrelated work is still scheduled there unless isolcpus keeps it away. Whether isolcpus is in
use is reported at the end.

The kernel's own threads are left out unless asked for. They are pinned one per processor by
construction, so on a large machine they would be several hundred correct findings drowning
whatever a reader came to look for.

Reading another user's processes needs privilege. Without it they are invisible, and the
summary says how many could not be read.

Usage:
  ./pinning_report.py                      every process with at least one restricted thread
  ./pinning_report.py --name postgres      only processes whose name or command matches
  ./pinning_report.py --pid 1234           only this process
  ./pinning_report.py --all                include processes with no restricted threads
  ./pinning_report.py --kernel             also include the kernel's own threads
  ./pinning_report.py --verbose            list every thread rather than grouping them
  ./pinning_report.py --json               machine-readable output
  ./pinning_report.py --quiet              only the findings, without the per-process listing
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

_PROC = Path("/proc")
_CPU_BASE = Path("/sys/devices/system/cpu")


def parse_cpu_list(text: str) -> list[int]:
    """Expand a kernel processor list such as "0-3,8,12-13" into [0,1,2,3,8,12,13]."""
    processors: list[int] = []
    for part in text.strip().split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            first, _, last = part.partition("-")
            processors.extend(range(int(first), int(last) + 1))
        else:
            processors.append(int(part))
    return sorted(set(processors))


def format_cpu_list(processors) -> str:
    """The inverse: [0,1,2,3,8] becomes "0-3,8"."""
    ordered = sorted(set(processors))
    if not ordered:
        return "(none)"
    pieces: list[str] = []
    start = previous = ordered[0]
    for processor in ordered[1:]:
        if processor == previous + 1:
            previous = processor
            continue
        pieces.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = processor
    pieces.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(pieces)


def online_processors() -> list[int]:
    """Every processor the kernel currently has online.

    This is what a thread is measured against: one allowed on all of them is not restricted,
    however its mask happens to be written.
    """
    try:
        return parse_cpu_list((_CPU_BASE / "online").read_text(encoding="utf-8"))
    except OSError:
        return list(range(os.cpu_count() or 1))


def isolated_processors() -> list[int]:
    """Processors the kernel was told to keep ordinary work off, from the boot command line."""
    try:
        command_line = (_PROC / "cmdline").read_text(encoding="utf-8")
    except OSError:
        return []
    for word in command_line.split():
        if word.startswith("isolcpus="):
            # isolcpus accepts flag words before the list, as in isolcpus=domain,managed_irq,2-15.
            ranges = [part for part in word.split("=", 1)[1].split(",") if part and part[0].isdigit()]
            try:
                return parse_cpu_list(",".join(ranges))
            except ValueError:
                return []
    return []


def sibling_map() -> dict[int, list[int]]:
    """Which processors share a physical core with each processor, including itself.

    Two processors of one core share that core's execution resources, so a thread on one of
    them competes with whatever is on the other. Nothing in the processor numbers says which
    pairs those are, and the pairing differs between machines.
    """
    siblings: dict[int, list[int]] = {}
    for processor in online_processors():
        path = _CPU_BASE / f"cpu{processor}" / "topology" / "thread_siblings_list"
        try:
            group = parse_cpu_list(path.read_text(encoding="utf-8"))
        except OSError:
            group = [processor]
        siblings[processor] = group or [processor]
    return siblings


def core_classes() -> dict[int, int]:
    """Each processor's maximum frequency, where the machine publishes one.

    On a machine whose processors are not all alike -- some fast, some efficient -- pinning a
    latency-critical thread to the wrong sort is a mistake that nothing else reveals. Where
    every processor reports the same maximum this says nothing and is not reported.
    """
    classes: dict[int, int] = {}
    for processor in online_processors():
        path = _CPU_BASE / f"cpu{processor}" / "cpufreq" / "cpuinfo_max_freq"
        try:
            classes[processor] = int(path.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            continue
    return classes


def read_first_line(path: Path) -> str:
    """One line from a /proc file, or an empty string if it has gone or cannot be read."""
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""


def process_command(pid: int) -> str:
    """The process's command line, or its kernel name in square brackets if it has none."""
    try:
        raw = (_PROC / str(pid) / "cmdline").read_bytes()
    except OSError:
        return ""
    if raw.strip(b"\0"):
        return " ".join(raw.decode("utf-8", "replace").split("\0")).strip()
    name = read_first_line(_PROC / str(pid) / "comm")
    return f"[{name}]" if name else ""


def is_kernel_thread(pid: int) -> bool:
    """Whether this is one of the kernel's own threads rather than a program someone started.

    A kernel thread has an empty command line, where a userspace process has its arguments.

    They are excluded by default, and the reason is that they would otherwise be the whole
    report. The kernel creates a set of per-processor threads on every processor -- the worker
    threads, the migration and hotplug threads, the software interrupt handlers -- and each is
    pinned to its own processor by construction. On a machine with 32 processors that is
    several hundred perfectly correct pinnings, and nobody reading this report put them there.
    """
    try:
        return not (_PROC / str(pid) / "cmdline").read_bytes().strip(b"\0")
    except OSError:
        return True


def thread_affinity(pid: int, thread_id: int) -> list[int] | None:
    """One thread's permitted processors, or None if it has gone or cannot be read."""
    try:
        status = (_PROC / str(pid) / "task" / str(thread_id) / "status").read_text(encoding="utf-8")
    except OSError:
        return None
    for line in status.splitlines():
        if line.startswith("Cpus_allowed_list:"):
            try:
                return parse_cpu_list(line.split(":", 1)[1])
            except ValueError:
                return None
    return None


def survey_process(pid: int, all_processors: set[int]) -> dict | None:
    """Gather one process's threads and their restrictions, or None if it cannot be read."""
    task_directory = _PROC / str(pid) / "task"
    try:
        thread_ids = sorted(int(entry.name) for entry in task_directory.iterdir())
    except (OSError, ValueError):
        return None

    threads: list[dict] = []
    for thread_id in thread_ids:
        allowed = thread_affinity(pid, thread_id)
        if allowed is None:
            continue
        threads.append({
            "thread_id": thread_id,
            "name": read_first_line(task_directory / str(thread_id) / "comm") or "?",
            "processors": allowed,
            "restricted": set(allowed) != all_processors,
        })

    if not threads:
        return None
    return {
        "pid": pid,
        "name": read_first_line(_PROC / str(pid) / "comm") or "?",
        "command": process_command(pid),
        "threads": threads,
        "restricted_threads": sum(1 for thread in threads if thread["restricted"]),
    }


def survey_all(wanted_pid, wanted_name, include_everything: bool, include_kernel: bool) -> dict:
    """Walk every visible process, keeping the ones asked for."""
    all_processors = set(online_processors())
    processes: list[dict] = []
    unreadable = 0
    threads_seen = 0

    if wanted_pid is not None:
        pids = [wanted_pid]
    else:
        pids = sorted(int(entry.name) for entry in _PROC.iterdir() if entry.name.isdigit())

    for pid in pids:
        if not include_kernel and wanted_pid is None and is_kernel_thread(pid):
            continue
        found = survey_process(pid, all_processors)
        if found is None:
            unreadable += 1
            continue
        threads_seen += len(found["threads"])
        if wanted_name is not None:
            haystack = f"{found['name']} {found['command']}".lower()
            if wanted_name.lower() not in haystack:
                continue
        if not include_everything and found["restricted_threads"] == 0:
            continue
        processes.append(found)

    return {"processes": processes, "unreadable": unreadable, "threads_seen": threads_seen}


def find_problems(processes: list[dict]) -> list[str]:
    """Everything about these processes' pinning that looks like a mistake.

    Each of these is something a hand-written mask per thread produces easily and silently.
    They are reported in the order a reader should care about them: what is certainly wrong
    first, what is merely suspicious after.
    """
    online = set(online_processors())
    siblings = sibling_map()
    classes = core_classes()
    fastest = max(classes.values()) if classes else 0

    # What counts as a materially slower processor.
    #
    # Not simply "below the fastest". A machine can publish several maximum frequencies that
    # all belong to the same sort of processor: one favoured core is often rated a few hundred
    # megahertz above its fellows, and flagging every ordinary core because of it would bury
    # the report in noise. A processor is called slower here only when its maximum is more
    # than a tenth below the highest, which separates a different CLASS of processor -- an
    # efficiency core against a performance core -- from that ordinary variation.
    slow_threshold = fastest * 0.9
    mixed_machine = bool(classes) and min(classes.values()) < slow_threshold

    certain: list[str] = []
    suspicious: list[str] = []
    claims: dict[int, list[str]] = {}

    for found in processes:
        label = f"{found['name']} (pid {found['pid']})"
        pinned = [thread for thread in found["threads"] if thread["restricted"]]
        if not pinned:
            continue

        # Two threads given the same single processor. They share it, so one of them is not
        # getting the core its mask was meant to buy.
        single: dict[int, list[dict]] = {}
        for thread in pinned:
            if len(thread["processors"]) == 1:
                single.setdefault(thread["processors"][0], []).append(thread)
        for processor, members in sorted(single.items()):
            if len(members) > 1:
                names = ", ".join(f"{thread['name']} (tid {thread['thread_id']})" for thread in members)
                certain.append(f"{label}: {len(members)} threads are pinned to processor {processor} and share it -- {names}")

        # A mask naming a processor this machine does not have online.
        for thread in pinned:
            missing = sorted(set(thread["processors"]) - online)
            if missing:
                certain.append(f"{label}: thread {thread['name']} (tid {thread['thread_id']}) names processor(s) "
                               f"{format_cpu_list(missing)}, which are not online on this machine")

        # A thread left unrestricted while its fellows are pinned: the mask that was missed.
        unpinned = [thread for thread in found["threads"] if not thread["restricted"]]
        if unpinned and pinned:
            names = ", ".join(f"{thread['name']} (tid {thread['thread_id']})" for thread in unpinned[:6])
            more = f", and {len(unpinned) - 6} more" if len(unpinned) > 6 else ""
            was_were = "is" if len(unpinned) == 1 else "are"
            suspicious.append(f"{label}: {len(pinned)} thread(s) are pinned but {len(unpinned)} {was_were} not -- {names}{more}")

        # Restricted, but to more than one processor, so it can still move between them.
        # Grouped by the set of processors and reported once for each, not once per thread: a
        # pool of workers sharing a range is one decision, and a process whose every thread is
        # on the same range is a tier rather than a mistake.
        loose: dict[str, list[dict]] = {}
        for thread in pinned:
            if len(thread["processors"]) > 1:
                loose.setdefault(format_cpu_list(thread["processors"]), []).append(thread)
        for processors, members in sorted(loose.items()):
            names = ", ".join(sorted({thread["name"] for thread in members})[:4])
            suspicious.append(f"{label}: {len(members)} thread(s) may run on {processors} rather than one processor "
                              f"-- {names}")

        # Threads of this process on two processors of the same physical core.
        for index, first in enumerate(pinned):
            for second in pinned[index + 1:]:
                if set(first["processors"]) == set(second["processors"]):
                    continue
                shared = {processor for processor in first["processors"] for sibling in siblings.get(processor, [])
                          if sibling in second["processors"] and sibling != processor}
                if shared:
                    suspicious.append(f"{label}: threads {first['name']} (tid {first['thread_id']}) and {second['name']} "
                                      f"(tid {second['thread_id']}) are on two processors of one physical core, and share it")

        # Threads on a slower sort of processor, where the machine has more than one sort.
        # Reported once for the process rather than once per thread: a process with sixteen
        # workers on the slow processors is one decision to look at, not sixteen.
        if mixed_machine:
            slow_threads = [thread for thread in pinned
                            if any(classes.get(processor, fastest) < slow_threshold for processor in thread["processors"])]
            if slow_threads:
                slow_processors = sorted({processor for thread in slow_threads for processor in thread["processors"]
                                          if classes.get(processor, fastest) < slow_threshold})
                suspicious.append(f"{label}: {len(slow_threads)} pinned thread(s) are on processor(s) "
                                  f"{format_cpu_list(slow_processors)}, which are a slower sort than this machine's fastest")

        # Only a thread pinned to exactly one processor is claiming it. A thread with a wider
        # mask is sharing a range on purpose -- a pool of background threads given the same
        # sixteen processors is a tier, not eighteen processes fighting over each of them --
        # and reporting those as clashes would bury every real one.
        for thread in pinned:
            if len(thread["processors"]) == 1:
                claims.setdefault(thread["processors"][0], []).append(label)

    # The same single processor claimed by threads of two different processes.
    for processor, owners in sorted(claims.items()):
        distinct = sorted(set(owners))
        if len(distinct) > 1:
            certain.append(f"processor {processor} is pinned by more than one process: {', '.join(distinct)}")

    return certain + suspicious


def group_threads_by_processors(threads: list[dict]) -> list[tuple[list[int], list[dict]]]:
    """Collect threads permitted on exactly the same processors, most threads first."""
    grouped: dict[str, list[dict]] = {}
    for thread in threads:
        grouped.setdefault(format_cpu_list(thread["processors"]), []).append(thread)
    ordered = sorted(grouped.items(), key=lambda item: (-len(item[1]), item[0]))
    return [(parse_cpu_list(key) if key != "(none)" else [], members) for key, members in ordered]


def report_process(found: dict, verbose: bool) -> None:
    """Print one process: what it is, how many threads it has, and where each may run."""
    print(f"\npid {found['pid']}  {found['name']}")
    if found["command"]:
        command = found["command"]
        print(f"  command : {command if len(command) <= 140 else command[:137] + '...'}")
    print(f"  threads : {len(found['threads'])} in total, {found['restricted_threads']} restricted to particular processors")

    for processors, members in group_threads_by_processors(found["threads"]):
        if not members[0]["restricted"]:
            print(f"  {len(members):>5} unrestricted")
            continue
        names = sorted({thread["name"] for thread in members})
        shown = ", ".join(names[:6]) + (f", and {len(names) - 6} more" if len(names) > 6 else "")
        word = "processor" if len(processors) == 1 else "processors"
        print(f"  {len(members):>5} on {word} {format_cpu_list(processors):<14} {shown}")
        if verbose:
            for thread in sorted(members, key=lambda item: item["thread_id"]):
                print(f"           tid {thread['thread_id']:<8} {thread['name']}")


def report_summary(survey: dict, problems: list[str]) -> None:
    """The context a reader needs before believing any of the above."""
    isolated = isolated_processors()
    print("\n" + "-" * 78)
    print(f"{len(survey['processes'])} process(es) reported, {survey['threads_seen']} thread(s) examined, {len(problems)} thing(s) worth looking at.")
    if survey["unreadable"]:
        print(f"{survey['unreadable']} process(es) could not be read, which usually means they belong to another user.")
    print(f"Processors online: {format_cpu_list(online_processors())}")
    if isolated:
        print(f"isolcpus covers processor(s) {format_cpu_list(isolated)}, so ordinary work is kept off those.")
    else:
        print("isolcpus is not in use, so nothing stops unrelated work being scheduled on a pinned processor.")
    print("A mask says where a thread MAY run, not where it did, and it reserves nothing from anything else.")


def parse_arguments() -> argparse.Namespace:
    """Command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--all", action="store_true", dest="include_everything",
                        help="report every process, not only those with restricted threads")
    parser.add_argument("--name", metavar="TEXT", help="only processes whose name or command line contains this text")
    parser.add_argument("--pid", type=int, metavar="PID", help="only this process")
    parser.add_argument("--kernel", action="store_true", dest="include_kernel",
                        help="also report the kernel's own threads, which are pinned per processor by construction "
                             "and are excluded by default because they would be the whole report")
    parser.add_argument("--verbose", action="store_true", help="list every thread individually rather than grouping them")
    parser.add_argument("--quiet", action="store_true", help="print only the findings, without the per-process listing")
    parser.add_argument("--json", action="store_true", dest="as_json", help="write the findings as JSON instead of text")
    return parser.parse_args()


def main() -> int:
    """Survey the machine and report. Exit status is 1 when something looks wrong, 0 otherwise."""
    arguments = parse_arguments()
    survey = survey_all(arguments.pid, arguments.name, arguments.include_everything, arguments.include_kernel)
    problems = find_problems(survey["processes"])

    if arguments.as_json:
        print(json.dumps({
            "processes": survey["processes"],
            "problems": problems,
            "online_processors": online_processors(),
            "isolated_processors": isolated_processors(),
            "unreadable_processes": survey["unreadable"],
        }, indent=2, default=list))
        return 1 if problems else 0

    if not survey["processes"]:
        print("No process was found with any thread restricted to particular processors.")
        report_summary(survey, problems)
        return 0

    if not arguments.quiet:
        for found in survey["processes"]:
            report_process(found, arguments.verbose)

    if problems:
        print("\nWorth looking at:")
        for problem in problems:
            print(f"  - {problem}")

    report_summary(survey, problems)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
