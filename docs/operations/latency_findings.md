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

### An idle machine is slow to start again, and that is most of the round trip

At 100 orders a second an order arrives every 10 milliseconds. Every thread on the path has
time to fall asleep between one order and the next, and starting again costs far more than
any of the work the venue does.

Measured without the venue at all, by two processes passing a 400-byte message back and forth
over a loopback socket, each pinned to a core of its own:

| Gap between messages | Round trip |
|---|---|
| none, sent back to back | 8.4 us |
| 100 us | 8.6 us |
| 400 us | 9.2 us |
| 600 us | 9.3 us |
| 800 us | 26.3 us |
| 1 ms | 49.5 us |
| 10 ms | 50.3 us |

**The same exchange costs six times as much once the receiver has been idle for a
millisecond**, and the change happens somewhere between 600 microseconds and 1 millisecond.

The venue shows the same thing. Driven hard enough that its threads stop sleeping, it answers
in less than half the time:

| Offered rate | Gap between orders | First leg | Round trip |
|---|---|---|---|
| 100 per second | 10 ms | 50.3 us | 193.8 us |
| 400 per second | 2.5 ms | 35.7 us | 164.5 us |
| 800 per second | 1.25 ms | 47.2 us | 191.9 us |
| 1200 per second | 0.83 ms | 17.1 us | 89.6 us |

**Read every other figure on this page in that light.** They were all taken at 100 orders a
second, so they describe a venue on an idle machine. Comparisons between them remain sound,
because both sides of each comparison paid the same charge, but the absolute numbers are not
what a busy venue would show.

What it is not: the processor's deep idle states, which are switched off, and the two that
remain cost a microsecond to leave. Nor the socket, which costs 4.2 microseconds one way when
busy. Nor waking a thread, which a futex round trip puts at about 1.7 microseconds each way.
What does cost 20 microseconds or more once a core has been idle for a millisecond is not yet
established.

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

### Three quarters of the round trip is moving messages between processes

Each component records how long an order had been inside the venue when it reached that
component, all counted from the moment the gateway read it off the client connection, so the
difference between two of them is the time spent in between. See `order_path_elapsed_nanoseconds`
in [metrics.md](metrics.md).

Over 6,000 orders at 100 per second:

| Stage | Median | |
|---|---|---|
| Gateway decodes and validates the order | 8.6 us | work |
| Gateway to sequencer | 40.9 us | transport |
| Sequencer, including the log commit | 12.0 us | work |
| Sequencer to matching engine | 56.6 us | transport |
| Matching engine matches | 12.8 us | work |
| Matching engine to sequencer | 20.5 us | transport |
| Sequencer handles the report | 5.4 us | work |
| Sequencer to gateway | 26.0 us | transport |
| Gateway encodes the report and sends it | 2.2 us | work |
| **Round trip** | **185.0 us** | |

About 144 microseconds is spent moving messages between processes and about 41 doing work.
The matching is 12.8 of it and the write-ahead log commit is inside the sequencer's 12.0.

The legs sum to 185.02 against a measured round trip of 185.01, which is the check that the
points are where they are meant to be. If they ever stop adding up, a point is in the wrong
place.

**The transport figures are mostly the idleness charge above, not the socket.** A loopback
message costs 4.2 microseconds one way when the receiver is busy. Every hop is on one machine
over 127.0.0.1, so no network card is involved in any of it and nothing a card could do would
change it.

The outbound legs cost about twice the inbound ones, which is not explained.

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

### Spinning before blocking, as it was measured

`ApplicationThread` can spin on an empty queue before it blocks, which trades processor time for
the wakeup a blocked thread would otherwise pay. Measured with a sweep over seven windows, the
best of them was 200 microseconds: it caught about one wait in three, bought about 9 per cent of
the mean inter-thread hop, and nothing at all at the 90th or 99th percentile.

**That sweep could not have found the effect that matters, and the conclusion should not be read
as more than it is.** Every window tried was 200 microseconds or shorter, and the load was 100
orders a second, which leaves 10 milliseconds between one order and the next. A thread spinning
for 200 microseconds out of every 10 milliseconds is asleep for 98 per cent of the gap and starts
again from cold exactly as it would have done. Keeping a thread awake across a gap that long
means spinning for most of it, which costs a core, and the sweep never tested that.

So `spin_before_block` is left at zero, and what has been ruled out is the cheap version of it.
Whether a thread that never sleeps at all is worth the core it burns is an open question, and now
a measurable one: an idle core costs roughly 20 microseconds per hop to restart, and there are
four hops.

It is a field of the thread configuration with no key in any TOML file, set only in code, so
repeating the sweep means rebuilding.

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

### Why the round trip's median appeared never to move

It appeared fixed at 175 to 180 microseconds under every machine configuration tried. Two
things were behind that, and neither was the venue being insensitive to change.

**Its bucket boundaries could not resolve anything in the range where the orders were.** Of
12,000 orders, 11,597 fell into a single bucket 150 microseconds wide, between 100 and 250.
A median drawn from that is not a measurement: it is linear interpolation across one bucket,
which comes out at 176.9 and stays there for almost any distribution inside it. Boundaries
between 100 and 250 microseconds have since been added, and the same venue then reads 185 --
so every round-trip figure recorded before that was understating by roughly 8 microseconds.

**What was left was mostly the idleness charge**, which is the same whatever the boot
parameters are, so changing them could not move it.

At 1200 orders a second the median is 89.6 microseconds. The round trip moves a great deal;
it just does not move in response to anything that was being changed.

### Binary is slower end to end than FIX at the median

The same orders that binary decodes 34 times faster at the median arrive back later:

| | FIX | Binary |
|---|---|---|
| median | 94.2 us | 163.0 us |
| 90th percentile | 221.9 us | 235.2 us |
| 99th percentile | 430.4 us | 388.8 us |

Both sets of figures were taken at 155 orders per second each, which is well inside the range
where the idleness charge above dominates, so they compare two protocols on an idle machine.

Ruled out: client pacing, since both clients held their offered rate smoothly, and queueing, since
the inter-thread waits were 3.97 against 3.84 microseconds at the median. The unmeasured stage is
the outbound half — encoding the execution report and sending it — which needs a second timestamp
rather than a second metric family, because the report path carries no time origin of its own.
[metrics.md](metrics.md) lists it under Open.

### Why leaving a core idle costs 20 microseconds

That it does is established above and measured two ways. Why is not. The deep processor idle
states are switched off and the two remaining cost a microsecond to leave, the socket costs
4.2 microseconds and waking a thread about 1.7, so none of those accounts for it. The change
happens between 600 microseconds and 1 millisecond of idleness, which is a clue and not yet an
answer.

This is the most valuable thing on this page to settle, because it is the largest single term
in what a member waits for.

---

See also [trading_day_load.md](trading_day_load.md) for the load profile and what a passing run
does and does not prove, [filesystem_requirements.md](filesystem_requirements.md) for the mount
option the sequencer's log needs, and [cpu_pinning.md](../framework/cpu_pinning.md) for how the
layout is computed and audited.

---

Back to the [documentation contents](../README.md).
