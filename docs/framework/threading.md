# Threading

## Design Goals
One thread per concern — no two subsystems share a thread. Threads communicate exclusively
through lock-free MPSC queues; there are no mutexes on any hot path. Shutdown is deterministic:
every thread drains its queue, acknowledges the shutdown signal, and joins within a configurable
timeout.

## ApplicationThread
`ApplicationThread` (abstract base class) is the unit of concurrency in the framework. Each
concrete subclass represents one concern — order routing, matching, sequencing, authentication,
etc. It owns:
- A `LockFreeMessageQueue` (its ITC inbox)
- A `std::thread` (via `ThreadWithJoinTimeout`)
- A non-blocking `eventfd` (`notify_fd_`) used to wake the thread when work arrives

The thread's run loop drains the queue in a tight loop. When the queue is empty it blocks on
`epoll_wait(notify_fd_, timeout=1s)`. Normal wakeup is sub-microsecond (kernel delivers the
eventfd write immediately); the 1-second timeout is a safety net only.

### Key Supporting Classes
| Class | Description |
|-------|-------------|
| `ApplicationThread` | Abstract base; owns queue and thread; timer APIs enforced from owning thread; `connect_to_service()` for outbound TCP; pure virtual `on_itc_message()` |
| `ThreadWithJoinTimeout` | Wraps `std::thread`; `join_with_timeout()` |
| `ThreadID` | Strongly-typed thread identifier |
| `ThreadLifecycleState` | `NotCreated`, `Started`, `InitialProcessed`, `Operational`, `ShuttingDown`, `Terminated` |

### Virtual Callbacks
Subclasses override these to implement their behaviour:

| Callback | When called |
|----------|-------------|
| `on_initial_event()` | Thread has started; perform one-time initialisation |
| `on_app_ready_event()` | All threads are operational; start sending traffic |
| `on_termination_event(reason)` | Shutdown in progress; release resources |
| `on_itc_message(msg)` | ITC message delivered from another thread — **pure virtual** |
| `on_timer_event(id)` | A timer fired; `id` is the `TimerID` returned by `start_one_off_timer`/`start_recurring_timer` (compare against retained ids to identify it) |
| `on_pubsub_message(msg)` | Pub/sub delivery |
| `on_raw_socket_message(msg)` | Raw byte stream delivery (see [Socket Comms](socket_comms.md)) |
| `on_framework_pdu_message(msg)` | Inbound PDU delivered — **caller must call `allocator.deallocate(msg.slab_id(), msg.payload())` after processing** |
| `on_connection_established(id)` | Outbound TCP connect succeeded |
| `on_connection_failed(reason)` | Outbound TCP connect failed |
| `on_connection_lost(id, reason)` | Connection dropped after establishment |

### Idle Blocking: eventfd-Based Wake (replaced BackoffWithYield)
Earlier versions used a `BackoffWithYield` spin strategy that degraded through busy-spin,
`sched_yield`, and finally `sleep_for(microseconds(10))`. On a `CONFIG_HZ=1000` kernel, the
sleep tier actually slept ~65 µs. With five `ApplicationThread` hops on the order pipeline
(order gateway → sequencer → matching engine → sequencer → order gateway), each potentially in
the sleep tier, the avoidable overhead was ~325 µs per round-trip. Measured ITC latency from
heartbeat timer pairs confirmed ~140 µs average wakeup per hop.

The fix replaced `BackoffWithYield` entirely:
- Each `ApplicationThread` owns a non-blocking `eventfd` (`notify_fd_`).
- A new public `enqueue(EventMessage)` method enqueues to the MPSC queue and then writes `1`
  to `notify_fd_`.
- The run loop calls `epoll_wait(notify_fd_, timeout=1s)` when the queue is empty rather than
  spin-sleeping.
- `shutdown()` also writes to `notify_fd_`, so the thread exits immediately rather than
  waiting for the 1-second timeout.

All producer call sites in `Reactor.cpp`, `InboundConnectionManager.cpp`,
`OutboundConnectionManager.cpp`, `PduParser.cpp`, `RawBytesProtocolHandler.cpp`, and
`TlsRawBytesProtocolHandler.cpp` were updated from `thing->get_queue().enqueue(msg)` to
`thing->enqueue(std::move(msg))`.

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
thread in the second case, next to a neighbour that matters a great deal. `BackoffWithYield`'s
`cpu_relax()` uses `_mm_pause()` accordingly.

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
singly-linked list of `Node` objects with two pointers:
- `head_` (cache-line-aligned `atomic<Node*>`) — producers append here.
- `tail_` (non-atomic `Node*`) — the consumer reads from here.

**Stub node:** the queue is never structurally empty. A permanent `stub_` node (stack-
allocated inside the queue object) anchors the list from construction to destruction.
`head_` and `tail_` both start pointing at `stub_`. The stub is never put into the node
allocator pool; its address is stable for the lifetime of the queue.

Initially: head_ ──► stub_ ──► nullptr
              tail_ ──────────────► stub_

After one enqueue(A):
              head_ ──► A ──► nullptr
              tail_ ──► stub_ ──► A

**Enqueue (any producer thread):**
1. Allocate a `Node` from `ExpandablePoolAllocator<Node>`.
2. Construct `T` in-place inside the node (`data_storage_`).
3. `node->next_.store(nullptr, relaxed)`.
4. `prev = head_.exchange(node, acq_rel)` — atomically swings `head_` to the new node and
   returns the previous head. This is the only synchronisation point between producers; the
   exchange serialises them.
5. `prev->next_.store(node, release)` — links the new node into the list. A consumer
   watching `tail_->next_` will see this once the store becomes visible.

**Dequeue (consumer thread only):**
1. Read `tail_` and `tail->next_` (acquire).
2. If `tail_ == &stub_` and `next == nullptr`: queue is empty, return `nullopt`.
3. If `tail_ == &stub_` and `next != nullptr`: advance `tail_` past the stub; retry with the
   new tail.
4. If `next != nullptr`: move data out of `tail`, advance `tail_` to `next`, return node to
   pool.
5. If `next == nullptr` but `head_ != tail_`: a producer is mid-enqueue (completed step 4
   but not yet step 5 above). Re-enqueue the stub to break the ABA condition and retry.

The stub re-enqueue in step 5 is the key correctness mechanism: it ensures that once the
producer's `prev->next_.store(release)` becomes visible, the consumer will find the node
on the next drain, without busy-spinning or blocking.

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

**Usage for TCP read backpressure:** when a connection's ITC queue fills past the high-water mark, the gateway deregisters `EPOLLIN` on the FIX listener, stopping new inbound reads. When the queue drains back below the low-water mark, `EPOLLIN` is re-registered. The hysteresis band prevents rapid toggling of socket events under fluctuating load.

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
| `shutdown()` | Any thread (atomic CAS) |

`ExpandablePoolAllocator` supplies queue nodes from a lock-free pool so node allocation
itself involves no heap calls on the hot path.

## Thread Lifecycle
Each `ApplicationThread` transitions through a fixed state machine:

NotCreated → Started → InitialProcessed → Operational → ShuttingDown → Terminated

| State | Meaning |
|-------|---------|
| `NotCreated` | `std::thread` not yet constructed |
| `Started` | Thread has entered its run loop |
| `InitialProcessed` | `on_initial_event()` has returned |
| `Operational` | `on_app_ready_event()` has returned; thread is processing work |
| `ShuttingDown` | `shutdown()` called; `is_running()` returns false; queue draining |
| `Terminated` | Thread has exited its run loop; safe to join |

`ApplicationThread::shutdown()` sets the lifecycle state to `ShuttingDown` atomically and
writes to `notify_fd_` to wake the thread from `epoll_wait` immediately.

The reactor calls `shutdown()` on every registered thread inside
`finalize_threads_after_shutdown()`, before the join-with-timeout loop.

## Stuck-Thread Detection
The reactor runs a periodic housekeeping tick (`on_housekeeping_tick()`). Part of that tick
calls `check_for_stuck_threads()`, which compares two timestamps maintained per thread:

| Field | Set when |
|-------|----------|
| `time_event_started_` | Entry to `process_message()` |
| `time_event_finished_` | Exit from `process_message()` |

If `time_event_started_ > time_event_finished_` and the elapsed wall time exceeds
`itc_maximum_inactivity_interval_` (default 60 s), the thread is considered stuck and
`shutdown()` is called on it.

An idle thread (queue empty, blocked in `epoll_wait`) is always safe: it sits between
messages with `time_event_started_ <= time_event_finished_` and is never falsely detected
as stuck.

**Outstanding Risk:** If any exit path from `process_message()` — including exception paths or early returns —
fails to update `time_event_finished_`, an idle thread could be falsely detected as stuck
60 s after the last message. The correct-path case updates `time_event_finished_` at the
bottom of `process_message()` (ApplicationThread.cpp:488). An audit of all exit paths has
not yet been completed.

## See Also
- [CPU Pinning](cpu_pinning.md) — how each thread claims a dedicated CPU
- [Reactor](reactor.md) — the epoll event loop that drives thread wakeup and housekeeping
- [Allocators](allocators.md) — pool allocator that backs the ITC queue nodes
