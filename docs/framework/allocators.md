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
| Variable-size slab | `ExpandableSlabAllocator` | Alloc: owning thread only; Dealloc: any thread | Demand-driven; owning thread only |

A fifth class, `GrowthReportingAllocator<T>`, is not an allocation strategy of its own. It is for
long-lived state that grows, and is described [at the end of this document](#allocators_growth_reporting).

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
| **`unsigned __int128`** | Used as the underlying storage type rather than `std::atomic<struct>`. `std::atomic<struct>` may link against `libatomic`, and `is_lock_free()` can return false even when the hardware supports the operation. The `#pragma GCC diagnostic ignored "-Wpedantic"` suppression is required because `unsigned __int128` is a GNU extension. |
| **`__sync_bool_compare_and_swap`, not `__atomic_compare_exchange`** | GCC 13 and later emit a call into `libatomic` for `__atomic_compare_exchange` on a 16-byte type even with `-mcx16`, which puts an indirect call on every compare-and-swap. `__sync_bool_compare_and_swap` compiles to an inline `lock cmpxchg16b`. Its sequentially consistent ordering costs nothing extra on x86-64, where `lock cmpxchg16b` is fully ordered anyway. |
| **An inline `movdqa` to read the head** | `__atomic_load` on a 16-byte type also calls `libatomic` on GCC 13 and later. The head is read instead with an inline SSE `movdqa` instruction, which is atomic on x86-64 for a 16-byte-aligned address. This is used both for an ordinary read of the head and to reload it after a failed compare-and-swap. It makes `load_head()` and `compare_exchange_weak()` specific to x86-64: a port to another architecture must replace both. |

### Why Not a 64-Bit Packed Index?

The table above justifies `unsigned __int128` over `std::atomic<HeadPtr>` — but both are
128-bit. The prior question is why the head is 128 bits at all.

The alternative is standard: because slots are a contiguous array, the head does not need a
*pointer*. A 32-bit slot index plus a 32-bit counter packs into one ordinary 64-bit atomic,
and `CMPXCHG16B` never enters the picture. That would remove, in one stroke, every
complication in the table above — the `-mcx16` flag, the `alignas(16)` requirement, the
`unsigned __int128` GNU extension, the `-Wpedantic` suppression, and the x86-64 inline
assembler — and would make the free list portable to platforms with no double-width CAS.

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

**So the trade is: a handful of build and portability complications, all of them one-time and
already paid, against an uptime-dependent correctness risk.** That is why the head is 128 bits. If this
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

Despite its name, `USING_VALGRIND` is set for TSan builds as well: it means "use the mutex path", not "Valgrind is running". Helgrind and DRD
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

`allocation_count_` and `deallocation_count_` are atomic counters, incremented on every
allocation and every free. The number of free slots is computed from them, so pool statistics
can be reported without walking the free list. Walking it would not be safe while other threads
are allocating and freeing, because a slot's `free_next` can change under the reader.

---

## ExpandablePoolAllocator\<T\>

Chains `FixedSizeMemoryPool<T>` instances. Existing pools are never removed or reallocated,
so every raw pointer into them remains valid for the allocator's lifetime. There is no upper
limit on the number of pools: the allocator keeps adding them for as long as the operating
system provides memory.

**Fast path.** `allocate()` first tries the one pool that `current_pool_ptr_` points at, which
is the pool that most recently satisfied an allocation. This takes no lock.

**Slow path.** If that pool is full, `allocate()` takes `expansion_mutex_` and walks the chain
from the first pool, because slots may have been freed in an earlier pool since it filled. If
it finds a free slot, it points `current_pool_ptr_` at that pool. If every pool is full, it
creates a new pool, links it onto the end of the chain, and allocates from it, all while still
holding the mutex. The new pool is published to the fast path only after that allocation has
succeeded.

**Reporting.** The allocator calls `handler_for_pool_exhausted` each time it has to add a
pool, so an application can log that it reached out to the operating system for memory.
`get_behaviour_statistics()` returns counts of how the allocator has behaved over its
lifetime.

**Used for:** the nodes of every `LockFreeMessageQueue`. Each `ApplicationThread` has a
`LockFreeMessageQueue<EventMessage>` and the reactor has a
`LockFreeMessageQueue<ReactorControlCommand>`; each queue owns an
`ExpandablePoolAllocator<Node>` for its own nodes, one node per message in flight.

---

## BumpAllocator

A non-owning bump allocator over a caller-supplied byte buffer.

**Contract (the same as snprintf's):** `allocate<T>(element_count)` always advances
`bytes_used()` by the space the request needs, including any padding to align `T`, even when
the buffer is too small. When the buffer is too small it returns `nullptr`. So the caller can
run an encode or decode twice: first against an allocator constructed over `nullptr` with
capacity 0, which measures how many bytes are needed, then against a real buffer of that size.

**Measuring mode:** constructing the allocator over `nullptr` with capacity 0 is explicitly
supported. Every `allocate<T>()` then returns `nullptr` and only counts.

For the common case where the largest list size is known in advance, the DSL generates
`max_encode_arena_bytes_<MessageName>(max_elements)` and
`max_decode_arena_bytes_<MessageName>(max_elements)`, which give an upper bound for sizing a
stack buffer, so the measuring pass is not needed. Fixed-size messages need no allocator at
all.

**Thread safety:** none. `BumpAllocator` is for scratch use within a single call stack,
typically DSL encode/decode.

**Used for:** DSL message encode and decode scratch space. The caller calls `reset()` before
reusing it; nothing is freed individually.

---

## SlabAllocator

A single `mmap`-backed region from which chunks of **any size** are bump-allocated. Every
chunk is aligned to `alignof(std::max_align_t)`. There is no per-chunk metadata.

**Thread safety:** `allocate()` is called only by the thread that owns the slab's
`ExpandableSlabAllocator` (see below for which thread that is). `deallocate()` may be called
from any thread.

**Outstanding count:** an atomic counter records how many chunks are currently allocated from
the slab. `allocate()` increments it and `deallocate()` decrements it.

**Current and not current.** A slab starts out as the *current* slab, the one its owner is
allocating from. While a slab is current, nothing is sent through `EmptySlabQueue` when its
count reaches zero: the owner already holds a pointer to it, and instead resets its bump
pointer the next time it allocates and sees a count of zero, so the same memory is reused.
When the owner moves on to a new slab, it clears the old slab's `is_current_` flag. From
then on, whichever thread drives the outstanding count to zero enqueues the slab's slot
number on `EmptySlabQueue` so that the owner can destroy it. If the count is already zero
when the owner clears the flag, the owner enqueues it itself.

---

## ExpandableSlabAllocator

Chains `SlabAllocator` instances of one fixed size, set at construction. When the current
slab cannot fit a request, a new slab is appended. A slab that is no longer current is
destroyed once its outstanding count reaches zero.

**Used for:** inbound PDU payloads, and outbound PDU frames and raw bytes. (Inbound raw bytes
do not use it; they are read into a `MirroredBuffer` owned by the connection's protocol
handler.) There are two kinds of instance, with different owning threads:

| Instance | Owning thread (calls `allocate()`) | Slab size set by |
|----------|-----------------------------------|------------------|
| The reactor's inbound allocator, one per process | Reactor thread | `ReactorConfiguration::inbound_slab_size` (default 262,144 bytes) |
| Each `ApplicationThread`'s outbound allocator | That application thread | `ApplicationThreadConfiguration::outbound_slab_size` (default 65,536 bytes) |

A single allocation may not be larger than the slab size; asking for more throws
`PreconditionAssertion`. For an outbound allocator this makes the slab size an upper bound on
the size of one outbound PDU frame, header included.

**Return value.** `allocate(size)` returns `std::tuple<SlabHandle, void*>`, and the caller
passes both back to `deallocate(handle, ptr)`. `EventMessage` carries the handle alongside the
payload pointer for an inbound PDU, so the application thread can free the chunk with
`ApplicationThread::release_pdu_payload()`.

**Reclamation is demand-driven.** At the start of every `allocate()` call, the owning thread
drains `EmptySlabQueue` and destroys the slabs found there. There is no background reclaiming
thread and no timer, so reclamation cannot fall behind while allocation continues.

### Slab Registry

Slabs are held in a two-level registry of fixed size, so that `deallocate()` on any thread can
find a slab by index without a lock:

```
pages_[max_pages]            -- std::atomic<Page*>, a fixed array inside the allocator, never moves
    Page::slots[page_size]   -- std::atomic<SlabAllocator*>, one Page allocated per 256 slots
    Page::generations[...]   -- std::atomic<uint32_t>, one per slot
    Page::next_free[...]     -- int, links released slots into a free list
```

- `pages_` has 1024 entries and each `Page` holds 256 slots, so at most 262,144 slabs can be
  live at once.
- Slot `N` is `pages_[N >> 8]->slots[N & 0xFF]`.
- The first page is allocated in the constructor and later pages in `append_new_slab()` when
  they are first needed. Pages are never freed while the allocator exists, so a pointer read
  from the registry stays valid.
- A growable container such as `std::vector` cannot be used here. When it grows it frees its
  old storage, and a thread in `deallocate()` may be reading from that storage at the time.
- `deallocate()` loads the page pointer with `acquire` and then the slot pointer with
  `acquire`. The owning thread stores both with `release` in `append_new_slab()`, so a thread
  that sees the slot also sees the slab it points to. The owning thread's own reads, in
  `drain_empty_slab_queue()` and `load_slab_reactor()`, are `relaxed`, because nothing else
  writes those locations.

**Slots are reused.** When a slab is destroyed its slot goes onto a free list threaded
through `next_free`, and the next slab to be appended takes a slot from that list before
using a new one. The free list needs no synchronisation because only the owning thread
releases or claims slots. Reusing slots is what stops the fixed registry from limiting how
long a process can run: if every new slab took a new slot, the registry would limit the
number of slabs a process could ever create, and so the total bytes it could ever receive.

**A handle is a slot plus a generation.** Because a slot can be reused, the slot number alone
does not identify a slab. `SlabHandle` packs the slot into its low 32 bits and the slot's
generation into its high 32 bits. Releasing a slot increments its generation, so a handle
that outlives its slab no longer matches, and `deallocate()` throws `PreconditionAssertion`
rather than freeing into whatever slab now holds the slot. `SlabHandle` is an `enum class`
rather than an alias for `uint64_t` so that storing one in an `int`, which would lose the
generation, is a compile error. `invalid_slab_handle` is the value for "no chunk".

### Deferred Reclamation

When `drain_empty_slab_queue()` pops a slab from `EmptySlabQueue`, it does **not** destroy the
slab straight away. The most recently popped slab is held in `deferred_reclaim_slot_` until
the next drain.

The reason is that each slab's queue node is embedded in the slab itself, and the node most
recently popped stays in the queue as its sentinel: `head_` points at it. Destroying the slab
would free memory the queue still uses. A later drain that successfully takes another item
has moved `head_` past the node, which also shows that no producer can still be part-way
through enqueuing onto it. Only then is the held-over slab destroyed.

### Wall-Clock Drain Tripwire

**`drain_empty_slab_queue` can throw, and the throw shuts the process down.** It is worth
stating plainly, because a reader working out a component's failure modes would otherwise not
expect an allocator to be able to stop the process. The drain runs on the owning thread. On
the reactor thread, `Reactor::run` catches the exception, logs it and shuts the reactor down;
on an application thread, the thread's own loop catches it, logs it and asks the reactor to
shut down.

The drain loop is bounded by the number of live slabs. Spinning far past any sensible multiple
of that means the queue is corrupt, and a corrupt lock-free queue does not recover by being
spun on: the alternative to failing is spinning until the machine runs out of memory. So it
fails fast, with a `PubSubItcException` carrying the counters that tell the possible causes
apart.

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

The exception message states how many iterations were spun, then gives the counters —
`got_item`, `retry`, `last_slab_id`, `same_id_repeats`, `live_slabs` (the number of registry
slots ever brought into use) and `ids_to_reclaim.size` — and puts the interpretation last: a
self-loop or a stuck producer gives a high retry or repeat count against few or no items
taken. It deliberately does not open by naming a cause, because a message that asserts a
corrupt queue while its own counters show an empty one sends the reader looking for a bug
that is not there.

Under normal conditions the drain completes in nanoseconds and none of this is reached.

---

## EmptySlabQueue

An intrusive Vyukov MPSC queue of slab slot numbers. It is how a deallocating thread tells the
owning thread that a slab which is no longer current has become empty.

**Intrusive:** one `EmptySlabQueueNode` is embedded directly in each `SlabAllocator`. No
separate allocation is needed for queue membership.

**Producers:** any thread that calls `deallocate` and drives a non-current slab's outstanding
count to zero, and the owning thread itself in the case described under `SlabAllocator`.

**Consumer:** the owning thread only, in `drain_empty_slab_queue`.

**One-shot enqueue:** `SlabAllocator::try_claim_enqueue()` uses a one-shot CAS on an
`is_enqueued_` flag. Only the thread that wins the CAS enqueues the node. This prevents
double-enqueue even if multiple threads concurrently decrement the outstanding count to zero.

### No Reset of the Queue

The consumer never resets the queue to empty by pointing `head_` and `tail_` back at the
`dummy_` node. A reset of that kind would race with a producer part-way through an enqueue:
the producer's `tail_.exchange(node)` would be overwritten, and its following
`prev->next.store(node)` would then link a node that `tail_` no longer leads to. That node
could be reached from `head_` but never consumed, and the consumer would stop making progress.

The standard Vyukov sentinel pattern does not need a reset. The most recently popped node stays
in place as the sentinel (which is why its slab is held in `deferred_reclaim_slot_`), and
`head_` and `tail_` are only ever advanced. Producers never read or write `head_`, so the
consumer advancing it cannot race with them.

**Diagnostics:** four `peek_*` const accessors (`peek_head`, `peek_head_next`, `peek_tail`,
`peek_dummy`) allow tests and the drain function to inspect queue state without mutating it.

---

## GrowthReportingAllocator {#allocators_growth_reporting}

The allocators above are for objects that are allocated and released constantly. They do not
cover long-lived state that grows, such as an order book, a session table or a subscription
registry. Those are usually a hash map or a vector using `std::allocator`, so they go straight
to the heap and none of the reporting above sees them.

`GrowthReportingAllocator<T>` is a standard allocator that delegates to `std::allocator` and
calls a `Reporter`'s `on_large_allocation` callback for any single allocation at or above
`report_threshold_bytes`. A growing container allocates its whole storage in one call and
reallocates when it grows, so the callback fires once each time the container grows, not once
per element. The cost is one comparison on an allocation that was already going to the heap.

---

## Where Each Allocator Is Used

| Allocator | Used for | Who allocates | Who deallocates |
|-----------|----------|---------------|-----------------|
| `ExpandablePoolAllocator<Node>` inside `LockFreeMessageQueue<EventMessage>` | An application thread's message queue nodes | Any thread that sends to that application thread | The application thread, as it dequeues |
| `ExpandablePoolAllocator<Node>` inside `LockFreeMessageQueue<ReactorControlCommand>` | The reactor's command queue nodes | Any thread that enqueues a command for the reactor | The reactor thread, as it dequeues |
| `ExpandableSlabAllocator` (inbound, owned by the reactor) | Inbound PDU payloads | Reactor thread (`PduParser`) | The application thread, through `release_pdu_payload()` |
| `ExpandableSlabAllocator` (outbound, one per application thread) | Outbound PDU frames and raw bytes | The application thread, before sending a `SendPdu` or `SendRaw` command | Reactor thread, once the send has completed |
| `BumpAllocator` | DSL encode and decode scratch space | The encoding or decoding call | Nothing is freed individually; the caller calls `reset()` |

---

## See Also

- [Reactor](reactor.md) — how the reactor drives slab reclamation and the outbound/inbound PDU paths
- [Threading](threading.md) — `ExpandablePoolAllocator` backing `LockFreeMessageQueue` node allocation
