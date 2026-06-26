# CPU Pinning

## Why Pin Threads to CPUs

On a standard Linux kernel, the scheduler moves threads between CPUs based on load. Each
migration flushes the thread's working set from cache. More significantly, each `epoll_wait`
wakeup carries 50–200µs of scheduler jitter on a normal desktop or server kernel — the time
between an event being ready and the thread actually running to process it.

The framework pins every `ApplicationThread` to a dedicated CPU at startup. With pinning:

- The thread's working set stays warm in L1/L2 cache between wakeups
- No other thread competes for that CPU
- Combined with `SCHED_FIFO` and `isolcpus`, wakeup jitter falls from 50–200µs to 5–20µs

CPU pinning is the primary mechanism for achieving predictable, low-latency event processing.
It is not optional — without it, measured latencies are dominated by scheduler noise rather
than actual system behaviour.

---

## The CPU Registry

Each process in the system claims its CPUs via a shared-memory registry at startup, so that
no two processes claim the same CPU.

**File:** `<install_dir>/run/pubsub_cpu_registry`  
**Lock file:** `<install_dir>/run/pubsub_cpu_registry.lock`  
**Class:** `CpuRegistry` (`libraries/pubsub_itc_fw/`)

### How it works

1. On startup, the Reactor calls `CpuRegistry::claim_cpus()`.
2. `claim_cpus()` acquires an `flock` on the lock file, opens the shared-memory file, and
   scans the registry for stale entries (PIDs that are no longer alive). Stale entries are
   compacted out before the availability scan.
3. Available CPUs are those in the configured range that are not currently claimed by a live
   process. The first N are claimed (N = number of threads the process will start).
4. Each claimed entry records the PID and the CPU ID. The entry persists until the process
   exits or the registry file is deleted.
5. On process exit (or `devenv.py start`), the registry file is deleted so the next run
   begins clean.

### Thread pinning

After `claim_cpus()` returns, the Reactor calls `pthread_setaffinity_np` on each
`ApplicationThread` as it starts, binding it to its claimed CPU. The Quill logging backend
thread is also pinned to a claimed CPU (separate from the hot-path threads).

---

## Configuration

CPU pinning is configured per-environment in the TOML files.

### `cpu_pinning_reserve_cpu0`

```toml
[shared]
reactor_cpu_pinning_reserve_cpu0 = true   # dev / test
# reactor_cpu_pinning_reserve_cpu0 = false  # prod / preprod
```

When `true`, CPU 0 is excluded from the pinning candidates and left for the OS, interrupt
handlers, and other system activity. Set `true` on development and test machines. Set `false`
on production machines where CPUs are isolated with `isolcpus`.

### Environment defaults

| Environment | `cpu_pinning_reserve_cpu0` |
|-------------|---------------------------|
| `dev.toml` | `true` |
| `test-*.toml` | `true` |
| `preprod.toml` | `false` |
| `prod.toml` | `false` |

### Hybrid CPUs (P-cores and E-cores)

Intel 12th generation (Alder Lake) and later CPUs have two core types:

- **P-cores** — high single-thread performance, lower wakeup latency
- **E-cores** — lower performance, higher wakeup latency; misleading in latency measurements

If the machine has a hybrid CPU, check whether CPUs are P-cores or E-cores:

```bash
# Compare performance values — higher = P-core, lower = E-core
cat /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference 2>/dev/null
# or
cat /sys/devices/system/cpu/cpu*/acpi_cppc/highest_perf 2>/dev/null | sort -u
```

If two distinct values appear, the higher is a P-core. Restrict the available CPU range
in the TOML to P-cores only. This is a configuration change, not a code change.

---

## SCHED_FIFO

CPU pinning alone reduces cache misses but does not prevent the kernel from preempting a
pinned thread to handle an interrupt or run a higher-priority task. For lowest jitter,
`ApplicationThread` should run at `SCHED_FIFO` priority.

### Check whether RT scheduling is available

```bash
# Try to set SCHED_FIFO at priority 1 (minimum)
chrt -f 1 echo "SCHED_FIFO works"
```

If this fails with `Operation not permitted`, the process lacks the capability. Grant it via
`/etc/security/limits.conf`:

```
# /etc/security/limits.conf
*    soft    rtprio    99
*    hard    rtprio    99
```

Log out and back in for the limit to take effect.

### RT throttle

Linux throttles `SCHED_FIFO` threads by default (95% CPU time per second). During
benchmarking this throttle can cause unexpected latency spikes. To disable:

```bash
# Disable RT throttle (not recommended for production without careful consideration)
echo -1 | sudo tee /proc/sys/kernel/sched_rt_runtime_us
```

---

## `isolcpus` — Removing CPUs from the Scheduler Pool

`isolcpus` is a kernel boot parameter that prevents the scheduler from placing any thread on
the named CPUs unless explicitly assigned. It is the most impactful single improvement for
consistent low-latency wakeup.

### Check current status

```bash
grep isolcpus /proc/cmdline      # is it set?
grep nohz_full /proc/cmdline     # stops timer ticks on isolated CPUs
grep rcu_nocbs /proc/cmdline     # moves RCU callbacks off isolated CPUs
```

### Enable (Ubuntu / grub)

```bash
sudo nano /etc/default/grub
# Edit: GRUB_CMDLINE_LINUX_DEFAULT="quiet splash isolcpus=A,B,C nohz_full=A,B,C rcu_nocbs=A,B,C"
# Replace A,B,C with the CPU IDs reserved for hot-path threads.
# These must match the CPUs claimed by the CPU registry.

sudo update-grub
sudo reboot
```

`nohz_full` stops the periodic timer tick on isolated CPUs when exactly one thread is
running there. `rcu_nocbs` moves RCU grace-period callbacks off those CPUs. Both are
recommended alongside `isolcpus`.

### Verify after reboot

```bash
cat /proc/cmdline | grep isolcpus
taskset -c <cpu_id> stress-ng --cpu 1 --timeout 5s &
# Confirm in htop that only the pinned process appears on that CPU
```

---

## What a Machine Needs for Sub-100µs Median Wakeup

All five steps are independent and cumulative. Steps 1–3 require no reboot.

| Step | Requires reboot | Expected benefit |
|------|-----------------|-----------------|
| 1. Set CPU governor to `performance` | No | Eliminates clock-scaling jitter |
| 2. Pin hot-path threads to P-cores only (hybrid CPUs) | No | Avoids E-core latency inconsistency |
| 3. Grant `rtprio`; set `SCHED_FIFO`; disable RT throttle for benchmarking | No | Prevents userspace preemption |
| 4. Install `linux-lowlatency` or `PREEMPT_RT` kernel | Yes | Reduces IRQ preemption; 5–20µs jitter range |
| 5. Add `isolcpus` + `nohz_full` + `rcu_nocbs` to boot params | Yes | Removes all scheduler interference from hot-path CPUs |

**Set CPU governor:**

```bash
# Check current governor
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort -u

# Switch all CPUs to performance (requires root)
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$cpu" > /dev/null
done
```

**Check kernel preemption model:**

```bash
uname -r                          # -lowlatency or -rt suffix = better preemption model
zcat /proc/config.gz | grep PREEMPT
# CONFIG_PREEMPT_VOLUNTARY — standard; 50–200µs jitter typical
# CONFIG_PREEMPT            — low-latency kernel; 10–50µs jitter typical
# CONFIG_PREEMPT_RT         — full RT patch; 5–20µs jitter typical
```

**Observed results on development hardware** (Linux Mint, no isolcpus):

- Before pinning: gateway internal latency ~520–660µs
- After pinning (steps 1–2, no SCHED_FIFO, no isolcpus): best 389µs, typical 490–690µs
- With SCHED_FIFO + isolcpus on dedicated hardware with PREEMPT_RT: consistent 5–15µs
  wakeup latency achievable

---

## Known Issue: Stale Registry Entries

If the registry file is left over from a previous run with a different process layout, all
processes may claim identical CPUs. The symptom is log lines showing every process pinned to
the same set of CPUs (e.g. CPUs 1, 2, 3 across gateway, sequencer, and ME).

**Fix:** `devenv.py start` deletes the registry file before starting components. If starting
processes manually, delete `<install_dir>/run/pubsub_cpu_registry` first.

The registry code itself defends against this via compaction of stale entries at the start
of each `claim_cpus()` call, but deletion is the clean reset.

---

## Outstanding

- `cpu_registry_shm_path` is not yet configurable from TOML. The path defaults to
  `<install_dir>/run/pubsub_cpu_registry` (injected by `deploy.py`). The C++ default in
  `ReactorConfiguration` is `/dev/shm/pubsub_cpu_registry` — not used in practice because
  `deploy.py` always overrides it. Making it fully configurable via TOML is deferred
  (see [Roadmap](../roadmap.md)).

## See Also

- [Threading](threading.md)
- [Reactor](reactor.md)
