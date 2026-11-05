#!/usr/bin/env python3
"""Show live process-thread CPU affinity and usage in a scrollable window."""

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
    """One complete set of process affinity and CPU usage measurements."""

    cards: list[CpuCard]
    process_count: int
    thread_count: int
    unreadable_process_count: int
    unreadable_thread_count: int
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


def _read_thread_affinity(pid: int, thread_id: int) -> list[int] | None:
    """Read a thread's allowed CPUs, or return None if its procfs entry is unavailable."""
    path = _PROC / str(pid) / "task" / str(thread_id) / "status"
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("Cpus_allowed_list:"):
                return parse_cpu_list(line.split(":", 1)[1])
    except (OSError, ValueError):
        return None
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
            processors = _read_thread_affinity(pid, thread_id)
            if processors is None:
                unreadable_threads += 1
                continue
            thread_name = _read_first_line(task_directory / str(thread_id) / "comm") or "?"
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
    """Parse command-line options."""
    parser = argparse.ArgumentParser(
        description="Show live process-thread CPU affinity and usage in a scrollable window."
    )
    parser.add_argument("--pid", type=int, help="show only this process")
    parser.add_argument("--name", help="show processes whose name or command contains this text")
    parser.add_argument("--kernel", action="store_true", help="include kernel threads")
    parser.add_argument(
        "--sample-seconds",
        type=_positive_float,
        default=1.0,
        help="CPU usage sampling and refresh interval in seconds (default: 1.0)",
    )
    return parser.parse_args()


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
            text=card.core_type.upper(),
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
        thread_frame = self.tk.Frame(frame, bg=_COLOURS["panel"])
        thread_frame.pack(fill="x")

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
            "thread_frame": thread_frame,
            "thread_signature": None,
        }

    def _update_thread_rows(self, widgets: dict, threads: list[AffinityThread]) -> None:
        """Update coloured thread details only when their affinity changes."""
        signature = tuple(threads)
        if widgets["thread_signature"] == signature:
            return

        for child in widgets["thread_frame"].winfo_children():
            child.destroy()

        for thread in threads:
            row = self.tk.Frame(
                widgets["thread_frame"],
                bg=_COLOURS["panel"],
                padx=0,
                pady=2,
            )
            row.pack(fill="x")
            marker = self.tk.Frame(
                row,
                width=5,
                bg=_CATEGORY_COLOURS[thread.category],
            )
            marker.pack(side="left", fill="y", padx=(0, 8))
            detail = (
                f"{thread.process_name} / {thread.thread_name}\n"
                f"PID {thread.process_id} | TID {thread.thread_id} | "
                f"CPUs {format_cpu_list(thread.allowed_processors)}"
            )
            self.tk.Label(
                row,
                text=detail,
                anchor="w",
                justify="left",
                wraplength=310,
                fg=_CATEGORY_COLOURS[thread.category],
                bg=_COLOURS["panel"],
            ).pack(side="left", fill="x", expand=True)

        widgets["thread_signature"] = signature

    def _update_cpu_card(self, widgets: dict, card: CpuCard) -> None:
        """Display fresh usage, isolation, availability and affinity data."""
        widgets["core_type_label"].configure(
            text=card.core_type.upper(),
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

        tags: list[str] = []
        if card.scheduler_isolated:
            tags.append("SCHEDULER-ISOLATED (isolcpus)")
        if card.managed_irq_isolated:
            tags.append("MANAGED-IRQ ISOLATION")
        if card.nohz_full:
            tags.append("NOHZ_FULL")
        if tags:
            isolation_text = " | ".join(tags)
            isolation_colour = _COLOURS["cyan"]
        elif card.isolation_known:
            isolation_text = "No isolation flags reported for this CPU"
            isolation_colour = _COLOURS["muted"]
        else:
            isolation_text = "Kernel isolation settings unavailable"
            isolation_colour = _COLOURS["red"]
        widgets["isolation_label"].configure(
            text=isolation_text,
            fg=isolation_colour,
        )

        single_count = card.single_cpu_thread_count
        multi_count = card.multi_cpu_thread_count
        all_count = card.all_online_thread_count
        widgets["summary_label"].configure(
            text=(
                f"Single-CPU threads: {single_count}  |  "
                f"Multi-CPU threads: {multi_count}  |  "
                f"All-CPU threads: {all_count}"
            )
        )
        flexible_count = multi_count + all_count
        if single_count == 0 and flexible_count > 0:
            availability = "OPEN TO THREADS ALLOWED ON MULTIPLE CPUs"
            availability_colour = _COLOURS["green"]
        elif single_count == 0:
            availability = "No reported process threads are allowed here"
            availability_colour = _COLOURS["muted"]
        else:
            availability = ""
            availability_colour = _COLOURS["muted"]
        widgets["availability_label"].configure(
            text=availability,
            fg=availability_colour,
        )
        if all_count:
            all_text = (
                f"{all_count} thread(s) in {card.all_online_process_count} process(es) "
                "are allowed on every online CPU"
            )
        else:
            all_text = ""
        widgets["all_online_label"].configure(text=all_text)
        self._update_thread_rows(widgets, card.threads)

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
            self._update_cpu_card(self.card_widgets[card.processor], card)

        summary = (
            f"{len(snapshot.cards)} online CPUs | "
            f"{snapshot.process_count} processes | {snapshot.thread_count} threads | "
            f"unavailable processes: {snapshot.unreadable_process_count} | "
            f"unavailable threads: {snapshot.unreadable_thread_count}"
        )
        self.summary_label.configure(text=summary)
        if snapshot.sample_seconds > 0:
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
        self, usage_by_cpu: dict[int, float | None], sample_seconds: float
    ) -> SystemSnapshot:
        """Collect process data and assemble the card models for the window."""
        survey = survey_processes(
            self.arguments.pid,
            self.arguments.name,
            self.arguments.kernel,
        )
        processors = online_processors()
        isolation = read_isolation_settings()
        core_types = read_cpu_core_types(processors)
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
            sample_seconds=sample_seconds,
            updated_at=time.strftime("%H:%M:%S"),
        )

    def _collect_snapshots(self) -> None:
        """Read procfs in the background so the window remains responsive."""
        try:
            previous = read_cpu_times()
        except OSError as error:
            self.messages.put((None, f"Cannot read /proc/stat: {error}"))
            return

        previous_time = time.monotonic()
        try:
            self.messages.put((self._make_snapshot({}, 0.0), None))
        except OSError as error:
            self.messages.put((None, f"Cannot read process affinity: {error}"))

        while not self.stop_event.wait(self.arguments.sample_seconds):
            try:
                current = read_cpu_times()
                current_time = time.monotonic()
                usage = calculate_cpu_usage(previous, current)
                sample_seconds = current_time - previous_time
                previous = current
                previous_time = current_time
                snapshot = self._make_snapshot(usage, sample_seconds)
            except OSError as error:
                self.messages.put((None, f"System scan failed: {error}"))
                continue
            self.messages.put((snapshot, None))

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
    """Open the live CPU affinity window."""
    arguments = parse_arguments()
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
