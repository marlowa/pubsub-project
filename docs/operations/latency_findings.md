# Latency: what is established, and what is ruled out

What is known about this venue's latency, what measurement has ruled out, and what is still
unexplained. Each entry says what was measured and what follows from it, so that a change that has
already been tried and found not to help is not tried again.

A finding here is a measurement on the development workstation: a hybrid processor with eight
performance cores, paired as two hardware threads each, and sixteen efficiency cores. Where a
conclusion depends on that hardware, it says so.

---

## The two instruments

| Metric | What it measures | Where to read it |
|---|---|---|
| `itc_queue_latency_nanoseconds` | One inter-thread hop: enqueue to dispatch | Every component |
| `order_round_trip_nanoseconds` | Order off the connection to the first byte of its report | The gateway that received it |

The hop is the sensitive instrument. It has roughly nineteen thousand observations in a run of six
thousand orders, so its median and 90th percentile are stable to within a few per cent between
runs of the same configuration. The round trip has exactly one observation per order, which is
what makes it comparable between runs but far too few for its tail.

**Do not draw conclusions from the round trip's 99th percentile at this sample size.** Six
thousand orders put sixty observations above it. Across six configurations it ranged from 471 to
3010 microseconds with no consistent relationship to what had been changed, including between the
two arms of one controlled comparison.

## How a comparison is run

```bash
taskset -c 0,1 python3 scripts/fix_load_client.py --port 9879 --orders-per-burst 6000 \
    --bursts 1 --rate 100 --cancel-ratio 0.9 --drain 4 --first-cl-ord-id <unused range>
```

Four rules, each of which has produced a wrong answer when broken.

**Pin the load client.** An unpinned client is not a fixed condition, so two unpinned arms are not
the same experiment run twice. Pinned to processors 0 and 1, three runs agreed on the hop's 99th
percentile to within 0.15 microseconds; five unpinned runs of the same load spanned fifteen
microseconds. The client's own placement also decides which hardware encodes the orders, which is
how a comparison between two boot configurations acquires an uncontrolled variable.

**Discard the first run.** It is always cold.

**Compare hop means only when the observation counts are close.** Every message dequeued produces
exactly one observation and nothing is batched, so about eleven thousand four hundred observations
per run are per-message and constant while the rest scale with how many socket reads the arriving
orders are spread over. A quicker client packs more messages into each read and so produces fewer
observations, which moves the mean without anything in the venue having changed.

**Read the histogram by subtracting the component's own metrics endpoint either side of the run**,
rather than through Prometheus, so that nothing depends on scrape alignment.

**Check the machine's power settings first.** They do not survive a reboot, and without them every
figure below is wrong by a factor of five to seven.

---

## Established

### Power settings dominate everything else

The governor and the processor idle states are worth more than every other finding on this page
put together.

```bash
cpupower frequency-set -g performance
cpupower idle-set -D 100
```

With the `powersave` governor the processors run at 800 MHz to 1.6 GHz against a 5500 MHz maximum.
With idle states C2 and C3 enabled, a core that has gone to sleep costs 127 or 1048 microseconds
to wake, and every hop in this venue hands work to a thread that may be asleep.

| Round trip | Before | After |
|---|---|---|
| median at 50 orders/s | 981.5 us | 185.6 us |
| 90th percentile at 50 orders/s | 2228.8 us | 310.5 us |
| median at 740 orders/s | 397.4 us | 157.8 us |
| 90th percentile at 740 orders/s | 899.5 us | 235.2 us |

**This also accounts for latency being worse when traffic is sparse**, which is backwards for
queueing and was for a time attributed to the thread wakeup alone. Before the settings were
corrected the median at 50 orders per second was more than twice the median at 740; afterwards the
two are within 30 microseconds of each other. A quiet period gives a core time to fall into a deep
idle state, and the first order after the quiet period pays for waking it.

**Neither setting survives a reboot.** Both must be re-applied before anything is measured.

### A component's two threads belong on one physical core

Each component has two hot-path threads, the reactor thread and the application thread, and they
exchange messages constantly. On the two hardware threads of one physical core they share the
level-one cache and the handoff is nearly free; on different physical cores every message crosses
a cache boundary.

| | One thread per physical core | Each component's two threads on one core |
|---|---|---|
| hop mean | 7.17 / 6.96 us | 6.53 us |
| hop 99th percentile | 46.34 / 45.55 us | 40.35 us |
| round trip median | 175.95 / 177.20 us | 165.15 us |

Sharing a physical core is not in itself contention: it depends who the sibling is. Two threads of
one component cooperate, two threads of different components compete. The policy `deploy.py`
implements follows from that — a component's threads fill one physical core, and a physical core
is never shared between components. See
[cpu_pinning_anti_affinity.md](../framework/cpu_pinning_anti_affinity.md).

### Binary decoding costs far less than FIX decoding

Measured with `dashboard_load.py --mode compare` at matched rates on
`order_ingress_to_forward_nanoseconds`, which covers the gateway's own work of parsing, validating
and building the envelope:

| | FIX | Binary | Ratio |
|---|---|---|---|
| mean | 10.20 us | 0.18 us | 57x |
| median | 4.75 us | 0.14 us | 34x |
| 90th percentile | 33.82 us | 0.42 us | 81x |

The spread matters more than the mean: FIX ranges over about 44 microseconds between its median
and its 99th percentile, binary over about 0.8. The supported claim for a binary interface is
lower processor cost per order and far less variation, not a faster end-to-end result — see
**Unexplained** below.

---

## Ruled out by measurement

### Kernel core isolation

`isolcpus=2-15 nohz_full=2-15 rcu_nocbs=2-15 irqaffinity=0,1,16-31` was applied, measured and
removed. The boot command line is stock.

The reasoning for it is sound and remains true: pinning a thread to a processor reserves that
processor for the thread, not from anything else, and only `isolcpus` stops unrelated work being
scheduled there. It simply does not pay here. Measured with the same deployment and the load
client pinned in both arms:

| Hop, client pinned to processors 0 and 1 | With isolation | Without |
|---|---|---|
| mean | 8.63 / 8.41 us | 6.75 / 7.15 / 6.72 us |
| median | 2.50 / 2.49 us | 2.53 / 2.64 / 2.66 us |
| observations | 18,801 / 18,583 | 19,211 / 18,986 / 19,008 |

Isolation costs about 1.7 microseconds of the mean, a fifth, and the median is unchanged, so the
whole of the cost sits above the median. The round trip's median was 175.6 to 180.1 microseconds
in every arm, with and without, which is to say a member experiences no difference at all.

It is not free to leave in place either. With `isolcpus` every unpinned process is confined to
processors 0, 1 and the efficiency cores, so a build loses the performance cores, and three
concurrency tests fail: `ExpandableSlabAllocatorTest.RepeatedSingleChunkAllocDeallocOfCurrentSlabDoesNotHang` livelocks,
`MirroredBufferTest.ExposeStaleHeadVisibilityRace` hangs, and
`CpuPinningTest.PinToDisallowedCoreFails` fails because the allowed set has changed underneath it.

### The cost of entering the kernel under `nohz_full`

`nohz_full` genuinely makes every kernel entry dearer, and removing it genuinely removes that cost.
Measured with the venue stopped, using a `getpid` loop and a futex round trip pinned to one
physical core's two hardware threads:

| | Processors 0 and 1 | Processors 4 and 5 | Processors 10 and 11 |
|---|---|---|---|
| `getpid` with `nohz_full` | 65.2 ns (housekeeping) | 82.8 ns (isolated) | 82.7 ns (isolated) |
| `getpid` without it | 57.2 ns | 57.9 ns | 58.1 ns |
| wake round trip without it | 3.47 us | 3.32 us | 3.15 us |

**And the venue did not speed up.** Three warm runs across two instances gave hop means of 10.97,
11.10 and 10.98 microseconds, against 10.49 and 10.68 with `nohz_full` still applied. The venue
does not enter the kernel often enough for a 25-nanosecond charge to reach the microseconds the
hop is measured in. The nanosecond figures were right; the conclusion drawn from them about the
venue was not.

### Spinning before blocking instead of sleeping

`ApplicationThread` can spin on an empty queue before it blocks, which trades processor time for
the wakeup a blocked thread would otherwise pay. It works, and it is not worth having: a spin
window catches about one wait in three, buys about 9 per cent of the mean hop and nothing at all
at the 90th or 99th percentile. A sweep over seven windows found 200 microseconds the best of
them, and nothing below 100 microseconds is worth having, because the window must reach 200 before
the median message arrives inside it.

`spin_before_block` is therefore left at zero. It is a field of the thread configuration with no
key in any TOML file, set only in code, so repeating the sweep means rebuilding.

Two things make a measurement of it lie, and both are easy to reach for. Printing from inside the
spin loop puts a blocking system call inside a measurement of blocking. Counting behind a modulo
gate reports that the spin never runs when what is wrong is the gate. Check the instrument against
a case whose answer is known before believing either.

### Where the load client runs, as an explanation for outliers

Without `isolcpus` nothing keeps the load client off the hot-path cores, and sampling its
placement directly shows it running on them, including the very processor the gateway's reactor
thread is pinned to. That is a true observation and it is not the explanation it appears to be:
the cleanest of eight runs, with two observations above 100 microseconds, was an unpinned run
sitting on exactly those cores, while another unpinned run had 155.

What the client's freedom produces is variance rather than a penalty, which is why pinning it is
part of the method and not a finding about the venue.

---

## Unexplained

### The hop is in two populations

In every configuration measured — with kernel isolation and without, with the client pinned and
unpinned, at every spin window including zero — the hop's median is 2.5 to 3.2 microseconds and
its 90th percentile is 19 to 23. One message in ten takes roughly eight times the median.

Nothing tried so far moves that ratio. Correcting the power settings, correcting the core layout,
removing a third of the thread wakeups and isolating the cores each changed the mean, and none of
them changed the shape.

### The round trip's median does not move

It is 175 to 180 microseconds under every machine configuration measured, while the hop underneath
it has been moved by a factor of one and a half. A round trip crosses several hops and does other
work besides, so a microsecond or two on one hop is not expected to show; what is unexplained is
that nothing moves it at all.

### Binary is slower end to end than FIX at the median

The same orders that binary decodes 34 times faster at the median arrive back later:

| | FIX | Binary |
|---|---|---|
| median | 94.2 us | 163.0 us |
| 90th percentile | 221.9 us | 235.2 us |
| 99th percentile | 430.4 us | 388.8 us |

Ruled out: client pacing, since both clients held their offered rate smoothly, and queueing, since
the inter-thread waits were 3.97 against 3.84 microseconds at the median. The unmeasured stage is
the outbound half — encoding the execution report and sending it — which needs a second timestamp
rather than a second metric family, because the report path carries no time origin of its own.
[metrics.md](metrics.md) lists it under Open.

### Whether a sparse arrival rate still costs a hop

The inter-thread hop was ten times slower at 120 orders per second than at 400, measured before
the power settings were corrected. Correcting them all but closed the equivalent gap in the round
trip, so most of that effect was the cost of leaving a deep idle state. It has not been
re-measured on the hop since, so how much of it the wakeup still accounts for is not known.

---

See also [trading_day_load.md](trading_day_load.md) for the load profile and what a passing run
does and does not prove, [filesystem_requirements.md](filesystem_requirements.md) for the mount
option the sequencer's log needs, and [cpu_pinning.md](../framework/cpu_pinning.md) for how the
layout is computed and audited.

---

Back to the [documentation contents](../README.md).
