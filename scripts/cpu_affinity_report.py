#!/usr/bin/env python3
"""Report which CPUs each process thread may run on, and how busy each CPU is.

One of two outputs must be chosen, or both:

--graphic           opens a window that shows the CPUs and their threads, refreshed every sample interval. It needs a
                    graphical display and the Python Tk package.
--summary FILENAME  writes the same information as plain text to FILENAME, once a full scan of the threads and one
                    measurement of every CPU's usage have been made. On its own it needs no graphical display and does
                    not load Tk, so it can be run on a machine with no graphics and no X11 forwarding.

With both, the window opens and the summary is written from the first complete scan the window makes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import math
import os
from pathlib import Path
import queue
import sys
import threading
import time


_PROC = Path("/proc")
_CPU_BASE = Path("/sys/devices/system/cpu")
_COLOURS = {
    "background": "#111827",
    "panel": "#1f2937",
    "panel_highlight": "#374151",
    "text": "#f8fafc",
    "muted": "#cbd5e1",
    "green": "#4ade80",
    "yellow": "#facc15",
    "red": "#fb7185",
    "cyan": "#38bdf8",
    "p_core": "#60a5fa",
    "e_core": "#c084fc",
    "bar": "#22d3ee",
    "bar_track": "#374151",
}
_CATEGORY_COLOURS = {
    "single": _COLOURS["green"],
    "few": _COLOURS["yellow"],
    "many": _COLOURS["red"],
}
_REFRESH_COLUMNS = 4
_CARD_MINIMUM_WIDTH = 360


@dataclass(frozen=True)
class AffinityThread:
    """One visible process thread and the CPUs where Linux allows it to run."""

    process_name: str
    thread_name: str
    process_id: int
    thread_id: int
    allowed_processors: tuple[int, ...]
    category: str


@dataclass
class CpuCard:
    """Affinity and usage information for one online CPU."""

    processor: int
    usage: float | None
    core_type: str
    scheduler_isolated: bool
    managed_irq_isolated: bool
    nohz_full: bool
    isolation_known: bool
    threads: list[AffinityThread] = field(default_factory=list)
    all_online_thread_count: int = 0
    all_online_process_count: int = 0

    @property
    def single_cpu_thread_count(self) -> int:
        """Number of reported threads whose affinity contains only this CPU."""
        return sum(1 for thread in self.threads if len(thread.allowed_processors) == 1)

    @property
    def multi_cpu_thread_count(self) -> int:
        """Number of reported threads whose affinity contains several CPUs."""
        return len(self.threads) - self.single_cpu_thread_count


@dataclass(frozen=True)
class SystemSnapshot:
    """One set of CPU, process-affinity and usage measurements."""

    cards: list[CpuCard]
    process_count: int
    thread_count: int
    unreadable_process_count: int
    unreadable_thread_count: int
    affinity_scan_complete: bool
    sample_seconds: float
    updated_at: str


def parse_cpu_list(text: str) -> list[int]:
    """Expand a CPU-list string such as "0-3,8" into [0, 1, 2, 3, 8]."""
    processors: list[int] = []
    for part in text.strip().split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            first, _, last = part.partition("-")
            if not first.isdigit() or not last.isdigit():
                raise ValueError(f"invalid CPU range: {part}")
            start = int(first)
            end = int(last)
            if start > end:
                raise ValueError(f"invalid CPU range: {part}")
            processors.extend(range(start, end + 1))
        else:
            if not part.isdigit():
                raise ValueError(f"invalid CPU number: {part}")
            processors.append(int(part))
    return sorted(set(processors))


def format_cpu_list(processors: list[int] | set[int] | tuple[int, ...]) -> str:
    """Compress processor ids into a kernel CPU-list string."""
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


def _format_thread_rows(
    threads: list[AffinityThread],
) -> tuple[str, list[tuple[str, int, int]]]:
    """Build thread text and contiguous colour ranges for one CPU card."""
    lines: list[str] = []
    colour_ranges: list[tuple[str, int, int]] = []
    current_category = None
    category_start_line = 1
    line_number = 1

    for thread in threads:
        if thread.category != current_category:
            if current_category is not None:
                colour_ranges.append((current_category, category_start_line, line_number))
            current_category = thread.category
            category_start_line = line_number
        lines.extend((
            f"{thread.process_name} / {thread.thread_name}",
            f"PID {thread.process_id} | TID {thread.thread_id} | "
            f"CPUs {format_cpu_list(thread.allowed_processors)}",
        ))
        line_number += 2

    if current_category is not None:
        colour_ranges.append((current_category, category_start_line, line_number))
    text = "\n".join(lines)
    if text:
        text += "\n"
    return text, colour_ranges


def _cpu_isolation_summary(card: CpuCard) -> str:
    tags: list[str] = []
    if card.scheduler_isolated:
        tags.append("SCHEDULER-ISOLATED (isolcpus)")
    if card.managed_irq_isolated:
        tags.append("MANAGED-IRQ ISOLATION")
    if card.nohz_full:
        tags.append("NOHZ_FULL")
    if tags:
        return " | ".join(tags)
    if card.isolation_known:
        return "No isolation flags reported for this CPU"
    return "Kernel isolation settings unavailable"


def _cpu_thread_summary(card: CpuCard) -> str:
    return (
        f"Single-CPU threads: {card.single_cpu_thread_count}  |  "
        f"Multi-CPU threads: {card.multi_cpu_thread_count}  |  "
        f"All-CPU threads: {card.all_online_thread_count}"
    )


def _cpu_availability_summary(card: CpuCard) -> str:
    flexible_count = card.multi_cpu_thread_count + card.all_online_thread_count
    if card.single_cpu_thread_count == 0 and flexible_count > 0:
        return "OPEN TO THREADS ALLOWED ON MULTIPLE CPUs"
    if card.single_cpu_thread_count == 0:
        return "No reported process threads are allowed here"
    return ""


def _cpu_all_online_summary(card: CpuCard) -> str:
    if card.all_online_thread_count == 0:
        return ""
    return (
        f"{card.all_online_thread_count} thread(s) in {card.all_online_process_count} "
        "process(es) are allowed on every online CPU"
    )


def format_system_summary(snapshot: SystemSnapshot) -> str:
    """Format the CPU cards and affinity details as a plain-text report."""
    lines = [
        "CPU affinity summary",
        f"Updated: {snapshot.updated_at}",
        (
            f"{len(snapshot.cards)} online CPUs | {snapshot.process_count} processes | "
            f"{snapshot.thread_count} threads | "
            f"unavailable processes: {snapshot.unreadable_process_count} | "
            f"unavailable threads: {snapshot.unreadable_thread_count}"
        ),
    ]
    if snapshot.sample_seconds > 0:
        lines.append(f"CPU usage sample: {snapshot.sample_seconds:.2f} seconds")
    else:
        lines.append("CPU usage sample: not available yet")
    lines.append("")

    for card in snapshot.cards:
        core_type_label = "" if card.core_type == "Unknown" else card.core_type.upper()
        heading = f"CPU {card.processor}"
        if core_type_label:
            heading += f" ({core_type_label})"
        usage_text = "sampling..." if card.usage is None else f"{card.usage:.1f}%"
        lines.extend((
            heading,
            f"CPU usage: {usage_text}",
            f"Isolation: {_cpu_isolation_summary(card)}",
            _cpu_thread_summary(card),
        ))
        availability = _cpu_availability_summary(card)
        if availability:
            lines.append(availability)
        all_online = _cpu_all_online_summary(card)
        if all_online:
            lines.append(all_online)
        lines.append("Threads with restricted CPU sets:")
        thread_text, _ = _format_thread_rows(card.threads)
        if thread_text:
            lines.extend(f"  {line}" for line in thread_text.rstrip().splitlines())
        else:
            lines.append("  none")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def _write_summary_when_ready(path: Path, snapshot: SystemSnapshot) -> bool:
    """Write a summary only after every online CPU has a usage measurement."""
    if (
        not snapshot.affinity_scan_complete
        or snapshot.sample_seconds <= 0
        or not snapshot.cards
        or any(card.usage is None for card in snapshot.cards)
    ):
        return False
    path.write_text(format_system_summary(snapshot), encoding="utf-8")
    return True


def online_processors() -> list[int]:
    """Read the online CPU set, with a fallback for systems without sysfs."""
    try:
        return parse_cpu_list((_CPU_BASE / "online").read_text(encoding="utf-8"))
    except OSError:
        return list(range(os.cpu_count() or 1))


def classify_cpu_core_types(
    processor_ids: list[int],
    cpu_capacities: dict[int, int],
    highest_performance: dict[int, int],
) -> dict[int, str]:
    """Classify processors by Linux capacity, with ACPI CPPC as a fallback."""
    maximum_performance = max(highest_performance.values(), default=0)
    core_types: dict[int, str] = {}
    for processor in processor_ids:
        if processor in cpu_capacities:
            core_types[processor] = "P-core" if cpu_capacities[processor] >= 1024 else "E-core"
        elif maximum_performance > 0 and processor in highest_performance:
            is_performance_core = (
                highest_performance[processor] * 100 >= maximum_performance * 80
            )
            core_types[processor] = "P-core" if is_performance_core else "E-core"
        else:
            core_types[processor] = "Unknown"
    return core_types


def _read_integer(path: Path) -> int | None:
    """Read an integer sysfs value, or return None when the kernel does not publish it."""
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None


def read_cpu_core_types(processor_ids: list[int]) -> dict[int, str]:
    """Read P/E-core information from Linux CPU capacity or ACPI CPPC data."""
    cpu_capacities: dict[int, int] = {}
    highest_performance: dict[int, int] = {}
    for processor in processor_ids:
        cpu_directory = _CPU_BASE / f"cpu{processor}"
        capacity = _read_integer(cpu_directory / "cpu_capacity")
        performance = _read_integer(cpu_directory / "acpi_cppc" / "highest_perf")
        if capacity is not None:
            cpu_capacities[processor] = capacity
        if performance is not None:
            highest_performance[processor] = performance
    return classify_cpu_core_types(processor_ids, cpu_capacities, highest_performance)


def parse_isolation_settings(command_line: str) -> dict[str, set[int]]:
    """Return scheduler, managed-IRQ and nohz_full CPU sets from a kernel command line."""
    settings = {
        "scheduler": set(),
        "managed_irq": set(),
        "isolcpus_nohz": set(),
        "nohz_full": set(),
    }

    for word in command_line.split():
        if word.startswith("isolcpus="):
            parts = word.split("=", 1)[1].split(",")
            flags = {part for part in parts if part and not part[0].isdigit()}
            cpu_parts = [part for part in parts if part and part[0].isdigit()]
            if not cpu_parts:
                continue
            processors = set(parse_cpu_list(",".join(cpu_parts)))
            if not flags or "domain" in flags:
                settings["scheduler"].update(processors)
            if "managed_irq" in flags:
                settings["managed_irq"].update(processors)
            if "nohz" in flags:
                settings["isolcpus_nohz"].update(processors)
        elif word.startswith("nohz_full="):
            cpu_list = word.split("=", 1)[1]
            settings["nohz_full"].update(parse_cpu_list(cpu_list))

    return settings


def read_isolation_settings() -> dict[str, set[int]] | None:
    """Read CPU isolation settings from the running kernel command line."""
    try:
        command_line = (_PROC / "cmdline").read_text(encoding="utf-8")
    except OSError:
        return None
    return parse_isolation_settings(command_line)


def read_cpu_times() -> dict[int, tuple[int, int]]:
    """Read busy and total CPU ticks for each processor from /proc/stat."""
    result: dict[int, tuple[int, int]] = {}
    for line in (_PROC / "stat").read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or not fields[0].startswith("cpu") or not fields[0][3:].isdigit():
            continue
        if len(fields) < 9:
            continue
        counters = [int(value) for value in fields[1:9]]
        total_ticks = sum(counters)
        idle_ticks = counters[3] + counters[4]
        result[int(fields[0][3:])] = (total_ticks - idle_ticks, total_ticks)
    return result


def calculate_cpu_usage(
    before: dict[int, tuple[int, int]], after: dict[int, tuple[int, int]]
) -> dict[int, float | None]:
    """Calculate busy time as a percentage of total time between two readings."""
    usage: dict[int, float | None] = {}
    for processor in before.keys() & after.keys():
        busy_delta = after[processor][0] - before[processor][0]
        total_delta = after[processor][1] - before[processor][1]
        if total_delta <= 0 or busy_delta < 0:
            usage[processor] = None
        else:
            usage[processor] = min(100.0, max(0.0, busy_delta * 100.0 / total_delta))
    return usage


def _read_first_line(path: Path) -> str:
    """Read one procfs line, replacing terminal control characters."""
    try:
        value = path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""
    return "".join(character if character.isprintable() else "?" for character in value)


def _parse_thread_status(status: str) -> tuple[str, list[int]] | None:
    """Read a thread name and allowed CPUs from one procfs status record."""
    thread_name = "?"
    allowed_processors = None
    for line in status.splitlines():
        if line.startswith("Name:"):
            thread_name = line.split(":", 1)[1].strip() or "?"
        elif line.startswith("Cpus_allowed_list:"):
            try:
                allowed_processors = parse_cpu_list(line.split(":", 1)[1])
            except ValueError:
                return None
        if allowed_processors is not None:
            return thread_name, allowed_processors
    return None


def _read_thread_details(pid: int, thread_id: int) -> tuple[str, list[int]] | None:
    """Read a thread's name and allowed CPUs from one procfs file."""
    path = _PROC / str(pid) / "task" / str(thread_id) / "status"
    try:
        return _parse_thread_status(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError):
        return None


def survey_processes(
    wanted_pid: int | None, wanted_name: str | None, include_kernel: bool
) -> dict:
    """Read visible processes and their threads from procfs."""
    if wanted_pid is None:
        process_ids = sorted(
            int(entry.name) for entry in _PROC.iterdir() if entry.name.isdigit()
        )
    else:
        process_ids = [wanted_pid]

    processes: list[dict] = []
    unreadable_processes = 0
    unreadable_threads = 0

    for pid in process_ids:
        process_directory = _PROC / str(pid)
        try:
            command_line = (process_directory / "cmdline").read_bytes()
        except OSError:
            unreadable_processes += 1
            continue

        is_kernel_thread = not command_line.strip(b"\0")
        if is_kernel_thread and not include_kernel:
            continue

        process_name = _read_first_line(process_directory / "comm") or "?"
        command = " ".join(
            command_line.decode("utf-8", "replace").split("\0")
        ).strip()
        if wanted_name is not None:
            if wanted_name.lower() not in f"{process_name} {command}".lower():
                continue

        task_directory = process_directory / "task"
        try:
            thread_ids = sorted(int(entry.name) for entry in task_directory.iterdir())
        except (OSError, ValueError):
            unreadable_processes += 1
            continue

        threads: list[dict] = []
        for thread_id in thread_ids:
            thread_details = _read_thread_details(pid, thread_id)
            if thread_details is None:
                unreadable_threads += 1
                continue
            thread_name, processors = thread_details
            threads.append({
                "thread_id": thread_id,
                "name": thread_name,
                "processors": processors,
            })

        if not threads:
            unreadable_processes += 1
            continue
        processes.append({"pid": pid, "name": process_name, "threads": threads})

    return {
        "processes": processes,
        "unreadable_processes": unreadable_processes,
        "unreadable_threads": unreadable_threads,
    }


def affinity_category(processors: list[int], online_cpu_count: int) -> str:
    """Classify a thread by how many online CPUs its affinity includes."""
    if len(processors) == 1:
        return "single"
    few_cpu_limit = (online_cpu_count + 1) // 2
    return "few" if len(processors) <= few_cpu_limit else "many"


def build_cpu_cards(
    processes: list[dict],
    online_cpu_ids: list[int],
    usage_by_cpu: dict[int, float | None],
    isolation_settings: dict[str, set[int]] | None,
    core_type_by_cpu: dict[int, str] | None = None,
) -> list[CpuCard]:
    """Build one display model for each online CPU."""
    online_set = set(online_cpu_ids)
    cards = {
        processor: CpuCard(
            processor=processor,
            usage=usage_by_cpu.get(processor),
            core_type=(core_type_by_cpu or {}).get(processor, "Unknown"),
            scheduler_isolated=(
                isolation_settings is not None
                and processor in isolation_settings["scheduler"]
            ),
            managed_irq_isolated=(
                isolation_settings is not None
                and processor in isolation_settings["managed_irq"]
            ),
            nohz_full=(
                isolation_settings is not None
                and (
                    processor in isolation_settings["isolcpus_nohz"]
                    or processor in isolation_settings["nohz_full"]
                )
            ),
            isolation_known=isolation_settings is not None,
        )
        for processor in online_cpu_ids
    }
    all_online_processes: dict[int, set[int]] = {
        processor: set() for processor in online_cpu_ids
    }

    for process in processes:
        for thread in process["threads"]:
            allowed = tuple(sorted(set(thread["processors"])))
            if not allowed:
                continue
            allowed_set = set(allowed)
            if len(online_cpu_ids) > 1 and allowed_set == online_set:
                for processor in online_cpu_ids:
                    cards[processor].all_online_thread_count += 1
                    all_online_processes[processor].add(process["pid"])
                continue

            category = affinity_category(list(allowed), len(online_cpu_ids))
            entry = AffinityThread(
                process_name=process["name"],
                thread_name=thread["name"],
                process_id=process["pid"],
                thread_id=thread["thread_id"],
                allowed_processors=allowed,
                category=category,
            )
            for processor in allowed_set & online_set:
                cards[processor].threads.append(entry)

    for processor, card in cards.items():
        card.all_online_process_count = len(all_online_processes[processor])
        card.threads.sort(
            key=lambda thread: (
                ("single", "few", "many").index(thread.category),
                thread.process_name.lower(),
                thread.thread_name.lower(),
                thread.process_id,
                thread.thread_id,
            )
        )
    return [cards[processor] for processor in online_cpu_ids]


# How many sample intervals the summary waits for a measurement of every CPU before it gives up.
_SUMMARY_MAXIMUM_SAMPLES = 10


def make_snapshot(
    usage_by_cpu: dict[int, float | None],
    sample_seconds: float,
    survey: dict | None = None,
) -> SystemSnapshot:
    """Read the online CPUs, their isolation and core types, and assemble the card for each with the threads from
    survey. With no survey, the cards have no threads and the snapshot says the affinity scan is not complete."""
    processors = online_processors()
    isolation = read_isolation_settings()
    core_types = read_cpu_core_types(processors)
    affinity_scan_complete = survey is not None
    if survey is None:
        survey = {
            "processes": [],
            "unreadable_processes": 0,
            "unreadable_threads": 0,
        }
    cards = build_cpu_cards(
        survey["processes"],
        processors,
        usage_by_cpu,
        isolation,
        core_types,
    )
    return SystemSnapshot(
        cards=cards,
        process_count=len(survey["processes"]),
        thread_count=sum(len(process["threads"]) for process in survey["processes"]),
        unreadable_process_count=survey["unreadable_processes"],
        unreadable_thread_count=survey["unreadable_threads"],
        affinity_scan_complete=affinity_scan_complete,
        sample_seconds=sample_seconds,
        updated_at=time.strftime("%H:%M:%S"),
    )


def write_summary_report(arguments: argparse.Namespace) -> int:
    """Write the text summary to arguments.summary with no window, and return the exit status.

    CPU usage is the share of time each CPU was busy between two readings of /proc/stat, so the summary waits one sample
    interval after the first reading. If a CPU still has no measurement, it waits another interval, up to
    _SUMMARY_MAXIMUM_SAMPLES intervals, and then gives up rather than write a summary with a CPU missing."""
    try:
        previous = read_cpu_times()
    except OSError as error:
        print(f"error: cannot read /proc/stat: {error}", file=sys.stderr)
        return 1
    previous_time = time.monotonic()
    for _ in range(_SUMMARY_MAXIMUM_SAMPLES):
        time.sleep(arguments.sample_seconds)
        try:
            current = read_cpu_times()
            current_time = time.monotonic()
            usage = calculate_cpu_usage(previous, current)
            survey = survey_processes(arguments.pid, arguments.name, arguments.kernel)
            snapshot = make_snapshot(usage, current_time - previous_time, survey)
        except OSError as error:
            print(f"error: the scan of the system failed: {error}", file=sys.stderr)
            return 1
        try:
            if _write_summary_when_ready(arguments.summary, snapshot):
                print(f"Summary written to {arguments.summary}")
                return 0
        except OSError as error:
            print(f"error: cannot write the summary file {arguments.summary}: {error}", file=sys.stderr)
            return 1
        previous = current
        previous_time = current_time
    print(
        f"error: no usage measurement for every online CPU after {_SUMMARY_MAXIMUM_SAMPLES} samples of "
        f"{arguments.sample_seconds} seconds, so no summary was written",
        file=sys.stderr,
    )
    return 1


def _positive_float(value: str) -> float:
    """Parse a finite positive floating-point command-line value."""
    try:
        seconds = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a number greater than zero") from error
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return seconds


def parse_arguments() -> argparse.Namespace:
    """Parse command-line options. At least one of --graphic and --summary must be given."""
    parser = argparse.ArgumentParser(
        description=(
            "Report which CPUs each process thread may run on, and how busy each CPU is. "
            "Choose --graphic for a window, --summary FILENAME for a text report, or both."
        )
    )
    parser.add_argument(
        "--graphic",
        action="store_true",
        help="open a window showing the CPUs and their threads, refreshed every sample interval (needs a graphical display)",
    )
    parser.add_argument("--pid", type=int, help="show only this process")
    parser.add_argument("--name", help="show processes whose name or command contains this text")
    parser.add_argument("--kernel", action="store_true", help="include kernel threads")
    parser.add_argument(
        "--summary",
        type=Path,
        metavar="FILENAME",
        help=(
            "write a text summary to FILENAME after the first complete affinity scan and CPU usage measurement; "
            "without --graphic, no window is opened and no graphical display is needed"
        ),
    )
    parser.add_argument(
        "--sample-seconds",
        type=_positive_float,
        default=1.0,
        help="CPU usage sampling and refresh interval in seconds (default: 1.0)",
    )
    arguments = parser.parse_args()
    if not arguments.graphic and arguments.summary is None:
        parser.error("choose what to produce: --graphic for a window, --summary FILENAME for a text report, or both")
    return arguments


class CpuAffinityWindow:
    """Display periodic live system snapshots in a scrollable CPU grid."""

    def __init__(self, tk_module, arguments: argparse.Namespace):
        self.tk = tk_module
        self.arguments = arguments
        self.root = self.tk.Tk()
        self.root.title("CPU affinity and usage")
        self.root.geometry("1280x850")
        self.root.minsize(800, 600)
        self.root.configure(bg=_COLOURS["background"])

        self.messages: queue.Queue = queue.Queue()
        self.stop_event = threading.Event()
        self.card_widgets: dict[int, dict] = {}
        self.cpu_ids: tuple[int, ...] = ()
        self._create_widgets()
        self._render_snapshot(self._make_snapshot({}, 0.0))

        self.root.bind_all("<Up>", self._scroll_up)
        self.root.bind_all("<Down>", self._scroll_down)
        self.root.bind_all("<Prior>", self._scroll_page_up)
        self.root.bind_all("<Next>", self._scroll_page_down)
        self.root.bind_all("<Home>", self._scroll_home)
        self.root.bind_all("<MouseWheel>", self._on_mouse_wheel)
        self.root.bind_all("<Button-4>", self._on_mouse_wheel_button)
        self.root.bind_all("<Button-5>", self._on_mouse_wheel_button)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.after(150, self._poll_messages)
        self.worker = threading.Thread(target=self._collect_snapshots, daemon=True)
        self.worker.start()

    def _create_widgets(self) -> None:
        """Create the window header, scrollable grid and explanatory footer."""
        header = self.tk.Frame(self.root, bg=_COLOURS["background"], padx=18, pady=12)
        header.pack(fill="x")

        title = self.tk.Label(
            header,
            text="CPU affinity and usage",
            font=("TkDefaultFont", 20, "bold"),
            fg=_COLOURS["text"],
            bg=_COLOURS["background"],
        )
        title.pack(anchor="w")

        legend = self.tk.Frame(header, bg=_COLOURS["background"], pady=8)
        legend.pack(anchor="w")
        self._add_legend_item(legend, "one CPU", _COLOURS["green"])
        self._add_legend_item(legend, "a few CPUs", _COLOURS["yellow"])
        self._add_legend_item(legend, "many CPUs", _COLOURS["red"])
        self._add_legend_item(legend, "P-core", _COLOURS["p_core"])
        self._add_legend_item(legend, "E-core", _COLOURS["e_core"])
        self.summary_label = self.tk.Label(
            header,
            text="Collecting live process affinity and CPU counters...",
            anchor="w",
            fg=_COLOURS["muted"],
            bg=_COLOURS["background"],
        )
        self.summary_label.pack(fill="x")

        scroll_area = self.tk.Frame(self.root, bg=_COLOURS["background"])
        scroll_area.pack(fill="both", expand=True)
        self.canvas = self.tk.Canvas(
            scroll_area,
            bg=_COLOURS["background"],
            highlightthickness=0,
            xscrollincrement=24,
            yscrollincrement=24,
        )
        self.horizontal_scrollbar = self.tk.Scrollbar(
            scroll_area,
            orient="horizontal",
            command=self.canvas.xview,
            bg=_COLOURS["panel_highlight"],
            troughcolor=_COLOURS["background"],
            highlightthickness=0,
        )
        self.vertical_scrollbar = self.tk.Scrollbar(
            scroll_area,
            orient="vertical",
            command=self.canvas.yview,
            bg=_COLOURS["panel_highlight"],
            troughcolor=_COLOURS["background"],
            highlightthickness=0,
        )
        self.canvas.configure(
            xscrollcommand=self.horizontal_scrollbar.set,
            yscrollcommand=self.vertical_scrollbar.set,
        )
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.vertical_scrollbar.grid(row=0, column=1, sticky="ns")
        self.horizontal_scrollbar.grid(row=1, column=0, sticky="ew")
        scroll_area.grid_rowconfigure(0, weight=1)
        scroll_area.grid_columnconfigure(0, weight=1)

        self.grid_frame = self.tk.Frame(self.canvas, bg=_COLOURS["background"])
        self.canvas_window = self.canvas.create_window(
            (0, 0), window=self.grid_frame, anchor="nw"
        )
        self.grid_frame.bind("<Configure>", self._update_scroll_region)
        self.canvas.bind("<Configure>", self._resize_grid_window)
        for column in range(_REFRESH_COLUMNS):
            self.grid_frame.grid_columnconfigure(
                column, minsize=_CARD_MINIMUM_WIDTH, weight=1
            )

        footer = self.tk.Frame(self.root, bg=_COLOURS["background"], padx=18, pady=8)
        footer.pack(fill="x")
        self.status_label = self.tk.Label(
            footer,
            text="Waiting for the first system scan...",
            anchor="w",
            fg=_COLOURS["cyan"],
            bg=_COLOURS["background"],
        )
        self.status_label.pack(fill="x")
        explanation = (
            "Affinity is where a thread may run; CPU usage is the recent measured load. "
            "Threads allowed on every CPU are counted in each card, not repeated in a long list. "
            "isolcpus means scheduler isolation, not kernel-only. Colour ranges are relative to "
            "the number of online CPUs. P/E labels use CPU capacity or ACPI CPPC data."
        )
        if not self.arguments.kernel:
            explanation += " Kernel threads are omitted; pass --kernel to include them."
        self.tk.Label(
            footer,
            text=explanation,
            anchor="w",
            justify="left",
            wraplength=1200,
            fg=_COLOURS["muted"],
            bg=_COLOURS["background"],
        ).pack(fill="x", pady=(5, 0))

    def _add_legend_item(self, parent, text: str, colour: str) -> None:
        """Add a coloured key to the affinity legend."""
        swatch = self.tk.Frame(parent, width=12, height=12, bg=colour)
        swatch.pack(side="left", padx=(0, 6))
        swatch.pack_propagate(False)
        self.tk.Label(
            parent,
            text=text,
            fg=_COLOURS["muted"],
            bg=_COLOURS["background"],
        ).pack(side="left", padx=(0, 18))

    def _resize_grid_window(self, event) -> None:
        """Keep the grid at least as wide as its CPU columns and the viewport."""
        requested_width = self.grid_frame.winfo_reqwidth()
        self.canvas.itemconfigure(
            self.canvas_window,
            width=max(requested_width, event.width),
        )

    def _update_scroll_region(self, _event) -> None:
        """Keep both scrollbars matched to the grid dimensions."""
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _scroll_up(self, _event):
        self.canvas.yview_scroll(-1, "units")
        return "break"

    def _scroll_down(self, _event):
        self.canvas.yview_scroll(1, "units")
        return "break"

    def _scroll_page_up(self, _event):
        self.canvas.yview_scroll(-1, "pages")
        return "break"

    def _scroll_page_down(self, _event):
        self.canvas.yview_scroll(1, "pages")
        return "break"

    def _scroll_home(self, _event):
        self.canvas.yview_moveto(0.0)
        return "break"

    def _on_mouse_wheel(self, event):
        if event.delta == 0:
            return "break"
        direction = -1 if event.delta > 0 else 1
        units = max(1, abs(event.delta) // 120)
        return self._scroll_at_pointer(event.widget, direction * units)

    def _on_mouse_wheel_button(self, event):
        direction = -1 if event.num == 4 else 1
        return self._scroll_at_pointer(event.widget, direction)

    def _scroll_at_pointer(self, widget, direction: int):
        for card_widgets in self.card_widgets.values():
            ancestor = widget
            while ancestor is not None:
                if ancestor == card_widgets["thread_list_frame"]:
                    card_widgets["thread_text"].yview_scroll(direction, "units")
                    return "break"
                ancestor = getattr(ancestor, "master", None)
        self.canvas.yview_scroll(direction, "units")
        return "break"

    def _create_cpu_card(self, card: CpuCard, index: int) -> dict:
        """Create a CPU panel and return the widgets updated by later scans."""
        row = index // _REFRESH_COLUMNS
        column = index % _REFRESH_COLUMNS
        frame = self.tk.Frame(
            self.grid_frame,
            bg=_COLOURS["panel"],
            padx=12,
            pady=10,
            highlightbackground=_COLOURS["panel_highlight"],
            highlightthickness=1,
        )
        frame.grid(row=row, column=column, padx=8, pady=8, sticky="nsew")

        heading = self.tk.Frame(frame, bg=_COLOURS["panel"])
        heading.pack(fill="x")
        cpu_label = self.tk.Label(
            heading,
            text=f"CPU {card.processor}",
            font=("TkDefaultFont", 15, "bold"),
            fg=_COLOURS["text"],
            bg=_COLOURS["panel"],
        )
        cpu_label.pack(side="left")
        core_type_label = self.tk.Label(
            heading,
            text=self._core_type_label(card.core_type),
            font=("TkDefaultFont", 9, "bold"),
            fg=self._core_type_colour(card.core_type),
            bg=_COLOURS["panel"],
        )
        core_type_label.pack(side="left", padx=(8, 0))
        usage_label = self.tk.Label(
            heading,
            text="sampling...",
            font=("TkDefaultFont", 11, "bold"),
            fg=_COLOURS["text"],
            bg=_COLOURS["panel"],
        )
        usage_label.pack(side="right")

        usage_bar = self.tk.Canvas(
            frame,
            height=10,
            bg=_COLOURS["bar_track"],
            highlightthickness=0,
        )
        usage_bar.pack(fill="x", pady=(8, 10))
        isolation_label = self.tk.Label(
            frame,
            anchor="w",
            justify="left",
            wraplength=320,
            fg=_COLOURS["cyan"],
            bg=_COLOURS["panel"],
        )
        isolation_label.pack(fill="x")
        summary_label = self.tk.Label(
            frame,
            anchor="w",
            justify="left",
            wraplength=320,
            fg=_COLOURS["muted"],
            bg=_COLOURS["panel"],
        )
        summary_label.pack(fill="x", pady=(5, 0))
        availability_label = self.tk.Label(
            frame,
            anchor="w",
            justify="left",
            wraplength=320,
            font=("TkDefaultFont", 9, "bold"),
            fg=_COLOURS["cyan"],
            bg=_COLOURS["panel"],
        )
        availability_label.pack(fill="x", pady=(4, 0))
        all_online_label = self.tk.Label(
            frame,
            anchor="w",
            justify="left",
            wraplength=320,
            fg=_COLOURS["muted"],
            bg=_COLOURS["panel"],
        )
        all_online_label.pack(fill="x", pady=(4, 0))
        thread_heading = self.tk.Label(
            frame,
            text="THREADS WITH RESTRICTED CPU SETS",
            anchor="w",
            font=("TkDefaultFont", 8, "bold"),
            fg=_COLOURS["muted"],
            bg=_COLOURS["panel"],
        )
        thread_heading.pack(fill="x", pady=(8, 3))
        thread_list_frame = self.tk.Frame(frame, bg=_COLOURS["panel"])
        thread_list_frame.pack(fill="both", expand=True)
        thread_text = self.tk.Text(
            thread_list_frame,
            width=38,
            height=8,
            wrap="word",
            takefocus=0,
            bg=_COLOURS["panel"],
            fg=_COLOURS["muted"],
            relief="flat",
            borderwidth=0,
            highlightthickness=0,
            state="disabled",
        )
        for category, colour in _CATEGORY_COLOURS.items():
            thread_text.tag_configure(category, foreground=colour)
        thread_scrollbar = self.tk.Scrollbar(
            thread_list_frame,
            orient="vertical",
            command=thread_text.yview,
            bg=_COLOURS["panel_highlight"],
            troughcolor=_COLOURS["background"],
            highlightthickness=0,
        )
        thread_text.configure(yscrollcommand=thread_scrollbar.set)
        thread_text.pack(side="left", fill="both", expand=True)
        thread_scrollbar.pack(side="right", fill="y")

        return {
            "frame": frame,
            "cpu_label": cpu_label,
            "core_type_label": core_type_label,
            "usage_label": usage_label,
            "usage_bar": usage_bar,
            "isolation_label": isolation_label,
            "summary_label": summary_label,
            "availability_label": availability_label,
            "all_online_label": all_online_label,
            "thread_list_frame": thread_list_frame,
            "thread_text": thread_text,
            "thread_signature": None,
        }

    def _update_thread_rows(self, widgets: dict, threads: list[AffinityThread]) -> None:
        """Update coloured thread details only when their affinity changes."""
        signature = tuple(threads)
        if widgets["thread_signature"] == signature:
            return

        text, colour_ranges = _format_thread_rows(threads)
        thread_text = widgets["thread_text"]
        thread_text.configure(state="normal")
        thread_text.delete("1.0", "end")
        if text:
            thread_text.insert("1.0", text)
            for category, start_line, end_line in colour_ranges:
                thread_text.tag_add(
                    category,
                    f"{start_line}.0",
                    f"{end_line}.0",
                )
        thread_text.configure(state="disabled")

        widgets["thread_signature"] = signature

    def _update_cpu_card(self, widgets: dict, card: CpuCard) -> None:
        """Display fresh usage, isolation, availability and affinity data."""
        widgets["core_type_label"].configure(
            text=self._core_type_label(card.core_type),
            fg=self._core_type_colour(card.core_type),
        )
        if card.usage is None:
            usage_text = "sampling..."
            usage_amount = 0.0
        else:
            usage_text = f"{card.usage:.1f}%"
            usage_amount = min(100.0, max(0.0, card.usage))
        widgets["usage_label"].configure(text=usage_text)
        usage_bar = widgets["usage_bar"]
        usage_bar.delete("all")
        usage_bar.update_idletasks()
        bar_width = max(1, usage_bar.winfo_width())
        usage_bar.create_rectangle(
            0,
            0,
            bar_width,
            10,
            fill=_COLOURS["bar_track"],
            outline=_COLOURS["bar_track"],
        )
        if usage_amount > 0:
            usage_bar.create_rectangle(
                0,
                0,
                bar_width * usage_amount / 100.0,
                10,
                fill=_COLOURS["bar"],
                outline=_COLOURS["bar"],
            )

        isolation_text = _cpu_isolation_summary(card)
        if card.scheduler_isolated or card.managed_irq_isolated or card.nohz_full:
            isolation_colour = _COLOURS["cyan"]
        else:
            isolation_colour = (
                _COLOURS["muted"] if card.isolation_known else _COLOURS["red"]
            )
        widgets["isolation_label"].configure(
            text=isolation_text,
            fg=isolation_colour,
        )

        widgets["summary_label"].configure(text=_cpu_thread_summary(card))
        availability = _cpu_availability_summary(card)
        availability_colour = (
            _COLOURS["green"] if availability.startswith("OPEN") else _COLOURS["muted"]
        )
        widgets["availability_label"].configure(
            text=availability,
            fg=availability_colour,
        )
        widgets["all_online_label"].configure(text=_cpu_all_online_summary(card))
        self._update_thread_rows(widgets, card.threads)

    @staticmethod
    def _core_type_label(core_type: str) -> str:
        """Return a badge label only when Linux identifies the processor type."""
        if core_type == "Unknown":
            return ""
        return core_type.upper()

    @staticmethod
    def _core_type_colour(core_type: str) -> str:
        """Return the badge colour for a P-core, E-core or unknown processor."""
        if core_type == "P-core":
            return _COLOURS["p_core"]
        if core_type == "E-core":
            return _COLOURS["e_core"]
        return _COLOURS["muted"]

    def _render_snapshot(self, snapshot: SystemSnapshot) -> None:
        """Refresh the CPU grid and the summary line."""
        new_cpu_ids = tuple(card.processor for card in snapshot.cards)
        if new_cpu_ids != self.cpu_ids:
            for widgets in self.card_widgets.values():
                widgets["frame"].destroy()
            self.card_widgets.clear()
            self.cpu_ids = new_cpu_ids
            for index, card in enumerate(snapshot.cards):
                self.card_widgets[card.processor] = self._create_cpu_card(card, index)

        for card in snapshot.cards:
            widgets = self.card_widgets[card.processor]
            self._update_cpu_card(widgets, card)
            if not snapshot.affinity_scan_complete:
                widgets["summary_label"].configure(text="Scanning thread affinity...")
                widgets["availability_label"].configure(text="")
                widgets["all_online_label"].configure(text="")

        if snapshot.affinity_scan_complete:
            summary = (
                f"{len(snapshot.cards)} online CPUs | "
                f"{snapshot.process_count} processes | {snapshot.thread_count} threads | "
                f"unavailable processes: {snapshot.unreadable_process_count} | "
                f"unavailable threads: {snapshot.unreadable_thread_count}"
            )
        else:
            summary = f"{len(snapshot.cards)} online CPUs | scanning process and thread affinity..."
        self.summary_label.configure(text=summary)
        if not snapshot.affinity_scan_complete:
            status = "CPU panels ready; scanning process and thread affinity..."
        elif snapshot.sample_seconds > 0:
            status = (
                f"Updated {snapshot.updated_at}; CPU usage covers "
                f"{snapshot.sample_seconds:.2f} seconds. Refreshing every "
                f"{self.arguments.sample_seconds:g} second(s)."
            )
        else:
            status = "Affinity loaded; waiting for the first CPU usage sample..."
        self.status_label.configure(text=status, fg=_COLOURS["cyan"])
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _make_snapshot(
        self,
        usage_by_cpu: dict[int, float | None],
        sample_seconds: float,
        survey: dict | None = None,
    ) -> SystemSnapshot:
        """Collect process data and assemble the card models for the window."""
        return make_snapshot(usage_by_cpu, sample_seconds, survey)

    def _collect_snapshots(self) -> None:
        """Read procfs in the background so the window remains responsive."""
        try:
            previous = read_cpu_times()
        except OSError as error:
            self.messages.put((None, f"Cannot read /proc/stat: {error}"))
            return

        previous_time = time.monotonic()
        try:
            survey = survey_processes(
                self.arguments.pid,
                self.arguments.name,
                self.arguments.kernel,
            )
            snapshot = self._make_snapshot({}, 0.0, survey)
            self.messages.put((snapshot, None))
        except OSError as error:
            self.messages.put((None, f"Cannot read process affinity: {error}"))

        summary_written = False
        while not self.stop_event.wait(self.arguments.sample_seconds):
            try:
                current = read_cpu_times()
                current_time = time.monotonic()
                usage = calculate_cpu_usage(previous, current)
                sample_seconds = current_time - previous_time
                previous = current
                previous_time = current_time
                survey = survey_processes(
                    self.arguments.pid,
                    self.arguments.name,
                    self.arguments.kernel,
                )
                snapshot = self._make_snapshot(usage, sample_seconds, survey)
            except OSError as error:
                self.messages.put((None, f"System scan failed: {error}"))
                continue
            self.messages.put((snapshot, None))
            if self.arguments.summary is not None and not summary_written:
                try:
                    summary_written = _write_summary_when_ready(self.arguments.summary, snapshot)
                except OSError as error:
                    summary_written = True
                    self.messages.put((
                        None,
                        f"Cannot write summary file {self.arguments.summary}: {error}",
                    ))

    def _poll_messages(self) -> None:
        """Apply the newest background scan result on Tk's main thread."""
        latest = None
        while True:
            try:
                latest = self.messages.get_nowait()
            except queue.Empty:
                break

        if latest is not None:
            snapshot, error = latest
            if error is not None:
                self.status_label.configure(
                    text=error,
                    fg=_COLOURS["red"],
                )
            elif snapshot is not None:
                self._render_snapshot(snapshot)
        self.root.after(150, self._poll_messages)

    def close(self) -> None:
        """Stop background collection and close the window."""
        self.stop_event.set()
        self.root.destroy()

    def run(self) -> None:
        """Run Tk's window event loop."""
        self.root.mainloop()


def main() -> int:
    """Write the text summary, or open the window, or both, as the options ask."""
    arguments = parse_arguments()
    if not arguments.graphic:
        # Tk is not imported at all, so this works on a machine with no graphical display and no Tk package.
        return write_summary_report(arguments)
    try:
        import tkinter as tk
    except ModuleNotFoundError:
        print(
            "error: Tkinter is unavailable; install the Python Tk package for your system.",
            file=sys.stderr,
        )
        return 1

    try:
        CpuAffinityWindow(tk, arguments).run()
    except tk.TclError as error:
        print(f"error: cannot open the graphical window: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
