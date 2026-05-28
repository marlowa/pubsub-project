#!/usr/bin/env python3
import time
import sys

try:
    import psutil
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation
except ImportError as e:
    print(f"Error: Missing dependency. Please run: pip install psutil matplotlib")
    sys.exit(1)

# Configuration
INTERVAL_MS = 100 
WINDOW_SIZE = 300 

# Data containers
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

def get_target_process():
    try:
        # Find the process using the most RSS
        procs = sorted([p for p in psutil.process_iter(['name', 'memory_info'])], 
                       key=lambda x: x.info['memory_info'].rss, 
                       reverse=True)
        return procs[0] if procs else None
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        return None

def update(frame):
    target = get_target_process()
    if target:
        try:
            mem = target.memory_info()
            name = target.info['name']
            proc_name[0] = f"{name} (PID: {target.pid})"
            
            curr_time = time.time() - start_time
            timestamps.append(curr_time)
            rss_data.append(mem.rss / (1024 * 1024))
            vsize_data.append(mem.vms / (1024 * 1024))

            if len(timestamps) > WINDOW_SIZE:
                timestamps.pop(0)
                rss_data.pop(0)
                vsize_data.pop(0)

            line_rss.set_data(timestamps, rss_data)
            line_vsize.set_data(timestamps, vsize_data)

            ax.set_title(f"Monitoring Top Process: {proc_name[0]}", fontsize=12)
            ax.relim()
            ax.autoscale_view()
        except Exception:
            pass
    return line_rss, line_vsize

print("Monitor started. Launch your test now...")
ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.tight_layout()
plt.show()
