# The sequencer holds its memory from the start of the day {#the_sequencer_holds_its_memory_from_the_start}

**Status: agreed with Andrew, 20 November 2026, for [BUG-0131](../bug_list.md#bug_0131) and
[BUG-0121](../bug_list.md#bug_0121), and built.** The growth measured in section 1 is explained in section 3 and removed
([BUG-0274](../bug_list.md#bug_0274)); the record of command identifiers is locked into memory as section 4 describes.
Section 6 lists the decisions.

## 1. The problem

The matching engine now takes all the memory its order book will need before it reports itself ready, or refuses to
start ([the_engine_holds_its_memory_from_the_start.md](the_engine_holds_its_memory_from_the_start.md)). The sequencer
does not, and was measured growing through a day.

On 20 November 2026 the development venue ran for 83 minutes under 9.9 million orders, most of them cancelled, with every
process's memory recorded every 10 seconds. Both matching engines stayed at 2,408 MB throughout. The sequencers did not:

| Resident memory, MB | 07:12, start | 07:55 | 08:24 | 08:31 | 08:35, end |
|---|---|---|---|---|---|
| Primary sequencer, which led | 6,983 | 8,474 | 8,473 | 11,021 | 9,962 |
| Secondary sequencer | 6,038 | 6,810 | 6,808 | 6,808 | 6,807 |

The leading sequencer grew by 4 GB at most, in steps, and gave 1 GB of it back near the end. The follower grew during
its first five minutes and then stayed flat. A sequencer that grows during the day can be ended by the kernel's
out-of-memory killer during the day, which is what [BUG-0010](../bug_list.md#bug_0010) decided must not happen: memory is
to be found at the start of the day, or its lack reported then.

## 2. What the sequencer reserves at start

One large structure, on the leading sequencer and on its follower: **the record of command identifiers**
(`LoggedCommandIdentifiers`, [BUG-0121](../bug_list.md#bug_0121) and
[one_check_against_sequencing_twice.md](../availability/one_check_against_sequencing_twice.md)), which holds the
identifier and WAL position of every command, so that a command is never sequenced twice. It is a `tsl::robin_map`
reserved for `[commands] identifiers_reserved`, which each environment file sets: 200 million in test-1, pre-production
and production, 6 GiB, and 20 million in development, 768 MiB. Reserving it constructs every bucket, which writes to
every page, so the kernel supplies all of it at start; at 200 million the log says it took 2.1 seconds. At that size it
is most of the 7 GB each sequencer holds at its start.

The sequencer keeps nothing for each order. A report from the matching engine names the session whose command it
answers, which the engine took from the command, and carries the time that session's gateway read the command, so the
sequencer routes each report by what it carries (section 4 of
[sequencer_order_path_without_the_heap.md](sequencer_order_path_without_the_heap.md)).

## 3. What the growth was

The leading sequencer kept a map of which session placed each order, keyed by the order's sequence number, and removed
an order's entry when a report answering that sequence number said the order had ended. Every command was entered, the
cancels too. A cancel is a command with a sequence number of its own, and its report answers the cancel, not the order:
so a cancelled order's entry, which had only ever been answered by its acknowledgement, was never removed, and the
venue, which does no matching, ends no order by filling it. The map held every order placed during the day. It was
reserved for the orders expected to be open at once, and when it was half full it allocated a table twice the size,
moved its entries across a few at a time, and freed the old table. Each slot was 93 bytes.

In the run of section 1, 1,925 orders a second filled the first table of 2^23 slots, 0.78 GB, in the first twenty
minutes; at 4.2 million orders, about 07:50, it moved to a table of 2^24 slots, 0.78 GB more once the old one was freed;
at 8.4 million, about 08:25, to one of 2^25 slots, 3.1 GB, holding both tables while it moved, the old one
1.56 GB, and the measured memory fell by 1 GB at the end of the run. Those are the steps in the table, at the times they happened. The follower filled no such map
and stayed flat.

A second run confirmed it. On 20 November 2026 the development venue, whose map was reserved for 2 million orders, took
3,500 orders a second for 15 minutes, 80% of them cancelled, so that no more than about 0.6 million were open at once.
At 2.1 million orders placed, at 13:06:10, ten minutes in as predicted, the leading sequencer's memory grew: a mapping of
352 MB, the old table, was freed, and 735 MB was added beside the record of identifiers, the new one. The mappings were
read from `/proc/<pid>/smaps` every 10 seconds. Every one of the sequencer's threads freed as much as it allocated in a
60-second count of its heap calls ([BUG-0087](../bug_list.md#bug_0087)), because a table doubles too rarely to fall in
the window.

The map has been removed rather than corrected ([BUG-0274](../bug_list.md#bug_0274)): everything it supplied, the
session and the gateway's time of reading, already travels on the report. The same 15-minute run without the map, from a
fresh WAL and order book, processed all 3,152,000 orders. The leading sequencer's anonymous memory (`RssAnon` in
`/proc/<pid>/status`, sampled every 10 seconds) went from 7,014 MB to 7,021 MB and never higher, where with the map it
went from 7,018 MB to 7,814 MB; the follower's went from 6,793 MB to 6,807 MB, as before.

## 4. What the sequencer does

The sequencer does what the engine does, with one difference that follows from building the record after it is ready:

1. **Before it is ready,** in the constructor of `SequencerThread`, it works out the size of the table,
   `LoggedCommandIdentifiers::table_bytes_for(identifiers_reserved)`, logs it with its limit on locked memory
   (`LockedMemoryLimit`), and refuses to start at Critical if the limit is too low. The exception leaves the
   sequencer's `main`, which returns 1, as the engine's refusal does.
2. **It locks the record of identifiers as it is reserved.** Unlocked, its pages may be swapped out under memory
   pressure and read back on the thread that sequences orders. `tsl::robin_map` keeps its buckets in a vector it
   allocates itself, and gives no access to their address, but it takes an allocator as a template argument. The record
   gives it a `MemoryLockingAllocator`, which locks each block it allocates and unlocks it when the block is freed, and
   records in a `MemoryLockRecord` whether it could. The helper thread that builds the record
   (`LoggedCommandIdentifiersBuilder`) owns that record of locking.
3. **A failure to lock after it has started** is found when the WAL takes the finished record over, which it does at
   its next lease tick, within a second of the record being built. The WAL logs it at Critical and tells the sequencer
   (`ReplicatedWalEventsInterface::identifiers_not_held_in_memory`), which stops its reactor for a failure
   (`Reactor::shutdown_for_a_failure`), so that the process exits with status 1 although it was running. If it leads,
   its peer takes over, as when a leader dies. With the limit checked first, this happens only when the machine does
   not have the memory free.

The limit, what is logged, and how to set the limit are in
[locked_memory_requirement.md](../operations/locked_memory_requirement.md).

## 5. What it costs

- **Time at the start.** None before the sequencer is ready: the table is reserved and locked on the helper thread.
  In development, the 768 MiB table of each sequencer was reserved, locked and filled from an empty WAL 0.36 and 0.38
  seconds after the sequencer logged its figure. At production's size reserving alone takes 2.1 seconds; locking at
  production's size has not been measured.
- **The machine's limit on locked memory** for the user the venue runs as must cover the sequencer, 6,144 MB in
  production and 768 MB in development, as well as the engine
  ([locked_memory_requirement.md](../operations/locked_memory_requirement.md)).

## 6. Decisions

Agreed with Andrew, 20 November 2026, each yes:

1. The growth of section 1 is found first, with BUG-0087's tool across all the sequencer's threads and with
   `/proc/<pid>/smaps` sampled through a run, and removed, before the sequencer locks anything. Section 3 says what it
   was; the map that grew is removed, and reports are routed by the session they carry.
2. The sequencer locks the record of command identifiers, by giving its `tsl::robin_map` an allocator that locks what it
   allocates, checks its limit on locked memory first, and refuses to start at Critical with status 1 if it cannot, as
   the engine does.
3. The limit is checked before the sequencer is ready; the table is locked on the helper thread that builds it, after;
   and if locking still fails then, the sequencer logs it at Critical and stops with status 1 even if it leads, so that
   its peer takes over.
4. `identifiers_reserved` is set by each environment: 20 million in development, inside the development machine's limit
   on locked memory of 3,980 MB, and 200 million in test-1, pre-production and production.
