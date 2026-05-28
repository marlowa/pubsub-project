#!/usr/bin/env python3
import argparse
import time
import sys
import datetime

try:
    import psutil
    import matplotlib.pyplot as plt
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
WINDOW_SIZE = 300   # graph sliding-window points only; log is unbounded

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
ax.set_xlabel("Time (s) - Sliding Window")
ax.grid(True, alpha=0.3)
ax.legend(loc='upper left')

start_time = time.time()

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

            if len(timestamps) > WINDOW_SIZE:
                timestamps.pop(0)
                rss_data.pop(0)
                vsize_data.pop(0)

            line_rss.set_data(timestamps, rss_data)
            line_vsize.set_data(timestamps, vsize_data)
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
