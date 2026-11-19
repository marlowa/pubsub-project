# The matching engine holds its memory from the start of the day {#the_engine_holds_its_memory_from_the_start}

**Status: proposed, 19 November 2026, for item 1 of [BUG-0028](../bug_list.md#bug_0028).** Section 7 lists the
decisions, none of them yet agreed.

## 1. The problem

A matching engine must never die during the trading day for lack of memory. Today it can.

The engine keeps its open orders in two places:

- **the open-order region**, a file on disk mapped into the engine's memory: 24.8 GB for 100 million orders in
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
default is small. On this machine it is about 4 GB. In production the limit for the user the venue runs as would have
to be raised above what the engine locks, a setting of the machine made once when it is deployed (in
`/etc/security/limits.d`, or in whatever starts the launcher). The engine would say, in its refusal, which limit
stopped it and what it would need.

## 4. The region

The region's pages can be locked the same way. They cannot get the engine killed, so locking them is not needed for
this bug; what locking them would add is that none is ever written back and dropped during the day, so that no order
waits for a page to be read from the disk. Today `warm()` writes every page at the start, but the kernel may still drop
clean pages later under memory pressure. Locking them would add 24.8 GB to what is locked in production, about 64 GB
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

## 6. What will be built and checked

- **The map** gains a way to lock its tables and touch every page, called by the order book where the region is warmed
  today, which reports how much it locked and how long it took.
- **The engine** refuses to start, at Critical, when locking fails, naming the limit or the shortage, and its exit
  status says it refused rather than failed.
- **A test** makes the lock fail on purpose, by lowering the limit on locked memory for a test process, and checks the
  refusal and its message.
- **A measurement** of the start's time before and after, in the development environment, and of the accept times with
  the order book's benchmark; and, with the map reserved for a large book, the engine's resident memory over a long run
  of orders and cancels, recorded with `vmstat`, which should be flat from the start.

## 7. Decisions

Each is a separate yes-or-no question.

1. **Does the engine write to every page of the map's tables before it reports itself ready, as it does for the
   region?** Recommended: yes. It is what BUG-0010 decided.
2. **Does it also lock the map's tables into memory with `mlock`, so that a shortage is an error the engine reports at
   Critical before refusing to start, rather than the out-of-memory killer, at the cost of raising the venue user's
   limit on locked memory in production?** Recommended: yes. It is the only way of the three in which the engine finds
   out for itself, and can say why.
3. **Does it lock the region's pages too, so that none is dropped and read back from the disk during the day, at 24.8
   GB more locked in production?** Recommended: yes, but it is a matter of latency, not of the engine dying, and could
   wait.
4. **Before it locks anything, does the engine work out from its capacities how much it will lock, log it, and compare
   it with its limit on locked memory, so that a limit too low is reported with the figure it needs before any time is
   spent?** Recommended: yes.
