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

### 128-Bit CAS: Compiler Flag Requirement

The 128-bit CAS maps to the x86-64 `CMPXCHG16B` instruction. Three things are required:

| Requirement | Detail |
|-------------|--------|
| **`-mcx16` compiler flag** | Tells the compiler that `CMPXCHG16B` is available. Without it, the `static_assert` in the constructor fires: *"Hardware 128-bit atomics not supported. Add -mcx16 to compiler flags."* Safe on all x86-64 CPUs manufactured after ~2006. In CMake: `target_compile_options(your_target PRIVATE -mcx16)` |
| **16-byte alignment** | The head structure is `alignas(16)`. `CMPXCHG16B` requires its operand to be 16-byte-aligned; misalignment causes a general protection fault. |
| **`unsigned __int128`** | Used as the underlying storage type rather than `std::atomic<struct>`. `std::atomic<struct>` may link against `libatomic` and `is_lock_free()` can return false even when hardware supports it. `unsigned __int128` with GCC's `__atomic_compare_exchange` intrinsic bypasses `libatomic` entirely and compiles directly to `CMPXCHG16B`. The `#pragma GCC diagnostic ignored "-Wpedantic"` suppression is required because `unsigned __int128` is a GNU extension. |

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

`drain_empty_slab_queue` aborts after 1 second of wall-clock time if `head_->next` has
still not become non-null. This is a safety net for a genuinely stuck producer; under
normal conditions the drain completes in nanoseconds.

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
