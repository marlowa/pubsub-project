#!/usr/bin/env python3
import argparse
import time
import sys
import datetime

try:
    import psutil
    import matplotlib.pyplot as plt
    import matplotlib.ticker as mticker
    from matplotlib.animation import FuncAnimation
except ImportError as e:
    print(f"Error: Missing dependency. Please run: pip install psutil matplotlib")
    sys.exit(1)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
_default_log = f"memory_monitor_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.log"

parser = argparse.ArgumentParser(description="Monitor top-RSS process + system memory")
parser.add_argument(
    "--trend",
    action="store_true",
    help=(
        "show a sliding window of the most recent samples, discarding older ones, rather "
        "than the whole run. Use when watching what is happening now; the default keeps "
        "the start of the run on the chart so a step can be read against where it began."
    ),
)
parser.add_argument(
    "--pid",
    type=int,
    metavar="N",
    help=(
        "monitor this process id rather than whichever process happens to be using the most "
        "memory. Pin the target for any run long enough that something else might overtake it."
    ),
)
parser.add_argument(
    "--process",
    metavar="NAME",
    help=(
        "monitor the largest process whose name contains NAME. Survives a restart of the "
        "process, unlike --pid, so it suits watching a component across a failover."
    ),
)
parser.add_argument(
    "-o", "--output",
    default=_default_log,
    metavar="FILE",
    help=f"ASCII log file for vmstat-style figures (default: {_default_log})",
)
args = parser.parse_args()

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
INTERVAL_MS = 100
WINDOW_SIZE = 300   # --trend sliding-window points only; log is unbounded

# Samples drawn at once when keeping the whole run. Everything is kept in memory and every
# sample reaches the log; this caps only what matplotlib is asked to redraw, ten times a
# second. Left uncapped, an hour at INTERVAL_MS is some 36,000 points per line and the
# monitor becomes a heavier consumer of CPU and memory than most of what it is watching.
MAX_PLOT_POINTS = 2000

# ---------------------------------------------------------------------------
# Log file setup
# ---------------------------------------------------------------------------
_COL_W = [11, 20, 22, 7, 9, 10, 12, 13, 11, 12]
_HEADER = (
    f"{'Elapsed(s)':>{_COL_W[0]}}"
    f"  {'DateTime':<{_COL_W[1]}}"
    f"  {'TopProcess':<{_COL_W[2]}}"
    f"  {'PID':>{_COL_W[3]}}"
    f"  {'RSS_MB':>{_COL_W[4]}}"
    f"  {'VSIZE_MB':>{_COL_W[5]}}"
    f"  {'SysFree_MB':>{_COL_W[6]}}"
    f"  {'SysAvail_MB':>{_COL_W[7]}}"
    f"  {'SysUsed_MB':>{_COL_W[8]}}"
    f"  {'SwapUsed_MB':>{_COL_W[9]}}"
)

log_file = open(args.output, "w", buffering=1)   # line-buffered so data survives a crash
log_file.write(f"# memory_monitor log started {datetime.datetime.now().isoformat(timespec='seconds')}\n")
log_file.write(f"# {_HEADER}\n")
log_file.flush()

print(f"Logging to: {args.output}")

# ---------------------------------------------------------------------------
# Graph setup
# ---------------------------------------------------------------------------
timestamps = []
rss_data = []
vsize_data = []
sys_used_data = []
sys_avail_data = []
swap_used_data = []
proc_name = ["None"]

# Elapsed times at which the sampled process changed identity, and the rules already drawn
# for them. Only meaningful when unpinned; --pid and --process cannot change target.
target_changes = []
_drawn_changes = []
_current_target = [None]

# Distinct failures already reported, so a fault that recurs on every one of ten frames a
# second is recorded once rather than ten times a second.
_reported_failures = set()

# Two panels sharing the time axis rather than two y scales on one. The units are the same
# -- megabytes -- but the magnitudes are not: a machine with tens of gigabytes in use dwarfs
# a process holding a few, and on a shared scale the process steps this tool exists to show
# would be flattened into a line. Stacking keeps each readable at its own size, and the
# shared x means a step in one can still be read straight down against the other.
# constrained_layout rather than a tight_layout() call at startup: the title is rewritten on
# every frame, and it grows a suffix when the sampled process changes, so a layout computed
# once against the title the chart started with clips the one it ends up showing.
fig, (ax, ax_sys) = plt.subplots(
    2, 1, figsize=(10, 7.5), sharex=True, gridspec_kw={"height_ratios": [2, 1]},
    constrained_layout=True,
)
line_rss, = ax.plot([], [], label="RSS (Physical RAM)", color='#1f77b4', linewidth=2)
line_vsize, = ax.plot([], [], label="VSIZE (Virtual/Reserved)", color='#d62728', linestyle='--')

ax.set_ylabel("process (MB)")
ax.grid(True, alpha=0.3)
ax.legend(loc='upper left')

# Available, not free. Free counts only pages nobody holds at all, so on a healthy machine it
# reads alarmingly low while gigabytes of reclaimable cache sit ready to be handed out.
# Available is the kernel's own estimate of what a new allocation could actually get, and it
# is the one that answers the question this panel is here for: how close is this run to the
# point where something gets killed.
line_sys_used, = ax_sys.plot([], [], label="system used", color='#7f7f7f', linewidth=1.6)
line_sys_avail, = ax_sys.plot([], [], label="system available", color='#2ca02c', linewidth=1.6)
line_swap, = ax_sys.plot([], [], label="swap used", color='#ff7f0e', linestyle=':', linewidth=1.6)

ax_sys.set_ylabel("system (MB)")
ax_sys.set_xlabel("time of day (sliding window)" if args.trend else "time of day (whole run)")
ax_sys.grid(True, alpha=0.3)
ax_sys.legend(loc='upper left', fontsize=8, ncol=3)

start_time = time.time()

# Ticks are the wall-clock time of day, not seconds since the monitor started. Everything a
# step on this chart has to be explained against -- the venue logs, Prometheus, the phase
# boundaries of a load run -- is stamped with the time of day, and an axis counting elapsed
# seconds makes the reader do the arithmetic every time they want to line two of them up.
#
# The x DATA stays as elapsed seconds; only the labels are converted.
#
# Whether seconds appear follows the span currently on the axis, not the mode and not how
# long the monitor has been running. A short span needs them: the sliding window covers well
# under a minute, and HH:MM alone would print the same label on every tick. A long one does
# not, and pays for them -- once a run is into the hours, eight-character labels crowd the
# axis to say a digit nobody can place to better than a tick's width anyway.
#
# Reading the limits rather than the elapsed time means this also follows an interactive
# zoom. Zooming into a minute of a three-hour run brings the seconds back, which is when
# they are wanted: that is someone lining a step up against a log line stamped to the second.
SECONDS_SHOWN_BELOW = 30 * 60


def format_clock_tick(elapsed_seconds, _position):
    low, high = ax.get_xlim()
    tick_format = "%H:%M:%S" if (high - low) < SECONDS_SHOWN_BELOW else "%H:%M"
    return datetime.datetime.fromtimestamp(start_time + elapsed_seconds).strftime(tick_format)


ax_sys.xaxis.set_major_formatter(mticker.FuncFormatter(format_clock_tick))

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def get_target_process():
    """Return the process to sample, honouring --pid and --process when given.

    Unpinned, this picks whatever is using the most memory AT THIS INSTANT, which is a
    different question from "the process I am watching". Over a long run something else can
    overtake, and the answer changes underneath the chart -- see note_target_change.
    """
    try:
        if args.pid is not None:
            return psutil.Process(args.pid)
        candidates = [p for p in psutil.process_iter(['name', 'memory_info'])]
        if args.process:
            wanted = args.process.lower()
            candidates = [p for p in candidates if wanted in (p.info['name'] or "").lower()]
        procs = sorted(candidates, key=lambda x: x.info['memory_info'].rss, reverse=True)
        return procs[0] if procs else None
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        return None

def decimate(times, values, max_points):
    """Thin a series for drawing while keeping every step in it visible.

    Taking every Nth sample is the obvious way and the wrong one here. What this chart is
    watched for is the moment memory jumps -- a pool chaining a slab, a hash table
    doubling -- and a step lasting fewer than N samples can fall between the samples kept,
    so the jump the run was being watched for is the thing the thinning removes.

    So the series is split into buckets and the lowest and highest sample of each is kept,
    in the order they occurred. An extreme cannot be dropped, because being extreme is what
    selects it. The cost is that flat stretches gain a little visual noise, which is a good
    trade on a chart whose flat stretches are the uninteresting part.
    """
    count = len(times)
    if count <= max_points:
        return times, values
    # Two samples per bucket, so the bucket count is half the budget.
    bucket = max(1, -(-count // max(1, max_points // 2)))
    out_times, out_values = [], []
    for start in range(0, count, bucket):
        chunk_values = values[start:start + bucket]
        if not chunk_values:
            continue
        low = min(range(len(chunk_values)), key=chunk_values.__getitem__)
        high = max(range(len(chunk_values)), key=chunk_values.__getitem__)
        for offset in sorted({low, high}):
            out_times.append(times[start + offset])
            out_values.append(chunk_values[offset])
    return out_times, out_values


def note_target_change(elapsed, pid, label):
    """Record a change of sampled process, so the trace is not read as one process's.

    Unpinned, the chart follows whichever process is largest at each sample. When that
    changes, everything already drawn belongs to a different process, and the lines carry on
    across the switch as though one had simply grown into the other. That is a splice, not a
    measurement, and nothing on the chart would otherwise say so.
    """
    if _current_target[0] == pid:
        return
    first = _current_target[0] is None
    _current_target[0] = pid
    if first:
        return
    target_changes.append(elapsed)
    log_file.write(f"# target changed at {elapsed:.2f}s -> {label}\n")


def draw_target_changes():
    """Draw a heavy rule at each change, once each. Deliberately the visualiser's convention:
    a black vertical means the series either side are not continuous."""
    for elapsed in target_changes[len(_drawn_changes):]:
        for panel in (ax, ax_sys):
            panel.axvline(elapsed, color="black", linewidth=3.0, alpha=0.85, zorder=5)
        _drawn_changes.append(elapsed)


def log_row(elapsed, proc_label, pid, rss_mb, vsize_mb, vm, swap):
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    row = (
        f"{elapsed:>{_COL_W[0]}.2f}"
        f"  {now_str:<{_COL_W[1]}}"
        f"  {proc_label:<{_COL_W[2]}}"
        f"  {pid:>{_COL_W[3]}}"
        f"  {rss_mb:>{_COL_W[4]}.1f}"
        f"  {vsize_mb:>{_COL_W[5]}.1f}"
        f"  {vm.free / 1048576:>{_COL_W[6]}.1f}"
        f"  {vm.available / 1048576:>{_COL_W[7]}.1f}"
        f"  {vm.used / 1048576:>{_COL_W[8]}.1f}"
        f"  {swap.used / 1048576:>{_COL_W[9]}.1f}"
    )
    log_file.write(row + "\n")

def note_sample_failure(elapsed, failure):
    """Record a sample that could not be taken, once per distinct fault.

    Sampling a process is allowed to fail -- it can exit between being chosen and being
    measured, and a pinned pid can go away entirely -- so one failure must not stop the
    monitor. It must not pass silently either. A chart that keeps drawing while recording
    nothing shows a flat line, and a flat line is a claim: it says memory held steady. The
    reader has no way to tell that apart from nothing having been measured at all, and the
    quiet version is the one that gets believed.

    Kept out of the plot deliberately. Whatever went wrong here, the trace has a gap in it,
    and the log is where the reason belongs; drawing it would put an explanation on a chart
    at a moment the chart has nothing to explain it against.
    """
    signature = f"{type(failure).__name__}: {failure}"
    if signature in _reported_failures:
        return
    _reported_failures.add(signature)
    log_file.write(f"# sample failed at {elapsed:.2f}s -- {signature}\n")
    log_file.flush()
    print(f"warning: sampling failed at {elapsed:.2f}s -- {signature}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Animation callback
# ---------------------------------------------------------------------------
def update(frame):
    target = get_target_process()
    elapsed = time.time() - start_time

    vm   = psutil.virtual_memory()
    swap = psutil.swap_memory()

    if target:
        try:
            mem  = target.memory_info()
            # name(), not info['name']: the latter exists only on objects that came from
            # process_iter with attrs requested, so --pid -- which constructs a Process
            # directly -- raised here and was swallowed by the except below, logging nothing
            # at all. name() works for both.
            name = target.name()
            pid  = target.pid
            rss_mb   = mem.rss / 1048576
            vsize_mb = mem.vms / 1048576
            proc_label = f"{name}({pid})"
            proc_name[0] = f"{name} (PID: {pid})"
            note_target_change(elapsed, pid, proc_label)

            timestamps.append(elapsed)
            rss_data.append(rss_mb)
            vsize_data.append(vsize_mb)
            sys_used_data.append(vm.used / 1048576)
            sys_avail_data.append(vm.available / 1048576)
            swap_used_data.append(swap.used / 1048576)

            if args.trend and len(timestamps) > WINDOW_SIZE:
                for series in (timestamps, rss_data, vsize_data,
                               sys_used_data, sys_avail_data, swap_used_data):
                    series.pop(0)

            # Whole-run mode keeps every sample and lets the x axis grow, so the run packs
            # itself into the same width as it lengthens and the start stays on the chart.
            # A step can then be read against where memory began rather than against
            # whatever happened to be on screen a few seconds earlier.
            plotted = [
                (line_rss, rss_data),
                (line_vsize, vsize_data),
                (line_sys_used, sys_used_data),
                (line_sys_avail, sys_avail_data),
                (line_swap, swap_used_data),
            ]
            for line, series in plotted:
                if args.trend:
                    line.set_data(timestamps, series)
                else:
                    line.set_data(*decimate(timestamps, series, MAX_PLOT_POINTS))
            draw_target_changes()
            heading = "Monitoring" if (args.pid or args.process) else "Monitoring Top Process:"
            suffix = f"  --  {len(target_changes)} target change(s)" if target_changes else ""
            ax.set_title(f"{heading} {proc_name[0]}{suffix}", fontsize=12)
            for panel in (ax, ax_sys):
                panel.relim()
                panel.autoscale_view()

            log_row(elapsed, proc_label, pid, rss_mb, vsize_mb, vm, swap)
        except Exception as failure:
            note_sample_failure(elapsed, failure)
    else:
        log_row(elapsed, "(none)", 0, 0.0, 0.0, vm, swap)

    return line_rss, line_vsize

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
print("Monitor started. Launch your test now...")
ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
log_file.close()
