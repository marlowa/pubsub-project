# Latency: what is established, and what is ruled out

What is known about this venue's latency, what measurement has ruled out, and what is still
unexplained. Each entry says what was measured and what follows from it, so that a change that has
already been tried and found not to help is not tried again.

A finding here is a measurement on the development workstation: a hybrid processor with eight
performance cores, paired as two hardware threads each, and sixteen efficiency cores. Where a
conclusion depends on that hardware, it says so.

---

## The instruments

| Metric | What it measures | Where to read it |
|---|---|---|
| `itc_queue_latency_nanoseconds` | A message arriving at an application thread: enqueue to dispatch | Every component |
| `reactor_command_latency_nanoseconds` | A command leaving an application thread for its reactor | Every component |
| `order_round_trip_nanoseconds` | Order off the connection to the first byte of its report | The gateway that received it |
| `order_path_elapsed_nanoseconds` | How far through the venue an order was when it reached a given point | Every component on the path |

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

    python3 scripts/measurement_machine.py              # which state is this machine in?
    sudo python3 scripts/measurement_machine.py --on    # make measurements mean something
    sudo python3 scripts/measurement_machine.py --off   # put it back afterwards

They are deliberately not left switched on. Disabling the deep idle states costs power and fan
noise continuously, and on a machine used for anything else that is a poor trade for most of its
life. Switch them on for a measuring session and off after it.

Forgetting is the failure that actually costs something, because it is silent: the run completes,
the report renders, and the figures are simply untrue. So `perf_run.py` refuses to start on a
machine in that state, and `fix_load_client.py` says so before it connects.

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

**A core that has been idle executes at about one seventh of its speed for the first forty
microseconds after it is woken.** Measured by timing a fixed piece of dependent integer
arithmetic, in registers, touching no memory at all, in twelve equal pieces as the core wakes:

| | piece 1 | 2 | 3 | 4 | 5 onwards |
|---|---|---|---|---|---|
| busy, no gap | 2.19 us | 2.19 | 2.19 | 2.19 | 2.19 |
| after 10 ms idle, thread blocking | 15.09 us | 15.09 | 15.08 | 5.58 | 2.20 |
| after 10 ms idle, thread polling | 2.20 us | 2.42 | 2.68 | 2.68 | 2.68 |

The ratio names the cause exactly: 15.09 divided by 2.19 is 6.89, and 5500 MHz divided by
6.89 is 798, which is the 800 MHz an idle core reports. It is the clock, not cold cache --
nothing in that loop can miss one.

**It is the thread blocking that does it, not the gap.** The third row has the same 10
millisecond gap as the second and runs at full speed from the first instruction, because a
thread that polls never lets its core go idle. Polling settles about 20 per cent below peak,
at the turbo frequency a core sustains under continuous work, which is a small price against
a sevenfold penalty.

What it is not: the processor's deep idle states, which are switched off, and the two that
remain cost a microsecond to leave. Nor the socket, at 4.2 microseconds one way when busy.
Nor waking a thread, which a futex round trip puts at about 1.7 microseconds each way.

**Raising the frequency floor does not help.** `min_perf_pct` has no effect on this machine,
where `intel_pstate` runs in active mode with hardware P-states -- it is accepted and never
reaches the per-processor policy. Setting `scaling_min_freq` to the maximum on all eight
performance cores does reach it, and still changes nothing: the hardware chooses frequency
from observed utilisation, and a core whose thread is blocked has none. It moved the knee in
the loopback measurement from 800 microseconds of idleness to about 1.2 milliseconds and left
the venue at 100 orders a second exactly where it was, at 193 microseconds. Not worth the
power it costs.

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

### Almost all of the round trip is moving messages between processes

Each component records how long an order had been inside the venue when it reached that
component, all counted from the moment the gateway read it off the client connection, so the
difference between two of them is the time spent in between. See `order_path_elapsed_nanoseconds`
in [metrics.md](metrics.md).

Measured over a thirty-minute trading-day session driving both gateways in lockstep at identical
rates, 234,000 orders through each, with the reactor threads polling for work as they are
configured to do. The session moves through the phases a trading day has, from fifty orders a
second to three hundred, so these medians are taken across that whole range rather than at one
rate.

| Stage | Median | |
|---|---|---|
| Gateway decodes and validates the order | 3.79 us | work |
| Gateway to sequencer | 18.89 us | transport |
| Sequencer, including the log commit | 1.38 us | work |
| Sequencer to matching engine | 13.91 us | transport |
| Matching engine matches | 3.40 us | work |
| Matching engine to sequencer | 24.42 us | transport |
| Sequencer handles the report | 6.21 us | work |
| Sequencer to gateway | 29.30 us | transport |
| Gateway encodes the report and sends it | 0.88 us | work |
| **Round trip** | **102.19 us** | |

**About 87 microseconds is spent moving messages between processes and about 16 doing work.** The
matching is 3.40 of it and the write-ahead log commit is inside the sequencer's 1.38.

The legs sum to 102.19 against a measured round trip of 102.19, which is the check that the points
are where they are meant to be. If they ever stop adding up, a point is in the wrong place.

The gateway's decode is measured on the FIX gateway. The sequencer and matching engine checkpoints
carry no label saying which protocol an order arrived by, so every other row is both gateways
pooled. The binary gateway decodes in about 0.05 microseconds, so for an order arriving that way
the first row is very nearly zero and the leg after it is correspondingly longer.

**Eighty-five per cent of what remains is transport, and it is not the socket.** A message over the
loopback interface costs about 4.2 microseconds one way when the receiver is busy, so four hops
should cost somewhere near 17 microseconds rather than 87. Every hop is on one machine over
127.0.0.1, so no network card is involved in any of it and nothing a card could do would change it.
Where the rest of the time goes is the largest open question about this venue's latency.

The outbound legs cost about twice the inbound ones -- 24.42 and 29.30 against 18.89 and 13.91 --
and that is not explained either.

### Most of the transport cost is the hand-off out of a component, not the socket

Every message a component sends crosses two inter-thread hand-offs and one socket. The
application thread does not touch the socket: it puts a command on the reactor's queue and wakes
the reactor, and the reactor does the writing. At the far end the receiving reactor reads the
socket and puts the message on its own application thread's queue.

| Direction | Metric |
|---|---|
| Application thread to its reactor, on the way out | `reactor_command_latency_nanoseconds` |
| Reactor to its application thread, on the way in | `itc_queue_latency_nanoseconds` |

Over 6,000 orders at 100 per second, the outward half costs several times the inward one:

| Component | Out: application thread to reactor | In: reactor to application thread |
|---|---|---|
| sequencer | 13.27 us | 2.34 us |
| matching engine | 6.55 us | 3.56 us |
| FIX order gateway | 6.50 us | 3.39 us |

Medians. The order path crosses eight of these hand-offs, which comes to about 51 microseconds
against the 87 the stage profile attributes to moving messages between processes. Four socket
crossings at the measured 4.2 microseconds each account for about 17 more.

**The wait is on the order's own critical path, not merely somewhere in the component.** Raising
one component's `quiet_spins_between_polls` from 64 to 4096 raised its own outward hand-off from
13.27 microseconds to 100.44 and the round trip from 99.74 to 367.93. An order crosses the
sequencer twice, so a hand-off charge appearing twice over is what that looks like.

The figure pools every command a component sends, including replication and the external
subscriber stream, which no member waits for. So it describes the queue the order's own command
sits in rather than the order's command alone.

### A polling reactor should find a send itself rather than be told

An application thread that wants something sent puts a command on its reactor's queue. Waking the
reactor to look at it costs a system call on the sending thread and another on the reactor to
drain the wakeup descriptor. A reactor that is polling is already going round a loop and will find
the command by itself, so both are avoidable: it checks the queue every time round, including
between the quiet spins, and the sender skips the wakeup while the reactor says it is polling.

Over 6,000 orders at 100 per second, first run discarded, client pinned:

| | Before | After |
|---|---|---|
| Round trip, median | 99.74 us | 93.66 us |
| Matching engine, application thread to reactor | 6.55 us | 4.09 us |
| Gateway, application thread to reactor | 6.50 us | 6.09 us |

The round trip is repeatable: two runs of the changed venue gave 93.66 and 93.50 microseconds.

**The sequencer's own hand-off cannot be read from this.** Two runs of identical code gave 19.14
and 10.57 microseconds, so the figure is not stable between runs at this sample size and nothing
should be concluded from a difference in it either way. The sequencer sends roughly eight commands
per message, far more than the other components, and how they fall into batches evidently varies.

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

### Looking for work more often

A polling reactor spins quietly between one look for work and the next, and
`reactor.quiet_spins_between_polls` sets how many spins. At the default of 64, and a measured
30.1 nanoseconds per spin, that is about 1.9 microseconds between looks.

Lowering it to 8 changed nothing: a round-trip median of 101.31 microseconds against 99.74, and
the outward hand-off unmoved at 14.57 microseconds against 13.27. Both differences are within the
run-to-run spread.

The setting does work, and the control proves it: raised to 4096 it took the same hand-off to
100.44 microseconds and the round trip to 367.93. It is simply already small enough at 64 that
whatever else the hand-off costs swamps it. Looking more often only buys back time that is not
being spent.

### The reactor being slow to come back and look

The reactor notices nothing between two calls to `epoll_wait`, so a message from another process
waits for the next one, and on average half that interval. If that interval were long it would
explain the twenty-two microseconds unaccounted for between the matching engine and the sequencer.
It is not. `reactor_lap_nanoseconds`, over 6,000 orders at 100 per second:

| Component | Laps | p50 | p90 | p99 | mean |
|---|---|---|---|---|---|
| gateway | 25,434,461 | 3.28 us | 4.66 us | 4.97 us | 2.53 us |
| sequencer | 25,279,343 | 3.19 us | 4.64 us | 4.97 us | 2.55 us |
| matching engine | 25,379,464 | 3.50 us | 4.70 us | 4.97 us | 2.54 us |

Every reactor comes back about every three microseconds, the tail reaches five, and all three
agree. A message waits about 1.6 microseconds on average to be noticed, which cannot be a
twenty-two microsecond leg.

**The figure also corroborates two earlier measurements it was not designed to test.** Sixty-four
quiet spins at the 30.1 nanoseconds measured for one `PAUSE` is 1.9 microseconds, and asking the
kernel costs about one more. That is the three microseconds observed, from two numbers taken by
different means on different days.

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

### The crossing between two processes, measured rather than inferred

`PduHeader::sent_at_ns` carries `CLOCK_MONOTONIC` nanoseconds, written by the sending reactor as
it hands the frame to a socket and read back by the receiving parser as soon as the header is
whole. `pdu_wire_nanoseconds` is the difference: the kernel's carriage of the bytes plus however
long the receiving reactor took to come back and look. It is comparable only between processes on
one host, and a reading that is negative or beyond a quarter of a second is counted in
`pdu_wire_unusable_total` rather than recorded.

| Receiving component | Crossings | Median | 90th |
|---|---|---|---|
| gateway, reports from the sequencer | 12,008 | 5.74 us | 9.16 us |
| matching engine, orders from the sequencer | 12,105 | 7.66 us | 9.96 us |
| sequencer, several senders pooled | 72,355 | 14.57 us | 42.13 us |

**A crossing costs five to eight microseconds, not four.** The four microseconds quoted elsewhere
on this page came from two processes doing nothing else; these are components under load.

### Adding up medians does not work, and three instruments have now shown it

With the crossing measured, a leg should be five measured parts and nothing left over. One is:

| Leg | Leg | Request wait | Send | Crossing | Receive | Queue | Left over |
|---|---|---|---|---|---|---|---|
| sequencer to gateway | 28.79 us | 12.26 | 3.84 | 5.74 | 2.79 | 3.52 | **0.62 us** |
| sequencer to matching engine | 15.29 us | 12.26 | 3.84 | 7.66 | 5.20 | 3.64 | **-17.31 us** |

The second is impossible, and it is impossible in a way that rules out a missing instrument
rather than suggesting one. **The receiving side alone -- crossing 7.66, receive path 5.20, queue
wait 3.64, totalling 16.50 -- already exceeds the whole 15.29 microsecond leg**, before the
sending component contributes anything.

**The reason is the arithmetic, not the measurements.** Every figure in that table is a median
over every message of its kind that the component handled. The matching engine's crossings
include replication and book updates as well as orders; its receive path and queue wait likewise.
Subtracting a median drawn from one population from a median drawn from another is not a valid
operation, and it happens to come out right only when one population dominates. That is why the
gateway leg closes and the matching engine leg does not.

**This has now been demonstrated three times with three different instruments**: separating the
sending half from the receiving half, separating what a member waits for from what nobody waits
for, and measuring the crossing directly. Each closed one more leg and left the same objection
standing. A fourth per-component histogram will not help.

**What would work is measuring one order rather than averaging many.**
`order_path_elapsed_nanoseconds` already does this for the stages: it stamps the same order at
successive points and every figure describes that order. The same approach inside the reactor --
checkpoints on the frames of the order path, counted from the same origin -- would give a profile
that adds up by construction, with no medians subtracted from one another.

### Eight bytes on every frame costs about four microseconds, and it is not understood

`PduHeader` grew from 24 bytes to 32 to carry a send time. Eight runs of the same load, alternating
between the two builds on one machine in one sitting:

| Header | Runs, round-trip median | Mean |
|---|---|---|
| 32 bytes | 100.98, 101.43, 101.73, 102.47, 104.49 | 102.22 us |
| 24 bytes | 97.58, 98.22, 98.22 | 98.01 us |

The lowest 32-byte run is above the highest 24-byte run, so the effect is real rather than run to
run variation. The 32-byte builds are also noticeably less repeatable: they span 3.5 microseconds
where the 24-byte builds span 0.6.

**It is the bytes, not the work done with them.** A third build kept the 32-byte header and did no
stamping and no recording: 101.43 and 102.47, which is the 32-byte figure. The clock reads and
histogram observations cost about 1.2 microseconds of the total; the rest follows the size of the
frame.

**Three things it is not.**

The allocator. No difference in expansions, slow-path allocations or slab chaining between the two
builds.

The gateway. Every gateway-local measurement is the same in both: its own decode and forward
within 0.13 microseconds, its request wait within 0.14, its send path within 0.01, its receive
path within 0.01, its queue wait within 0.04.

The outward path. The stages from the member to the matching engine agree between builds to within
0.25 microseconds. The difference accumulates entirely on the report path.

**What the traffic suggests.** Every PDU in the venue grew, and components send very different
numbers of them. Over one run of 23,377 orders and cancels: the gateway sent 46,861 PDUs, the
matching engine 35,455, and the sequencer 95,888 -- about four per message, because it also
replicates to the standby, feeds the subscriber stream and acknowledges the log. A per-PDU cost
would therefore fall most heavily where the sequencer works, which is where the difference appears.
That is consistent with the measurements and is not established by them.

**Why it matters beyond the instrument that found it.** If frame size has an effect of this
magnitude, it applies to any message that grows, not only to this one. Whatever is behind it is a
property of the message path.

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

### Binary against FIX, end to end

Both gateways driven at identical rates in lockstep by `dashboard_load.py --mode compare`,
31,200 orders each, with both reactors looking for work rather than sleeping:

| | The gateway's own decode | End to end, median | p90 | p99 |
|---|---|---|---|---|
| FIX | 3.78 us | 105.12 us | 146.17 us | 186.29 us |
| Binary | **0.05 us** | **103.29 us** | **132.39 us** | **174.68 us** |

Binary is faster on every measure, and only slightly so at the median: it decodes 76 times
faster and that buys 1.8 microseconds of the round trip. The round trip is dominated by what
the two protocols share -- the sequencer, the matching engine, the log, and the report path --
and the decode is a small part of it. The clearer gain is at the tail, 13.8 microseconds at the
90th percentile and 11.6 at the 99th, which is an argument about consistency rather than speed.

**This reverses an earlier finding and the earlier one should not be quoted.** It said binary
was *slower* end to end at the median, by 69 microseconds, despite decoding far faster. That
was measured at 155 orders a second, which is well inside the range where a core waking from
idle dominated every figure, so it compared which gateway happened to wake more slowly rather
than the protocols themselves.

### What it would cost to stop the venue's threads sleeping

Not letting a thread sleep removes the penalty, which is established: with the reactor threads
of the gateway, the sequencer and the matching engine polling for work instead of sleeping on
it, the same load at 100 orders a second gave a round trip of **134.1 microseconds against
194.8**, a saving of 60.

The components' own work is where it shows most plainly. The sequencer's handling of an order
fell from 12.0 microseconds to 0.8, and the matching engine's from 11.3 to 3.2 -- the same code
doing the same work at full speed rather than at 800 MHz.

**Giving each thread a core of its own makes it worse, not better.** The obvious objection to
the above is that a spinning reactor starves the application thread sharing its physical core,
so each thread should have one to itself. Measured, with the same three components and the same
load:

| | threads blocking | threads polling |
|---|---|---|
| a component's two threads share one core | 192.70 us | **111.67 us** |
| each thread on a core of its own | 199.61 us | 146.64 us |

Separating them costs 6.9 microseconds when the threads block and 35.0 when they poll. The
reason is the one above about level-one cache: the reactor and application threads of one
component exchange messages constantly, and separating them puts a cache boundary in the middle
of the hottest exchange the venue has. That costs more than the contention it avoids.

**So the best arrangement is the pairing already deployed, with the threads polling: 111.67
microseconds against 192.70, a reduction of 42 per cent.** The sequencer's own handling of an
order falls from 11.7 microseconds to 0.37.

**Polling the application threads as well buys little for double the cores.** Three
arrangements, same build, same load, same three components:

| what polls | round trip | threads burned |
|---|---|---|
| nothing | 192.12 us | none |
| each component's reactor thread | 111.84 us | 3 |
| its reactor and its application thread | 102.56 us | 6 |

The reactors are where nearly all of it is: they buy 80 microseconds for three threads. Adding
the application threads buys 9 more for another three, and part of that is given straight back
-- the sequencer's handling of the report went from 6.6 microseconds to 17.6, because a
component's two threads share one physical core and both were now spinning on it.

So the arrangement worth having is the reactors polling and the application threads left to
block: 42 per cent, for three of the seven performance cores this machine has for the hot path.

This is also why that figure beats the 134.14 measured with the same polling but every component
ranked for a hot-path core: taking the binary gateway, the publishers and the standby instances
off the hot path was worth another 22 microseconds, because fourteen threads on eight cores
costs the three that matter.

**This is now a setting rather than an experiment.** `reactor.spin_before_block` in a
component's configuration says how long its reactor keeps looking for work before it sleeps, and
the four components of the order round trip are switched on in the development environment. The
other environments are left off, because whether a deployment spends cores on waiting is a
decision about that deployment.

Measured as deployed, with nothing edited by hand:

| | Round trip |
|---|---|
| Every reactor sleeping | 193.53 us |
| The four on the order path looking | **93.13 us** |

That is 52 per cent, for four cores of the seven the hot path has.

**Two details decide most of it.** The first is that the loop mostly spins quietly and asks the
kernel only now and then -- a quiet spin costs about 30 nanoseconds and keeps the core just as
awake as a system call does, while a system call costs a thousand times that and disturbs
whatever shares the physical core. An earlier attempt that asked the kernel every iteration made
its own application thread nearly three times slower.

The second is that **the standby sequencer has to be looking too**. The primary holds each
report until the standby acknowledges the log record, so a sleeping standby puts its wake-up
cost straight onto the member's critical path: with it asleep that single leg cost 31.59
microseconds, and with it awake, 1.09.

---

See also [trading_day_load.md](trading_day_load.md) for the load profile and what a passing run
does and does not prove, [filesystem_requirements.md](filesystem_requirements.md) for the mount
option the sequencer's log needs, and [cpu_pinning.md](../framework/cpu_pinning.md) for how the
layout is computed and audited.

---

Back to the [documentation contents](../README.md).
