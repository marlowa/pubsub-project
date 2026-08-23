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
proc_name = ["None"]

fig, ax = plt.subplots(figsize=(10, 6))
line_rss, = ax.plot([], [], label="RSS (Physical RAM)", color='#1f77b4', linewidth=2)
line_vsize, = ax.plot([], [], label="VSIZE (Virtual/Reserved)", color='#d62728', linestyle='--')

ax.set_ylabel("Megabytes (MB)")
ax.set_xlabel("time of day (sliding window)" if args.trend else "time of day (whole run)")
ax.grid(True, alpha=0.3)
ax.legend(loc='upper left')

start_time = time.time()

# Ticks are the wall-clock time of day, not seconds since the monitor started. Everything a
# step on this chart has to be explained against -- the venue logs, Prometheus, the phase
# boundaries of a load run -- is stamped with the time of day, and an axis counting elapsed
# seconds makes the reader do the arithmetic every time they want to line two of them up.
#
# The x DATA stays as elapsed seconds; only the labels are converted. Seconds are shown in
# the sliding window and not in the whole-run view: the window spans well under a minute, so
# HH:MM alone would print the same label on every tick.
_TICK_FORMAT = "%H:%M:%S" if args.trend else "%H:%M"


def format_clock_tick(elapsed_seconds, _position):
    return datetime.datetime.fromtimestamp(start_time + elapsed_seconds).strftime(_TICK_FORMAT)


ax.xaxis.set_major_formatter(mticker.FuncFormatter(format_clock_tick))

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def get_target_process():
    try:
        procs = sorted(
            [p for p in psutil.process_iter(['name', 'memory_info'])],
            key=lambda x: x.info['memory_info'].rss,
            reverse=True,
        )
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
            name = target.info['name']
            pid  = target.pid
            rss_mb   = mem.rss / 1048576
            vsize_mb = mem.vms / 1048576
            proc_label = f"{name}({pid})"
            proc_name[0] = f"{name} (PID: {pid})"

            timestamps.append(elapsed)
            rss_data.append(rss_mb)
            vsize_data.append(vsize_mb)

            if args.trend and len(timestamps) > WINDOW_SIZE:
                timestamps.pop(0)
                rss_data.pop(0)
                vsize_data.pop(0)

            # Whole-run mode keeps every sample and lets the x axis grow, so the run packs
            # itself into the same width as it lengthens and the start stays on the chart.
            # A step can then be read against where memory began rather than against
            # whatever happened to be on screen a few seconds earlier.
            if args.trend:
                line_rss.set_data(timestamps, rss_data)
                line_vsize.set_data(timestamps, vsize_data)
            else:
                line_rss.set_data(*decimate(timestamps, rss_data, MAX_PLOT_POINTS))
                line_vsize.set_data(*decimate(timestamps, vsize_data, MAX_PLOT_POINTS))
            ax.set_title(f"Monitoring Top Process: {proc_name[0]}", fontsize=12)
            ax.relim()
            ax.autoscale_view()

            log_row(elapsed, proc_label, pid, rss_mb, vsize_mb, vm, swap)
        except Exception:
            pass
    else:
        log_row(elapsed, "(none)", 0, 0.0, 0.0, vm, swap)

    return line_rss, line_vsize

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
print("Monitor started. Launch your test now...")
ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.tight_layout()
plt.show()
log_file.close()
