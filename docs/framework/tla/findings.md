# TLA+ models of the framework's lock-free structures {#framework_tla_findings}

The framework's allocators and queues are used by several threads at once without locks. Tests and
ThreadSanitizer find a fault in such code only when a run happens to reach the interleaving that shows
it, and ThreadSanitizer cannot check the pool allocator's lock-free path at all
([allocators.md](../allocators.md), "Build Paths"). The structures here are therefore also specified in
TLA+ and checked with its model checker, TLC, which explores every interleaving of the threads' steps
within stated bounds. This is the work of [BUG-0162](../../bug_list.md#bug_0162).

Each specification names the code it describes, and must be kept in step with it.

## What a model checks, and what it does not

Each step a thread takes in the code, such as an atomic exchange or a read of a flag, is one step of the
specification, so TLC tries every order in which the threads' steps can happen. A property is a
statement that must be true in every state reached, such as "no thread touches a slab after it has been
destroyed". When a property fails, TLC prints the shortest sequence of steps that breaks it.

A model checks the algorithm, not the C++. It assumes every thread sees every write in a single order,
so it does not check whether each memory ordering given to a C++ atomic is strong enough; that would
need a checker for C++ atomics, such as GenMC. It checks only within its bounds: a fault that needs more
slabs or chunks than the bounds allow is not found.

A model that finds nothing proves nothing until it has been shown able to fail. Every property below
has been made to fail by a deliberate change to a copy of the specification, and each specification
that models a fault the code once had, or a rule the code depends on, has a second configuration in
which TLC must find that fault.

## How to run

TLC is in `tla2tools.jar`, from the TLA+ project's releases at
<https://github.com/tlaplus/tlaplus/releases>; the Rocky 8 Dockerfile installs version 1.7.4 in
`/opt/tla2tools-1.7.4`. From this directory:

```
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config SlabReclaim.cfg SlabReclaim.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config SlabReclaimBeforeBug0157.cfg SlabReclaim.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config QueueOfEmptySlabs.cfg QueueOfEmptySlabs.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config QueueOfEmptySlabsNoKeepBack.cfg QueueOfEmptySlabs.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config PoolFreeList.cfg PoolFreeList.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config PoolFreeListNoCounter.cfg PoolFreeList.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config PoolFreeValidation.cfg PoolFreeList.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config FollowerHandOver.cfg FollowerHandOver.tla
java -cp tla2tools.jar tlc2.TLC -deadlock -workers auto -config FollowerHandOverNoCompare.cfg FollowerHandOver.tla
```

To check the three pool free-list configurations as one test, run
`python3 scripts/check_pool_free_list_tla.py` from the repository root. The command exits successfully
only when the tagged free-list and free-validation models pass and the no-counter model reports the
expected `FreeListSound` violation. Pass `--jar path/to/tla2tools.jar` if the jar is not available
through `TLA2TOOLS_JAR` or `THIRDPARTY_DIR`.

`-deadlock` turns off TLC's check that some step is always possible: the specifications stop when their
bounds are used up, which is not a fault. TLC writes its working files under `states/` beside the
specification unless given `-metadir`; `.gitignore` here keeps them out of the repository.

## 1. Giving back an empty slab: SlabReclaim.tla

**The code.** `SlabAllocator` and `ExpandableSlabAllocator`. One thread, the owner, allocates chunks
from its current slab and makes a new slab current when that one is full. Any thread may free a chunk.
A slab that is no longer current and has no chunk outstanding is put on a queue, and the owner destroys
it when it next allocates. The count in each slab holds the chunks outstanding plus one held by the
owner while the slab is current, and whichever thread takes the count to zero puts the slab on the
queue ([allocators.md](../allocators.md), "SlabAllocator").

**The properties.**

| Property | Meaning |
|---|---|
| `NoUseAfterFree` | No thread touches a slab, its count, its flags or its queue node after the owner has destroyed it |
| `CurrentNeverDestroyed` | The owner never destroys its current slab |
| `QueuedAtMostOnce` | No slab is put on the queue twice, which would have the owner destroy it twice |
| `DestroyedOnlyWhenEmpty` | No slab is destroyed while a chunk from it is still held |
| `NothingLost` | Once every chunk is freed and the owner has drained the queue, every slab but the current one has been destroyed or is the one kept back |

**The design before BUG-0157** is the same specification with `Design = "before_bug_0157"`: the count
holds only the chunks outstanding, and a thread that takes it to zero reads whether the slab is current
and then claims, with a compare-and-exchange, the right to put it on the queue, as the owner does when it
moves to a new slab. TLC finds `NoUseAfterFree` violated, in 32 states, which are the fault
[BUG-0157](../../bug_list.md#bug_0157) describes:

1. A thread frees slab 1's only chunk and takes the count to zero while slab 1 is still current, and is
   held up before it reads whether the slab is current.
2. The owner moves to slab 2: it clears slab 1's flag, reads the count as zero, claims slab 1, and puts
   it on the queue.
3. The owner drains the queue twice: the first drain takes slab 1 and keeps it back; the second takes
   slab 2, which had become empty meanwhile, and destroys slab 1.
4. The held-up thread reads slab 1's flag, in memory that has been destroyed.

**The design now** (`SlabReclaim.cfg`): every property holds. With one chunk to a slab, so that every
allocation after the first moves to a new slab, the bounds and results are:

| Chunks | Slabs | Chunks to a slab | Distinct states | Time on 16 workers | Result |
|---|---|---|---|---|---|
| 7 | 7 | 1 | 1,140,536 | 3 s | holds (the configuration as committed) |
| 8 | 8 | 1 | 10,358,112 | 23 s | holds |
| 10 | 6 | 2 | 681,695 | 3 s | holds |
| 9 | 9 | 3 | 37,368 | 2 s | holds |

**Made to fail.** Each change below was made to a copy of the specification, with the design now:

| Change | Property TLC found violated |
|---|---|
| The owner puts the slab on the queue whenever it gives up its hold | `QueuedAtMostOnce` |
| The owner never puts the slab on the queue | `NothingLost` |
| A freeing thread puts the slab on the queue when it takes the count to one, not zero | `QueuedAtMostOnce`; checked alone, also `NoUseAfterFree`, `DestroyedOnlyWhenEmpty` and `CurrentNeverDestroyed` |
| A new slab starts without the owner's hold | `QueuedAtMostOnce`; checked alone, also `CurrentNeverDestroyed` and `NoUseAfterFree` |
| The owner destroys the last slab it takes at once, instead of keeping it back | none: this specification treats each use of the queue as one step, and the reason for keeping it back lies inside the queue, which section 2 checks |

## 2. The queue of empty slabs: QueueOfEmptySlabs.tla

**The code.** `EmptySlabQueue` and `ExpandableSlabAllocator::drain_empty_slab_queue`. The queue is a
linked list whose nodes are inside the slabs. A thread puts a node on it in three steps: it clears the
node's link, swaps the node into the queue's tail with one atomic exchange, and sets the link of the
node that was the tail before. The owner takes nodes from the head; the node it took last stays in use
as the head. So the owner keeps back the slab it took last until a later drain has taken another
([allocators.md](../allocators.md), "Deferred Reclamation").

**The properties.**

| Property | Meaning |
|---|---|
| `NoUseAfterFree` | No node is read or written after its slab has been destroyed |
| `TakenAtMostOnce` | No slab is taken off the queue twice |
| `NothingLost` | When the owner finds the queue empty and no thread is part way through putting a node on it, every node put on the queue has been taken |

**Without keeping the last slab back** (`QueueOfEmptySlabsNoKeepBack.cfg`): TLC finds `NoUseAfterFree`
violated, in 11 states. A thread puts slab 1's node on the queue; the owner takes it, so that it becomes
the head, and destroys slab 1 at once; at its next drain the owner reads the head's link, inside the
destroyed slab 1.

**As the code is** (`QueueOfEmptySlabs.cfg`): every property holds. Each slab's node is put on the queue
once, by a process of its own, interleaved in every way with the owner and the other processes.

| Slabs | Distinct states | Time on 16 workers | Result |
|---|---|---|---|
| 5 | 170,289 | 1 s | holds (the configuration as committed) |
| 6 | 2,217,753 | 4 s | holds |

**Made to fail.**

| Change | Property TLC found violated |
|---|---|
| The owner destroys every slab it takes at once | `NoUseAfterFree` (the second configuration) |
| The exchange does not move the tail, so two threads can link to the same earlier node | `NothingLost` |
| Taking a node does not move the head | `TakenAtMostOnce` |

## 3. The pool's free list: PoolFreeList.tla

**The code.** `FixedSizeMemoryPool`, on the lock-free path the venue runs, which neither ThreadSanitizer
nor Valgrind can check, because neither can follow a 16-byte compare-and-exchange. The free slots form a
stack linked through each slot's `free_next`. The head is a pointer and a counter in one 16-byte word,
changed only by a 16-byte compare-and-exchange, and every change adds one to the counter. To take a slot,
a thread reads the head, reads the top slot's `free_next`, and exchanges the head for that; to give one
back, it reads the head, sets the slot's `free_next` to it, and exchanges the head for the slot. A failed
exchange starts again ([allocators.md](../allocators.md), "Treiber Stack Free List").

The counter is what stops the fault known as ABA: a thread reads the head as slot A, with slot B after it,
and is held up while others take A, take B and give A back. Its exchange then sees A at the head again,
and without the counter it would succeed and make B the head, although B is in use.

The C++ implementation reads the head with inline SSE `movdqa` assembly. The specification treats
that read as one atomic observation, but TLC does not verify the instruction's behaviour, compiler
output or C++ memory-order rules. A slot's `free_next` remains addressable because slots are not freed
while the pool exists.

`PoolFreeList.cfg` checks the free-list algorithm with valid push requests. The public
`ExpandablePoolAllocator::deallocate()` checks the pointer against its pool chain and atomically
changes `is_constructed` from 1 to 0 before calling the fixed pool's push. `PoolFreeValidation.cfg`
models those checks before the push: null pointers are rejected, pointers outside the pool or not
aligned to a slot are rejected, and a request for a slot that is not currently allocated is rejected.
The check and claim are one model action, representing the atomic compare-and-exchange on
`is_constructed`; this ensures that concurrent requests to free the same still-free slot cannot both
reach the free list. The model permits a different thread to free an allocated slot, as the allocator
does. TLC checks the same free-list invariants while exploring these accepted and rejected requests.

The model represents pointers by slot addresses. It cannot distinguish an old pointer from a newly
allocated object if the same slot address has been reused. A stale pointer used after that reuse names
the current object at that address, and the allocator's `is_constructed` check cannot identify the
pointer's earlier allocation. This model also does not claim that callers can bypass the checks in
`ExpandablePoolAllocator` and safely call `FixedSizeMemoryPool::deallocate()` with an invalid or
already-freed pointer; that lower-level operation requires a valid allocated slot.

**The properties.**

| Property | Meaning |
|---|---|
| `NeverTakenTwice` | No slot is taken while another thread holds it |
| `FreeListSound` | The free list never runs in a circle, and never contains a slot a thread holds |
| `NothingLost` | In every state, every slot is on the free list or held, including a slot part way through being returned |

**Without the counter** (`PoolFreeListNoCounter.cfg`, two threads): TLC finds `FreeListSound` violated,
in 17 states, which are the ABA fault. Thread 2 reads the head as slot 1 with slot 2 after it, and is
held up. Thread 1 takes slot 1, takes slot 2, and gives slot 1 back. Thread 2's exchange sees slot 1 at
the head, succeeds, and makes slot 2 the head while thread 1 holds it.

**As the code is** (`PoolFreeList.cfg`): every property holds in the checked finite model. The model
uses an unbounded counter and `CounterBound` excludes states whose head counter exceeds `MaxCounter`.
This lets TLC check the tagged algorithm without adding a wraparound at the model's small bound.
The model leaves out the theoretical `std::uint64_t` wraparound on practical grounds: it requires
`2^64` successful changes to the same head. At a continuous rate of one successful change per cycle
on a hypothetical 6 GHz processor, that would take about 97 years. A real successful change also
requires a 128-bit compare-and-swap on the shared head, so the comparison rate is deliberately
generous. This is a practical assumption about the allocator's operating lifetime, not a proof that
the counter cannot wrap.

| Slots | Threads | Most slots one thread holds | `MaxCounter` | Distinct states | Time on 16 workers | Result |
|---|---|---|---|---|---|---|
| 3 | 3 | 2 | 7 | 13,654,140 | 24 s | holds (the configuration as committed) |
| 3 | 3 | 2 | 10 | 100,146,444 | 194 s | holds |
| 3 | 2 | 2 | 14 | 1,207,007 | 3 s | holds |

**Allocator free-request validation** (`PoolFreeValidation.cfg`, two threads and two slots): TLC
checks the same invariants while exploring null pointers, non-pool pointers, requests for slots that
are not allocated, and valid frees. The valid request may come from a thread other than the one that
allocated the slot. All properties hold across 105,256 distinct states.

These results apply only to the listed numbers of slots, threads, held slots and counter bounds.
`PoolFreeValidation.cfg` explores invalid and already-free requests at the allocator boundary.
All models treat the inline x86-64 `movdqa` read of the aligned 128-bit head as one atomic
observation. TLC does not verify the compiler output, instruction behaviour or C++ memory-ordering
rules. The models also do not check progress: retries may continue indefinitely, and the
implementation's bounded retries for an apparently empty list are not modelled.

**Made to fail.** With two threads:

| Change | Property TLC found violated |
|---|---|
| The exchange compares only the pointer | `FreeListSound` (the second configuration) |
| A slot given back is linked to itself instead of to the head read | `FreeListSound` |
| Giving a slot back succeeds whether or not the head still matches | `FreeListSound` |
| Taking a slot succeeds whether or not the head still matches | checked alone, `NeverTakenTwice` |
| Taking a slot sets the head to nothing instead of the next slot | `NothingLost` |

## 4. Handing a follower over to new records: FollowerHandOver.tla

**The code.** `WalReadingThread` and `WalFollowerService`, as designed in
[following_the_wal.md](../../availability/following_the_wal.md), section 3.4. A follower of a WAL is
caught up by the reading thread and then handed over to the thread that owns the WAL, which sends it each
new record from then on. The owning thread releases records one at a time, and a follower may be sent a
record only once it is released. The reading thread reads the limit, sends every record up to it, and
reports the position it reached; the owning thread hands the follower over only if that position still
equals the limit, and otherwise tells the reading thread to carry on. Every send from either thread goes
on one queue into the reactor, in order. Each serving has a number, and a report from an earlier serving
is ignored. The follower's connection may be lost, after which sends queued for it are dropped, the
reading thread is told to stop, and the follower asks again from the last record it holds.

**The properties.**

| Property | Meaning |
|---|---|
| `NoGapNoRepeat` | On each connection, the follower is sent every record exactly once and in order |
| `NeverAboveLimit` | Nothing is queued for the follower that has not been released |
| `NothingMissedAtHandOver` | Once the follower is handed over and every queued send has gone out, it holds every record released |

**Handing over on any report** (`FollowerHandOverNoCompare.cfg`): TLC finds `NothingMissedAtHandOver`
violated, in 8 states. The reading thread reads the limit as record 1; the owning thread releases record 2,
and sends it nothing, because the follower is still being caught up; the reading thread reports that it
reached record 1; the owning thread hands the follower over, and record 2 is never sent.

**As designed** (`FollowerHandOver.cfg`): every property holds.

| Records | Follower holds at the start | Connections lost | Distinct states | Time on 16 workers | Result |
|---|---|---|---|---|---|
| 7 | none | up to 2 | 255,528 | 1 s | holds (the configuration as committed) |
| 8 | record 1 | up to 2 | 255,654 | 1 s | holds |
| 9 | none | up to 3 | 5,272,145 | 7 s | holds |

**Made to fail.**

| Change | Property TLC found violated |
|---|---|
| The follower is handed over on any report | `NothingMissedAtHandOver` (the second configuration) |
| A report from an earlier serving is acted on | `NothingMissedAtHandOver` |
| The owning thread sends new records while the follower is still being caught up | `NoGapNoRepeat` |
| The reading thread reports before it has queued its last record | `NothingMissedAtHandOver` |
| The reading thread reads one record past the limit | checked alone, `NeverAboveLimit` |

---

Back to [the framework](../README.md).
