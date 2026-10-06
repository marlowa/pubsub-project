# CPU Core Layout — Declared Allocation and Background by Default

This document explains why CPU cores are allocated the way they are: declared per environment,
resolved into concrete core ids by `deploy.py` on the target host, and applied with every thread
starting in a shared background tier unless it is explicitly promoted. The mechanism itself —
the files `deploy.py` writes, the calls each component makes, and the audit — is described in
[CPU Pinning](cpu_pinning.md).

It has been checked on the 32-core development workstation: every ranked component lands on the
cores the layout allocates it, `matching_engine_secondary` is left in the background tier at
rank 5 with its reason logged, and both JVM components, `fix_test_client` included, start masked
to the background tier.

---

## The problem the layout solves

Some threads must not share a core with a hot-path thread. The Prometheus endpoint is the
clearest case: it runs an embedded HTTP server (civetweb, inside `prometheus-cpp`) on a thread
that an external scraper connects to, doing blocking I/O on a timescale of scrape intervals.

Two parts of keeping such a thread off the hot-path cores are easy:

- **Applying a mask.** `sched_setaffinity` takes a CPU set, so restricting a thread to many
  cores costs no more than pinning it to one.
- **Reaching the thread.** The thread's operating-system id can be obtained, so there is no
  need to search `/proc/self/task` for it.

The hard part is knowing *which* cores are hot-path cores, on this machine, at this moment. The
sections below explain why that cannot be worked out at run time from what other processes have
already claimed, and the design that follows answers it before any process starts.

---

## Why cores cannot be claimed at run time

Suppose each process claimed free cores when it started, recording them in a shared table such
as `CpuRegistry` (an `mmap`'d table of `(core_id, owning_pid)` entries guarded by an `flock`),
and every other thread were restricted to whatever was left. Three things go wrong.

### Difficulty 1: what is left is only known once every process has started

Each process claims during its own start-up, so the table's contents depend on *when you look*,
and no process can know when the last one has arrived:

- A process that starts early sees an almost empty table. The set of unclaimed cores is then
  nearly every core, including those about to be claimed. The mask is applied successfully and
  protects nothing, and nothing reports it.
- Within one process, a thread created before the reactor claims its cores cannot see even its
  own process's claims.
- If every core is claimed, there is nothing left. An empty affinity mask is `EINVAL`: a thread
  allowed to run nowhere cannot run.

A process that has not started yet leaves no trace in the table, so looking harder does not help.

### Difficulty 2: first come, first served gives the wrong cores to the wrong components

If cores are taken in start order, allocation follows the order `devenv.py` starts things in,
not how much each component matters, and the gateways start last.

This was measured on the development workstation with run-time claiming, same binaries and same
configuration in both rows:

| Deployment | `FixOrderGatewayThread` | `BinaryOrderGatewayThread` |
|---|---|---|
| Full high availability — 8 components claiming, 24 threads | CPU 19, **E-core** | CPU 22, **E-core** |
| `devenv.py --no-ha` — 5 components claiming, 15 threads | CPU 10, **P-core** | CPU 13, **P-core** |

In the full deployment the matching engines, sequencers and publishers started first and used
up the fifteen claimable P-cores, so both gateways landed on E-cores.

That threatens the comparison of the two gateways ([Roadmap](../roadmap.md) item 16). They run
the same venue over different client protocols with common code downstream, so the point is a
like-for-like measurement. If their threads sit on different core types the comparison measures
core type. Worse, the two gateways landing on the *same* core type is luck: the boundary between
P-cores and E-cores falls wherever the running total of threads reaches fifteen, so one more
thread anywhere earlier in the start order would put one gateway on a P-core and the other on an
E-core, in a single run, with nothing in the output to say so.

### Difficulty 3: there are not enough fast cores, and "secondary" does not mean "unimportant"

On the development workstation more threads want P-cores than there are P-cores, so some must go
to E-cores whatever the policy. The obvious candidates are the high availability followers, but
they are not one latency class:

- **The sequencer follower is inside every order's round trip.** Under the two-tier commit
  described in [WAL and High Availability](../availability/wal_and_ha.md), the leader holds each
  execution report until the follower's `WalAck` arrives. The follower's wake-up latency is
  therefore added to every order's round trip, and a follower on a slow core slows the leader's
  responses to clients.
- **The matching engine secondary is not.** The primary sends book updates to it without waiting
  for any acknowledgement; the secondary only follows the book.

Same high availability pattern, opposite conclusion. This is why the design declares a ranking
rather than deriving one: no tool can infer the `WalAck` dependency from where processes run or
from their names, and the obvious rule — "demote a secondary that shares a machine with its
primary" — gets the matching engine right and the sequencer badly wrong.

---

## Mandatory is not the same as latency-critical

A deployment needs to know which processes run on each machine, and which of them must be up for
the venue to trade. But *mandatory* must not also decide core allocation, because the two
properties are independent. The sequencer secondary shows it: the venue trades without it, so it
is not mandatory, yet its acknowledgement holds back every execution report, so it is
latency-critical. A single flag for both would put it on an E-core and slow every order.

| process | mandatory for trading | latency-critical | basis |
|---|---|---|---|
| `fix_order_gateway_a`, `binary_order_gateway_a` | yes | **yes** | client edge |
| `fix_order_gateway_b`, `binary_order_gateway_b` | no | no | second instances; unranked, so they run in the background tier |
| `sequencer_primary` | yes | **yes** | the sequencing point |
| `sequencer_secondary` | **no** | **yes** | `WalAck` holds back every execution report |
| `matching_engine_primary` | yes | **yes** | matching |
| `matching_engine_secondary` | **no** | **no** | book updates are not acknowledged |
| `mep_primary`, `mep_secondary` | undecided | no | downstream of the execution report path |
| `auth_service_a`, `auth_service_b` | yes | no | logon only, not per order |
| `arbiter_primary`, `arbiter_secondary`, `witness` | for resilience, not trading | no | election and failover only |
| `admin_service` | no | no | operator UI |
| `fix_test_client` | no | no | load generator; dev, FT and NFT only |

So there are two independent properties: **mandatory**, which belongs to readiness and health
checks and whether the launcher may declare the system operational; and a **latency rank**,
which decides which tier of cores a process's threads receive. They agree on most rows, which is
exactly why one flag would look adequate while misplacing the row that matters.

Only the latency rank is part of this design. *Mandatory* is left to whoever builds readiness
checks, and may not be a simple yes or no — the arbiters are not needed for the venue to keep
trading, only for it to survive a failure.

---

## Why development is the environment that matters

`prod.toml`, `preprod.toml` and `test-1.toml` run one component per host, apart from the two
gateway instances that share a host. There is little contention there: each host has far more
cores than its components want. Production hosts are also candidates for `isolcpus`, `nohz_full`
and a low-latency kernel.

`dev.toml` runs everything on one workstation — seventeen components, eight of them ranked —
with no CPU isolation and a stock kernel.

Every difficulty above is therefore one that only development has, which suggests it matters
less. The opposite is true: all latency measurement and all protocol comparison happens in
development. Production is where the system runs; development is where its numbers come from. A
design that is sound in production and unpredictable in development gives a system that cannot be
measured.

This does constrain the solution: production must not carry configuration whose only purpose is
the development case. The design satisfies that, because the ranking is declared once, is the
same on every machine, and on a dedicated production host never makes a difference.

---

## The design

### Rank and cut point

"Should this instance get hot-path cores?" is two questions, not one:

- **Rank — declared, the same on every machine.** The order in which components give up their
  claim to hot-path cores when a machine is short of them. This is knowledge about the venue —
  the sequencer follower's `WalAck` holds back every execution report, the matching engine
  secondary's book updates do not — and it is true in every environment.
- **Cut point — computed for each machine.** Where the supply of cores runs out. It depends on
  which components the machine runs and on its real core topology, both of which `deploy.py` can
  find out on the target host.

On a dedicated production host the cut point falls below everything, the rank makes no
difference, and a secondary gets exactly what its primary gets, with no per-environment setting.
On the development workstation the cut point falls somewhere real and the rank decides who is
above it.

This is why a fixed class ("this component is background") would be wrong. There is no reason to
withhold a P-core from `matching_engine_secondary` on a host where nothing else wants one. Being
left in the background tier is a result of contention, not a property of the component.

### Declared input 1: which processes run on which machine

Each environment file has a `[machines.*]` section:

```toml
[machines.localhost]
minimum_background_cores = 6
components = [
    "auth_service_a", "auth_service_b", "witness",
    "arbiter_primary", "arbiter_secondary",
    "matching_engine_primary", "matching_engine_secondary",
    "sequencer_primary", "sequencer_secondary",
    "mep_primary", "mep_secondary",
    "fix_order_gateway_a", "fix_order_gateway_b",
    "binary_order_gateway_a", "binary_order_gateway_b",
    "admin_service", "fix_test_client",
]
```

```toml
[machines."matching-engine.exchange.internal"]  # REPLACE
components = ["matching_engine"]
minimum_background_cores = 2
```

- **`localhost`** is the machine name `dev.toml` uses, where everything runs on one workstation.
- **The list names every process on the machine, not only those that pin.** A component that pins
  nothing still needs the background mask, or it is free to run on the cores the gateways are
  pinned to. Every component sets `cpu_pinning_enabled = true`, which means "take part in the
  machine's layout"; one with no rank is simply not admitted and stays in the background tier.
- Counts are absolute rather than fractions of the machine: clearer to reason about, and there
  are few machines.

#### What `minimum_background_cores` means

The two tiers are two ways of using a core, not "used" and "unused":

| tier | occupancy |
|---|---|
| hot-path | **dedicated** — a physical core is given whole to one component |
| background | **shared** — ordinary multitasking, many threads per core |

Background cores are not idle; they are where everything else runs. The setting is **a floor on
the number of logical CPUs left in the background tier**, not a share of the machine, and it is
small on every machine whatever its size. `deploy.py` treats an absent value as zero.

**Its first job is correctness: the background tier must never be empty.** Every C++ process has
at least a Quill backend that must run somewhere, and an empty affinity mask is `EINVAL`. On an
8-core machine without hyperthreading, rank 1 takes four cores and leaves four; rank 2 wants four
more, which would leave none. The floor is what refuses that.

**It makes no difference on the development workstation**, although `dev.toml` sets it to 6.
That machine is hybrid: CPUs 0-15 are P-cores (eight physical cores with hyperthreading) and
CPUs 16-31 are E-cores, as read from `acpi_cppc/highest_perf`. Hot-path threads only ever go on
P-cores, so the background tier always keeps at least the sixteen E-cores, and any floor from 0
to 16 gives the same layout. The value is there for the same file deployed on a machine with
uniform cores, where it does decide the outcome — see the second worked example.

**It makes no difference in production either**, where a host runs one component wanting two
hot-path threads. It matters on a machine with uniform cores running several ranked components,
where every core counts as a P-core and nothing else stops the hot-path tier from taking the
whole machine.

### Declared input 2: the rank

`hot_path_rank` is set on each `[components.*]` entry, alongside `ha_only`, because it belongs to
an instance and `dev.toml` already gives each instance its own entry:

```toml
[components.fix_order_gateway_a]
binary  = "bin/fix_order_gateway"
config  = "etc/fix_order_gateway/fix_order_gateway_a.toml"
workdir = "etc/fix_order_gateway"
ha_only = false
hot_path_rank = 1

[components.sequencer_secondary]
ha_only = true
hot_path_rank = 2

[components.matching_engine_secondary]
ha_only = true
hot_path_rank = 5
```

**No `hot_path_rank` means background.** Forgetting to rank a component puts it where it almost
certainly belongs, and the worst outcome is a background thread on a background core.

The ranking, following Difficulty 3 and the table above:

| rank | components | hot-path threads | physical cores, with hyperthreading |
|---|---|---|---|
| 1 | `fix_order_gateway_a`, `binary_order_gateway_a` | 4 | 2 |
| 2 | `sequencer_primary`, `sequencer_secondary` | 4 | 2 |
| 3 | `matching_engine_primary` | 2 | 1 |
| 4 | `mep_primary`, `mep_secondary` | 4 | 2 |
| 5 | `matching_engine_secondary` | 2 | 1 |
| — | everything else | 0 | background |

The ranks and `minimum_background_cores = 6` are one decision, because the ranking alone does not
say where the cut falls. Together they put the publishers above the cut on the 32-core
workstation and below it on a 20-core uniform machine. The cost accepted is that on the 20-core
machine the time from publishing to receiving is measured with the publishers in the background
tier. The alternative, a floor of 5 so that rank 4 is admitted there, would leave five background
cores for fifteen Quill backends, both JVMs and `fix_test_client` under load, which would disturb
the measurement more than unpinned publishers do.

### Ties are a constraint, not just an ordering

**A rank group is admitted whole or not at all.** Both rank 1 gateways are either above the cut
or below it; they are never split. Under run-time claiming, as Difficulty 2 shows, one extra
thread anywhere could split them silently. Here an extra thread can only push the *lowest-ranked
whole group* below the cut, so the gateway comparison stays valid by construction.

### Thread counts stay in the code

**The application knows how many threads it registers with the reactor, and the environment file
must not have to know.** A count in configuration is a second copy that goes wrong, silently, the
moment someone adds a thread.

> hot-path demand = the reactor thread + the registered `ApplicationThread`s

Threads registered through `register_extra_thread()` stay in the background tier, like any other
thread the process creates, and do not count. That matters because `FixOrderGatewayThread`
registers its `FixCaptureWriter` thread only when FIX capture is enabled, so counting extra
threads would make the same binary's demand depend on a configuration flag. As it is, every
ranked component wants two hot-path threads either way, and `FixCaptureWriter`, which writes
files, never takes a dedicated core.

Each ranked component declares the number in a constant, `hot_path_thread_count` (2 in every
case today), and reports it when run as `<binary> --hot-path-thread-count`. `deploy.py` runs on
the target host, so it asks the binary rather than reading a number from configuration. At
start-up, `Reactor::verify_hot_path_thread_count()` checks that the layout gave the component at
least as many cores as it actually registered threads for, and refuses to start if not, so a
constant left too low after a thread is added is caught.

### What `deploy.py` computes

`deploy.py` takes no host argument and runs on the machine it is installing to, so it reads
`/sys/devices/system/cpu` and sees the real topology. The same declaration resolves differently on
each machine, and re-running `deploy.py` after a hardware change recomputes it. This is the reason
for declaring a *rank* rather than a CPU bitmask: a bitmask states the answer, is specific to one
machine, must be worked out again for every environment, and cannot be checked against anything.

```
claimable = online CPUs, minus the whole physical core holding cpu0
            when reactor_cpu_pinning_reserve_cpu0 is true
groups    = components on this machine having hot_path_rank, grouped by rank, ascending

for group in groups:
    cores wanted = for each component, its hot-path threads divided by the CPUs
                   per physical core, rounded up; summed over the group
    if that many physical P-cores remain unallocated, and
       the CPUs left over would be at least minimum_background_cores:
        admit the group, giving each component whole physical P-cores
    else:
        stop

background = claimable CPUs not on any physical core given to the hot-path tier
```

A physical core given to the hot-path tier is given whole. Its sibling CPU goes to the same
component or to nobody — not to the background tier, because a background thread there would
compete with the hot-path thread exactly as another hot-path thread would. Each component's two
hot-path threads therefore share one physical core on a hyperthreaded machine, which also makes
the hand-over between the reactor thread and the application thread cheap, since they share
first-level cache.

Both conditions must hold, and the loop **stops** rather than skips: once a group does not fit,
no lower-ranked group is considered either. Otherwise a small low-ranked group could take cores
ahead of a larger higher-ranked one.

On a hybrid machine the hot-path tier is drawn from P-cores only. On a machine where no core
type can be read, every core counts as a P-core, so the background floor is what limits the
hot-path tier instead.

### When demand plus the background floor exceeds the machine

Four cases, which need different responses.

**A — some rank groups fit, some do not.** The designed case, and not an error: admission stops
at the first group that fails either condition, and everything below goes to the background tier.
The decision is made once, at deploy time, and is visible in one place. The 20-core worked example
below is this case.

**B — not even rank 1 fits.** For example a 4-core machine without hyperthreading,
`minimum_background_cores = 2`, and rank 1 wanting four cores: none would be left, which is below
the floor, so rank 1 is refused and admission stops. **Nothing is pinned and the whole machine is
background.**

That is the correct outcome. Admitting rank 1 partly would pin one gateway and not the other,
which is the invalid comparison whole-group admission exists to prevent, and the system runs
correctly unpinned. But it is reasonable on a functional-test VM and alarming on a production
host, and the layout cannot tell those apart. So `deploy.py` prints the computed layout, naming
every rank group left in the background tier and why, and records the same in the layout file. A
demotion that appears in the layout file can be diagnosed; one that appears only as unexplained
latency cannot.

**C — `minimum_background_cores` is greater than or equal to the number of claimable CPUs.** Not a
shortage but a configuration that makes no sense: no group could ever be admitted. `deploy.py`
stops with an error.

**D — the machine changes after deployment.** If the layout was computed for 32 CPUs and the
machine later has 16 online — CPUs taken offline, a VM resized, hardware replaced — then the
layout names CPUs that do not exist. `CpuLayout::verify_cores_present()` checks this at start-up,
both in `apply_background_affinity()` and in the reactor, and the component refuses to start. The
remedy is to re-run `deploy.py`. Refusing to start is right: a latency-critical component running
under a layout computed for different hardware is worse than one that does not run.

### Worked examples

**Development workstation — 8 physical P-cores with hyperthreading (CPUs 0-15), 16 E-cores
(CPUs 16-31), cpu0's physical core reserved, leaving 7 physical P-cores and 30 claimable CPUs:**

| rank | physical cores wanted | P-cores used | CPUs left for background | admitted |
|---|---|---|---|---|
| 1 | 2 | 2 | 26 | yes |
| 2 | 2 | 4 | 22 | yes |
| 3 | 1 | 5 | 20 | yes |
| 4 | 2 | 7 | 16 | yes — all 7 P-cores in use |
| 5 | 1 | 8 | — | **no** — no P-core left |

`matching_engine_secondary` goes to the background tier, every component above it is on P-cores,
and both rank 1 gateways are on P-cores in both the full and the `--no-ha` deployments, which is
the outcome Difficulty 2's table shows going wrong under run-time claiming.

The background tier is then the sixteen E-cores, shared by:

| source | threads |
|---|---|
| Quill backends, one per C++ component | 15 |
| seven unranked C++ components (two authentication services, the witness, two arbiters, the two `_b` gateways) — reactor and application thread each | about 14 |
| `matching_engine_secondary`, below the cut at rank 5 | 2 |
| `admin_service` JVM — Jetty pool, garbage collection, JIT | dozens, nearly all idle |
| `fix_test_client` JVM — MINA pool, garbage collection, JIT | dozens, **busy under load** |

The one that really uses the tier is `fix_test_client`, which is where it belongs rather than on
the gateways' cores.

**A 20-core machine with uniform cores and no hyperthreading, cpu0 reserved (19 claimable),
floor 6:**

| rank | cores wanted | total | CPUs left for background | admitted |
|---|---|---|---|---|
| 1 | 4 | 4 | 15 | yes |
| 2 | 4 | 8 | 11 | yes |
| 3 | 2 | 10 | 9 | yes |
| 4 | 4 | 14 | 5 | **no** — below the floor of 6 |

The publishers and everything below them go to the background tier, and ten threads are on the
hot path.

### Background by default, promotion by exception

The original requirement was "restrict this thread to cores nobody has pinned". The design meets
it the other way round: **background cores are the default for every thread in every process, and
a hot-path core is something a thread must be explicitly given.**

1. The process's affinity is set to the background tier **before it creates threads**.
2. Every thread created afterwards **inherits that mask automatically**. `pthread_create` gives
   the new thread a copy of its creator's mask; this was checked on the development machine, where
   a parent restricted to CPUs 28 and 30 produced a child reporting exactly `28 30`.
3. The reactor then pins the threads that are to have hot-path cores — the reactor thread itself
   and each registered `ApplicationThread` — overriding the default for those alone.

Promotion works because **an affinity mask can be widened again.** `sched_setaffinity` is limited
by the process's cgroup cpuset, not by its current mask, so a thread can move itself back onto a
hot-path core. This is why the mask is applied with `taskset` and not a cgroup cpuset: a cpuset is
a hard limit, and promotion onto a core outside it would fail. A cpuset is the stronger tool if
real exclusion is ever wanted, but then the hot-path cores must be inside the same cpuset as the
threads promoted onto them.

What this gives:

- **Threads started by libraries never need to be known about.** No list of threads, no
  per-thread setting, no search of `/proc/self/task`. `prometheus-cpp`'s civetweb thread, and
  whatever thread a library starts in its next release, are all safe without anyone noticing.
- **Forgetting is harmless.** A thread nobody declared lands in the background tier, where it
  belongs.
- **There is no separate "keep off the hot-path cores" step for the civetweb thread**, because the
  process-wide default already does it.
- **Difficulty 1 does not arise.** Nothing works out what is left from what has been claimed, so
  nothing needs to know when every process has started.

The alternative of giving each thread that wants pinning its own configured CPU bitmask, per
environment, fails in the opposite direction. It is work proportional to threads times
environments, and threads started inside libraries get forgotten. That is not something more
care would fix: when the absence of configuration means "run anywhere", and "anywhere" includes
the hot-path cores, every forgotten thread silently disturbs the most latency-sensitive thread in
the process.

### The Quill backend is pinned explicitly

The Quill backend thread starts when the first logger is constructed, before the configuration
naming the layout file has been read, so it is created before the process mask is applied and
does not inherit it. `apply_background_affinity()` therefore finds it with
`quill::Backend::get_thread_id()` and pins it to one particular background core, which `deploy.py`
allocates round-robin across the background tier so that the fifteen backends in the development
environment do not all share one core. A backend that stays on one core is also more predictable
than one moved around by the scheduler.

This is done in `apply_background_affinity()`, in `main()`, rather than in the reactor, because
the reactor's pinning returns early for a component that was not admitted, and the backend of
such a component would then be left unmasked.

### Applying the mask: the background_tier wrapper and the call in main()

The mask must be in place before the process creates any thread. Rather than depend on a
particular launcher — `devenv.py` now, possibly a scheduler or `systemd` in production — `deploy.py`
writes a wrapper script, `run/background_tier`, with the machine's background CPUs written into
it:

```sh
#!/bin/sh
# Generated by deploy.py -- do not edit.  Re-run deploy.py to recompute.
#   usage: background_tier <command> [args...]
exec taskset --cpu-list 16-31 "$@"
```

It takes the command to run as its arguments, so it needs to know nothing about any component.
`devenv.py` starts every component through it when it exists. Whatever invokes it — `devenv.py`,
`perf_run.py`, a scheduler, a person at a shell — gets the same behaviour.

It also does three things a process cannot do for itself:

- **It covers the JVM components.** `fix_test_client` and `admin_service` are JARs, and a JVM
  cannot set its own affinity portably. An affinity mask is **kept across `execve`**, so a
  `taskset` prefix constrains the JVM and every thread it will ever create — garbage collection,
  JIT, MINA's I/O pool — with no Java code.
- **It is one place to interpose a tool.** `taskset` goes outside and the tool inside
  (`background_tier valgrind ./binary_order_gateway`), so the mask applies to the tool and
  everything it starts.
- **It covers the time before `main()`.** A thread started by a library's static initialiser
  before `main()` runs would escape a call made from `main()`. Masking before `exec` leaves no
  such window.

**The wrapper is not the guarantee.** Each C++ component also calls
`apply_background_affinity()` early in `main()`, once its configuration is loaded, which masks the
whole process to the background tier. A binary started without the wrapper still lands in the
background tier and still promotes its own hot-path threads, so nothing on the production hot
path depends on how it was launched. The wrapper covers only what a process cannot reach from
inside itself: the JVMs and any thread started before `main()`.

### `fix_test_client` is the process that most needs this

It is a Java load generator and pins nothing. It is not on the edge of the measurement but in
it: in `dev.toml` it drives the `_a` instances of both gateways (`fix_gateway_port = 9879`,
`binary_order_gateway_port = 9890`), and it runs in dev, FT and NFT, where the performance
numbers are taken. Being a load generator, it keeps cores busy by design, at exactly the moment a
measurement is being made.

Unconstrained, it could be scheduled onto the very CPUs the gateway threads are pinned to,
because pinning restricts the pinned thread and keeps nobody else out, and the development
workstation has no `isolcpus`. Nothing would make that disturbance fall equally on both gateways,
so the comparison would measure the wrong thing. The wrapper confines it to the background tier.

### `--no-ha` needs no special handling

The layout is computed at deploy time over the **full** list of components. `--no-ha` is a
`devenv.py` option that skips components marked `ha_only = true`; the cores allocated to them
simply stay idle. So the full and reduced deployments get identical allocations, and **nothing in
the allocation needs to know what a high availability component is**. Only `devenv.py` does, and
it reads `ha_only`.

Identifying high availability components by a name suffix instead would misclassify some:
`witness` and `arbiter_primary` are both `ha_only = true` without being anybody's secondary,
because they exist only to serve high availability. `ha_only` means "not needed when high
availability is off"; a naming rule would mean "is the secondary of something"; the two differ
exactly for components like these. A declared flag also allows an exception where a derived rule
does not.

### The registry is a record, and the layout file is the authority

`CpuRegistry` does not allocate. `deploy.py` does that, into one machine-wide layout file,
`run/cpu_layout.toml`, from which each component reads its own entry. That is better for an
operator than the same facts spread across every component's configuration, and it gives the
audit one authority to check against. The layout file holds both tiers, including the core each
Quill backend is pinned to.

The registry still records what each process pinned. That is the only thing that can see across
two installations on one machine, each with its own layout file and each correct on its own, that
have handed out the same core. Such a collision is logged as an error.

### Checking it: cpu_audit.py

The invariant is that *no thread outside the declared hot-path set has a mask that includes a
hot-path core*. It is checked rather than assumed, because a bypassed wrapper or a library that
sets its own affinity would otherwise fail silently. Nothing can prevent a thread changing its own
mask, so the answer is detection.

A check from inside one process is not enough, because the JVMs cannot be checked from inside.
`cpu_audit.py` reads every running thread's mask from `/proc/<pid>/task/<tid>/status`, compares it
with the layout, names any offending thread, and exits non-zero so that a performance run can be
gated on it. It was tested by deliberately moving a `fix_test_client` thread onto the gateway's
core, which it reported by thread, core and owner. How it also samples where threads actually ran
is described in [CPU Pinning](cpu_pinning.md).

---

## Not done: moving a promoted follower onto hot-path cores

When the arbiter promotes a follower that is in the background tier, its threads should move to
the hot-path tier. Affinity can be changed at any time, and with a declared layout this would mean
"take over the failed leader's cores". It is not done: the static layout comes first and is to be
measured first. The design is still correct without it — a promoted `matching_engine_secondary`
runs on background cores until it is restarted, which is slower under a condition that is already
degraded, not wrong.

---

## Approaches that do not work

Recorded so that they are not proposed again without a new argument.

**1. Work out the unclaimed cores once, when the metrics thread starts.** Simplest possible
change. Because of Difficulty 1 it protects almost nothing for any component that starts early,
and it fails silently.

**2. Wait until the registry stops changing.** Apply the mask once the registry has been unchanged
for some time T, and re-apply it if it changes again. T is arbitrary, and the mask is wrong until
it settles, when the layout design is right from the start.

**3. Wait for a configured number of claimants.** A configured number is an upper bound, not an
expectation; the two agree only when every component runs. `devenv.py --no-ha` runs fewer, so a
number configured for the full deployment is never reached and everything waiting blocks forever.
"Processes running" is in any case the wrong quantity, since several components claim nothing.

**4. Have the launcher declare the layout final.** Each process records "claiming complete", and
`devenv.py` writes a "layout final" flag once all have done so. Sound at a cold start, but a
restarted component claims again from whatever is free and can change the layout after it was
declared final. Restarting components is routine here (`ha_test.py` does it constantly), so this
is fatal.

**5. Declare the layout directly as CPU bitmasks in configuration.** Removes the race and survives
a restart, but a bitmask states the *answer* rather than the *intent*: it is specific to one
machine, must be worked out again for every environment and every machine shape, and cannot be
checked against anything. Declaring a rank and letting `deploy.py` turn it into core ids for the
target host survives a hardware change; a bitmask does not.

**6. Control the start order.** Not the same as declaring the allocation. A controlled order makes
a cold start predictable and does nothing for a restart, because a component killed by
`ha_test.py` comes back outside any controlled sequence. It makes the fault harder to reproduce
rather than fixing it.

---

## Open questions

1. Does a reduced (`--no-ha`) deployment keep the freed cores idle — as designed, so that the two
   deployments are comparable — or is there ever a reason to want a denser allocation?
2. Does case B need anything stronger than printing the layout? A per-machine
   `required_hot_path_rank = N` has been considered and set aside, for three reasons. The rank
   scale is global and the setting would be per machine, so on a dedicated host, where only one or
   two ranks are present, "ranks 1 to N" says nothing about the ranks with no components there.
   It is also a number that would have to be kept consistent with the rank table by hand. And on
   the deployments in hand it could never fire. If an assertion is ever wanted, the form that
   means the same on every host is a yes-or-no: *every ranked component on this machine must be
   admitted*. The case to design it for is several ranked components consolidated onto a small
   host — for example both gateways and both sequencers on an 8-core machine, where rank 1 takes
   four cores and rank 2 is silently left out for want of two more.
3. **A declared thread count that is too high is not reported.**
   `Reactor::verify_hot_path_thread_count()` catches a component that registers more threads than
   its `--hot-path-thread-count` constant declares, but not the opposite: a constant left at 4
   when the component registers 2 wastes cores on every instance and the layout looks healthy.
   The asymmetry is deliberate for now — declaring too many costs cores, declaring too few costs a
   shared hot-path core — but too high is still a defect that nothing reports. The natural place
   to report it is the audit, which already knows which allocated cores have no thread pinned to
   them.
4. **Three questions about the environment files themselves.** None affects the mechanism; all
   three affect whether the declared deployment is the intended one.
   - **Both `_a` gateways are placed on one host** (`gateway.<env>.exchange.internal`), because that
     is what the `*_host` values say. That contradicts `prod.toml`'s header comment that each
     component runs on its own dedicated host. Either the comment or the placement is wrong. The
     layout handles it correctly: the two share a rank, so they are admitted together or not at
     all.
   - **`admin_service`'s host, `admin.<env>.exchange.internal`, is a placeholder** marked
     `# REPLACE`, with nothing yet to confirm it.
   - **The machine keys repeat the `*_host` values.** Both are marked `# REPLACE` and must be kept
     consistent by hand. Deriving one from the other would be a `deploy.py` change.

---

## See Also

- [CPU Pinning](cpu_pinning.md) — the mechanism, the files `deploy.py` writes, and the audit
- [WAL and High Availability](../availability/wal_and_ha.md) — the two-tier commit, and why the
  sequencer follower is on the critical path
- [Roadmap](../roadmap.md) — item 16, the gateway comparison and Prometheus metrics
