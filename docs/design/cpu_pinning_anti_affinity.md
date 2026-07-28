# CPU Core Layout — Declared Allocation and Background by Default

**Status: design agreed 2026-07-27; ranks and reserve settled and the design implemented
2026-07-28.** Live-verified on the 32-core development workstation: every ranked component landed on
the cores the layout allocated it, `matching_engine_secondary` was demoted at rank 5 with its reason
logged, and both JVMs -- `fix_test_client` included -- start masked to the background tier.

This document began as a record of an open problem found while planning the Prometheus metrics
endpoint ([Roadmap](../roadmap.md) item 16). That problem is now resolved in design, and the agreed
shape is set out under "The agreed design" below. The original problem statement and the approaches
rejected along the way are retained, because the reasoning is the justification for the design and
re-covering the ground would be waste.

All three points that were originally marked **proposal** have since been ratified. The remaining
open questions are listed at the end.

It is a companion to [CPU Pinning](cpu_pinning.md), which describes the mechanism as built.

---

## The requirement, as originally posed

The Prometheus endpoint runs an embedded HTTP server (civetweb, inside `prometheus-cpp`) on a
background thread that an external scraper connects to. That thread does blocking I/O on a
timescale of scrape intervals and has no business sharing a core with a hot-path thread.

So it needed the inverse of the existing facility. The framework can say *pin thread T to core C*
(`pin_thread_to_core`, `pin_tid_to_core` in `CpuPinning.hpp`). What was wanted is *restrict thread
T to whatever cores nobody has pinned* — anti-affinity against the CPU registry.

Two parts of this were never difficult:

- **Applying the mask.** `sched_setaffinity` takes a CPU set, so a multi-core mask costs no more
  than a single-core one. The natural formulation is a positive affinity mask over the complement
  of the claimed set, rather than anything genuinely "negative".
- **Reaching the thread.** The civetweb thread's OS thread id is obtainable, so no `/proc/self/task`
  walking or affinity-inheritance trickery is required.

Determining *which cores* belong in the mask was the whole problem — and the design below dissolves
the question rather than answering it, by never computing a complement from observed claims at all.

---

## Why the registry cannot answer the question

`CpuRegistry` (see [CPU Pinning](cpu_pinning.md)) is an `mmap`'d table of `(core_id, owning_pid)`
entries guarded by an `flock`. `claim_cpus()` evicts entries whose pid is dead, computes the cores
with no live owner, sorts to prefer P-cores, and takes the first N.

That makes the registry a **record of what has happened**, not a **statement of what will
happen** — and the anti-affinity mask needs the latter.

### Difficulty 1: the complement is only correct once all claiming has finished

Claiming happens at each process's startup, in `Reactor::pin_registered_threads()`. The registry's
contents are therefore a function of *when you look*, and no process can know when the last
claimant has arrived:

- A process that starts early sees an almost-empty registry. Its "unpinned" set is nearly every
  core, including the ones pinned seconds later. The mask is then a no-op that has nonetheless
  been applied successfully — the failure is silent.
- There is an intra-process ordering hazard as well: the Reactor claims during `initialize()`, so
  a thread created before that point cannot see even its own process's pins.
- If every core is claimed, the complement is empty. An empty affinity mask is `EINVAL`; a thread
  permitted to run nowhere is not a thread.

A process that has not started yet leaves no trace, so this is not a matter of looking harder.

### Difficulty 2: greedy claiming gives the wrong cores to the wrong components

The P-core preference in `get_available_cpu_ids()` works, but it applies to *whatever is left when
a process happens to start*. Allocation therefore follows `devenv.py` start order rather than
importance, and the gateways start last.

Measured 2026-07-26 on the development workstation, same binaries and same configuration:

| Deployment | `OrderGatewayThread` | `BinaryGatewayThread` |
|---|---|---|
| Full HA — 8 claimants, 24 threads | CPU 19, **E-core** | CPU 22, **E-core** |
| `devenv.py --no-ha` — 5 claimants, 15 threads | CPU 10, **P-core** | CPU 13, **P-core** |

The full-HA allocation in start order: `matching_engine_primary` 1-3, `matching_engine_secondary`
4-6, `sequencer_primary` 7-9, `sequencer_secondary` 10-12, `mep_primary` 13-15 — which exhausts
the 15 claimable P-cores — then `mep_secondary` 16-18, `order_gateway` 19-21, `binary_gateway`
22-24, all E-cores.

This directly threatens the comparison that item 16 exists to enable. The two gateways run the
same venue over different client protocols with common code downstream, so the whole point is a
like-for-like measurement. If their threads sit on different core types the comparison measures
core type. Worse, **the symmetry that makes it valid at all is an accident of arithmetic**: the
P/E boundary falls wherever the cumulative thread count reaches 15, so adding one thread anywhere
upstream would split the pair — one gateway on a P-core, the other on an E-core, within a single
run, with nothing in the output to say so.

Note the parallel with the warning already recorded under item 16 in the summary: if one gateway
is instrumented more heavily than the other, the comparison measures the instrumentation. This is
the scheduling form of the same trap.

### Difficulty 3: the shortfall

Twenty-four threads want fifteen P-cores, so nine must land on E-cores whatever policy is chosen.
Two obvious places to economise: the Quill backend threads (eight of them, genuinely off the hot
path by design) and the HA followers (six threads). The backends are straightforwardly available.
The followers need care, because **"secondary" is not a uniform latency class in this system.**
Verified in the code:

- **The sequencer follower is synchronously inside the client round trip.** Under the two-tier
  commit described in [WAL and High Availability](wal_and_ha.md), the leader parks each execution
  report in `pending_er_` and releases it to the gateway only when the matching `WalAck` arrives
  (`SequencerThread.cpp:558` and `:1076`). The follower's wake-up latency is therefore added to
  every order's round trip — measured p90 wake-up for that thread is around 354 us. A follower on
  a slow core slows the *leader's* client-visible responses.
- **The matching engine secondary is not.** `send_book_update()` is fire-and-forget
  (`MatchingEngineThread.cpp:453` and `:561`); the primary does not wait for the secondary to
  acknowledge anything. The secondary merely tails the book. Same HA pattern as the sequencer,
  opposite conclusion.

This pair is the reason the design carries a declared ranking rather than deriving one. No tool can
infer the `WalAck` fact from placement or from the code, and the tempting rule — "demote a
secondary when it is co-located with its primary" — gets the matching engine right and the
sequencer catastrophically wrong.

---

## Process taxonomy: mandatory is not the same as latency-critical

A production deployment needs to know which processes will run on a given machine, and which of
those are mandatory for an operational system. That is the right question to ask, and the
per-machine half of it is what makes the layout computable at all.

But the *mandatory* flag must not also drive core allocation, because the two properties are
orthogonal. The sequencer secondary is the counterexample that proves it: the venue trades
perfectly well without it, so it is not mandatory, while its acknowledgement gates every execution
report, so it is latency-critical. A single flag serving both purposes would place it on an E-core
and slow every order in the system.

| process | mandatory for trading | latency-critical | basis |
|---|---|---|---|
| `order_gateway`, `binary_gateway` | yes | **yes** | client edge |
| `sequencer_primary` | yes | **yes** | the sequencing point |
| `sequencer_secondary` | **no** | **yes** | `WalAck` gates every ER |
| `matching_engine_primary` | yes | **yes** | matching |
| `matching_engine_secondary` | **no** | **no** | `BookUpdate` is fire-and-forget |
| `mep_primary`, `mep_secondary` | undecided | no | downstream of the ER path |
| `auth_service_a`, `auth_service_b` | yes | no | logon only, not per order |
| `arbiter_primary`, `arbiter_secondary`, `witness` | for resilience, not trading | no | election and failover only |
| `admin_service` | no | no | operator UI |
| `fix_test_client` | no | no | load generator; dev, FT and NFT only |

Two independent axes: **mandatory**, driving readiness and health checks and whether the launcher
may declare the system operational; and a **latency rank**, driving which core tier a process's
threads receive. They agree on most rows, which is precisely why one flag would look adequate while
quietly misplacing the row that matters.

Only the latency rank is part of this design. *Mandatory* is left for whoever builds readiness
checks, and may not be cleanly binary — the arbiters are not needed for the venue to keep trading,
only for it to survive a failure, which suggests an enum rather than a boolean.

---

## Why development is the environment that matters

`prod.toml`, `preprod.toml` and `test-1.toml` all run **one component per dedicated host**. There
is no contention there: a single claimant takes what it needs from a whole machine. Production
hosts are also candidates for `isolcpus` / `nohz_full` and a low-latency kernel.

`dev.toml` runs everything on one workstation — fifteen components, eight of which pin — with no
CPU isolation and a stock kernel.

Every difficulty above is therefore **development-only**, which invites the conclusion that it
matters less. The opposite holds: all latency measurement and all protocol comparison happens in
development. Production is where the system runs; development is where its numbers come from. A
design that is sound in production and indeterminate in development yields a system that cannot be
characterised.

This does constrain the solution, though — production must not be made to carry configuration
whose only purpose is the development case. The design below satisfies that: the ranking is
declared once and is machine-invariant, and on a dedicated production host it never binds.

---

## The agreed design

### The two halves

The decisive observation is that "should this instance get hot-path cores" is not one quantity but
two, and conflating them is what made earlier attempts awkward:

- **Rank — declared, machine-invariant.** The order in which entitlement to a hot-path core is
  given up when a machine is short. This is domain knowledge: the sequencer follower's `WalAck`
  gates every execution report, the matching engine secondary's `BookUpdate` does not. That fact is
  true in every environment and never varies by deployment.
- **Cut point — computed, per machine.** Where the pool runs out. A function of the machine's
  population and its actual core topology, both of which `deploy.py` can determine on the target
  host.

On a dedicated production host the cut point falls below everything, the rank never binds, and a
secondary receives exactly what its primary receives — with no per-environment configuration and no
special case. On the development workstation the cut point lands somewhere real and the rank
decides who is above it.

This is why a *class* ("this component is background") would have been wrong. There is no reason to
withhold a P-core from `matching_engine_secondary` on a host where nothing else wants one. Demotion
is a consequence of contention, not a property of the component.

### Declared input 1: which processes run on which machine

A new `[machines.*]` section in the environment TOML:

```toml
[machines.localhost]
components = [
    "auth_service_a", "auth_service_b", "witness",
    "arbiter_primary", "arbiter_secondary",
    "matching_engine_primary", "matching_engine_secondary",
    "sequencer_primary", "sequencer_secondary",
    "mep_primary", "mep_secondary",
    "order_gateway", "binary_gateway",
    "admin_service", "fix_test_client",
]
# minimum_background_cores omitted: this workstation is hybrid, so the P-core
# ceiling caps hot-path at 15 and the 16 E-cores are background regardless.
```

```toml
[machines.matching-engine-1.exchange.internal]
components = ["matching_engine"]
minimum_background_cores = 2
```

Points of substance:

- **`localhost` is a recognised machine name**, used by `dev.toml` where everything runs on one
  workstation. It needs no resolution and carries no `# REPLACE`.
- **The list must name every process on the machine, not only those that pin.** Seven of the
  fifteen dev components claimed nothing under the old scheme, and so were free to run *anywhere*,
  including on the cores the gateways were pinned to. (Resolved in implementation by making
  `cpu_pinning_enabled` mean "take part in the machine's layout" and setting it on every component;
  one that pins nothing is simply unadmitted and stays in the background tier.)
  They are listed because they need a background mask, which is the point they were previously
  missing.
- Absolute counts rather than fractions: clearer to reason about, and there will be few machines.

#### What `minimum_background_cores` means

The two tiers are two *ways of using a core*, not used versus unused:

| tier | occupancy |
|---|---|
| hot-path | **dedicated** — one thread per core, exclusively |
| background | **shared** — ordinary multitasking, many threads per core |

So background cores are not free or idle; they are where everything else runs. The setting asks how
much of the machine stays ordinary shared multitasking, at minimum. It is **a floor on the size of
the background pool**, not an apportionment of the machine, and it is small on every machine
regardless of size:

| machine | dedicated | shared | minimum |
|---|---|---|---|
| prod matching-engine host, 20 cores | 2 | 18, holding 1-2 threads | 2 |
| development workstation, 32 cores | 14 | 17, holding ~25 threads plus two JVMs | omitted — see below |
| work machine, 20 cores uniform | 10 | 9 | 4-6 |
| 8-core VM | 4 | 4 | 2 |

**Its first job is correctness, not tuning: the background pool must never be empty.** Every process
has at least a Quill backend that must run somewhere, and an empty affinity mask is `EINVAL` — a
thread permitted to run nowhere is not a thread. On an 8-core VM, rank 1 takes four cores leaving
four; rank 2 wants four more, which would leave zero. The floor is what refuses that.

**It does not bind on a hybrid machine, so `dev.toml` omits it.** Verified on the development
workstation via `acpi_cppc/highest_perf`: cores 0-15 report 70 or 74 (P-cores, eight physical with
hyperthreading), cores 16-31 report 43 (E-cores). With cpu0 reserved that is 15 claimable P-cores
and 16 E-cores. Hot-path threads only ever occupy P-cores, so the hot-path pool can never exceed 15
and the background pool can never fall below 16 — the P-core ceiling always binds first, and any
value from 0 to 16 yields an identical layout. A setting that cannot affect the outcome on the
machine it is written for is worse than no setting, so it is omitted and `deploy.py` warns if one
appears on a hybrid host.

**It does not bind in production either**, where a host runs one component wanting two cores out of
twenty. It is therefore a development-and-VM safety valve: it constrains a **uniform-core** machine,
where every core reads as `CoreType::Unknown`, is treated as a P-core, and nothing else stops
hot-path growth from consuming the host. Missing on a uniform-core machine is a hard error. The
20-core worked example below is that case.

A bonus that falls out and is worth taking: `prod.toml` currently repeats hostnames across roughly
twenty `# REPLACE` entries — `sequencer-primary.exchange.internal` appears five times. With a
machine manifest those become derivable from the machine each component sits on, in the same place
`deploy.py` already derives registry paths and WAL directories.

### Declared input 2: the rank

On `[components.*]`, alongside `ha_only`, because it is a per-instance property and `dev.toml`
already gives each instance its own entry:

```toml
[components.order_gateway]
binary        = "bin/order_gateway"
config        = "etc/order_gateway/order_gateway.toml"
workdir       = "etc/order_gateway"
ha_only       = false
hot_path_rank = 1

[components.sequencer_secondary]
ha_only       = true
hot_path_rank = 2          # WalAck gates every ER; not demotable

[components.matching_engine_secondary]
ha_only       = true
hot_path_rank = 5          # BookUpdate is fire-and-forget; first to yield
```

**Absence of `hot_path_rank` means background.** Forgetting to rank a component places it where it
almost certainly belonged, and the worst outcome is a background thread on a background core. This
is the same default-value discipline the rest of the design rests on.

The ranking, following the reasoning in Difficulty 3 and the taxonomy table:

| rank | components | hot-path threads |
|---|---|---|
| 1 | `order_gateway`, `binary_gateway` | 4 |
| 2 | `sequencer_primary`, `sequencer_secondary` | 4 |
| 3 | `matching_engine_primary` | 2 |
| 4 | `mep_primary`, `mep_secondary` | 4 |
| 5 | `matching_engine_secondary` | 2 |
| — | everything else | background |

These values are **settled**, together with `minimum_background_cores = 6` for a consolidated
deployment on a uniform machine. The two were decided together because they are one decision: the
ranking alone does not say where the cut falls, and on the 32-core workstation the P-core ceiling
binds before any floor does, so the reserve only becomes visible on the smaller uniform machine. The
combination places the MEPs below the cut there and above it on the workstation. That is deliberate:
it reproduces the tiering chosen by hand when Difficulty 3 was first analysed, on the machine that
motivated the choice, without anyone maintaining a second set of numbers.

The cost accepted is that publish-to-receive latency on the 20-core machine is measured with the
publisher in the background tier. The alternative — a reserve of 5, admitting rank 4 — was rejected
because it leaves five background cores to absorb thirteen Quill backends, both JVMs and
`fix_test_client` under NFT load, which would contaminate the measurement more than an unpinned MEP
does.

### Ties are a constraint, not merely an ordering

**A rank group is admitted whole or not at all.** Both gateways are rank 1, so either both are above
the cut or both are below it. They can never be split.

This is the point of the whole encoding. Today the gateway symmetry is, per Difficulty 2, an
accident of arithmetic — one extra thread anywhere upstream splits the pair mid-run with nothing in
the output to say so. Under whole-group admission an extra thread demotes the *lowest-ranked entire
group*. The comparison that item 16 exists to enable stays valid by construction rather than by
luck.

### Thread counts stay in the code; the TOML never declares them

**The application knows how many threads it registers with the Reactor, and the environment TOML
must not need to know.** A count in configuration is a second source of truth that drifts the moment
someone adds a thread, and drifts silently.

Under this design the quantity is simple and stable:

> hot-path demand = the reactor thread + the registered `ApplicationThread`s

Threads registered through `register_extra_thread()` are **background by default**, like any other
thread the process creates, and so do not enter the calculation. Every component currently registers
exactly one `ApplicationThread` (`MatchingEngine.cpp:62`, `Sequencer.cpp:72`,
`OrderGateway.cpp:67`, `AuthenticationService.cpp:51`), so every ranked component wants two cores.

That extras are excluded is not a simplification for its own sake — it removes a real hazard.
`OrderGatewayThread` registers `FixCaptureWriter` **conditionally**, on
`config.fix_capture_enabled`. So today the same binary has different core demand depending on a
configuration flag (`Reactor.cpp:549` adds `1 + thread->get_extra_threads().size()` per thread). Any
figure written into the environment TOML would be correct for one setting of that flag and silently
wrong for the other. With extras in the background tier, `order_gateway` wants two hot-path cores
either way, and `FixCaptureWriter` — a file-writing thread — stops consuming a dedicated core, which
it should never have had.

`deploy.py` therefore asks the binary rather than reading a number: it runs on the target host, so
the binary is present and can be queried (`bin/sequencer --hot-path-thread-count`). This is backed
by a fail-loud check at component startup when fewer cores have been assigned than the component
needs, so a stale layout is diagnosed rather than absorbed.

### Resolution: what `deploy.py` computes

`deploy.py` takes no host argument and runs on the machine it is installing to, so it can read
`/sys/devices/system/cpu` directly and see the real topology — 32 cores on the development
workstation, 20 on the work machine, fewer on a VM. The same declaration resolves differently on
each, and re-running deploy after a hardware change recomputes it. This is the reason for declaring
a *rank* rather than a CPU bitmask: a bitmask states the answer, is machine-specific, must be
re-derived for every environment, and offers no way to check one encoding against another.

```
claimable = online cores, minus cpu0 when reactor_cpu_pinning_reserve_cpu0
groups    = components on this machine having hot_path_rank, grouped by rank, ascending
hot_path  = []

for group in groups:
    demand = sum of hot_path_thread_count over the group
    if enough P-cores remain for demand, and
       claimable - |hot_path| - demand >= minimum_background_cores:
        admit the group, allocating P-cores first
    else:
        stop

background = claimable - hot_path
```

Two constraints, both of which must hold, and **`stop` rather than `skip`**: once a group does not
fit, no lower-ranked group is considered either. Otherwise a small low-rank group could leapfrog a
larger high-rank one, which would be surprising and would undermine the point of ranking.

On a hybrid machine the hot-path pool is drawn from P-cores only. On a uniform machine every core
reads as `CoreType::Unknown`, which `get_available_cpu_ids()` already treats as a P-core, so the
reserve is the binding constraint instead.

### When demand plus the background floor exceeds the machine

Four cases, distinguished because they need different responses.

**A — some rank groups fit, some do not.** The designed case, and not an error: admission stops at
the first group failing either constraint and everything below goes to background. The 20-core
worked example is this.

Note this differs from what the code does today. `Reactor.cpp:552-562` logs a `Warning` on shortfall
and pins whatever it can, so *which* threads miss out falls out of map iteration order — the
arbitrary splitting this design exists to prevent. Whole-group admission replaces it, and the
decision is taken once at deploy time and visible in one place, rather than discovered at startup
across fourteen log files.

**B — not even rank 1 fits.** For example a 4-core VM with `minimum_background_cores = 2` and rank 1
wanting four threads: `4 - 0 - 4 = 0`, below the floor, rejected, stop. **Nothing is pinned and the
whole machine is background.**

That is the correct outcome. Admitting rank 1 partially would pin one gateway and not the other,
which is precisely the invalid comparison whole-group admission exists to prevent, and the system
runs correctly unpinned — it is what disabling pinning altogether gives.

It is also legitimate on a functional-test VM and alarming on a production host, and nothing in the
layout tells those apart. So **the computed layout must be reported prominently by `deploy.py` and
recorded in the layout file**, naming every rank group that was demoted and why. A demotion that
appears in the layout file is diagnosable; one that appears only as unexplained latency is not.

Whether anything stronger than reporting is needed — an assertion that `deploy.py` refuses to
install a layout dropping something the deployment declared it needs — is open question 6. It is
deliberately *not* in the schema above, because on the deployments in hand it would be unreachable:
a dedicated production host running `matching_engine` wants two cores out of twenty, so admission
fails only when fewer than four cores are claimable, which is already case C or case D. The case
where such an assertion could genuinely fire is consolidation — several ranked components on one
small host, for example both gateways and both sequencers on an 8-core VM, where rank 1 takes four
and rank 2 is silently demoted for want of two more.

**C — `minimum_background_cores` is greater than or equal to the claimable core count.** Not a
shortfall but a nonsensical configuration: no group can ever be admitted whatever the demand. Hard
error at deploy time.

**D — the machine changes shape after deployment.** New with a declared layout, and worth naming
because the negotiated registry discovered cores at runtime and this does not. If the layout was
computed for 20 cores and the machine later has 16 online — cores offlined, a VM resized, hardware
replaced — then `taskset -c 16-31` fails and the wrapper does not start the process, and
`pthread_setaffinity_np` against a nonexistent core returns `EINVAL`. Both must be startup
**errors**, not warnings; the remedy is to re-run `deploy.py`. Refusing to start is correct: a
latency-critical component running under a layout computed for different hardware is worse than one
that does not run.

### Worked examples

**Development workstation — 32 cores, 15 claimable P-cores, cpu0 reserved:**

| rank | demand | cumulative | admitted |
|---|---|---|---|
| 1 | 4 | 4 | yes |
| 2 | 4 | 8 | yes |
| 3 | 2 | 10 | yes |
| 4 | 4 | 14 | yes — 14 of 15 P-cores |
| 5 | 2 | 16 | **no** — exceeds the P-core pool |

`matching_engine_secondary` goes to background, everything above it is on a P-core, and both
gateways are rank 1 on P-cores in both the full-HA and `--no-ha` deployments. That is the
E-core/P-core flip in Difficulty 2's table, fixed.

The background pool is then the one spare P-core plus all sixteen E-cores — seventeen cores, for:

| source | threads |
|---|---|
| Quill backends, thirteen C++ components | 13 |
| five non-pinning C++ components (two auth services, witness, two arbiters) — reactor + app | ~10 |
| `matching_engine_secondary`, demoted at rank 5 | 2 |
| `admin_service` JVM — Jetty pool, GC, JIT | dozens, nearly all idle |
| `fix_test_client` JVM — MINA pool, GC, JIT | dozens, **saturating under NFT load** |

Comfortable on thread count. The one that genuinely consumes the tier is `fix_test_client`, which is
the intended outcome: it is where it belongs rather than on CPUs 19 and 22.

**Work machine — 20 uniform cores, cpu0 reserved (19 claimable), reserve 6:**

| rank | demand | cumulative | background left | admitted |
|---|---|---|---|---|
| 1 | 4 | 4 | 15 | yes |
| 2 | 4 | 8 | 11 | yes |
| 3 | 2 | 10 | 9 | yes |
| 4 | 4 | 14 | 5 | **no** — below the reserve of 6 |

The MEPs go to background and ten threads are hot-path. Note that this reproduces, on the smaller
machine, exactly the tiering that was chosen by hand when Difficulty 3 was first analysed — and the
machine that motivated that choice was the smaller one. The policy arrives at the same answer
without anyone maintaining it.

### The runtime mechanism: background by default, promotion by exception

The requirement was "restrict this thread to cores nobody has pinned". The better formulation, and
the one adopted, is: **make background cores the default for every thread in the process, and treat
hot-path placement as something a thread must be explicitly given.**

1. The process's affinity is set to the background pool **before it starts creating threads**.
2. Every thread created afterwards **inherits that mask automatically**. This is documented Linux
   behaviour — `pthread_create` gives the new thread a copy of the creator's mask — and was
   verified on the development machine: a parent restricted to cores 28 and 30 produced a child
   reporting exactly `28 30`.
3. The Reactor then explicitly pins the threads that earn hot-path cores — each `ApplicationThread`
   and the reactor thread itself — overriding the default for those alone.

Promotion works because **an affinity mask is not a ratchet.** `sched_setaffinity` is bounded by
the process's cgroup cpuset, not by its current mask, so a thread may widen its own affinity back
out to a hot-path core. This is the reason the design uses `taskset` and not cgroup cpusets: a
cpuset is a hard ceiling and promotion into a core outside it would fail. Cpusets remain the
stronger tool if genuine exclusion is ever wanted, but then the hot-path cores must sit inside the
same cpuset as the threads being promoted into them.

What this buys:

- **Library threads never need to be known about.** No enumeration, no per-thread configuration, no
  `/proc/self/task` sweep at the point of use. `prometheus-cpp`'s civetweb thread, a future Kafka
  client, whatever a library spawns in its next release — all safe without anyone noticing.
- **Forgetting is harmless.** An undeclared thread lands in the background tier, which is where it
  belonged.
- **The anti-affinity requirement dissolves.** There is no negative pin to apply to the civetweb
  thread, because the process-wide default already is one.
- **Difficulty 1 disappears.** Nothing computes a complement from observed claims, so nothing needs
  to know when machine-wide claiming finished.

This is the failure mode of the author's workplace scheme, avoided. There, every thread wanting
pinning has a per-environment configuration variable holding a CPU bitmask — O(threads ×
environments) to maintain, and threads spawned inside libraries get forgotten. That is not a
diligence failure that more care would fix; it is a **default-value bug**, in which the absence of
configuration means "run anywhere" and "anywhere" includes the hot-path cores. A scheme requiring
complete enumeration of third-party threads will fail eventually, and its failure is silent
interference with the most latency-sensitive thread in the process.

### Quill is pinned explicitly, not left to inheritance

The Quill backend threads are *not* left to float within the background pool. `Reactor.cpp:630`
already locates the backend by tid via `quill::Backend::get_thread_id()` and calls
`pin_tid_to_core`; that call stays, aimed at a **background** core instead of a claimed hot-path
one. Backends pinned to specific background cores are more deterministic than backends drifting
under the scheduler.

Note the count changes under this design. The earlier analysis said eight Quill backends, counting
only the eight components that pin. Background by default masks **every** C++ component on the
machine, and `dev.toml` has thirteen of those — fifteen components less `admin_service` and
`fix_test_client`, which are JVMs. Thirteen backends, not eight.

The consequence, which must not be overlooked: **if anything is pinned explicitly into the
background tier, the background tier needs allocating too.** `CpuRegistry` today allocates only
what gets pinned, which is the hot-path set. See open question 2.

The reactor thread and every `ApplicationThread` are pinned explicitly, as now.

### Applying the mask: a generated wrapper script

The mask must be in place before the process creates any thread. Rather than depend on a particular
launcher — `devenv.py` today, possibly schedulix or something else in production, undecided — the
design puts it in a **launch wrapper generated by `deploy.py`**, with that machine's resolved core
list baked in:

```sh
#!/bin/sh
# bin/run_binary_gateway  --  generated by deploy.py for host <name>
exec taskset -c 16-31 /opt/pubsub/bin/binary_gateway "$@"
```

Whatever invokes it — schedulix, systemd, `devenv.py`, `perf_run.py`, a person at a shell — gets
identical behaviour. The launch mechanism becomes irrelevant, which is what is wanted while it is
still undecided, and choosing a production scheduler later costs nothing.

Three further benefits:

- **It covers the JVM components.** `fix_test_client` and `admin_service` are JARs; a JVM cannot set
  its own affinity portably, but an affinity mask is **preserved across `execve`**, so a `taskset`
  prefix constrains the JVM and every thread it will ever create — GC, JIT, MINA's I/O pool — with
  no Java code, no JNI, no library.
- **It is an interposition point.** A component can be run under `perf`, `valgrind` or `gdb` by
  editing one generated script. The ordering works out: `taskset` outside, tool inside
  (`exec taskset -c 16-31 valgrind ./binary_gateway`), so the mask applies to the tool and
  everything it spawns.
- **It closes the pre-`main()` hole.** Threads created by a third-party static initialiser before
  `main()` runs would escape an in-process call. Masking before `exec` means there is no window.

`devenv.py` currently injects `LD_LIBRARY_PATH` at `start_one()` to locate `libpubsub_itc_fw.so`.
That is a natural thing to move into the wrapper as well, since it has the same "however you launch
it, launch this" character — but it is an opportunity, not a requirement of this design.

**The wrapper is not the guarantee.** Each C++ component also sets its own process affinity to the
background pool early in `main()`, immediately after configuration is loaded and before any
subsystem is initialised. A binary launched bare, bypassing the wrapper, still lands in the
background tier and still promotes its own hot-path threads. So nothing in the production hot path
depends on launcher cooperation; the wrapper covers only what a process cannot reach from inside
itself — the JVMs and any pre-`main()` threads.

### `fix_test_client` is the process that most needs this

It is a Java load generator with `cpu_pinning_enabled` effectively absent — it pins nothing. That
was previously read as "it cannot be helped, and it is only a test program". Both halves are wrong:

- `taskset` at launch constrains it completely, with no Java changes.
- It is not peripheral to the measurement, it is *on* it. `dev.toml:437` shows it driving both
  gateways (`fix_gateway_port = 9879`, `binary_gateway_port = 9890`), and it exists in dev, FT and
  **NFT** — the environment where the performance numbers are taken. It is a load generator, so it
  is saturating cores by design, at precisely the moment a measurement is being made.

Unconstrained, it can be scheduled onto CPUs 19 and 22 while `OrderGatewayThread` and
`BinaryGatewayThread` are pinned there — because pinning restricts the pinned thread and excludes
nobody, and the development workstation has no `isolcpus`. Nothing makes that contamination land
equally on both gateways. It is the same shape as the logging asymmetry already recorded under item
16: instrument, or contaminate, both equally or the comparison measures the wrong thing.

### `--no-ha` needs no special handling

The assignment is computed at deploy time over the **full** manifest. `--no-ha` is a runtime flag
that makes `devenv.py` skip components marked `ha_only = true` (`devenv.py:77`); their pre-assigned
cores simply sit idle.

Identical assignment across the two deployments therefore falls out for free, and **nothing in the
allocation path needs to know what an HA component is.** Only `devenv.py` needs that, and it
already has `ha_only`.

This also disposes of a heuristic that was considered and rejected: identifying HA components by
name suffix. It would misclassify two of dev.toml's fifteen — `witness` and `arbiter_primary` are
both `ha_only = true` while being nobody's secondary, because they exist *only* to serve HA. The
declared flag means "not needed when HA is off"; the naming rule would encode "is the secondary of
something"; those differ exactly for HA infrastructure with no non-HA counterpart. A derived rule
also has no escape hatch for a component that does not fit, where a declared flag does.

### Verification: a machine-wide affinity audit

The invariant is *no thread outside the declared hot-path set has a mask intersecting the hot-path
pool*. It should be checked, not hoped for — a bypassed wrapper would otherwise fail silently,
which is the failure mode this whole document keeps warning about.

An in-process `/proc/self/task` sweep is not sufficient, because the design now includes processes
that cannot be checked from inside — the JVMs. The check therefore wants a machine-wide form: read
`Cpus_allowed_list` from `/proc/*/task/*/status` and report anything overlapping the hot-path pool
that is not a declared hot-path thread.

A natural Python diagnostic, per the project's script conventions. It names the offending pid
instead of leaving unexplained jitter to be investigated later.

---

## Proposals within the design

All three have been ratified. Proposal 1 was folded into the design above; 2 and 3 are recorded here
because they are what the implementation builds, and because the reasoning for each is worth keeping
next to the alternatives it displaced.

**Proposal 2 — `CpuRegistry` becomes a record and collision detector.** Allocation moves to
`deploy.py`, so the registry stops negotiating. It still earns its place: catching two installations
on one machine claiming the same core is a real service, and the runtime record is what the audit
above compares against. This costs a rework of the tests in `CpuRegistryTest.cpp` that currently
assert negotiation behaviour.

**Proposal 3 — one machine-wide layout file, written by `deploy.py` into `run/`.** Each component
reads its own entry. Better for an operator than the same facts scattered across fourteen component
TOMLs, and it gives the audit a single authority to check against. It also answers the background
allocation problem raised by explicit Quill pinning, since background assignments live in the same
file — which is why open question 2 needed no separate answer: the layout file carries both pools,
and `CpuRegistry` does not grow a second one.

---

## Deferred

**Re-pin on promotion.** When the arbiter promotes a follower that is sitting in the background
tier, its threads should move to the hot-path tier. Affinity can be changed at any time —
`pthread_setaffinity_np` is not restricted to startup — and under a declared layout this becomes
"adopt the dead leader's assignment" rather than "reclaim whatever the dead leader released", which
is cleaner than the version considered before the layout was declared. `ha_test.py` kills and
restarts components routinely, so this is designed-for behaviour rather than an exception.

Deliberately deferred: the static scheme should land and be measured first. Note the design is
still correct without it — a promoted `matching_engine_secondary` runs on background cores until
restarted, which is a performance shortfall under an already-degraded condition, not an error.

---

## Approaches rejected along the way

Recorded so the ground is not re-covered.

**1. Compute the complement once, when the metrics thread starts.** Simplest possible change. Per
Difficulty 1 it is close to useless for any early-starting component, and it fails silently.

**2. Quiescence detection.** Apply the mask once the registry has been unchanged for T, re-applying
if it changes again. A heuristic, and T is arbitrary. Its redeeming argument was that it is
self-correcting and never worse than the status quo — but "time-to-correctness" is not a property
worth having when the alternative is correct at t=0.

**3. A barrier on a configured expected claimant count.** A configured maximum is an *upper bound*,
not an *expectation*, and the two coincide only at exactly full capacity. `devenv.py --no-ha` drops
three of the eight claimants, so a count configured for full HA is never reached and waiters block
forever. "Processes running" is in any case the wrong quantity — several components never claim,
and `register_extra_thread()` means components do not consume equal numbers of cores.

**4. Have the launcher close the layout.** Each process records "claiming complete"; `devenv.py`
writes a "layout final" flag once all claimants have recorded it. Sound at cold start, and it
degrades honestly. **Its weakness is mid-life restart**: a restarted component re-claims greedily
and can shift the layout after closure. Since routine restart is designed-for behaviour here, that
is fatal.

**5. Declare the core layout directly in configuration as bitmasks.** Removes the race and survives
restart, and was the direct ancestor of the adopted design. Rejected in that form because a bitmask
declares the *answer* rather than the *intent*: it is machine-specific, must be re-derived for every
environment and every machine shape, and cannot be checked against another encoding. Declaring a
rank and letting `deploy.py` resolve rank to core ids for the target host survives a hardware
refresh; a bitmask does not.

**6. Control the start order.** Not equivalent to declaring the allocation, and worth stating
because the two are easy to blur. A controlled order makes a cold start deterministic and does
nothing for restart, because a component killed by `ha_test.py` comes back outside any
launcher-controlled sequence and re-claims from whatever is free. It makes the fault harder to
reproduce rather than fixing it.

---

## Open questions

1. ~~The rank values are proposed, not settled.~~ **Closed.** The table stands as proposed, with
   `minimum_background_cores = 6` for a consolidated uniform machine; the MEPs are therefore
   promoted on the 32-core workstation and demoted on the 20-core one, deliberately. See "Declared
   input 2: the rank" for the reasoning and the cost accepted. This closed question 4 with it.
2. ~~Does background allocation for explicit Quill pinning live in the layout file or a second
   `CpuRegistry` pool?~~ **Closed** by ratifying Proposal 3: the layout file carries both pools and
   the registry grows nothing.
3. Does a reduced (`--no-ha`) deployment keep the freed cores idle — as designed above, so the two
   deployments are comparable — or is there ever a reason to want a denser assignment?
4. ~~What is `minimum_background_cores` for the work machine?~~ **Closed** with question 1: 6,
   declared once on `dev.toml`'s `[machines.localhost]` and therefore in force on both machines.
   It is inert on the hybrid workstation — the P-core ceiling binds first there, so any floor from
   0 to 16 gives an identical layout — and binding on the 20-core uniform machine. Omitting it, as
   was first drafted, is not neutral: with no floor the same file admits ranks 4 *and* 5 on the work
   machine, leaving three background cores for two JVMs and thirteen Quill backends.
5. ~~Residual hole: a library that sets its own affinity explicitly overrides the inherited mask.~~
   **Closed as far as it can be.** Nothing can prevent it -- an affinity mask is advisory and any
   thread may change its own at any time -- so it is detected instead. `cpu_audit.py` reads every
   running thread's real mask from `/proc/<pid>/task/<tid>/status` and compares it against the
   layout, exiting non-zero on a mismatch so it can gate a performance run rather than being read by
   eye. Verified by deliberately moving a `fix_test_client` JVM thread onto `order_gateway`'s core,
   which the audit reported by thread, core and owner.
6. Does case B need an assertion at all, beyond prominent reporting of the computed layout? A
   `required_hot_path_rank = N` on the machine entry was drafted and withdrawn, for three reasons
   worth recording so it is not re-proposed unexamined. First, a **global** rank scale does not
   compose with a **per-machine** assertion: on a dedicated host the ranks present are an arbitrary
   subset, so "ranks 1 to N" asserts nothing about the ranks that have no components there, and the
   value looks like a meaningful threshold while being merely the one rank present. Second, it is a
   number that must be kept consistent with the rank table by hand, with nothing to check it
   against. Third and decisively, on the deployments in hand it is unreachable — see case B. If an
   assertion is ever wanted, the form that composes is a boolean *every ranked component on this
   machine must be admitted*, which means the same thing on every host and needs nothing kept in
   sync. The scenario to design it for is consolidation onto a small host, not a dedicated one.
7. **The declared thread count can only drift upwards safely.**
   `Reactor::verify_hot_path_thread_count()` compares the constant a component reports through
   `--hot-path-thread-count` against what it really registered, and fails startup when the
   allocation is too small. It cannot catch the opposite: a constant left at 4 when the component
   registers 2 wastes two cores per instance, silently, and the layout looks entirely healthy. The
   asymmetry is deliberate for now — over-declaring costs cores, under-declaring costs a shared
   hot-path core — but a component that has *fewer* threads than it claimed is still a defect, and
   nothing reports it. The natural place is the audit rather than startup, since the audit already
   knows which allocated cores have no thread pinned to them.
8. **Three questions about the environment TOMLs themselves, unresolved since the manifests were
   first written.** None block the mechanism; all three affect whether the declared deployment is
   the intended one.
   - **Both gateways are placed on one host** (`gateway.<env>.exchange.internal`), because that is
     what the existing `*_host` values say. This contradicts `prod.toml`'s own header comment that
     each component gets a dedicated host — a comment that predates this work. Either the comment
     or the placement is wrong. Note the layout copes with it correctly: they share a rank, so they
     are admitted together or not at all, four hot-path cores on that host.
   - **`admin_service` had no host anywhere.** `admin.<env>.exchange.internal` was invented and
     marked `# REPLACE`. It is a guess and needs confirming.
   - **Machine keys duplicate the `*_host` values.** Both carry `# REPLACE`, and they must be kept
     consistent by hand. Deriving one from the other is a `deploy.py` change and was deliberately
     not attempted while the layout mechanism was being built.

---

## See Also

- [CPU Pinning](cpu_pinning.md) — the mechanism as built
- [WAL and High Availability](wal_and_ha.md) — two-tier commit, why the sequencer follower is on
  the critical path
- [Roadmap](../roadmap.md) — item 16, Prometheus metrics
