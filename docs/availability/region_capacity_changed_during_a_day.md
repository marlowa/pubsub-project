# A matching engine whose region's capacity differs from its configuration {#region_capacity_changed_during_a_day}

**Status: built.** This is the design for [BUG-0211](../bug_list.md#bug_0211). Option A was agreed with Andrew on 9
November 2026 (section 5), and is built: `MappedSlotStore::open` throws `StoreCapacityDiffersException` for an intact
region of another capacity, the engine refuses to start on it, and scenario 104 checks the refusal both ways.

## 1. The problem

The matching engine keeps its open orders in a memory-mapped region of fixed-size records, sized by `[order_book]
region_capacity` ([open_order_checkpoint.md](../durability/open_order_checkpoint.md)). An order arriving when the region
is full is refused. A region whose header gives a different number of records from the configured capacity is treated as
unusable, as a damaged one is, and the engine rebuilds its book from the WAL, from the day's first record, with the
configured capacity ([BUG-0208](../bug_list.md#bug_0208)).

A rebuild with a different capacity can give a different book from the one members were told of:

- **smaller:** an order accepted at the time is refused by the rebuild, because the smaller region fills sooner. The
  member holds an order the engine does not, and is sent a refusal, marked as a possible repeat, of an order it was told
  is open;
- **larger:** an order refused at the time, because the region was full, is accepted by the rebuild. The engine holds an
  order the member was told was refused.

The design document says to change the capacity only between trading days, and nothing enforces it.

## 2. What already exists

The region's header holds a magic number, a format version, the record size and the number of records (`slot_count`),
and opening the region checks the first three. So a region whose header is valid but whose record count differs from
the configuration can be told apart from a damaged region: its header, and its records, are intact.

## 3. The design

When the engine opens its region and finds a valid header whose record count differs from `region_capacity`, it does not
rebuild. It refuses to start, with an Error that names the region's file, the capacity it was created with, the capacity
configured, and what to do: set `region_capacity` back to the region's capacity for the rest of the trading day, and
change it between days. A damaged region, whose header is not valid, is moved aside and the book rebuilt from the WAL.

The region is left exactly as it was. Its header is read before the file is given its length or mapped, so opening
a region with a smaller capacity than it holds cannot cut off the records past the new end before it is refused.

With high availability on, the other engine goes on serving, so this is a loss of resilience and not of service; with it
off, the venue cannot trade until the configuration is put back, which is the right outcome for a configuration that
would otherwise change what members were told.

## 4. Steps, and how each is known to be right

1. A unit test of `OrderBook::open` (or of the region's opening): a region created with one capacity and opened with
   another, smaller and larger, is reported as "capacity differs", not as damaged; a region with a damaged header is still
   reported as damaged. Each made to fail on purpose by treating the two the same.
2. The engine refuses to start on "capacity differs", with the Error above; a scenario restarts an engine with its
   `region_capacity` changed and requires it to exit saying so, while the other engine keeps leading.
3. The full high-availability suite, with the WALs cleared first.

## 5. Decision

### Decision 1. Does the engine refuse whenever the capacity differs, or only when the configured one is smaller? (Choose one.)

- **A. Whenever it differs.** Both directions can give a different book, as section 1 shows.
- **B. Only when smaller**, as the bug entry first proposed. A larger capacity is then allowed, and gives a different book
  only on a day when the region was full at some moment.

Agreed: **A**. A larger region gives a different book exactly on the day it matters, the day the region filled, and
the operator's fix in both directions is the same: put the capacity back until the day ends.
