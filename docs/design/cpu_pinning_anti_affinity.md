# CPU Pinning — Anti-Affinity for Non-Hot-Path Threads

**Status: open design problem. Nothing implemented. Raised 2026-07-26.**

This document records a problem found while planning the Prometheus metrics endpoint
([Roadmap](../roadmap.md) item 16), together with the approaches considered so far and why each
falls short. It is a companion to [CPU Pinning](cpu_pinning.md), which describes the mechanism as
built; this one describes what the mechanism cannot currently do.

The problem is not solved. Do not treat any option below as chosen.

---

## The requirement

The Prometheus endpoint runs an embedded HTTP server (civetweb, inside `prometheus-cpp`) on a
background thread that an external scraper connects to. That thread does blocking I/O on a
timescale of scrape intervals and has no business sharing a core with a hot-path thread.

So it needs the inverse of the existing facility. Today the framework can say *pin thread T to
core C* (`pin_thread_to_core`, `pin_tid_to_core` in `CpuPinning.hpp`). What is needed is *restrict
thread T to whatever cores nobody has pinned* — anti-affinity against the CPU registry.

Two parts of this are not difficult:

- **Applying the mask.** `sched_setaffinity` takes a CPU set, so a multi-core mask costs no more
  than a single-core one. The natural formulation is a positive affinity mask over the complement
  of the claimed set, rather than anything genuinely "negative".
- **Reaching the thread.** The civetweb thread's OS thread id is obtainable, so no `/proc/self/task`
  walking or affinity-inheritance trickery is required. (Threads do inherit their creator's mask
  on Linux, verified, but it turns out not to be needed.)

Determining *which cores* belong in the mask is the whole problem.

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
  permitted to run nowhere is not a thread. A fallback must be defined — leaving the mask untouched
  and logging at `Error` is the obvious candidate, since silently running unrestricted is the
  thing being prevented.

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

### Difficulty 3: the shortfall, and why "demote the standbys" is only half wrong

Twenty-four threads want fifteen P-cores, so nine must land on E-cores whatever policy is chosen.
Two obvious places to economise: the Quill backend threads (eight of them, genuinely off the hot
path by design) and the HA followers (six threads). The backends are straightforwardly available.
The followers need care, because **"secondary" is not a uniform latency class in this system.**
Verified in the code:

- **The sequencer follower is synchronously inside the client round trip.** Under the two-tier
  commit described in [WAL and High Availability](wal_and_ha.md), the leader parks each execution
  report in `pending_er_` and releases it to the gateway only when the matching `WalAck` arrives
  (`SequencerThread.cpp:558` and `:1076`). The follower's wake-up latency is therefore added to
  every order's round trip -- measured p90 wake-up for that thread is around 354 us. A follower on
  a slow core slows the *leader's* client-visible responses. This one cannot be demoted.
- **The matching engine secondary is not.** `send_book_update()` is fire-and-forget
  (`MatchingEngineThread.cpp:453` and `:561`); the primary does not wait for the secondary to
  acknowledge anything. The secondary merely tails the book. Same HA pattern as the sequencer,
  opposite conclusion.

The remaining objection to demoting a follower was that roles swap, and that this is the
tested-for case rather than an exception -- `ha_test.py` kills and restarts components routinely,
and a promoted follower is instantly the latency-critical path. That objection has a
straightforward answer:

**Re-pin on promotion.** When a leader dies the registry evicts its entries automatically (the
dead-pid compaction at the start of `claim_cpus()`), so its cores are free the moment it goes.
Affinity can be changed at any time -- `pthread_setaffinity_np` is not restricted to startup. So a
follower may sit on the cheap tier and, on being granted the leader role by the arbiter, claim the
cores the dead leader has just released. That is a small piece of work, and it makes follower
demotion sound for the matching engine and the publisher.

So the honest statement of Difficulty 3 is narrower than "HA doubles the requirement": **one**
follower, the sequencer's, needs hot-path cores continuously; the rest can be demoted provided
promotion re-pins.

With that, the arithmetic becomes comfortable rather than impossible:

| tier | threads |
|---|---|
| Quill backends, all 8 components | 8 |
| `matching_engine_secondary`, `mep_primary`, `mep_secondary` -- app + reactor | 6 |
| **other-work tier total** | **14** |
| both gateways, both sequencers, `matching_engine_primary` -- app + reactor | 10 |
| **hot-path tier total** | **10**, against 15 available P-cores |

Both gateways on P-cores, deterministically, with headroom.

---

## Process taxonomy: mandatory is not the same as latency-critical

A production deployment needs to know which processes will run on a given machine, and which of
those are mandatory for an operational system. That is the right question to ask, and the
per-machine half of it is what makes the anti-affinity complement computable at all -- see approach
6 below.

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

Two independent axes: **mandatory**, driving readiness and health checks and whether the launcher
may declare the system operational; and a **latency class**, driving which core tier a process's
threads receive. They agree on most rows, which is precisely why one flag would look adequate while
quietly misplacing the row that matters.

*Mandatory* may not be cleanly binary either. The arbiters are not needed for the venue to keep
trading, only for it to survive a failure, so "mandatory for trading" and "mandatory for
resilience" are arguably distinct -- which suggests an enum rather than a boolean.

---

## Why development is the environment that matters

`prod.toml`, `preprod.toml` and `test-1.toml` all run **one component per dedicated host**. There
is no contention there: a single claimant takes what it needs from a whole machine, the complement
is large and stable, and anti-affinity is trivial because the only claimant has finished claiming
before anything else starts. Production hosts are also candidates for `isolcpus` / `nohz_full` and
a low-latency kernel.

`dev.toml` runs everything on one workstation — fifteen components, eight of which pin — with no
CPU isolation and a stock kernel.

Every difficulty above is therefore **development-only**, which invites the conclusion that it
matters less. The opposite holds: all latency measurement and all protocol comparison happens in
development. Production is where the system runs; development is where its numbers come from. A
design that is sound in production and indeterminate in development yields a system that cannot be
characterised.

This does constrain the solution, though — production must not be made to carry configuration
whose only purpose is the development case.

---

## Approaches considered

None chosen. Recorded so the ground is not re-covered.

**1. Compute the complement once, when the metrics thread starts.** Simplest possible change. Per
Difficulty 1 it is close to useless for any early-starting component, and it fails silently.

**2. Quiescence detection.** Apply the mask once the registry has been unchanged for T, re-applying
if it changes again; the housekeeping tick is the natural place. A heuristic, and T is arbitrary.
Its redeeming argument: it is *self-correcting*, and never worse than the status quo in which
nothing restricts the thread at all — so the window is time-to-correctness rather than a
correctness failure.

**3. A barrier on a configured expected claimant count.** Rejected. A configured maximum is an
*upper bound*, not an *expectation*, and the two coincide only when the deployment runs at exactly
full capacity. `devenv.py --no-ha` drops three of the eight claimants, so a count configured for
full HA is never reached and waiters block forever; a count configured for `--no-ha` would refuse
to start half the HA deployment. Separately, "processes running" is the wrong quantity — five of
the components have `cpu_pinning_enabled = false` and never claim, so the figure needed is a
claimant count that changes whenever a config flag is flipped, with nothing to detect the
divergence. A program count is in any case a lossy proxy for core demand: `register_extra_thread()`
means components do not consume equal numbers of cores.

**4. Have the launcher close the layout.** Each process records "claiming complete" at the end of
`pin_registered_threads()`; `devenv.py`, which does know the intended component set for this
deployment, writes a "layout final" flag into the registry once all claimants have recorded it; the
metrics thread applies its mask when it sees the flag. This is sound at cold start — no timeout, no
heuristic, and the authority is the one participant that knows the population, so `--no-ha` is
simply a smaller plan. It also degrades honestly: a component that fails to start means the layout
is never closed, which is diagnosable. **Its weakness is mid-life restart**: a restarted component
re-claims greedily and can shift the layout after closure.

**5. Declare the layout in configuration; derive nothing.** State which cores are for hot-path
pinning and which are for everything else, constrain claiming to the former, and mask the metrics
thread to the latter — for example a `cores_for_other_work` entry in the `[shared]` section of the
environment TOML, with the hot-path pool as its complement.

This removes the race (nothing is inferred), survives restart (a component reclaims its own
declared cores), fixes Difficulty 2, and makes measurements reproducible run to run — which for
latency work is a property in its own right. The Quill backends and the metrics thread move to the
other-work set by definition, freeing eight P-cores and making the arithmetic fit.

Costs: a maintained configuration; a check that the two sets are disjoint and cover the machine,
failing hard if not (a misconfiguration that silently degrades pinning being worse than one that
refuses to start); and it changes `CpuRegistry` from an allocator into a verifier, which would
require reworking several of the tests in `CpuRegistryTest.cpp` that currently assert negotiation
behaviour.

Note that the objection "there is no shared config in this project" does not hold: the `[shared]`
section of the environment TOML plus `deploy.py`'s template expansion is exactly that mechanism.
`reactor_cpu_pinning_reserve_cpu0` is authored once and expanded into fourteen component
templates today, and `deploy.py` already *computes* values rather than merely copying them — the
registry paths and the WAL directories are both derived there. A layout planner could live in the
same place, since `deploy.py` is the one participant that reads the whole component list.

**6. Declare, per machine, which processes run there and what each one is.** The strongest option
on this list, and the one that makes the anti-affinity complement computable rather than inferred.
A production deployment has to know which processes a given host runs anyway -- for readiness
checks, for monitoring, for knowing whether the system is operational. Once that manifest exists,
"which cores will be pinned on this machine" is answerable by reading it, with no barrier, no
claimant count, no quiescence timer and no launcher closure flag. Difficulty 1 does not arise,
because nothing is being inferred from observed claims.

Each entry needs the two orthogonal properties described under "Process taxonomy" above --
*mandatory* and a *latency class* -- and the latency class is what assigns the core tier, which
also disposes of Difficulty 2.

A variant worth recording, suggested externally: rather than each component reading its assignment
from its own config, have the launcher **pre-populate the registry with the whole layout before any
binary starts**. The registry then inverts from a negotiation into a lookup table; a component
reads its own pre-assigned cores, and a component restarted at any later time reads the same ones.
That keeps a single machine-wide view of the layout, which is better for an operator than the same
information scattered across per-component files. It is a presentation choice on top of approach 6
rather than a different approach.

Note one distinction that is easy to blur: **controlling the start order is not equivalent to
declaring the allocation.** A controlled order makes a cold start deterministic and does nothing
for restart, because a component killed by `ha_test.py` comes back outside any launcher-controlled
sequence and re-claims from whatever is free. A declared allocation is restart-safe by
construction. Since routine restart is designed-for behaviour here, only the latter is a solution;
the former merely makes the fault harder to reproduce.

---

## Open questions

1. Is the core layout something we are willing to declare in configuration, or must the system
   keep working it out at runtime?
2. Should the per-machine manifest carry *mandatory* and *latency class* as two separate fields, and
   is *mandatory* a boolean or an enum ("for trading" versus "for resilience")?
3. Is re-pin-on-promotion worth building, so that non-critical followers can occupy the cheap tier
   and take the dead leader's cores when the arbiter promotes them?
2. If declared: does a reduced (`--no-ha`) deployment get the same assignment as full HA, so that
   the two are comparable, or a denser one that uses the freed cores?
3. What does `CpuRegistry` become if it stops being the allocator? Proposal: a runtime record and
   collision detector — it still earns its place, because catching two installations claiming the
   same core on one machine is a real service.
4. What is the fallback when the anti-affinity set is empty?

---

## See Also

- [CPU Pinning](cpu_pinning.md) — the mechanism as built
- [WAL and High Availability](wal_and_ha.md) — two-tier commit, why the follower is on the
  critical path
- [Roadmap](../roadmap.md) — item 16, Prometheus metrics
