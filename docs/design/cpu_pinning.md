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

## Where cores come from

**Cores are allocated at deploy time, not negotiated at run time.** `deploy.py` runs on the target
host, reads its real CPU topology, resolves the layout declared in the environment TOML into
concrete core ids, and writes the answer to a single file that every component reads. The full
reasoning is in [CPU Core Layout](cpu_pinning_anti_affinity.md); this section covers the mechanism.

**Layout file:** `<install_dir>/run/cpu_layout.toml` — generated, never edited  
**Wrapper:** `<install_dir>/run/background_tier` — generated, applies the background mask  
**Registry:** `<install_dir>/run/pubsub_cpu_registry` (+ `.lock`) — a record, not an allocator  
**Classes:** `CpuLayout`, `CpuRegistry` (`libraries/pubsub_itc_fw/`)

### Two tiers

Every machine's cores are split in two. The **background tier** is shared by everything not
explicitly promoted. A component's **hot-path cores** are its own, one per hot-path thread.

The default direction matters: a process starts entirely in the background tier and the Reactor
promotes the few threads that were allocated cores. Enumerating what should *not* be hot-path would
be the other way round, and the threads someone forgot are precisely the ones that would end up
contaminating a pinned core.

### How it works

1. `deploy.py` writes `cpu_layout.toml` (both tiers, per component) and the `background_tier`
   wrapper script.
2. The launcher starts each component through the wrapper, which `exec`s it under
   `taskset --cpu-list <background>`. This covers the JVM components, which have no `main()` of
   ours, and the window before a C++ `main()` is entered. An affinity mask survives `execve`.
3. In `main()`, each C++ component calls `apply_background_affinity()`, which masks the process to
   the background tier — a backstop so production never depends on the launcher cooperating — and
   places the Quill backend (see below).
4. Threads created after that inherit the background mask.
5. `Reactor::pin_registered_threads()` reads this component's entry and calls
   `pthread_setaffinity_np` to promote the reactor thread and each registered `ApplicationThread`
   onto its allocated core. Threads registered through `register_extra_thread()` are deliberately
   left in the background tier.
6. `CpuRegistry::record_assignment()` records what was pinned and reports any core already held by
   a live process from another installation.

This works because **an affinity mask is not a ratchet**: a thread masked to the background pool can
still be moved onto a hot-path core afterwards. That is why the mask is applied with `taskset` and
`sched_setaffinity` rather than a cgroup cpuset, which would forbid the later promotion.

### The Quill backend is a special case

The backend thread starts on first logger construction, which happens *before* the configuration
naming the layout file has been read. `sched_setaffinity(0, ...)` affects the calling thread and
threads created after it, so the backend inherits nothing and would keep an unrestricted mask —
free to be scheduled onto the very cores this design reserves.

It is therefore placed explicitly, by `apply_background_affinity()`, onto a background core that
`deploy.py` allocated it. `deploy.py` hands out one per component round-robin, because there are
thirteen C++ components on the development box and putting all thirteen backends on one core would
simply move the contention rather than remove it.

Placing it there rather than in the Reactor is deliberate: the Reactor returns early for a component
the layout demoted, so a backend handled there would be left unmasked on exactly those components.

### Verifying it

`cpu_audit.py` reads every running thread's real mask from `/proc/<pid>/task/<tid>/status` and
compares it against the layout, exiting non-zero on a mismatch so a performance run can be gated on
it. Nothing can *prevent* a thread changing its own affinity — the mask is advisory, and some
NUMA-aware thread pools in third-party libraries do exactly that — so it is detected instead.

---

## Configuration

CPU pinning is configured per-environment in the TOML files.

### `cpu_pinning_enabled` and the layout settings

`cpu_pinning_enabled` now means **take part in the machine's declared CPU layout**, and should be
true for *every* component on a machine that has one — not only those expecting dedicated cores. A
component that pins nothing still needs the background mask; without one it is free to be scheduled
onto the cores other components depend on. Such a component simply finds itself unadmitted in the
layout and stays in the background tier.

Four settings are mandatory whenever it is true, with no C++ defaults — startup fails rather than
running unpinned if any is absent:

| Setting | Meaning |
|---|---|
| `cpu_layout_file` | The generated `run/cpu_layout.toml` |
| `cpu_layout_component` | This component's key in the environment TOML, e.g. `sequencer_secondary` |
| `cpu_registry_shm_path` | Cross-installation collision record |
| `cpu_registry_lock_file` | `flock` file serialising access to it |

`cpu_layout_component` must be the *instance* name, not the binary name: a primary and its secondary
run the same binary and are ranked and placed separately. `deploy.py` expands it per component when
it expands the config templates, so it is not maintained by hand.


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

## A stale layout is a startup error, not a warning

The layout is computed once, at deploy time, so a machine that changes shape afterwards — cores
offlined, a VM resized, hardware replaced — leaves the file describing CPUs that are no longer
there. `CpuLayout::verify_cores_present()` checks this at startup and refuses to continue.

Refusing to start is the right response: a latency-critical component running under a layout
computed for different hardware is worse than one that does not run. The remedy is to re-run
`deploy.py`, which recomputes against the machine as it now is.

The stale-*registry* problem that used to appear here — every process pinned to the same CPUs
because a leftover file confused the availability scan — cannot happen any more. Nothing is
inferred from the registry's contents; the layout file says which cores belong to which component,
and a restarted component gets the same ones it had before because the file did not change.

## See Also

- [CPU Core Layout — Declared Allocation and Background by Default](cpu_pinning_anti_affinity.md)
- [Threading](threading.md)
- [Reactor](reactor.md)
