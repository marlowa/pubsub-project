# The matching engine holds its memory from the start of the day {#the_engine_holds_its_memory_from_the_start}

**Status: agreed with Andrew, 20 November 2026, for item 1 of [BUG-0028](../bug_list.md#bug_0028), and built.**
Section 7 lists the decisions. What a machine must allow is in
[locked_memory_requirement.md](../operations/locked_memory_requirement.md).

## 1. The problem

A matching engine must never die during the trading day for lack of memory. Today it can.

The engine keeps its open orders in two places:

- **the open-order region**, a file on disk mapped into the engine's memory: 26.4 GB for 100 million orders in
  production. Its pages belong to the file, so when memory is short the kernel can write them back to the file and
  reuse them. A shortage makes the region slow, by making the engine wait for the disk, but it cannot get the engine
  killed. The engine already writes to every page of it before it reports itself ready (`OrderBook::warm`), so that
  the first write to a page is not on the order path.
- **the map** from an order's identity to its record in the region, `IncrementalRehashMap`: ordinary memory, which
  the kernel calls anonymous, with nothing on disk behind it. In production it is reserved for 100 million orders:
  2^28 slots, each of about 140 bytes of entry, 4 bytes of kept hash and 1 byte of state, about 39 GB in all.

**Asking the kernel for memory is not getting it.** When the map asks for its tables, the kernel gives back a range of
addresses and supplies a page of real memory only when the program first writes to it. With the setting Linux uses by
default, and that this machine and, as far as is known, production use (`vm.overcommit_memory = 0`), the kernel does
not check at the time of the request that the memory will be there when it is needed. So the request succeeding at the
start of the day proves nothing.

Reserving the map today writes only the state byte of each slot, about 268 MB. The rest is supplied page by page as
orders arrive and land in slots. An order's slot is chosen by a hash of its identity, so orders are spread over the
whole table and not filled in from the front: by calculation, about two thirds of the map's pages are first written by
the time 10 million orders have arrived, and nearly all of them by 30 million. The map's demand for real memory
therefore rises steeply through the first part of the day.

**When the kernel has no free page to give,** and no swap left, it runs its out-of-memory killer, which chooses the
process using the most memory, the engine, and kills it outright, in the middle of whatever order it was handling. That
is what killed the primary engine on 21 August 2026, at 10.3 GB on a 31 GB machine.

[BUG-0010](../bug_list.md#bug_0010) decided the principle: the engine is to reserve and touch its memory when it
starts, and either get it then or fail then, logging at Critical and stopping, so that a shortage is found at the
start of the day, before any trading, rather than in the middle of it. This document is how.

## 2. Touching every page at the start

Before the engine reports itself ready, it writes to every page of the map's tables: the entries, the kept hashes and
the states. The kernel must then supply all of it before the first order arrives. The write does not change what the
map holds: it writes zero to each page of the entries and hashes, which hold nothing meaningful until an order is
placed, and the states are already written.

The engine already does this for the region, in `OrderBook::warm`; the map's part goes in the same place, so that one
step before the engine is ready covers both.

On its own this is not enough. If the memory is not there, the engine is still killed by the out-of-memory killer,
only at the start of the day: no message says why, and the launcher sees a process that died rather than one that
refused to start. And memory the kernel has supplied can still be taken back later, by swapping it out, if the machine
has swap.

## 3. Locking the map into memory

The kernel lets a process lock pages into memory (`mlock`). Locking a range makes the kernel supply every page of it
at once, and keep each in memory from then on: it is never swapped out or taken back. If the kernel cannot supply the
pages, or the process is not allowed to lock that much, the call **returns an error**, rather than anything being
killed. The engine can then log at Critical what it asked for and why it failed, and refuse to start, which is what
BUG-0010 asks for. Locking also does the touching of section 2: a locked page has been supplied.

What it needs: a process may lock only as much as its limit on locked memory (`RLIMIT_MEMLOCK`) allows, and the
default is small. On this machine it is about 4 GB. In production the limit for the user the venue runs as has
to be raised above what the engine locks, a setting of the machine made once when it is deployed (in
`/etc/security/limits.d`, or in whatever starts the launcher). The engine says, in its refusal, which limit
stopped it and what it needs.

## 4. The region

The region's pages can be locked the same way. They cannot get the engine killed, so locking them is not needed for
this bug; what locking them adds is that none is dropped during the day, so that no order
waits for a page to be read from the disk. `warm()` alone writes every page at the start, but the kernel may still drop
clean pages later under memory pressure. Locking them adds 26.4 GB to what is locked in production, about 65 GB
in all on a machine of 200 GB.

## 5. What it costs

- **Time at the start.** Supplying about 39 GB at the speed this machine writes memory, several gigabytes a second,
  takes several seconds; locking takes about the same, since it is the same work. The engine's start already takes 8
  to 17 seconds in production for the region (BUG-0028), and a few seconds more is within the guide that the start may
  take some seconds. It will be measured.
- **Memory held from the start rather than during the day.** The same memory, held earlier. Nothing more is used: the
  map is already reserved for that size.
- **A faster order path.** An order that is the first to write to a page of the map today pays for the kernel supplying
  the page. With every page supplied at the start, no order pays it. The measurements of backward-shift deletion
  ([deleting_without_erased_marks.md](../framework/deleting_without_erased_marks.md), section 8.1) put an accept at up
  to 900 nanoseconds at the 99th percentile, against about 60 at the median; part of that tail is likely to be this.

## 6. What is built and checked

- **The map** can lock its tables (`IncrementalRehashMap::hold_in_memory`) and say beforehand how much that is
  (`bytes_to_hold_in_memory`). A table it releases is unlocked. A table allocated later, by growth, is not locked; a map
  reserved for what it will hold never grows. **The region** can lock its mapping (`MappedSlotStore::lock_in_memory`).
- **The engine** (`MatchingEngineThread::hold_book_in_memory`), after opening the region and before recovering any
  order, logs how much it will lock and its limit, and refuses to start if the limit is too low; then locks the map and
  the region. A refusal is logged at Critical, and the engine exits with status 1, as for its other refusals to start.
- **Tests**, in the map's and the store's unit tests: locking locks every page of every array, and releasing unlocks
  them, for a table large enough to be mapped on its own and for one small enough to come from the heap; and locking
  fails with ENOMEM when the test lowers its own limit. Each test was made to fail on purpose by five breaks of the code:
  locking nothing, locking only some arrays, not unlocking, and the store locking nothing.
- **Measured** on the development machine, with the region for 2 million orders and the map reserved for 262,144: the
  engine locks 576 MB in 31 to 45 milliseconds, at its start in high-availability scenarios 1 and 111, both of which
  pass. Started by hand with a limit of 1 MB, it logs the refusal at Critical and exits with status 1 at once.
- **Still to measure:** the engine's resident memory over a long run of orders and cancels with the map reserved for a
  large book, recorded with `vmstat`, which should be flat from the start; and the accept times with every page of the
  map supplied first.

## 7. Decisions

Agreed with Andrew, 20 November 2026:

1. The engine has every page of the map supplied before it reports itself ready. Locking does this.
2. It locks the map's tables into memory, so that a shortage is an error the engine reports at Critical before refusing
   to start, and the venue user's limit on locked memory is raised in production.
3. It locks the region's pages too, 26.4 GB more locked in production, so that none is dropped and read back from the
   disk during the day.
4. Before it locks anything, it works out how much it will lock, logs it, and compares it with its limit, so that a limit
   too low is reported with the figure needed before any time is spent.
5. A refusal for lack of memory exits with status 1, as the engine's other refusals to start do.
