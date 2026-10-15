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
```

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

## Still to be modelled

[BUG-0162](../../bug_list.md#bug_0162) lists two more, in this order:

3. `FixedSizeMemoryPool`: the free list built on a 16-byte compare-and-exchange, and its protection
   against a block being taken, given back and taken again between one thread's read and its exchange.
4. The hand-over of a follower from the reading thread to new records
   ([following_the_wal.md](../../availability/following_the_wal.md), section 3.4): never a record missed,
   never one sent twice.

---

Back to [the framework](../README.md).
