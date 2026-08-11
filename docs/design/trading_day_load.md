# Compressed Trading Day Load Profile

**Status: DRAFT, decisions taken 2026-08-08. Nothing built.**

A load run shaped like a trading day — quiet periods, steady activity, short bursts of varying
intensity, sustained elevated periods and cancels — paired with the latency band chart so the run
produces a *reading* rather than a number.

## This is happy-path testing

Heavy load, bursty traffic and members pulling their books are what a **normal day** looks like.
Nothing here is a failure scenario: no component is killed, no fault is injected, no failover is
provoked. High availability needs its own test approach and has one; mixing the two would make
every result ambiguous about which thing was being tested.

If something breaks under this profile, that is a **finding**, not a scenario.

## Decisions taken

| | |
|---|---|
| Gateway | **Binary only** to begin with — the client is ours, so rate and cancel behaviour can be extended where needed. FIX has no rate control. |
| Failover | **Excluded.** Separate concern, separate test. |
| Cancels | **Included**, up to and including pulling 50–100 outstanding orders for one comp id. |
| Duration | **At least one hour.** |
| Where it lives | **An extension of `perf_run.py`**, not a new script. |

---

## The framework is the subject; the applications are the instrument

The gateways do almost no order validation and **the matching engine does no matching** — it emits
only `New`, `Canceled` and `Rejected`, never `Filled`. That is deliberate. These applications exist
to exercise `pubsub_itc_fw` and to flush out its requirements, not to be an exchange.

This decides how the run should be read. A trading-day shape is worth building **because it puts
the framework through load transitions that a steady rate never produces** — queues filling and
draining, slab allocators growing under burst, the reactor scheduling across a tenfold change in
arrival rate, the WAL absorbing a sustained hour. What the run finds are **framework** findings.

Two practical consequences:

- **Orders never fill, so everything sent rests until cancelled.** The runner's outstanding-order
  bookkeeping is therefore trivial: the outstanding set is everything sent, minus everything
  cancelled. No fill notifications to reconcile.
- **Venue realism is a means, not the goal.** Where a choice arises between a more faithful venue
  behaviour and a load shape that stresses the framework harder, prefer the latter and say so.

---

## Why, beyond "more load"

Every measurement taken so far has been either saturating or trivially small. `perf_run.py`
without `--rate` deliberately offers load faster than the pipeline drains, so its figures are
queueing-dominated: a 20,000-order run gave p50 123ms and p99 247ms, which bear no relation to the
119/356/528µs recorded on rate-limited runs. Meanwhile `order_round_trip_nanoseconds`, its bucket
bounds, the percentile band chart and the ceiling breach counter were all designed against
synthetic demo data.

**So the venue has never been observed under a realistic load profile, and the metrics have never
had a realistic input.** This closes both gaps at once.

It also asks the question a steady load cannot: what happens at the *transitions*. Does latency
return to baseline after a burst or ratchet upward? Does queue depth recover? Is the open
distinguishable from midday?

---

## A sustained burst is not a long short burst

This is the design's central claim, and it decides the shape of everything below.

| | tests | finds |
|---|---|---|
| **1–3 minutes** | absorption | queue growth and drain, spike-and-recover |
| **~1 hour** | accumulation | WAL growth, slab allocators that expand and never return memory, log volume, disk, and above all **monotonic degradation** — anything that fails to reach a steady state |

A short burst structurally cannot find the second kind. Both belong in the profile.

### Consequence: compress the quiet, preserve the bursts

Uniform compression breaks on this ratio. Real bursts of 1–3 minutes against a 60-minute one is a
20–60× spread; compress the day 6× and the short bursts fall to 30 seconds — two samples at a 15s
step, no shape — while the long one is unaffected.

So **compression applies only to phases marked compressible**, which means the quiet periods. They
need only enough duration to establish a baseline and show volume falling. Bursts keep their real
durations, because they are the subject.

Total run time is then roughly the sum of the bursts plus short gaps: around 90 minutes with one
genuine hour-long phase, which is nothing for an unattended run.

### What compressing a sustained phase would and would not preserve

Worth choosing knowingly if it is ever compressed. Much of what accumulates is a function of
**total orders**, not elapsed time — WAL bytes, slab growth, book size — so 60 minutes at rate R
resembles 15 minutes at 4R for those. But not for anything time-based: log rotation, session
heartbeats, TCP keepalive, disk fill racing background retention. And raising the rate changes the
regime into an overload test, which is a different question.

**Recommendation: keep at least one phase at genuine duration.** It is the only way the
accumulation question gets asked at all.

---

## Calibrate first: the probe run

Absolute rates are a property of the machine, not of the profile. A figure that saturates a
20-core work machine may barely warm a 32-core workstation, and a profile written in absolute
orders/second silently means something different on each.

So the runner gains a **probe mode**: ramp the offered rate until latency departs from baseline,
and report the rate at which it did. That figure is the machine's **ceiling**, and every phase rate
in the profile is then written as a fraction of it.

```toml
[run]
ceiling_orders_per_second = 3500   # from the probe, recorded in the manifest
```

Two things this buys:

- **The same profile is meaningful on a different machine.** "The open runs at 100% of ceiling, the
  lull at 1%" transfers; "the open runs at 3,500/s" does not.
- **The starting figures in this document stop being guesses.** They are a plausible first
  calibration for one workstation and nothing more. The rate-limited measurements available
  (119/356/528µs at 4/20/40 sessions) are round-trip *latencies*, not throughput, and the
  saturating runs establish only that saturation was reached. Neither yields a ceiling.

The probe should be short — ten minutes is ample — and its result recorded in the manifest, so a
run always says which ceiling it was calibrated against.

---

## The resolution floor

The band chart wants `--step` at several times the 5s scrape interval, so ~15s is the practical
floor, and a phase needs several samples to show shape rather than a dot.

**`min_phase_seconds` is therefore a hard input, not a guideline.** The runner computes it from the
chart step and a minimum sample count, and **refuses to start** if any phase — after compression —
falls below it, naming the phase and what it would have to become. Failing loudly beats producing
a run nobody can read, which is the same rule the breach counter follows when no bucket bound sits
on the ceiling.

---

## The three patterns, and why they matter more than the load

The profile must be able to produce, deliberately, each of the patterns that all raise a
short-window arithmetic mean by a similar amount and which a mean cannot tell apart:

| pattern | what the chart should show | how the profile produces it |
|---|---|---|
| **steady** | flat tracks, steady volume | constant offered rate |
| **uniform slowdown** | all tracks lift together, volume unchanged | raise the offered rate toward saturation |
| **tail excursion** | p50 and p90 flat, p99 climbs, volume unchanged | short sub-second micro-bursts inside an otherwise steady interval, so most orders are normal and a few queue behind each other |
| **volume collapse** | nothing moves, volume falls | drop the rate sharply, without reaching zero |

All four are reachable with rate control alone — no venue modification, no fault injection.

**This is the strongest reason to build it.** The band chart is currently a chart we believe. Once
it has distinguished four constructed inputs whose ground truth we chose, it is a chart that has
been *shown* to work — a real negative control, of the kind whose absence let something through in
the gateway HA work.

It also answers the original question directly: if the venue can produce all three and the chart
tells them apart, then the same reading applied to a real system says which one is happening, and
what evidence would settle it.

---

## Shape of the profile

A TOML file, matching the project's configuration convention. Sketch, not final:

```toml
[run]
gateway            = "binary"   # see "Rate control is binary-only", below
compression        = 6          # applied ONLY to phases with compressible = true
ceiling_orders_per_second = 3500 # from the probe run; all phase rates are fractions of it
minimum_samples    = 6          # per phase, at the chart step -- sets min_phase_seconds
ceiling            = "2.5ms"    # must be a configured bucket bound, or breaches are refused

[[phase]]
name         = "pre-open"
pattern      = "steady"
duration     = "20m"
compressible = true
rate         = 0.015

[[phase]]
name         = "open auction"
pattern      = "uniform_slowdown"
duration     = "3m"
compressible = false
rate         = 1.15   # deliberately above ceiling: this is a stress phase

[[phase]]
name         = "sustained elevated"   # the event of interest -- REAL duration, never compressed
pattern      = "uniform_slowdown"
duration     = "60m"
compressible = false
rate         = 0.35

[[phase]]
name         = "tail excursion"
pattern      = "tail"
duration     = "5m"
compressible = false
rate         = 0.12
micro_burst  = { every = "10s", orders = 40 }

[[phase]]
name         = "midday lull"
pattern      = "volume_collapse"
duration     = "30m"
compressible = true
rate         = 0.006
# No cancels here, deliberately: cancels are invisible to the chart's volume line,
# so they would be indistinguishable from the collapse this phase exists to produce.

[[phase]]
name          = "cancel storm"
pattern       = "uniform_slowdown"
duration      = "4m"
compressible  = false
rate          = 1.0          # NOT a lull: cancels arrive when everything else is busy too
cancel_ratio  = 0.25         # sustained cancel stream, as a fraction of this phase's order rate
cancel_sweeps = { comp_id = "BINCLIENT1", orders = 80, rate = 200, count = 8 }
```

`rate` is a fraction of `ceiling_orders_per_second` from the probe run, so the profile means the
same thing on a different machine.

Rates are **aggregate offered orders per second**; the driver decides how to achieve them.

A `cancel_sweep` cancels that many of the comp id's resting orders, at its own defined rate, as
individual requests — the venue has no mass-cancel message. It runs *alongside* the phase's
new-order flow rather than instead of it, which is the realistic shape and the one that loads
ingress, the ME's book lookup and egress at the same time. `cancel_sweep.rate` is the parameter to
push between runs.

---

## Cancels

A member pulling its book is ordinary behaviour and belongs in a normal day. The profile must
include cancels, up to and including cancelling **50–100 outstanding orders for one comp id**.

### There is no mass-cancel message in this venue

`OrderMassCancelRequest` (msgtype `q`) and `OrderMassCancelReport` (`r`) exist in the stock
`FIX50SP2.xml` dictionary, but **not** in `applications/fix_orders.dd.xml`, which generates only
NewOrderSingle, OrderCancelRequest and ExecutionReport. The binary client offers `--cancel` with
`--cl-ord-id`, which cancels one order.

So "cancel everything for this comp id" means **50–100 individual cancel requests**, not one
message. That is worth being explicit about, because it is a different test:

- what it exercises is a **burst of individual cancels** — the gateway's `drain_pending_cancels`
  path, already known to account for around 11.3% of gateway CPU, and the ME's book lookup once
  per order;
- what it does **not** exercise is mass-cancel semantics — one request, one report, the ME walking
  its own book.

Implementing `q`/`r` is a separate feature and a sibling of the Order Mass Status Request already
on the candidate list. Stock FIX50SP2 permits both, so the data dictionary rule is satisfied if it
is ever picked up. **Not in scope here.**

### Cancels are invisible to the latency chart, and that is a confound

`order_round_trip_nanoseconds` observes **only `OrdStatus=New`** — every ER for an order carries
the same ingress stamp, so counting a later Canceled one would record a round trip as long as the
order rested on the book. The band chart's volume line comes from that same histogram's `_count`.

The consequence is sharp: **a cancel-heavy phase shows falling volume on the chart while the venue
is busier than ever.** That looks exactly like the volume-collapse pattern, which is one of the
three the chart exists to tell apart.

Two responses, both **decided**:

1. **Never overlap a cancel-heavy phase with the volume-collapse phase.** A design rule for the
   profile, free, and it protects the negative-control argument. Note this does *not* forbid
   overlapping cancels with a **burst** — see below, where it is required.
2. **Plot `rate(framework_pdu_messages_total)` beside the new-order volume.** That counter already
   exists per thread and counts every PDU, cancels included, so the *divergence* between the two
   lines is the cancel activity. No new instrumentation.

Considered and **not** taken for now: a cancel round-trip histogram. Honest and useful, but new
instrumentation and a scope decision of its own.

The manifest records cancels offered per phase regardless, so the reading can always be corrected.

### A cancel storm is an intensity event, not a quiet one

Observed in production: **when a lot of cancels are going on, the traffic at that moment is
extremely intense.** That is not incidental, and the profile must reflect it rather than testing
cancels in a calm phase.

Three reasons it compounds:

- **Every cancel is two messages, not one.** N cancel requests in, N Canceled reports out, *on top
  of* the ongoing new-order flow and its reports. The egress path takes the worst of it.
- **Whatever made the member pull its book is affecting everyone else too.** A price move or a news
  event produces the cancels *and* a surge of new orders. Cancels arriving during a lull is the
  unrealistic case.
- **`drain_pending_cancels` is already about 11.3% of gateway CPU**, and it is a hot spot that only
  becomes significant under exactly this condition. A cancel phase run at a polite rate would never
  reveal it.

So the cancel sweep is layered **on top of an elevated order rate**, and it is a natural candidate
for one of the phases meant to stress the venue to its limits — more realistic than simply cranking
the new-order rate, because it loads ingress, the ME's book lookup and egress simultaneously.

### Two different things: a cancel stream and a cancel sweep

Easily conflated, and they need separate parameters because they are separate phenomena.

- **The stream** — sustained cancel traffic, many members churning their quotes. This is what makes
  the storm a *phase* rather than an instant, and it is expressed as `cancel_ratio`, a fraction of
  the phase's order rate.
- **The sweep** — one member pulling its whole book: a bounded burst of N cancels for one comp id.
  This is the panic event, and several of them are spread across the phase.

**A single 80-order sweep is invisible at chart resolution.** At 40 cancels/s it lasts two seconds;
at a 15s step it cannot appear as anything but a contribution to one interval's p99. That is not a
reason to drop it — the tail is exactly what it should affect — but it is a reason not to expect a
sweep to show up as a feature of its own, and a reason to run several.

### Why the stream is a ratio, not an absolute

**You can only cancel what is resting.** The ME never fills, so the outstanding book grows only
from new orders and shrinks only from cancels:

| cancel rate versus order rate | outcome |
|---|---|
| below | book grows; sustainable indefinitely |
| about equal | book flat — the **sustained ceiling**, and the interesting endpoint: double the message work for zero net book change |
| above | possible only in bursts, drawing down the accumulated book; self-terminating |

Expressing it as a fraction makes the impossible case unrepresentable, and rescales automatically
when the phase's order rate is tuned.

### What the framework actually sees is messages per second

Orders and cancels are both just PDUs to the reactor and its queues. At 3,500 orders/s with
`cancel_ratio = 0.25`:

```
in    3,500 NOS      +  875 cancels        = 4,375 msg/s
out   3,500 New ERs  +  875 Canceled ERs   = 4,375 msg/s
                                           ≈ 8,750 msg/s through the gateway
```

That is the figure to reason about, and it is why a cancel storm loads the framework harder than
simply cranking the order rate: the same throughput increase arrives split across two ingress paths
and two egress paths.

Suggested starting point, to be superseded by the probe run below: `cancel_ratio = 0.25`, pushed
across runs to 0.5, 0.75 and 1.0.

### The harness must track its own outstanding orders

Mechanically new, and easy to miss. You can only cancel what is resting, and the binary client
cancels by `--cl-ord-id`, so the runner has to maintain **its own book of outstanding ClOrdIDs per
comp id** — which orders it sent, and which are still open. `perf_run.py` today fires and forgets.

Two things the implementation must establish first: whether the ME ever fills a resting order in
this configuration (if it does not, everything sent stays outstanding and the bookkeeping is
trivial), and that a cancel-heavy phase is preceded by enough order flow to have 50–100 resting
orders to sweep.

---

## Rate control is binary-only, and that is the first decision

`perf_run.py:881` rejects `--rate` for the FIX gateway, because f8test has no rate control. So:

1. **Binary gateway first.** Full shaping immediately, no new client. Recommended for the first
   version — it makes every pattern above reachable today.
2. **FIX by session waves.** Start and stop f8test sessions to vary *aggregate* load. No
   per-session control and coarse, but it produces genuine bursts and quiet periods through the
   real FIX stack.
3. **A rate-controlled FIX load client.** Most faithful, most work, and replacing f8test was
   already judged disproportionate once.

The profile format above is agnostic; only the driver differs.

---

## The manifest

Written incrementally so a run that dies still has one, alongside the existing `perf_run` output:

```
run_id, gateway, instance, ceiling, chart step
per phase: name, pattern, planned duration, actual start/end epoch seconds,
           orders offered, orders acknowledged
```

Without it, reading the chart is guesswork — *was that bump the second burst or the failover?*
With it the chart becomes evidential, and `pubsub_metrics.py` can later shade the phases behind
the percentile tracks.

---

## Assertions — what the run actually verdicts

Reported, and only some of them fatal. Consistent with `docs/testing.md`: a number that needs
judgement is reported, a process failure is fatal.

**Fatal:**

- **No loss.** Every NOS acknowledged by an ER. `perf_run.py` already derives this ground truth
  from `GW-NOS-RECV` / `GW-ER-SENT`, and already knows to distinguish *the venue lost it* from *the
  harness never sent it* — a lesson worth inheriting, since it once reported 5,000 phantom losses.
- **Every component still alive at the end**, and no component restarted mid-run.
- **The harness offered what it promised.** Achieved rate within tolerance of offered, per phase.
  A run whose load generator quietly under-delivered is not evidence of anything.

**Reported, never fatal:**

- **Recovery.** After each burst ends, how long until p99 returns within tolerance of the
  pre-burst baseline. The headline number for a short burst.
- **Monotonic degradation across the sustained phase.** Compare p99 and volume in the first third
  against the last third. *This is the point of the hour-long phase* — a rising trend within a
  phase of constant offered rate is accumulation, and is the finding the whole design exists to
  surface.
- **Breach count per phase.** Exact, from the bucket bound at the ceiling. `1,412 orders exceeded
  2.5ms during burst 4` is a better overload verdict than any percentile, because a p99 under the
  ceiling still permits one order in a hundred above it.
- **Framework resource accumulation at phase boundaries.** Latency is the symptom; these are the
  cause, and they are the framework observables the whole run exists to produce:
  - **Slab and pool allocator growth** — `ExpandableSlabAllocator` and `ExpandablePoolAllocator`
    expand on demand. Whether they plateau or climb across a sustained hour is a framework
    question, and one no short burst can ask.
  - **Queue depth behaviour** — `LockFreeMessageQueue` already keeps an atomic size for its
    watermark handlers. Whether watermarks fire, and whether depth returns to baseline after each
    burst, is the backpressure story.
  - **WAL bytes written and retained**, against the sustained phase's duration.
  - **RSS per component**, as the coarse catch-all for anything the above misses.

---

## Finding causes, not just symptoms

The run exists to flush out **framework** bottlenecks — unpinned hot threads, allocation on the hot
path, computations that cost more than they look. The metrics say *when* the framework struggled;
these say *why*.

### Two run modes, and their figures are never comparable

- **Measured** — metrics only, nothing attached. Produces the latency figures and the assertions.
- **Profiled** — `perf` or callgrind attached. Produces attribution.

`perf record --call-graph dwarf` attached to the ME and a gateway measurably inflates round-trip
figures; a previous run's p50 of 123ms was taken with it attached and is not a service-time number.
**A profiled run's latencies must never be compared with a measured run's, or quoted as venue
performance.** The runner should record the mode in the manifest so the two cannot be confused
later.

### The manifest makes profiling phase-aligned

This is what turns the manifest from a convenience into a requirement. With each phase's start and
end epoch seconds recorded, a `perf` capture can be **sliced per phase** — profile the cancel storm
separately from the lull, and attribute cost to load *shape* rather than averaging it across an
hour in which the venue did four different things. Averaged over a whole run, a cost that only
appears during bursts disappears.

### The specific bottleneck classes, and what detects each

| looking for | detector |
|---|---|
| **Unpinned hot threads** | `cpu_audit.py` checks every running thread's real mask in `/proc` against the declared layout and exits non-zero on a mismatch. **Run it after the load as well as before** — a thread created lazily under load escapes a start-up-only audit entirely, which is exactly the case worth catching. |
| **Allocation on the hot path** | The `deploy.py`-generated wrapper already exists as a `perf`/`valgrind` interposition point; a malloc-counting interposer over a phase shows whether the hot path reaches the heap at all. The framework has slab and pool allocators precisely so it should not. |
| **Costly computations** | `perf record` sliced by phase, per the manifest. |

### Triage: which findings are in scope

A load run produces a list of hot spots, and without a rule the temptation is to optimise whatever
looks biggest. That would mean tuning application code, which is not what this is for. **A finding
is in scope only if it says something about the framework.** Three ways it can:

1. **A framework bottleneck.** Cost inside `pubsub_itc_fw` itself. Fix it there.
2. **The application using the framework sub-optimally.** The framework offers the right facility
   and the application is not using it, or is using it badly. Fix the application — and ask whether
   the framework made the wrong thing easy, because that is the more valuable half of the finding.
3. **The framework not providing something that would be beneficial.** The application does
   something awkward because it has no better option. **This is a framework requirement**, and it
   is the most valuable outcome the run can produce — flushing these out is what the applications
   are for. It belongs in the roadmap, not in a patch to the application.

**Out of scope: an application inefficiency that implies nothing about the framework.** Note it and
move on. The matching engine does no matching and the gateways barely validate; making their stub
logic faster proves nothing about `pubsub_itc_fw` and costs time that the framework should have had.

### Known suspects, and how they triage

Worth checking against rather than rediscovering. Earlier profiling found the **Quill logging
backend at about 27%** and **`drain_pending_cancels` at about 11.3%**, dwarfing FIX parsing at
roughly 8%. They land in different categories, which makes them a good worked example:

- **The logging backend** is a category 1 or 2 finding depending on what the run shows. If the cost
  is inherent to how the framework wraps Quill, that is the framework's to answer. If the
  applications are simply logging too much on the hot path, that is category 2 — and the follow-up
  question is whether the framework should make over-logging harder to do by accident. A sustained
  hour is the first real test of whether its share holds steady or grows.
- **`drain_pending_cancels`** is gateway application code, so it is only in scope if the run shows
  it is awkward *because* the framework lacks a suitable queue or batching facility — category 3,
  and a genuine requirement. If it is merely an inefficient loop in a stub, it is out of scope.
  The cancel storm phase targets it directly, which is what makes the distinction answerable.

---

## Running it

```bash
# 1. Deploy, if the installed configs still hold ${placeholders}.
python3 scripts/deploy.py --skip-db --skip-certs
grep -c '\${' installed/etc/binary_order_gateway/binary_order_gateway_a.toml   # expect 0

# 2. Make sure NO venue is running, then start Prometheus ON ITS OWN.
python3 scripts/devenv.py stop
python3 scripts/devenv.py start prometheus
curl -s http://localhost:9090/-/healthy

# 3. Run the profile. perf_run.py starts and stops the venue itself.
python3 scripts/perf_run.py --gateway binary --clients 4 --profile profiles/trading_day.toml

# 4. Read it, against the phases in the manifest.
python3 scripts/pubsub_metrics.py --application pubsub --component binary_order_gateway_a --metrics bands \
        --ceiling 2.5ms --since 120 --step 30 --graphic
```

### The trap: `perf_run.py` owns the venue, and does not start Prometheus

Both halves matter, and getting either wrong wastes the run.

- **`perf_run.py` starts and stops every component itself.** If `devenv.py start` has already
  brought the venue up, `perf_run` starts a *second* copy, the ports are taken, and the first thing
  to fall over is the auth service — `auth_service_a (PID ...) died during startup (exit code
  255)`, followed by a run that generates no load at all. The message names the auth service, so it
  reads like an auth fault rather than the port conflict it is.
- **`perf_run.py` does not start Prometheus**, so a run started against a stopped environment
  collects no metrics and the band chart has nothing to draw.

Hence the order above: stop everything, start **only** Prometheus, then let `perf_run.py` own the
venue. Prometheus scrapes `127.0.0.1:92xx` and does not care which process owns those ports, so it
happily follows components it did not start.

**Verify within the first two minutes** that load is actually flowing, rather than trusting the
absence of an error:

```bash
curl -s -G 'http://localhost:9090/api/v1/query' \
     --data-urlencode 'query=sum(rate(order_round_trip_nanoseconds_count[1m]))'
```

The query **must** be URL-encoded. The `[` and `]` of the range selector are not legal in a query
string, and an unencoded one returns an empty body rather than an error — which reads as no load
and invites killing a run that is perfectly healthy. `-G --data-urlencode` encodes it; a bare
`?query=` does not.

It should report roughly the first phase's aggregate rate. An empty `result` array means the venue
is not receiving orders, whatever the console says. Cross-check against the counter itself,
`sum(order_round_trip_nanoseconds_count)`, which needs no range selector and so cannot fail this
way: if it is climbing, load is flowing.

### On RHEL8

`devenv.py --no-prometheus` skips the scraper for hosts where it is not wanted. The venue still
exposes its metrics endpoints; nothing collects them, so there is no band chart and the run yields
only the client's own figures and the manifest. If the chart is wanted, Prometheus has to be
running before step 3.

---

## First run, 2026-08-08: what it found

Seven phases, 82 minutes, 9.56M orders offered through the binary gateway at up to 3,850/s.
**The run found a framework requirement on its first outing**, which is what it was built for.

### A hash rehash stalls the hot path for over a second

The matching engine's order book is a `tsl::robin_map`. It grows by doubling, and each doubling
rehashes the whole table **on the matching engine's callback thread**. The framework's own reactor
watchdog caught it (`Reactor.cpp:913`, "callback not finished yet"), which is what made it
diagnosable at all — it fired 281 times over the run.

The stalls land on exact powers of two, and roughly double each time:

| book size | when | p99 |
|---|---|---|
| 2^21 = 2,097,152 | 12:16:34 | **96 ms** |
| 2^22 = 4,194,304 | 12:34:43 | **733 ms** |
| 2^23 = 8,388,608 | 13:14:18 | over 1s; the pipeline did not recover |

At 2^23 the load client gave up: the last two phases overran and exited non-zero, and 1,167,392 of
the 9,556,000 offered orders were never accepted. Memory was never short — 16 GB free on a 31 GB
machine — so this is a latency failure, not exhaustion.

**On the band chart, p50 and p90 do not move through any of it.** That is the tail-excursion
signature exactly: a handful of orders delayed catastrophically while the bulk are untouched, and
the reason the chart reports percentiles rather than a mean. A one-second mean over the 12:35
interval would have shown a modest bump and explained nothing.

### Second run, same day: what per-order logging actually costs

A controlled pair. Same profile, same machine, same 9.56M orders offered; the only change
that touches the hot path is whether the matching engine emits one `Info` line per accepted
order.

| | logging on | logging off |
|---|---|---|
| ME log | **1.95 GB**, 8,388,985 lines | **0.076 MB**, 394 lines |
| p99 spike at the 2^21 doubling | 96 ms | 96 ms |
| p99 spike at the 2^22 doubling | 733 ms | ~790 ms |
| p50 / p90 baseline | unchanged | unchanged |
| sustained hour | 3,598s of 3,600s | 3,598s of 3,600s |
| orders over the 2.5 ms ceiling | 118,776 | 133,421 |

**Removing 8.4 million log lines saved 1.95 GB and did not measurably change latency.**

The two charts are kept for comparison — the spikes are the same features in both:

- `docs/measurements/trading_day_logging_on.png`
- `docs/measurements/trading_day_logging_off.png`

The profile said so in advance. Quill splits the work: the calling thread serialises raw
arguments into a lock-free ring buffer, and a backend thread formats them. On the matching
engine's own thread the frontend accounted for **0.06%** of samples —
`Codec<string_view>::encode`, `_reserve_queue_space`, `_encode_header`,
`_commit_log_statement`. The formatting, which is the expensive half, showed up on the
**QuillBackend** thread at 78.76% of the process's samples — and that thread is pinned to a
**background core**, not a hot-path one.

#### The condition this result depends on

The backend being pinned away from the hot path is **load-bearing, not incidental**. The
78.76% is real CPU; it simply is not competing with the thread that matters. On a machine
where the logging backend can be scheduled onto a hot-path core, the same experiment would
not be expected to give the same answer.

So the claim this run supports is narrow and specific: *with a deferred-formatting logger
whose backend is pinned to a non-hot-path core, a log line per order costs the hot path
almost nothing.* It does not support "logging is free".

#### What it does cost

- **Disk.** 1.95 GB over 83 minutes here. At 30–40 million orders a day that is 7–10 GB
  from one statement, before any other component logs anything.
- **Headroom.** Nothing hit it in these runs, but if the backend ever fails to drain, the
  frontend queue fills and the cost stops being 0.06% very abruptly.
- **Argument size, not format cost.** The frontend copies the arguments; it does not format
  them. `Codec<string_view>::encode` was the largest frontend entry because it copies the
  string's bytes. Fewer and smaller arguments on the hot path; an expensive *format* is
  free to the caller.

#### Honest limits of the comparison

One machine, one configuration, two runs. The runs are not identical in every respect: the
breach count differed by 12% and the closing phases overran by different amounts, so
run-to-run variation at that scale is real. What is solid is that **the two large p99 spikes
appear in both runs at the same points and the same magnitudes**, and the p50/p90 baselines
are unchanged — the effect being looked for would have been far larger than the noise.

Run 4 also carried two unrelated changes (a metrics-loader fix and a counter read), neither
of which touches the order path.

### Third run, with cancels: the OOM was a test artefact

Same profile, cancels added at a ratio of 0.7-0.9 depending on phase. **Every phase completed
on schedule, every order was accepted, and nothing was OOM-killed.**

| | run 4, no cancels | run 5, with cancels |
|---|---|---|
| orders accepted | 8,388,608 of 9,556,000 | **9,556,000 of 9,556,000** |
| resting after the sustained hour | 6,928,000 | **1,385,600** |
| peak matching-engine RSS | **9.9 GB, OOM-killed** | **1.45 GB** |
| lowest MemAvailable | exhausted | 7.21 GB |
| afternoon burst | overran 142s, exit 1 | 180s of 180s, exit 0 |
| close | overran 30s, exit 1 | 240s of 240s, exit 0 |
| largest p99 spike | 733 ms | ~60 ms |

The memory profile is the interesting part, because it **plateaus**:

```
21:35   330 MB     22:12  1486 MB
21:47   550 MB     22:25  1486 MB
22:00   862 MB     22:37  1486 MB
22:12   862 MB     22:50  1486 MB
```

The steps are the doublings; between them it is flat. That answers the accumulation question
the sustained hour exists to ask: with a realistic cancel flow the book reaches a working
size and stays there, rather than climbing monotonically until the kernel intervenes.

**So the OOM was an artefact of the test, not a defect in the venue.** Nothing removed an
order -- the matching engine does no matching, and until this the load client could not
cancel -- so the book was a pure accumulator. A real book is bounded by open interest, not
by daily volume.

### What that does NOT excuse

Three findings survive a passing run, and they are recorded in [Bug List](../bug_list.md):

- **The venue accepts orders indefinitely with no matching engine**, at INFO, once per
  order, telling nobody. Run 4 accepted and acknowledged 924,000 orders that nothing could
  process.
- **HA fails over into a condition both nodes share.** Detection, arbitration, promotion and
  reconciliation were all correct and fast; the promoted secondary then died of the same
  memory exhaustion two minutes later.
- **The rehash still stalls the reactor callback thread.** A smaller book gives smaller
  stalls -- 60 ms here against 733 ms -- but the mechanism is untouched, and it scales with
  whatever the book grows to.

The run passing means those are no longer *masked* by a test artefact. It does not mean they
are fixed.

### Two honesty notes on this comparison

**Watchdog firings are suppressed on the primary matching engine, and only there.** The
"callback not finished yet" line is INFO, and `perf_run.py` raises `applog_level` to `warning`
for the duration of a profile run — but it does that to `me_config` alone, which is the
**primary**. So a zero firing count on `matching_engine_primary` is suppression rather than
absence, and the p99 spikes on the chart are the evidence that stalls still occurred.

**Every other component's watchdog output survives, and those counts are comparable between
runs.** `matching_engine_secondary`, `sequencer_primary`, `sequencer_secondary` and the gateways
all stay at `info`. The secondary replicates the same book, so its stalls stand in well for the
primary's: run 8 recorded 79 firings on the secondary peaking at 1,229 ms while the primary
reported none at all.

**The breach count rose** -- 179,463 over the 2.5 ms ceiling, against 133,421 in run 4. That
is not a regression: run 5 completed 9.56M orders where run 4 managed 8.39M and then stopped
producing reports entirely. More orders completed means more of them counted.

### The new instrumentation earned its place

`GrowthReportingAllocator` logged four warnings, one per doubling -- 156, 312, 624 and
1248 MB. In run 4 the equivalent growth to 9.9 GB produced **no memory warning at all**.
`resource_usage.csv` recorded every component's RSS every five seconds alongside the
machine's MemAvailable, so the plateau above is measured rather than inferred.

Chart: `docs/measurements/trading_day_with_cancels.png`.

### Triage: this is a framework requirement, not an application bug

By the rule above, the tempting reading is that the matching engine picked the wrong container and
should call `reserve()`. That would remove the symptom from this stub and teach nothing.

The finding is category 3. **The framework offers slab and pool allocators for messages in
flight, and nothing at all for long-lived hot-path state that grows.** Every application built on
`pubsub_itc_fw` that keeps state — an order book, a session table, a subscription registry — will
grow a container on a reactor callback thread and will eventually stall it. The framework should
offer a growable structure that rehashes incrementally, or in the background, or at least a way to
be told the growth is coming.

Recorded in the roadmap. `reserve()` in the matching engine would be a workaround for the stub,
not the answer.

---

## Runs 7 and 8, 2026-08-09: confirming the allocator tripwire fix

Run 6 killed the binary gateway with a **false** allocator tripwire: the machine was exhausted,
the reactor thread was descheduled between computing the deadline and the first check, and
`drain_empty_slab_queue()` threw on iteration one against an empty queue, blaming lock-free
corruption. Fixed in `a47da86`, which requires both a spent budget and a loop that has actually
spun. Two runs were needed to confirm it, because each reproduced a different half of the
original conditions.

### Run 7 — the deschedule, without exhaustion

`cancel_ratio 0.48`, 83 minutes. All seven phases PASS, **9,556,000 of 9,556,000 orders
acknowledged**, nothing died, MemAvailable floor 3.48 GB.

The gateway's reactor callback thread nevertheless exceeded the tripwire's one-second budget
**seven times, peaking at 1,822 ms**, all at the sustained/lull phase boundary — and nothing
threw. That is the tripwire's precondition reproduced without the memory pressure.

### Run 8 — the exhaustion, without the deschedule

`cancel_ratio 0.10` and a 90-minute sustained phase, chosen to reach the next order-book growth
step. Peak RSS summed to **37.20 GB on a 32 GB machine**, so the venue genuinely demanded more
memory than exists. The book crossed 2^23 at the predicted minute:

```
11:17:49  order book storage growing -- allocating 9984 MB (largest so far 9984 MB)
11:17:52  Out of memory: Killed process (matching_engine) anon-rss 12,935,836 kB
11:19:39  MemAvailable floor 0.42 GB          (run 6 bottomed at 0.48 GB)
```

The 15.9 GB `total-vm` in the kill record is 4,992 + 9,984 MB: the engine was holding the old
table and the new one at once. **No tripwire, no `bad_alloc`, no exception in any component.**
The gateway logged zero callback stalls over a second and ran on for 31 minutes to a clean
`Reactor::run has finished`.

### What this does and does not establish

Neither run is the whole of run 6, which had both halves at once. Between them both halves have
now been reproduced without a false trip, and the decision itself is pinned by unit tests on
`drain_loop_has_stalled()` — including `(1, true)`, which is the production failure. That is
corroboration plus a test, rather than two independent proofs, and it is worth being clear about
which is which.

### The growth curve, and why `initial_capacity` cannot fix it

The engine's memory is a step function of the resting-order count, and the steps are exact powers
of two. Measured across runs 6, 7 and 8, each doubling allocates a new table while the old one is
still live:

| resting orders | engine RSS |
|---|---|
| 2^20 | 1.45 GB |
| 2^21 | 2.80 GB |
| 2^22 | 5.36 GB |
| 2^23 | 12.9 GB at the moment of the kill (old + new) |

Raising `order_book.initial_capacity` moves the sequence up but does not change its shape: the
fatal allocation is always the last doubling, which needs three times the settled size of the
table before it. Only a reservation at or above the peak book avoids it — about 10 GB per engine
at this profile's 9.3M resting orders, and there are two engines. Raising it is still worth doing
to remove the *early* doublings, each of which is a multi-second stall on the reactor callback
thread, but it is a latency improvement rather than a defence against the OOM killer.

---

## Run 10, 2026-08-09: what a member actually experienced

The first run on the recycled-slot allocator and the extended bucket bounds together. It
reached the same state as runs 8 and 9 -- the primary matching engine OOM-killed at the 2^23
rehash, 18:39:54, and the secondary promoted 60 seconds later -- and completed the day.

### The allocator question, answered completely

Run 6 killed the gateway with a false tripwire when its reactor thread was descheduled for
over a second **while the machine was exhausted**. Run 7 reproduced the deschedule without the
exhaustion; run 8 reproduced the exhaustion without the deschedule. **Run 10 had both at
once**: the gateway's own reactor stalled 2,877 ms on a machine that was simultaneously
OOM-killing another process, and the allocator did not throw. No tripwire, no stale-handle
rejection, no registry exhaustion, in any component.

### The bucket bounds, answered too

The `+Inf` bucket was **empty**, where run 9 put 223,824 orders in it. The failover population
now lands in the new bounds, with the mass where the `_sum` arithmetic predicted:

| bound | orders |
|---|---|
| 5s | 4,860 |
| 10s | 9,712 |
| 25s | 29,864 |
| 50s | 45,559 |
| 250s | 97,521 |
| +Inf | **0** |

### But HA was not transparent, and this is the number that matters

The mechanism worked and the venue recovered fully. A member would still have noticed, and
the run says exactly how much. Phase 4 carried the whole outage:

| | phase 4 | every other phase |
|---|---|---|
| p50 | 530 us | 477 us - 1.4 ms |
| **p99** | **45.2 seconds** | 1.5 - 3.4 ms |
| p99.9 | 99.4 seconds | 3.8 - 33 ms |
| worst | **104.7 seconds** | 6 - 173 ms |
| orders unacknowledged | **36,536** | none |
| result | **FAIL** | PASS |

**What a member saw was silence, then extreme lateness.** No session was disconnected, no
order was rejected, nothing was reported as dropped -- the gateway stayed up throughout. One
order in a hundred waited over 45 seconds for its acknowledgement and the worst waited nearly
two minutes, and 36,536 never got one inside the client's window at all.

Those orders were WAL-committed and would have been replayed into the promoted engine, so the
venue very likely processed them; what failed is that the reports did not reach the client in
time. That is a distinction worth keeping: *probably arrived later* is not *acknowledged*.

**Recovery was complete and immediate.** Phases 5, 6 and 7 all passed on the promoted
secondary, with p99 back to 1.9 ms -- latency returned to its previous shape rather than
settling somewhere worse. The cost of this failover was bounded and paid once.

### The whole run, in one line

| | |
|---|---|
| orders sent | **13,016,000** |
| cancels sent | 1,299,576 |
| messages sent | 14,315,576 |
| reports received | 14,275,388 |
| **orders acknowledged** | **12,979,464 -- 99.719%** |
| orders not acknowledged | 36,536 -- 0.281% |

Six of the seven phases reconciled exactly: orders plus cancels equals reports, nothing
outstanding. The entire shortfall of 40,188 reports sits in the one phase that contained the
failover.

Thirteen million orders through a venue that exhausted a 32 GB machine, lost its matching
engine and failed over, with 99.719% acknowledged and every loss confined to a two-minute
window.

Logs and phase artefacts kept outside the tree, since `installed/` does not survive a
rebuild: `pubsub-run-archives/run10-20260809/`.

---

## Prerequisites

- `metrics_enabled = true` — dev only today, which is where this runs.
- Prometheus running, on the background cores, so the collector does not perturb what it collects.
- **The hot-path cores quietened first.** Until irqbalance is restricted and IRQs steered off
  cores 1–14, the tail of the distribution during a burst is partly measuring interrupt noise —
  and the tail is the entire point. The last audit found 51 IRQs pinned to hot cores, up from 15.
- `cpu_audit.py` green, so the run is measuring the declared layout.

---

## Built as an extension of `perf_run.py`

Not a new script. `perf_run.py` already holds the parts that are easy to get wrong and were got
wrong once already:

- `wait_for_fix_logons()` — verifying every session is logged on before load starts. It replaced a
  `time.sleep(3.0)`, and the gap it left meant one client silently contributed nothing.
- Loss accounting from `GW-NOS-RECV` / `GW-ER-SENT`, which knows to distinguish *the venue lost it*
  from *the harness never sent it*. Getting that backwards once produced a report of 5,000 phantom
  losses.
- Client-log capture off by default, because making the load generator write ~5 MB per client
  during a timed run perturbs the very rate being offered.

A trading-day profile that reimplemented these would reproduce their bugs. The new work is the
phase scheduler, the profile file, the manifest and the assertions.

---

## Settled

- **The sustained phase runs at its real duration**, never compressed. Accumulation is what it
  exists to find, and there is no shortcut to an hour of elapsed time.
- **The ME never fills.** `MatchingEngineThread.cpp` emits only `New`, `Canceled` and `Rejected`,
  so orders rest until cancelled and the outstanding-order bookkeeping is everything sent minus
  everything cancelled.
- **Cancel load is a ratio, not an absolute**, bounded above by the order rate. Stream and sweeps
  are separate parameters.
- **Every phase rate is a fraction of a probed ceiling**, so the profile transfers between machines.

## First run

The figures in the sketch above are a plausible first calibration for one 32-core workstation and
nothing more — they may well be too high. That is what the probe run is for, and why nothing in the
profile is written as an absolute. Expect the first real run to move them.

---

## See Also

- [Testing and Code Coverage](../testing.md) — the report-not-gate principle these assertions follow
- [Metrics](metrics.md) — `order_round_trip_nanoseconds` and its bucket bounds
- [CPU Core Layout](cpu_pinning_anti_affinity.md) — why the cores must be quiet first, and cpu_audit.py
