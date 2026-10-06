# Threading

## Design Goals
One thread per concern — no two subsystems share a thread. Threads communicate through
lock-free MPSC queues; there are no mutexes on any hot path. Shutdown is bounded in time: each
thread's queue stops accepting messages, the thread is woken and leaves its run loop, and the
reactor joins every thread within `shutdown_timeout_`.

## ApplicationThread
`ApplicationThread` (abstract base class) is the unit of concurrency in the framework. Each
concrete subclass represents one concern — order routing, matching, sequencing, authentication,
etc. It owns:
- A `LockFreeMessageQueue<EventMessage>` (its ITC inbox)
- A `std::thread` (via `ThreadWithJoinTimeout`)
- A non-blocking `eventfd` (`notify_fd_`) used to wake the thread when work arrives
- An `ExpandableSlabAllocator` for the PDUs and raw bytes it sends (see [Allocators](allocators.md))

The thread's run loop takes every message off the queue and processes it. When the queue is
empty it can poll the queue for a short, configured time (see *Polling before blocking*
below), and then blocks in `epoll_wait` on `notify_fd_` with a one-second timeout. Normally the
thread is woken as soon as a producer writes to the eventfd; the one-second timeout is only a
safety net.

### Key Supporting Classes
| Class | Description |
|-------|-------------|
| `ApplicationThread` | Abstract base; owns queue and thread; timer APIs enforced from owning thread; `connect_to_service()` for outbound TCP; pure virtual `on_itc_message()` |
| `ApplicationThreadConfiguration` | Per-thread settings: the outbound slab size, the decode arena size, the metrics scope, and `spin_before_block` |
| `ThreadWithJoinTimeout` | Wraps `std::thread`; `join_with_timeout()` |
| `ThreadID` | Strongly-typed thread identifier |
| `ThreadLifecycleState` | `NotCreated`, `Created`, `Started`, `InitialProcessed`, `Operational`, `ShuttingDown`, `Terminated` |

### Virtual Callbacks {#threading_callbacks}
Subclasses override these to implement their behaviour:

| Callback | When called |
|----------|-------------|
| `on_initial_event()` | Thread has started; perform one-time initialisation |
| `on_app_ready_event()` | All threads are operational; start sending traffic |
| `on_termination_event(reason)` | A `Termination` event arrived, for example because the process received SIGTERM or SIGINT; release resources |
| `on_itc_message(msg)` | ITC message delivered from another thread — **pure virtual** |
| `on_timer_event(id)` | A timer fired; `id` is the `TimerID` returned by `start_one_off_timer`/`start_recurring_timer` (compare against retained ids to identify it) |
| `on_pubsub_message(msg)` | Pub/sub delivery |
| `on_raw_socket_message(msg)` | Raw byte stream delivery (see [Socket Comms](socket_comms.md)) |
| `on_framework_pdu_message(msg)` | Inbound PDU delivered — **the thread must call `release_pdu_payload(msg)` once it has finished with the payload**, which returns the chunk to the reactor's inbound slab allocator |
| `on_connection_established(id)` | A connection is ready: an outbound connect has succeeded, or an inbound connection has been accepted |
| `on_connection_failed(reason)` | Outbound TCP connect failed |
| `on_connection_lost(id, reason)` | Connection dropped after establishment |
| `on_connection_writable(id)` | A connection can accept another outbound frame. Delivered once for each call to `request_writable_notification()` |

One further virtual changes the order of delivery rather than receiving an event.
`prioritise_data_over_timers()` returns `false` by default. A thread that overrides it to return
`true` has `Timer` events held back during each pass over its queue and processed after the
other messages taken in the same pass, so that a heartbeat or snapshot timer does not delay a
data message that arrived at the same moment.

### Idle Blocking: eventfd-Based Wake
A thread with an empty queue blocks rather than spin-sleeping, because sleeping is slow to
wake. A `sleep_for(microseconds(10))` on a `CONFIG_HZ=1000` kernel actually sleeps about
65 µs, and an order passes through five `ApplicationThread` hops on its way through the venue
(order gateway → sequencer → matching engine → sequencer → order gateway), so a sleep at each
hop would add about 325 µs to a round trip.

So:
- Each `ApplicationThread` owns a non-blocking `eventfd` (`notify_fd_`).
- `ApplicationThread::enqueue(EventMessage)` puts the message on the MPSC queue and then writes
  `1` to `notify_fd_`. Every producer uses it. Calling `get_queue().enqueue()` directly would
  skip the eventfd write, and the thread would not wake until the one-second timeout.
- The run loop calls `epoll_wait` on `notify_fd_`, with a one-second timeout, when the queue is
  empty.
- `shutdown()` also writes to `notify_fd_`, so the thread exits immediately rather than
  waiting for the timeout.

### Polling before blocking
Blocking has its own cost. A blocked thread is descheduled, and on a pinned hot-path core
nothing else is runnable, so the core goes idle; waking it costs whatever its idle state costs
to leave, which with the usual machine defaults can be around a millisecond.

`ApplicationThreadConfiguration::spin_before_block` sets how long the thread polls its empty
queue before it blocks. The default is zero, meaning block immediately. When it is set, and a
pass over the queue found nothing, the thread loops checking `empty()` and calling
`cpu_relax()` (a `PAUSE` instruction) until either a message arrives or the time runs out.
A message found this way is taken by the ordinary path on the next pass, and the thread reads
`notify_fd_` once to clear the producer's signal so that its next `epoll_wait` does not return
at once for work already done. `spins_entered()` and `spins_caught()` count how many polls
were started and how many found a message. The poll is compiled out of Valgrind and TSan
builds.

Polling only helps when messages arrive closer together than the polling time, and it holds
the core for that whole time, so it should be chosen from the traffic a thread actually
receives and left at zero for threads off the latency-critical path. The next section explains
why it uses `PAUSE`.

## Waiting for work: spinning, PAUSE, and who shares your core

A thread with nothing to do can either block, and be woken when work arrives, or spin, and
notice for itself. This framework blocks, for the reasons in the section above. Where spinning
is being considered instead, there is a piece of conventional advice that comes with it: put a
`PAUSE` instruction in the loop. This section records what that advice is actually worth,
because on modern processors it is worth much less than it used to be, and because whether it is
worth anything at all depends on how the threads are pinned.

Everything below was measured on the development workstation with
`libraries/pubsub_itc_fw/performance/src/SpinWaitBench.cpp`, which is built as
`spin_wait_bench` and takes the two processors to use. The numbers are a property of the
processor rather than of this project, so re-run it anywhere the answer matters rather than
quoting these.

### What a PAUSE costs

Thirty nanoseconds. One loop iteration takes 1.37 nanoseconds without it and 31.47 with, so the
instruction itself accounts for 30.10. On a 5500 MHz core that is roughly 165 cycles.

That is the whole difficulty with the conventional advice. `PAUSE` used to cost around ten
cycles, and Intel lengthened it substantially from the Skylake generation onwards; the 165
cycles measured here is consistent with that change. Advice written before it was describing a
different instruction. A loop that contains one now spends most of its time inside it, and
therefore looks at the thing it is waiting for far less often than a loop without one.

### What it buys

Only one thing: the other hardware thread of the same physical core gets more done.

Two hardware threads of one core share that core's execution resources. A thread spinning as
fast as it can takes a large share of them, and the thread beside it slows down. `PAUSE` stands
the spinning thread down between checks and hands those resources over.

Measured, with a neighbour doing ordinary memory-touching work for a fixed period:

| Arrangement | Neighbour's work | Noticing latency |
|---|---|---|
| Neighbour on a **different** physical core | 182,155 rounds | 95 ns |
| Sharing a core, spinner **without** PAUSE | 180,805 rounds | 97 ns |
| Sharing a core, spinner **with** PAUSE | 182,340 rounds | 121 ns |

So sharing a core with a spinner costs the neighbour about 0.7 per cent of its throughput, and
`PAUSE` gives all of it back -- the third row matches the first. The effect is real, it is
exactly what `PAUSE` is for, and it is small.

### What it costs

About 25 nanoseconds of the only thing a waiting thread is for, which is noticing that work has
arrived. In the table above, 97 nanoseconds becomes 121.

That figure is not a coincidence and is worth understanding, because it says the cost cannot be
tuned away. One `PAUSE` takes 30 nanoseconds. A loop is, on average, somewhere in the middle of
one when the thing it is watching changes, so it finds out roughly one `PAUSE` later. The cost
of being polite is the length of the instruction, and the instruction is now long.

### Why this is a pinning question

**The entire benefit of `PAUSE` goes to the other hardware thread of the same physical core.**
If there is no such thread -- if the layout gives a spinning thread a whole core to itself --
then there is nobody to be polite to, and `PAUSE` is pure cost. The control row above shows
this: with the neighbour on a different core, `PAUSE` still costs its 17 nanoseconds of
noticing and buys nothing, because there was nothing to buy.

So the two decisions cannot be made separately, and which way each falls depends on the other:

  A spinning thread with a core to itself should not use `PAUSE`. It pays the noticing cost and
  there is no neighbour to benefit.

  A spinning thread sharing a core with a thread that matters should use it. Giving up 25
  nanoseconds of noticing to hand a neighbour back 0.7 per cent of its throughput is a
  reasonable trade when that neighbour is the thread being waited for.

This project pins a component's two hot-path threads onto the two hardware threads of one
physical core deliberately, because they exchange messages constantly and sharing level-one
cache makes the handoff nearly free -- see
[cpu_pinning_anti_affinity.md](cpu_pinning_anti_affinity.md). That decision puts any spinning
thread in the second case, next to a neighbour that matters a great deal. `cpu_relax()`,
declared in `BackoffWithYield.hpp` and used by the polling described above, calls
`_mm_pause()` accordingly.

### The thing that matters more than either

**How a thread spins matters far more than whether it says `PAUSE`.**

A loop that calls into the kernel on every iteration -- `epoll_wait` with a zero timeout, for
instance -- is doing something far more disruptive than a quiet loop over a memory location.
Measured on this venue, with a reactor thread polling that way on a core shared with its
application thread, the application thread's handling of an execution report went from 6.6
microseconds to 17.6. That is nearly three times, against `PAUSE`'s 0.7 per cent, and it
happened with `PAUSE` present.

So the first question to ask about a spin loop is not whether it is polite but what it does on
each pass. A loop that reads one memory location and pauses is cheap for its neighbour. A loop
that enters the kernel is not, and no amount of `PAUSE` will fix it.

## Inter-Thread Communication (ITC)
Threads communicate by posting `EventMessage` values to each other's queues. The reactor and
its managers are the primary producers; `ApplicationThread` subclasses may also post to each
other's queues directly.

| Class | Description |
|-------|-------------|
| `LockFreeMessageQueue<T>` | Vyukov MPSC queue; nodes from `ExpandablePoolAllocator<Node>`; watermark hysteresis callbacks; shutdown semantics |
| `QueueConfiguration` | Watermark thresholds and callbacks |

### LockFreeMessageQueue — Vyukov MPSC Algorithm
`LockFreeMessageQueue<T>` implements Dmitry Vyukov's intrusive MPSC queue. It is a
singly-linked list of `Node` objects with two pointers, each on its own cache line:
- `head_` (`atomic<Node*>`) — producers append here.
- `tail_` (plain `Node*`) — the consumer reads from here.

**Stub node:** the queue is never structurally empty. A permanent `stub_` node, a member of the
queue object, anchors the list. `head_` and `tail_` both start pointing at `stub_`. The stub
never comes from the node allocator, so its address is stable for the lifetime of the queue.

Initially: head_ ──► stub_ ──► nullptr
              tail_ ──────────────► stub_

After one enqueue(A):
              head_ ──► A ──► nullptr
              tail_ ──► stub_ ──► A

**Enqueue (any producer thread):**
1. If the queue has been shut down, return without doing anything.
2. Allocate a `Node` from `ExpandablePoolAllocator<Node>`.
3. Construct `T` in-place inside the node (`data_storage_`).
4. `node->next_.store(nullptr, relaxed)`.
5. `prev = head_.exchange(node, acq_rel)` — atomically swings `head_` to the new node and
   returns the previous head. This is the only synchronisation point between producers; the
   exchange serialises them.
6. `prev->next_.store(node, release)` — links the new node into the list. A consumer
   watching `tail_->next_` will see this once the store becomes visible.

**Dequeue (consumer thread only):**
1. Read `tail_` and `tail->next_` (acquire).
2. If `tail_` is the stub: if `next` is `nullptr` the queue is empty, so return `nullopt`;
   otherwise move `tail_` past the stub and continue with the node after it.
3. If `next != nullptr`: move the data out of `tail`, advance `tail_` to `next`, return the
   node to the pool, and return the data.
4. If `next == nullptr` and `head_ != tail_`: a producer has done step 5 of the enqueue but not
   yet step 6, so the node after `tail` is not yet reachable. Return `nullopt`. The producer
   writes to the thread's eventfd after it finishes, so the thread will look again.
5. If `next == nullptr` and `head_ == tail_`: `tail` is the last node in the queue. Taking it
   would leave nothing for `head_` and `tail_` to point at, so the stub is enqueued behind it
   first. The stub is then `tail->next_`, and `tail` can be taken as in step 3, leaving the
   stub as the anchor again.

**Valgrind / TSan fallback:** when built with `USING_VALGRIND`, the lock-free algorithm is
replaced by a `std::mutex`-protected `std::deque`. This lets Helgrind and DRD analyse the
surrounding code without misidentifying the intentional data races in the atomic operations.

### Watermark Hysteresis

`QueueConfiguration` carries two thresholds and two callbacks:

| Field | Trigger |
|-------|---------|
| `high_watermark` + `gone_above_high_watermark_handler` | Called (once) when queue depth rises to or above `high_watermark` |
| `low_watermark` + `gone_below_low_watermark_handler` | Called (once) when queue depth falls below `low_watermark` after a high-watermark breach |

**Hysteresis** is a phenomenon where the state of a system depends not only on its current input but also on its historical path. Essentially, it is a form of "memory" within a physical or abstract system, where the system lags behind changes in the force or input applied to it. When you reverse the direction of an input, the output does not immediately return along the same path it followed initially. Instead, it follows a different route, creating a loop known as a hysteresis loop.

In this queue, hysteresis is implemented via the gap between the `high_watermark` and `low_watermark` together with the internal flag `is_high_watermark_breached_`. This creates a dead-band that prevents rapid oscillation ("chattering"):
```
Queue Depth
    ▲
    │                  High Watermark ─────────────────────
    │                       │
    │   Hysteresis Band     │   ← high callback fires once on upward crossing
    │                       │
    │                  Low Watermark ─────────────────────
    │                       │
    └───────────────────────┴──────────────────────────────► Time
            High regime                    Low regime
```
- When the queue depth rises to or above the high watermark, the high-water callback fires **once** and the system enters the "high" regime.
- No further callbacks fire while the queue stays above the low watermark.
- Only when the queue drains **below the low watermark** does the low-water callback fire, resetting the state.

**Use in the venue:** every venue component sets the two thresholds on its application
thread's queue, but none sets either callback, so in practice nothing is called. The backpressure
that does stop the venue reading from a TCP connection is a different mechanism, driven by how
full a raw-bytes connection's receive buffer is rather than by the depth of an ITC queue: when
the buffer reaches its high-water mark the handler stops watching the socket for `EPOLLIN`, and
when the application has consumed enough to bring it below the low-water mark it starts again.
See `RawBytesProtocolHandler.hpp`.

### Shutdown Semantics
`LockFreeMessageQueue::shutdown()` sets `shutting_down_` atomically (CAS from false to
true). After that point, `enqueue()` is a no-op — producers silently drop messages. The
consumer thread continues to drain any messages already in the queue via `dequeue()`.

`shutdown()` is called by `ApplicationThread::shutdown()` as part of the graceful shutdown
sequence, and also by the queue's destructor.

### Thread Safety Summary
| Operation | Who may call |
|-----------|-------------|
| `enqueue()` | Any thread (MPSC — multiple producers) |
| `dequeue()` | Consumer thread only (the owning `ApplicationThread`) |
| `empty()` | Consumer thread only (reads `tail_` without lock) |
| `size()` | Any thread. The count can be out of date by the time it is used, so it is for measurement, not for deciding whether a dequeue will succeed |
| `shutdown()` | Any thread (atomic CAS) |

`ExpandablePoolAllocator` supplies queue nodes from a lock-free pool so node allocation
itself involves no heap calls on the hot path.

## Thread Lifecycle
Each `ApplicationThread` moves through these states:

NotCreated → Created → Started → InitialProcessed → Operational → ShuttingDown → Terminated

| State | Meaning |
|-------|---------|
| `NotCreated` | The object's constructor has not finished |
| `Created` | The constructor has finished: the queue, the eventfd and the allocators exist |
| `Started` | The thread has entered its run loop |
| `InitialProcessed` | `on_initial_event()` has returned |
| `Operational` | `on_app_ready_event()` has returned; thread is processing work |
| `ShuttingDown` | `shutdown()` has been called, or the run loop has exited; `is_running()` returns false |
| `Terminated` | `on_termination_event()` has returned, or an exception escaped the run loop |

The order is not strict at the end. A thread that processes a `Termination` event moves to
`Terminated`, leaves its run loop as a result, and is then set to `ShuttingDown` on the way
out. A thread asked to stop by `shutdown()` leaves its run loop in `ShuttingDown` and never
reaches `Terminated`. Neither state on its own says the thread has finished executing;
`has_exited()` does, and it is set as the very last action of the thread on every path.

`ApplicationThread::shutdown()` sets the lifecycle state to `ShuttingDown`, shuts the queue
down so that it accepts no more messages, and writes to `notify_fd_` to wake the thread from
`epoll_wait` immediately. The run loop checks `is_running()` before each pass, so the thread
leaves the loop without processing what remains in its queue. A message dequeued while the
thread is shutting down is dropped, with a Debug log line, unless it is one of the four
events the reactor itself sends: `Initial`, `AppReady`, `Timer` and `Termination`.

An exception that escapes the run loop is caught in `ApplicationThread::run()`, logged, and
followed by a request to the reactor to shut down the whole process.

The reactor calls `shutdown()` on every registered thread inside
`finalize_threads_after_shutdown()`, before it joins each one with `shutdown_timeout_`.

## Stuck-Thread Detection
The reactor runs a periodic housekeeping tick (`on_housekeeping_tick()`, every
`inactivity_check_interval_`, default one second). Part of that tick calls
`check_for_stuck_threads()`, which compares two timestamps maintained per thread:

| Field | Set when |
|-------|----------|
| `time_event_started_` | In `process_message()`, after the checks on the thread's state and before the callback is called |
| `time_event_finished_` | At the end of `process_message()`, after the callback returns |

For a thread in `Operational`:
- If `time_event_started_ > time_event_finished_`, a callback is running now. If it has been
  running for longer than `itc_maximum_inactivity_interval_` (default 60 s), the reactor shuts
  down the whole process with the reason "callback appears to be stuck". Below that limit it
  logs, at Info, how long the callback has taken so far.
- Otherwise the most recent callback has finished. If it took longer than
  `itc_maximum_inactivity_interval_`, the reactor shuts down the process with the reason
  "callback took too long".

For a thread still in `Started`, the reactor shuts down the process if more than
`init_phase_timeout_` (default 10 s) has passed since `time_event_started_`.

An idle thread (queue empty, blocked in `epoll_wait`) is never mistaken for a stuck one: it
sits between messages with `time_event_started_ <= time_event_finished_`, and the gap between
messages is not measured. That depends on every way out of `process_message()` either setting
`time_event_finished_` or ending the thread. There are only three: the early return for a
message that arrives during shutdown, which happens before `time_event_started_` is set; a
thrown exception, which ends the thread and shuts the process down; and the normal return,
which sets `time_event_finished_`.

## See Also
- [CPU Pinning](cpu_pinning.md) — how each thread claims a dedicated CPU
- [Reactor](reactor.md) — the epoll event loop that drives thread wakeup and housekeeping
- [Allocators](allocators.md) — pool allocator that backs the ITC queue nodes
