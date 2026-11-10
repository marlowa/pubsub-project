# The leading sequencer's order path without the heap {#sequencer_order_path_without_the_heap}

**Status: section 4, the session map, is built; section 3, the waiting reports, is being built.** This is the design for
[BUG-0131](../bug_list.md#bug_0131). Items 1 to 4 below were
agreed with Andrew on 11 October 2026, and both decisions in section 6 on 9 November 2026: the venue's figure for open orders is each environment's
`order_book_region_capacity`.

## 1. The problem

Two containers on the leading sequencer's order path allocate from the heap for every order or report (principle F5),
and one copies each report that waits (principle F6):

- **The session each order came from** (`MemberSessions::origins_`), a `std::unordered_map<int64_t, OriginSession>` keyed
  by the order's sequence number. It gains an entry for every order sequenced and loses it when the order ends, so it holds
  every open order. Each new key allocates a node, and when the table grows it rehashes every entry at once, the stall the
  order book suffered before it moved to `IncrementalRehashMap`.
- **Reports waiting for the record they depend on to be committed** (`ReportForwarding::pending_`), a
  `std::multimap<int64_t, Pending>` ordered by that record. Each `Pending` copies the report into a `std::vector<uint8_t>`
  and the ClOrdID into a `std::string`: with the map's node, up to three allocations and one copy for each waiting report.

## 2. What was agreed on 11 October 2026

1. The session map becomes an `IncrementalRehashMap`, sized at start-up from the expected number of open orders, so that in
   a normal day it never grows, and if it does, moves entries a few at a time.
2. A waiting report keeps the slab chunk it arrived in instead of being copied, and the chunk is released when the report
   is sent or discarded; and a limit is set on how much may wait, because a slab cannot be reclaimed until every chunk in it
   is freed.
3. The ClOrdID is held in a fixed-length `std::array`.
4. Each waiting report is then a fixed-size record, taken from storage sized so that it does not run out in a normal day.

## 3. The design for waiting reports

**A ring of fixed-size records, in the order reports arrive.** Each record holds the report's slab handle and pointer (the
chunk it arrived in, not a copy), the record it waits for, the ClOrdID in a fixed array, and the fixed fields `Pending`
holds now. The ring is allocated once, at start-up.

**Released from the front.** When a record is committed, reports are sent from the front of the ring while the front
report's record is committed through that point, and each report's chunk is released as it is sent. Reports arrive in the
order the engine applies commands, so the records they wait for rise; if one ever arrived out of order it would wait
behind the one before it, which delays it but never sends it before its record is committed.

**How much waits, and for how long.** A report waits only while the leader waits for its follower to acknowledge a record,
which is at most the follower's acknowledgement timeout, 100 milliseconds, before the leader commits without it. At the
highest rate measured on this venue, about 34,500 orders a second with two reports each, that is about 7,000 reports; the
ring holds 32,768, the size the room for reports held for unannounced sessions already uses. Slab chunks are therefore
held for at most about a tenth of a second.

**When the ring is nearly full** (decision 1): the leader stops reading the engines' connections for reports, with
`pause_reading`, until a commit frees room, and reads them again then. Nothing is copied, allocated or lost, and the
engine's own pacing (BUG-0234) keeps it from piling up reports meanwhile.

Pausing stops only the reads that come after it: reports the reactor has already read and queued for the sequencer's
thread still arrive. So each engine's connection for reports is limited to 1,024 messages waiting in that queue
(`limit_messages_waiting`, the framework's limit of [bounding_messages_waiting.md](../framework/bounding_messages_waiting.md)),
and the leader pauses once the ring has no more than 1,024 places left for each connection for reports. Every report
already read then has a place, and the ring cannot overflow. Agreed with Andrew on 10 November 2026, rather than copying
an overflow to the heap.

## 4. The session map

`MemberSessions::origins_` is an `IncrementalRehashMap<int64_t, OriginSession>`, sized at start-up for the expected
number of open orders (decision 2): `[commands] open_orders_expected` in each sequencer's configuration, set from the
environment's `matching_engine_order_book_region_capacity`, and reserved by `MemberSessions::reserve_orders` on the
sequencer's own thread before it sequences anything. `OriginSession` is a fixed size already. Inserting then allocates
nothing, and a day with more open orders than expected moves entries a few at a time rather than stalling.

Reserving writes only the table's one byte of state for each slot, 4 MB at two million orders, which costs the
sequencer's start-up a few milliseconds. The entries, 352 MB at two million orders, are left for the operating system to
give real memory to as orders first use each page: about one page fault for every 46 new orders, on the thread that
sequences them. The matching engine's book uses the same map in the same way. Touching every page at start-up would
remove those faults at the cost of holding the whole table in memory from the start, about 24 GB in production; that is
decided against, with Andrew, on 10 November 2026: the faults are left until they are measured to cost something.

## 5. Steps, and how each is known to be right

1. Unit tests of the ring: reports released in order as records are committed; a report never sent before its record is
   committed, including one that arrives out of order; chunks released as reports are sent and when they are discarded on
   losing the lead; reading paused when full and resumed when room is freed. Each made to fail on purpose.
2. A test that counts heap allocations on the order path, with an allocation counter around a burst of orders through the
   sequencer, before and after: before, at least one per order; after, none once the venue is running.
3. The full high-availability suite, and a latency run compared with the last, so that the change is known not to cost the
   order path anything.

## 6. Decisions

### Decision 1. When the ring of waiting reports is full, does the leader stop reading the engines' connections for reports until room is freed? (Yes or no.)

Agreed: **yes**. It allocates nothing, loses nothing, and uses a mechanism the framework already has; the alternative,
spilling to the heap, would bring back what this removes, in exactly the moment of most load.

### Decision 2. How many open orders is the venue built for? (Choose one.)

The session map is sized for it, and [BUG-0072](../bug_list.md#bug_0072) needs the same figure for the gateways' pools of
open orders, which today disagree with each other by a factor of 256. The matching engine's region is sized per
environment by `order_book_region_capacity`: 2,000,000 in development, 1,000,000 in the preprod and test-1 environments,
and 100,000,000 in production, where the engine never fills an order, so a day of 100 million orders with none cancelled
must still be accepted.

Agreed with Andrew on 9 November 2026: **the figure is each environment's own `order_book_region_capacity`**, so that the
session map, the gateways' pools and the engine's region agree in every environment. The session map costs 89 bytes for
each slot, an 88-byte entry and a byte of state, and keeps at least half its slots free, so it has at least twice as many
slots as orders, rounded up to a power of two. At 2,000,000 orders that is 4,194,304 slots, about 370 MB; at 100,000,000
it is 268,435,456 slots, about 24 GB, against the engine region's 24.8 GB. It is reserved once at start-up.
