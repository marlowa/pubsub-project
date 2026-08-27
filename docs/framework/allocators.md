# Allocators

## Design Goals

Heap allocation (`new` / `malloc`) is banned on every hot path. The reasons are latency and
predictability: heap allocators take locks, may call the OS for more pages, and produce
unpredictable tail latencies under load.

The framework uses four distinct allocation strategies, each matched to its use case:

| Strategy | Class | Hot-path thread-safe | Reclamation |
|----------|-------|---------------------|-------------|
| Fixed-size pool, Treiber-stack free list | `FixedSizeMemoryPool<T>` | Yes | Never |
| Expanding chain of pools | `ExpandablePoolAllocator<T>` | Yes | Never |
| Bump | `BumpAllocator` | No | `reset()` only |
| Variable-size slab | `ExpandableSlabAllocator` | Alloc: reactor thread only; Dealloc: any thread | Demand-driven; reactor thread only |

---

## FixedSizeMemoryPool\<T\>

A single fixed-capacity pool backed by one `mmap` region. All `Slot<T>` objects live in
contiguous memory for the lifetime of the pool.

### Treiber Stack Free List

The free list is a **Treiber stack** — a lock-free LIFO stack of free `Slot<T>` pointers.
The stack is intrusive: when a slot is free, its own `free_next` field stores the pointer to
the next free slot. No separate node allocation is needed.

**The ABA problem.** A naive lock-free stack suffers from the ABA problem:
1. Thread A reads `head → X`, then is pre-empted.
2. Thread B pops `X` (uses it), then pops `Y`, then pushes `X` back.
3. Thread A resumes. Its CAS sees `head == X` (unchanged), so it succeeds — but `X->next`
   now points somewhere unexpected. The stack is corrupt.

**Solution: 128-bit tagged pointer.** The head is stored as a `{pointer, counter}` pair.
Every successful CAS increments the counter. Even if the same address `X` is pushed back, the
counter will differ, so Thread A's stale CAS fails. Because `Slot<T>` objects are never freed
during the pool's lifetime (only the entire `mmap` region is freed at destruction), there is
no additional ABA risk from address reuse across pools.

**Why LIFO.** The stack order is not incidental. `deallocate()` pushes and `allocate()` pops
the same end, so a slot handed out is usually the one most recently returned — still hot in
L1/L2, and often still in the same cache line set. A FIFO free list would hand back the
coldest slot every time, which is the worst possible choice for a hot path.

This is the answer to the standard objection that a linked free list defeats the hardware
prefetcher. It is not wrong, but it applies to the *steady state after sustained churn*, not
to the common case: under bursty allocate/free of a working set smaller than the pool, LIFO
reuse keeps the same few slots resident and the prefetcher is barely involved. Where locality
does decay — many threads, long runs, high slot turnover — the decay is bounded by the fact
that every slot lives inside one contiguous `mmap` region, so the TLB footprint stays fixed
however scrambled the pointer order becomes.

### 128-Bit CAS: Compiler Flag Requirement

The 128-bit CAS maps to the x86-64 `CMPXCHG16B` instruction. Three things are required:

| Requirement | Detail |
|-------------|--------|
| **`-mcx16` compiler flag** | Tells the compiler that `CMPXCHG16B` is available. Without it, the `static_assert` in the constructor fires: *"Hardware 128-bit atomics not supported. Add -mcx16 to compiler flags."* Safe on all x86-64 CPUs manufactured after ~2006. In CMake: `target_compile_options(your_target PRIVATE -mcx16)` |
| **16-byte alignment** | The head structure is `alignas(16)`. `CMPXCHG16B` requires its operand to be 16-byte-aligned; misalignment causes a general protection fault. |
| **`unsigned __int128`** | Used as the underlying storage type rather than `std::atomic<struct>`. `std::atomic<struct>` may link against `libatomic` and `is_lock_free()` can return false even when hardware supports it. `unsigned __int128` with GCC's `__atomic_compare_exchange` intrinsic bypasses `libatomic` entirely and compiles directly to `CMPXCHG16B`. The `#pragma GCC diagnostic ignored "-Wpedantic"` suppression is required because `unsigned __int128` is a GNU extension. |

### Why Not a 64-Bit Packed Index?

The table above justifies `unsigned __int128` over `std::atomic<HeadPtr>` — but both are
128-bit. The prior question is why the head is 128 bits at all.

The alternative is standard: because slots are a contiguous array, the head does not need a
*pointer*. A 32-bit slot index plus a 32-bit counter packs into one ordinary 64-bit atomic,
and `CMPXCHG16B` never enters the picture. That would remove, in one stroke, every
complication in the table above — the `-mcx16` flag, the `alignas(16)` requirement, the
`unsigned __int128` GNU extension, and the `-Wpedantic` suppression — and would make the
free list portable to platforms with no double-width CAS.

Two things argue the other way, and only one of them is decisive:

| | Packed 64-bit index | 128-bit tagged pointer (chosen) |
|---|---|---|
| Pop path | index → address arithmetic on every pop | pointer dereferenced directly |
| ABA counter | 32 bits | 64 bits |
| Capacity ceiling | 2^32 slots | none |

**The capacity ceiling is not a real argument.** `objects_per_pool` is an `int`, so a pool is
already capped near 2^31 slots. A 32-bit index cannot be the binding constraint.

**The counter width is.** The ABA counter must not wrap while a pre-empted thread holds a
stale head. A 32-bit counter wraps after 2^32 successful CAS operations — at 10M
allocations/sec, about seven minutes. For a process expected to run for days, that is not a
theoretical bound but a routinely-reached one. A wrap is only *exploitable* if it coincides
with a pre-empted thread's window, so the practical failure rate is far below one per wrap —
but it is a real probability that grows with uptime, and it buys a failure mode that is
silent, rare, and corrupts the free list. A 64-bit counter at the same rate wraps after
roughly 58,000 years, which retires the question rather than shrinking it.

The address arithmetic is a genuine but minor saving, and is not on its own a reason to
prefer 128 bits.

**So the trade is: four build-system complications, all of them one-time and already paid,
against an uptime-dependent correctness risk.** That is why the head is 128 bits. If this
allocator is ever ported to a platform without double-width CAS, the packed index is the
right fallback — but it should carry a 48-bit counter and a 16-bit index, or an explicit
argument about why a 32-bit counter is safe at that platform's allocation rate.

### Why Not Hazard Pointers?

Hazard pointers are the other standard answer to unsafe lock-free stacks, and they get
proposed here often enough to be worth settling.

**First, the usual reason for rejecting them does not apply.** Hazard pointers do not need a
background reclamation thread. Each thread pushes retired nodes onto a thread-local list and,
once that list crosses a threshold, scans the published hazard array *itself* and frees
whatever no thread has claimed. Reclamation happens inline, on the retiring thread, in
bounded work. There is no reclaimer to fall behind and nothing to starve. That objection
belongs to epoch-based reclamation and RCU-style schemes, where a grace period really can be
held open indefinitely by a stalled participant.

**The reason that does apply is that they solve a problem this pool does not have.** Hazard
pointers answer one question: *is it safe to free this node yet?* `FixedSizeMemoryPool` never
frees an individual node. Every slot is constructed during pool construction and stays valid
until the entire `mmap` region is unmapped at destruction — see the *Safety of Treiber Stack
in Non-GC Environments* section in `FixedSizeMemoryPool.hpp` for the full argument. A slot
popped by one thread while another holds a stale pointer to it is not freed memory; it is
live, mapped, correctly-typed storage that simply belongs to someone else now.

So there is no use-after-free window to protect, and hazard pointers would buy nothing while
charging a store-plus-fence to publish a hazard on every single access to the head. The only
residual risk is *logical* ABA — a slot legitimately recycled through the free list — and
that is exactly what the tagged counter handles, at the cost of an increment already folded
into a CAS the code must perform regardless.

The general rule this is an instance of: **hazard pointers are a memory-reclamation
technique, not an ABA technique.** They are the right tool when a lock-free structure hands
memory back to the allocator, and the wrong tool when its nodes are immortal by construction.
Reach for them here only if the pool ever gains the ability to release slots individually,
which would invalidate the whole argument above.

### Build Paths

| Build | Macro | Free-list implementation |
|-------|-------|--------------------------|
| Production / ASan | *(none)* | Lock-free Treiber stack, `CMPXCHG16B` |
| Valgrind (Helgrind / DRD) | `USING_VALGRIND` | `std::mutex` + `std::vector` |
| ThreadSanitizer | `USING_VALGRIND` | `std::mutex` + `std::vector` |

The `USING_VALGRIND` macro name is historical — it also covers TSan builds. Helgrind and DRD
cannot model `CMPXCHG16B` and report false positives. TSan intercepts memory accesses to
track per-thread ordering but cannot decompose `CMPXCHG16B` into the individual accesses it
needs to instrument. Both tools require the mutex path to analyse the surrounding code
correctly. ASan is compatible with the lock-free path because it instruments memory safety
(bounds, lifetime) without decomposing atomics.

### `Slot<T>` Layout

```
Production path:   [ is_constructed (atomic) ][ free_next (atomic) ][ canary (u64) ][ storage (alignas T) ]
Valgrind/TSan path:[ is_constructed (atomic) ][                     canary (u64)   ][ storage (alignas T) ]
```

The two paths are **not** byte-for-byte identical. `free_next` only exists in the production
path. Both paths share the invariants that matter for `ExpandablePoolAllocator`'s helper
functions: `is_constructed` is the first field, `canary` is immediately before `storage`, and
`storage` is last. Offsets are computed via `offsetof(SlotType, storage)`, which is
build-path-correct in each translation unit.

**Canary (`0xDEADC0DEFEEDFACE`):** written at slot construction; checked before
destruction and on deallocation. A one-byte underrun from a `T` object corrupts the canary
rather than `is_constructed`, making buffer-underrun bugs diagnosable in core dumps.

`deallocation_count_` is an atomic counter incremented on every free, allowing pool statistics
to be reported safely without traversing the free list.

---

## ExpandablePoolAllocator\<T\>

Chains `FixedSizeMemoryPool<T>` instances. When all pools are exhausted, a new pool is
appended under a mutex (expansion is infrequent and off the critical path). Existing pools
are never removed or reallocated — all raw pointers into them remain valid for the
allocator's lifetime.

Fast-path allocation tries each pool in order without taking the mutex. The mutex is only
needed during pool expansion.

**Used for:** `LockFreeMessageQueue` node objects (one node per in-flight `EventMessage`)
and `ReactorControlCommand` queue nodes.

---

## BumpAllocator

A non-owning bump allocator over a caller-supplied byte buffer.

**Contract (snprintf-style):** `allocate(size)` always advances `bytes_used()` by `size`,
even when the buffer is exhausted. This means the caller can call it twice — first with
`nullptr` and size 0 to measure the required buffer, then with a real buffer — and the
second call will produce valid output provided `bytes_used() <= capacity()`.

**Measuring mode:** passing `nullptr` + 0 as the buffer is explicitly supported.

**Thread safety:** none. `BumpAllocator` is for scratch use within a single call stack,
typically DSL encode/decode.

**Used for:** DSL message encode and decode scratch space.

---

## SlabAllocator

A single `mmap`-backed slab that bump-allocates fixed-size chunks. Each chunk is the same
size; the size is set at construction.

**Allocation:** bump pointer advance only. No per-chunk metadata. Allocation is O(1) and
branchless on the fast path.

**Thread safety:** allocation is reactor-thread-only. Deallocation may happen on any thread
(application threads call `deallocate` after processing an inbound PDU).

**Outstanding count:** an atomic counter tracks how many chunks are currently allocated.
When the count reaches zero after the slab has been exhausted, `SlabAllocator` notifies
`ExpandableSlabAllocator` via `EmptySlabQueue` that it is ready for reclamation.

**Slab ID:** each `SlabAllocator` has an integer ID. `EventMessage` carries the `slab_id`
alongside the payload pointer so the application thread can call the right slab's
`deallocate`.

---

## ExpandableSlabAllocator

Chains `SlabAllocator` instances. When the current slab is exhausted, a new one is
appended on demand. Old slabs are reclaimed when their outstanding count drops to zero.

**Used for:** inbound and outbound PDU payloads. Chunks are large enough to hold one PDU
payload; chunk size is configured in `ReactorConfiguration`.

### Slab Directory (SIGSEGV Fix)

The original implementation stored slabs in a `std::vector<unique_ptr<SlabAllocator>>`.
When `push_back` triggered reallocation, it freed the old backing array while application
threads were concurrently reading raw `SlabAllocator*` pointers out of it via `deallocate`.
This was undefined behaviour that crashed under ASan/TSan.

Fix: the vector was replaced with a two-level segmented atomic array.

```
pages_[kMaxPages]          — std::atomic<Page*>, in-object, never moves
    Page::slots[kPageSize] — std::atomic<SlabAllocator*>, heap-allocated once per page
```

- `pages_` has 1024 directory slots; each `Page` holds 256 slots.
- Slab ID `N` maps to `pages_[N >> 8]->slots[N & 0xFF]`.
- Page 0 is allocated in the constructor. Further pages are allocated on demand in
  `append_new_slab`.
- Pages are **never freed** during the allocator's lifetime, so raw pointers into them
  remain valid forever.
- Workers in `deallocate()` load the page pointer with `acquire`, then the slot pointer
  with `acquire`. The reactor's `release` stores in `append_new_slab` guarantee
  visibility to concurrent `deallocate` callers.
- `drain_empty_slab_queue` and `load_slab_reactor` use `relaxed` loads (reactor-thread
  only — no concurrent writer at those sites).

### Deferred Reclamation

When `drain_empty_slab_queue` pops a slab from `EmptySlabQueue`, it does **not** destroy
the slab immediately. Instead, the slab is held in `deferred_reclaim_slab_id_` for one
more drain cycle.

The reason: the popped slab's `EmptySlabQueue` node (embedded in the slab itself) is still
acting as the Vyukov queue's sentinel — `head_` and `tail_` both point at it. Destroying
the slab would free the node while the queue still holds a pointer to it. Deferring
destruction by one drain guarantees that by the time the slab is destroyed, a subsequent
successful `GotItem` has advanced `head_` past it, confirming that no producer can still be
mid-enqueue on that node.

### Wall-Clock Drain Tripwire

**`drain_empty_slab_queue` can throw, and the throw terminates the reactor.** That is the
intended behaviour and it is worth stating plainly here, because reasoning about a component's
failure modes from this document would otherwise miss that the allocator can stop the process.

The drain loop is bounded by the number of live slabs. Spinning far past any sane multiple of
that means the queue state is corrupt, and a corrupt lock-free queue does not recover by being
spun on: the alternative to failing is spinning until the machine is out of memory. So it fails
fast, with a `PubSubItcException` carrying the counters that distinguish the causes, and the
reactor terminates cleanly where an operator can see why.

**Two conditions are required, not one**, and the reason is the interesting part:

| | |
|---|---|
| A spent budget | more than one second of `steady_clock` time has passed |
| A loop that has actually spun | more than 1,000 iterations |

Wall-clock time alone does not describe progress. A tight retry loop runs in tens of nanoseconds
per iteration, so a sub-millisecond preemption can cover hundreds of thousands of iterations —
but a thread the scheduler simply has not run manages **one** iteration in the same second. The
first is a stuck producer; the second is an ordinary preemption that resolves itself. Only the
iteration count separates them, which is why the budget alone would fire on a healthy system
under load. See `drain_loop_has_stalled` in `ExpandableSlabAllocator.cpp`.

The clock is read on **every** iteration and its result passed in, rather than being
short-circuited away by the cheaper iteration test. That read yields the few tens of nanoseconds
a mid-enqueue producer needs to finish, so a retry spin does not starve the producer it is
waiting on.

The exception message leads with the counters and puts the conclusion last — `got_item`,
`retry`, `last_slab_id`, `same_id_repeats`, `live_slabs`. A self-loop or a stuck producer gives a
high retry or repeat count against few or no items taken. An earlier wording opened with "likely
a corrupted lock-free queue state" while its own diagnostics said the queue was empty and no id
had repeated, and it sent a reader hunting a lock-free bug that did not exist.

Under normal conditions the drain completes in nanoseconds and none of this is reached.

---

## EmptySlabQueue

An intrusive Vyukov MPSC queue of slab IDs. It is the feedback channel from application
threads back to the reactor: when a `SlabAllocator`'s outstanding count reaches zero, it
enqueues its own ID so the reactor knows to reclaim it.

**Intrusive:** one `EmptySlabQueue::Node` is embedded directly in each `SlabAllocator`. No
separate allocation is needed for queue membership.

**Producers:** any `ApplicationThread` that calls `deallocate` and drives the outstanding
count to zero.

**Consumer:** the reactor thread only, in `drain_empty_slab_queue`.

**One-shot enqueue:** `SlabAllocator::try_claim_enqueue()` uses a one-shot CAS on an
`is_enqueued_` flag. Only the thread that wins the CAS enqueues the node. This prevents
double-enqueue even if multiple threads concurrently decrement the outstanding count to zero.

### Race Condition and Fix (Session 16)

The original `EmptySlabQueue` had a `reset_to_empty()` method that the consumer called
after each drain. It wrote `dummy_.next = nullptr; head_ = &dummy_; tail_ = &dummy_`.
This interleaved with an in-flight producer enqueue, clobbering the producer's
`tail_.exchange(node)` back to `&dummy_`. The producer's subsequent
`prev->next.store(node)` then wrote `dummy_.next = node`, but `tail_` no longer pointed
at `dummy_`. Result: a "ghost-enqueued" slab visible via `head_->next` but unreachable
from `tail_`, wedging the consumer permanently.

Fix: `reset_to_empty()` was removed entirely. The classical Vyukov sentinel pattern does
not need it — the most-recently-popped node remains alive as the sentinel (via
`deferred_reclaim_slab_id_`) and `head_` and `tail_` are never reset. Producers never
read or write `head_`, so the consumer advancing `head_` is race-free.

**Diagnostics:** four `peek_*` const accessors (`peek_head`, `peek_head_next`, `peek_tail`,
`peek_dummy`) allow tests and the drain function to inspect queue state without mutating it.

---

## Where Each Allocator Is Used

| Allocator | Used for | Who allocates | Who deallocates |
|-----------|----------|---------------|-----------------|
| `ExpandablePoolAllocator<Node>` | `LockFreeMessageQueue` nodes | Producer threads (reactor, connection managers) | Consumer thread (`ApplicationThread`) |
| `ExpandablePoolAllocator<ReactorControlCommand>` | Reactor command queue nodes | `ApplicationThread` subclasses | Reactor thread |
| `ExpandableSlabAllocator` (inbound) | Inbound PDU payload buffers | Reactor thread (`PduParser`) | `ApplicationThread` after `on_framework_pdu_message` |
| `ExpandableSlabAllocator` (outbound) | Outbound PDU payload buffers | `ApplicationThread` before `SendPdu` command | Reactor thread after send completes |
| `BumpAllocator` | DSL encode/decode scratch | Call stack (per message) | Implicit on stack unwind |

---

## See Also

- [Reactor](reactor.md) — how the reactor drives slab reclamation and the outbound/inbound PDU paths
- [Threading](threading.md) — `ExpandablePoolAllocator` backing `LockFreeMessageQueue` node allocation
