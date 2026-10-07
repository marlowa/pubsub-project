# A histogram that never makes the recording thread wait {#single_writer_histogram}

**Status: agreed, not built.** This is the design for [BUG-0125](../bug_list.md#bug_0125). No code
has been changed. Section 9 records the two decisions it needed.

## 1. What this document covers

Every histogram the venue records is a `prometheus::Histogram` from the prometheus-cpp library,
version 1.3.0. Recording a value into one locks a mutex. The thread that serves the metrics endpoint
locks the same mutex each time Prometheus scrapes the process, so a thread that records a value while
a scrape is reading that histogram goes to sleep in the kernel until the scrape lets go.

This document proposes replacing `prometheus::Histogram` with a histogram of the framework's own, in
which the thread that records values never takes a lock and never waits for any other thread, and the
scrape reads the values without stopping the thread that writes them. It sets out today's behaviour
with the measurements behind it, the design, the alternatives and why they are not recommended, the
one call site that needs a decision, the tests, and the order of the work.

Counters and gauges are not changed. Section 8 explains why.

## 2. What happens today

### 2.1 The call chain

No call site in the venue holds a lock when it records a value. The lock is two calls further down,
inside the library:

1. A call site, for example `SequencerThread::append_to_wal`
   (`applications/sequencer/SequencerThread.cpp`, line 281), calls `HistogramHandle::observe`.
2. `HistogramHandle::observe` (`libraries/pubsub_itc_fw/include/pubsub_itc_fw/HistogramHandle.hpp`,
   line 27) calls `HistogramInterface::observe`.
3. When metrics are enabled, the object behind that interface is a `PrometheusHistogram`
   (`PrometheusHistogram.hpp`, line 17), which calls `prometheus::Histogram::Observe`.
4. `Histogram::Observe` finds the bucket, then takes `std::lock_guard<std::mutex>` on the
   histogram's mutex and adds to the bucket's count and to the running sum (prometheus-cpp 1.3.0,
   `core/src/histogram.cc`, line 46).
5. `Histogram::Collect`, which runs on the endpoint's scrape thread for every histogram on every
   scrape, takes the same mutex (line 76) and allocates a vector while it holds it.

When metrics are disabled, step 3 is a `NoOpHistogram` and nothing is locked.

### 2.2 Who records into histograms

Fourteen call sites record into histograms. Eight of them are in the framework and run in every
process:

| Histogram | Recorded by | Thread |
|---|---|---|
| `reactor_lap_nanoseconds` | `Reactor::record_look_for_work` (`Reactor.cpp`, line 1018) | The reactor's thread, on every pass of its loop and on every turn while it spins waiting for work |
| `reactor_receive_path_nanoseconds` | `Reactor::observe_receive_path` (line 1027) | The reactor's thread |
| `reactor_send_path_nanoseconds`, two scopes | `Reactor::observe_send_path` (lines 1037 and 1039) | The reactor's thread |
| `reactor_command_latency_nanoseconds`, two scopes | `Reactor::process_control_commands` (lines 1226 and 1228) | The reactor's thread |
| `itc_queue_depth` | `ApplicationThread.cpp`, line 478 | The application thread that owns the queue |
| `itc_queue_latency_nanoseconds` | `ApplicationThread::process_message` (line 571) | The application thread that owns the queue |

The other six are in the applications:

| Histogram | Recorded by | Thread |
|---|---|---|
| `order_round_trip_nanoseconds`, `order_ingress_to_forward_nanoseconds` | Both order gateways | The gateway's application thread |
| `order_path_elapsed_nanoseconds`, four scopes | `order_path_metrics::observe_checkpoint`, in the gateways, the sequencer and the matching engine | Each component's application thread |
| `wal_append_nanoseconds` | `SequencerThread::append_to_wal` | **Two threads on the following sequencer.** See section 6 |

Each histogram except `wal_append_nanoseconds` is recorded by exactly one thread. That was checked by
following every call site to the function that runs it, not assumed from the names.

### 2.3 What it costs

Measured on the development machine with prometheus-cpp 1.3.0, the recording thread pinned to one
core, 50 million values recorded in a timed batch:

| | Per value recorded |
|---|---|
| `prometheus::Histogram::Observe`, nothing else touching the histogram | 12.6 ns |
| The same, with another thread reading the histogram once a millisecond | 19.6 ns |
| The same, with another thread reading the histogram continuously | 456 to 526 ns |

With nothing else touching the histogram, `Observe` makes no system call. Once another thread holds
the mutex, the recording thread calls `futex` and sleeps.

In the venue under load (`perf_run.py`, the binary gateway, 4 sessions at 500 orders a second each),
every context switch in each sequencer, matching engine and binary gateway process was recorded with
its stack. For 40 seconds with no Prometheus running, no thread waited for a histogram's mutex. For 40
seconds with Prometheus scraping every process every 5 seconds, the primary matching engine's reactor
thread waited twice, both times in `Reactor::record_look_for_work`, recording into
`reactor_lap_nanoseconds`. That histogram is recorded far more often than any other, which is why it
is the one that collided. The others take the same mutex. The method is set out in
`lock-audit-report.txt`.

## 3. What the replacement must do

1. The thread that records a value takes no lock, makes no system call, and never waits for another
   thread, whether or not a scrape is running.
2. A scrape never stops or slows the recording thread by more than the cost of moving a few cache
   lines between cores.
3. What a scrape returns is byte-for-byte the same text that prometheus-cpp returns today for the same
   values, so Prometheus, the Grafana dashboards and `scripts/pubsub_metrics.py` need no change.
4. Every count a scrape reports is one that really was reached, and no count ever goes down from one
   scrape to the next. Prometheus's `rate()` treats a decrease as a restart of the process, so a
   count that went down would be read as a restart.
5. Nothing at a call site changes. `HistogramHandle`, `HistogramInterface` and `NoOpHistogram` stay
   as they are.
6. When metrics are disabled, nothing is allocated and recording does nothing, as today.

## 4. The design

### 4.1 One writer, ordinary stores

A new class, `SingleWriterHistogram`, implements `HistogramInterface`. It holds:

- the bucket upper bounds, fixed at registration, ascending;
- one count for each bucket and one for values above the last bound, each a `std::atomic<int64_t>`;
- the sum of every value recorded, a `std::atomic<double>`.

`observe(value)` does three things:

1. Finds the first bound that is greater than or equal to the value, with `std::lower_bound` over the
   bounds. A value equal to a bound goes in that bound's bucket, which is how Prometheus defines a
   bucket ("less than or equal to") and what prometheus-cpp does.
2. Reads that bucket's count with `load(std::memory_order_relaxed)`, adds one, and writes it back with
   `store(..., std::memory_order_relaxed)`.
3. Does the same for the sum.

Only one thread ever writes a given histogram, so a read followed by a write cannot lose an update:
there is no other writer whose update could be overwritten. That is what lets the increment be a
plain read and a plain write rather than an atomic read-modify-write instruction.

The values are still `std::atomic`, for the sake of the thread that reads them. On x86-64 a relaxed
atomic load or store of a 64-bit value compiles to an ordinary `mov`, with no `lock` prefix and no
fence. The disassembly of the measurement program was checked for this: the timed loop increments the
count with `add $0x1,%rcx` followed by `mov %rcx,(%rax)`. As a control, the same search finds
`lock cmpxchg` in prometheus-cpp's `Gauge::Change`, so it would have found one here. Declaring the
values atomic costs nothing at run time. What it buys is that reading them from another thread is
defined behaviour in C++ rather than a data race, and that a reader can never see half of an old value
and half of a new one. `std::atomic<int64_t>` and `std::atomic<double>` both report
`is_always_lock_free` on this platform. Both load and store are available in C++17, which is what the
project builds as. The C++20 `fetch_add` for `double` is not needed.

### 4.2 Keeping histograms off each other's cache lines

Each histogram's counts and sum are stored together in one block of memory that starts on a 64-byte
boundary and is padded to a whole number of 64-byte cache lines. Without that, a histogram recorded by
the reactor's thread and one recorded by an application thread could be allocated side by side and
share a cache line. Each write by one thread would then take the line away from the other core, and
the next write on that core would have to fetch it back. This is called false sharing. It makes no
thread wait for a lock, but it costs a transfer between cores on writes that should cost nothing.

A histogram with 16 buckets needs 17 counts and a sum, 144 bytes, which is three cache lines.

### 4.3 What a scrape reads

The scrape thread reads each bucket's count and the sum with `load(std::memory_order_relaxed)`, one
after another, and builds the same `prometheus::ClientMetric` that `Histogram::Collect` builds: a
running total across the buckets in bound order for the cumulative counts, an extra bucket whose
upper bound is infinity, the total as `sample_count` and the sum as `sample_sum`.

The counts are read one at a time while the writer may still be recording, so they are not one
instantaneous picture of the histogram. This is what that means for each property Prometheus relies
on:

- **Each bucket's count is a value that bucket really had.** An atomic load returns a value some store
  wrote, never a mixture.
- **No count goes down between scrapes.** The C++ memory model guarantees that one thread reading the
  same atomic twice never sees an older value after a newer one. Each count only ever increases.
- **The cumulative counts never go down from one bucket to the next, and `_count` always equals the
  `+Inf` bucket.** Both are worked out by the scrape from the single set of values it read, as
  prometheus-cpp does today.
- **The sum may include a value whose bucket count the scrape read before that value arrived, or the
  other way round.** Only values recorded while the scrape is part way through reading that one histogram
  can be affected, and the next scrape includes all of them. A latency query divides
  the change in `_sum` by the change in `_count` over minutes or seconds, so the difference does not
  show.

Relaxed ordering is enough because the scrape needs each value on its own and relies on no
relationship between them. No fence and no stronger ordering is needed on either side.

Reading a cache line the writer has written leaves the line shared between two cores, so the writer's
next store to that line has to fetch it back for itself. That happens at most once per cache line per
scrape: three transfers for a 16-bucket histogram every 5 seconds. Measured with a reader once a
millisecond, five thousand times more often than the venue scrapes, the cost per value recorded was
3.29 ns against 3.33 ns with no reader at all. That is within the measurement's variation.

### 4.4 Registration and the endpoint

`PrometheusEndpoint::register_histogram` keeps its signature and its rules. A key registered twice
still throws, and every key sharing a metric name still has to supply the same help text. What
changes is what it builds:

- It creates a `SingleWriterHistogram` owned by the endpoint and returns a `HistogramHandle` to it.
  The histograms are held in a container whose elements never move once inserted, as the existing
  `std::map` members are, because the handles keep pointers to them for the life of the process.
- It records, for each metric name, the help text and the labels of each child, so the scrape can
  group children into families just as the prometheus-cpp registry does.
- `prometheus::BuildHistogram` and the `histogram_families_` map are no longer used.
  `PrometheusHistogram.hpp` is deleted.

The endpoint gives the `Exposer` one more `prometheus::Collectable`, next to the registry it already
gives it. Its `Collect()` returns one `prometheus::MetricFamily` of type histogram for each metric
name, with a child for each registered key. prometheus-cpp's own `TextSerializer` turns those into the
scrape's text, which is how requirement 3 is met. Nothing renders text by hand.
`PrometheusEndpoint::exposition_text()` serialises the registry's families and then these.

`Collect()` takes the endpoint's existing `mutex_`, so that a registration cannot add to the container
while a scrape walks it. No thread that records values ever takes that mutex. Registration happens
while components start, and the lock audit already lists this mutex as off the order path.

The `Collectable` is held by a `std::shared_ptr` owned by the endpoint, because `Exposer` keeps a
`std::weak_ptr` to each collectable. The existing rule that `exposer_` is declared last, so that it is
destroyed first and no scrape can be in flight while anything it reads is destroyed, covers the new
member too.

### 4.5 Checking that there is one writer

The whole design depends on each histogram having one writer. If a second thread ever recorded into
the same histogram, the two read-then-write sequences could interleave and one of the increments would
be lost. Nothing would fail; the count would simply be lower than it should be.

The framework already has a check for this kind of rule. `IncrementalRehashMap` remembers the first
thread that uses it and throws `PreconditionAssertion` if another thread does, compiled in only when
`PUBSUB_ITC_FW_THREAD_CHECKS` is defined. The top-level `CMakeLists.txt` defines it for Debug,
AddressSanitizer and coverage builds. `SingleWriterHistogram::observe` uses the same check, written the
same way. A release build pays nothing. A test build that lets a second thread record into a histogram
fails at that call instead of quietly losing counts.

The first call to `observe` establishes the owner, not registration. Registering a histogram and
recording into it need not happen on the same thread, and the rule being checked is about recording.

## 5. Alternatives considered

**A sequence number around each update.** The writer increments a sequence number before and after
each update, and the scrape reads the number, the values, and the number again, retrying if they
differ. This gives the scrape one instantaneous picture of the histogram. It is not recommended: it
costs the writer two more stores and an ordering fence on every value, and section 4.3 shows that
nothing Prometheus computes needs an instantaneous picture.

**A copy per thread, merged when scraped.** This is what the metrics document names as the usual
remedy. It is the right answer when several threads record into one histogram. Here each histogram
already has one writer, so there is nothing to merge, and the per-thread copy would be this design
with extra bookkeeping.

**The owning thread copies its histogram to the scrape on a timer.** This keeps every value private to
its thread. It is not recommended: it adds a timer and a copy to every thread that records a
histogram, including the reactor's thread, and the values in a scrape would be up to one timer period
old.

**A newer prometheus-cpp.** The mutex is part of prometheus-cpp's histogram design, which allows any
number of threads to record into one histogram, so no later version can be relied on to remove it.

## 6. The one histogram with two writers

On the following sequencer, `wal_append_nanoseconds` is recorded by two threads.
`SequencerThread::write_replicated_record` calls `append_to_wal` both from the inline handler, which
runs on the reactor's thread when a record arrives from the leader (`SequencerThread.cpp`, line 2043),
and from the sequencer's own thread for records the inline handler passed on to it (line 1772). Today
`ReplicatedRecordWriter`'s mutex makes the two take turns, so no update is lost even under
`SingleWriterHistogram`. But the check in section 4.5 would throw in a test build, and BUG-0124 plans
to remove that mutex.

The help text also says the time is measured "on the reactor thread". That is true of records the
inline handler writes on the follower, and not of anything the leader writes, which happens on the
sequencer's thread.

Two ways to resolve it:

- **(a) Two children of the same metric, one for each thread.** The sequencer registers
  `wal_append_nanoseconds` under scope `sequencer_thread`, as now, and again under scope
  `reactor_thread`, and the inline handler records into the second. The help text becomes neutral
  about the thread, since both children must share it. The Grafana panel sums by component, so it
  shows the same figure as today without change. A new view becomes possible: how long a write takes
  on each thread.
- **(b) Keep one histogram and exempt it from the check.** This relies on `ReplicatedRecordWriter`'s
  mutex for correctness. It stops being correct when BUG-0124 removes that mutex, and nothing would
  report it.

**Decided: (a).**

## 7. Tests

Each test is made to fail on purpose once, before it is trusted, and how it was made to fail is
recorded with it.

1. **Bucket placement.** A value below the first bound, equal to each bound, between two bounds, and
   above the last one each land in the expected bucket. Made to fail by using `std::upper_bound`,
   which puts a value equal to a bound in the next bucket.
2. **The scrape's text is unchanged.** The same values are recorded into a `prometheus::Histogram`
   and into a `SingleWriterHistogram` registered under the same key, help text and bounds, and the two
   serialised texts are compared byte for byte, for a metric with two scopes using different bounds.
   Made to fail by leaving out the infinite bucket.
3. **The existing `PrometheusEndpointTest` histogram tests pass unchanged.** They assert on the real
   exposition text.
4. **A second writer is caught.** In a build with `PUBSUB_ITC_FW_THREAD_CHECKS`, recording from a
   second thread throws `PreconditionAssertion`. Made to fail by removing the check. The test is
   compiled only in such builds, as `IncrementalRehashMap`'s is.
5. **Recording while a scrape runs.** One thread records a known number of values while another
   collects continuously. Every collect sees each bucket's count no lower than the collect before, and
   the final collect after the writer stops reports exactly the number recorded. Made to fail by
   adding a second writer thread with the thread check compiled out, which loses counts and fails the
   exact total. Also run in the ThreadSanitizer build (`ENABLE_TSAN`), which must report nothing.
6. **The recording thread never sleeps on a histogram in the venue.** The two-arm measurement from
   section 2.3 is repeated with Prometheus scraping. A count of zero waits in a function that no
   longer exists would prove nothing, so the count covers every mutex wait on the reactor and
   application threads, with the function that waited. It is checked first on the same test program
   used before, where it must find the 261 waits. In the venue, every mutex wait it finds is listed
   with the function that waited, and none may be in metrics code.
7. **The dashboards still read the same.** A short `perf_run.py` with Prometheus running, then
   `scripts/pubsub_metrics.py` and the Grafana latency dashboard, compared with a run before the
   change: the same metric names, labels and buckets, and percentiles of the same size.

The reactor's lap histogram is recorded on every turn of the reactor's loop, so this change touches
the framework's busiest code. Tests 6 and 7 are the fairness and soak checks that a change to the
reactor needs, and they are run in full, not reduced to unit tests.

## 8. Not in scope: counters and gauges

prometheus-cpp's `Counter` and `Gauge` take no mutex. In this build `Gauge::Change`, which
`Counter::Increment` calls, is a `lock cmpxchg` loop, because the library was compiled without C++20's
atomic `fetch_add` for `double`. `Counter::Increment` measured 7.9 ns. It is an atomic instruction
rather than a lock, so it never sleeps and never makes a system call, and a scrape reading it does
not make the writer wait. It could be made cheaper by the same technique, but that is a separate
change with a separate measurement, and it is not part of BUG-0125.

## 9. Decisions

1. **The design in section 4 is agreed**, including that a scrape reads the counts one at a time and
   is not one instantaneous picture (section 4.3), in exchange for nothing at all on the writer's side.
2. **The follower's two writers are resolved by (a)** in section 6: `wal_append_nanoseconds` has two
   children, scope `sequencer_thread` and scope `reactor_thread`.

## 10. Order of the work

1. `SingleWriterHistogram` with tests 1, 4 and 5.
2. The endpoint's `Collectable` and registration, with tests 2 and 3. Delete `PrometheusHistogram.hpp`.
3. The sequencer's second child of `wal_append_nanoseconds`, if (a) is chosen.
4. Through `scripts/devsetup.sh`, then tests 6 and 7 on the deployed venue.
5. Update `docs/operations/metrics.md`. Its section "Metric objects and locking" describes the mutex as
   current and says to remove it only once a measurement shows it matters. Also update the lock audit
   and close BUG-0125.
