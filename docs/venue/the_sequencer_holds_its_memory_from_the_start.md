# The sequencer holds its memory from the start of the day {#the_sequencer_holds_its_memory_from_the_start}

**Status: proposed, 20 November 2026, for [BUG-0131](../bug_list.md#bug_0131) and [BUG-0121](../bug_list.md#bug_0121).**
Section 7 lists the decisions, none of them yet agreed.

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

Two large structures, both on the leading sequencer and on its follower:

- **The record of command identifiers** (`LoggedCommandIdentifiers`, [BUG-0121](../bug_list.md#bug_0121) and
  [one_check_against_sequencing_twice.md](../availability/one_check_against_sequencing_twice.md)), which holds the
  identifier and WAL position of every command, so that a command is never sequenced twice. It is a `tsl::robin_map`
  reserved for `[commands] identifiers_reserved`, 200 million in every environment: 6 GiB. Reserving it constructs every
  bucket, which writes to every page, so the kernel supplies all 6 GiB at start; the log says it took 2.1 seconds. This
  is most of the 7 GB each sequencer holds at its start. It is not locked, so the kernel may swap its pages out later.
- **The map of which session placed each open order** (`MemberSessions::origins_`, section 4 of
  [sequencer_order_path_without_the_heap.md](sequencer_order_path_without_the_heap.md)), an `IncrementalRehashMap`
  reserved for `[commands] open_orders_expected`, the environment's matching engine region capacity. Each slot holds the
  order's sequence number and the session's identity, 88 bytes measured with `sizeof`, most of it the comp id's 64
  characters, plus 4 bytes of kept hash and 1 of state: 93 bytes. Reserving it writes only the state bytes; the rest is
  supplied page by page as orders first land in each slot, as the engine's map was.

| Open orders | Session map slots | Session map | Record of identifiers | Together |
|---|---|---|---|---|
| 4 million, the run above | 2^23 | 0.78 GB | 6.4 GB | 7.2 GB |
| 100 million, production | 2^28 | 25.0 GB | 6.4 GB | 31.4 GB |

## 3. What is not yet explained

The session map accounts for at most 0.78 GB of the leading sequencer's growth in the run above. The other 2 to 3 GB,
and the 1 GB given back near the end, are not explained by anything reserved at start. The ring of reports waiting for
the follower is allocated once, and the record of identifiers does not grow below its 200 million. So something on the
leading sequencer allocates during the day, which is a breach of principle F5 whatever it turns out to be.

Finding it comes first, before any locking, because locking the reserved structures would not stop it. Two instruments
are to hand: [BUG-0087](../bug_list.md#bug_0087)'s counting of heap allocations by thread, which is being built now, and
the kernel's map of the process's memory (`/proc/<pid>/smaps`), sampled through a run, which says which mapping grew.

## 4. The proposal

Once the growth of section 3 is found and removed, the sequencer is to do what the engine does:

1. **Before it reports itself ready,** work out how much it will lock, log it with its limit on locked memory, and
   refuse to start at Critical if the limit is too low.
2. **Lock the session map** with `IncrementalRehashMap::hold_in_memory`, which the engine's map already uses. The kernel
   then supplies every page at start.
3. **Lock the record of identifiers.** Its pages are already supplied at start, but not locked, so they may be swapped
   out under memory pressure and read back on the thread that sequences orders. `tsl::robin_map` keeps its buckets in a
   vector it allocates itself, and gives no access to their address. It does take an allocator as a template argument,
   so the record would pass one that locks each block it is given and unlocks it when the block is freed.
4. **A refusal** is logged at Critical and the sequencer exits with status 1, as the engine does.

## 5. A smaller session map

The session map's slot is 93 bytes because it holds the session's whole identity, 64 bytes of it the comp id, once for
every open order. Members are few and orders are many: a venue has hundreds to a few thousand comp ids. If the map held
a small number naming the session instead, an index into a table of the sessions the sequencer has seen today, a slot
would be the sequence number, the session's number and the gateway's ingress time, about 24 bytes, plus the 5 bytes of
hash and state: 29 bytes. At 100 million open orders that is 7.8 GB instead of 25.0 GB, and the whole sequencer would
lock about 14 GB instead of 31. The table of sessions is small, filled as sessions log on, and kept for the day, so a
session that logs off still has its number when its orders' reports arrive.

## 6. What it costs

- **Time at the start.** Locking supplies the session map's pages at about the rate the engine's book was locked in the
  run above, 2,167 MB in about 0.3 seconds, some 7 GB a second on the development machine: about 3.5 seconds for 25 GB,
  or about 1 second for 7.8 GB. This adds to the 2.1 seconds
  the record of identifiers already takes, and adds to [BUG-0121](../bug_list.md#bug_0121), a restarted sequencer
  taking longer to start than its peer's lease, which is already the case.
- **The machine's limit on locked memory** for the user the venue runs as must cover the sequencer as well as the
  engine ([locked_memory_requirement.md](../operations/locked_memory_requirement.md)).

## 7. Decisions

Each is a separate yes-or-no question.

1. **Is the growth of section 3 found first, with BUG-0087's tool and `/proc/<pid>/smaps`, and removed, before the
   sequencer locks anything?** Recommended: yes.
2. **Does the sequencer lock its session map at start, check its limit on locked memory first, and refuse to start at
   Critical with status 1 if it cannot, as the engine does?** Recommended: yes.
3. **Does it lock the record of command identifiers too, by giving its `tsl::robin_map` an allocator that locks what it
   allocates?** Recommended: yes. Its pages are supplied at start already; locking keeps them from being swapped out.
4. **Does the session map hold a small number for each session instead of the session's whole identity, as section 5
   describes, cutting the map from 25 GB to 7.8 GB in production?** Recommended: yes, and before decision 2 is built,
   so that what is locked is the smaller map.
