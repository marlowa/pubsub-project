# Deleting from the order map without leaving erased marks {#deleting_without_erased_marks}

**Status: proposed, 19 November 2026, for [BUG-0028](../bug_list.md#bug_0028).** Section 9 lists the decisions, none of
them yet agreed.

## 1. The problem

The matching engine finds an open order's record in the open-order region through a map from the order's identity to
the record's number: `IncrementalRehashMap`, in the library. In production the map is reserved, when the engine starts,
for the most orders that may be open at once, 100 million: 2^28 slots of about 140 bytes, 37.8 GB. Reserving it is meant
to make its memory fixed for the whole day, with nothing allocated after the start.

It is not fixed today, because of the way an order is removed. The map is an open-addressing table with linear
probing:

- an order's identity is turned by a hash function into a slot number, the order's **home slot**;
- a new order goes into its home slot, or, if that is taken, into the first free slot after it, wrapping round from
  the last slot to the first;
- a search starts at the home slot and moves along one slot at a time until it finds the order, or reaches an empty
  slot, which means the order is not in the map.

A cancelled order's slot cannot simply be emptied. A later order whose home slot is before it, and which was placed
beyond it because it was taken, would no longer be found: its search would stop at the newly empty slot. So today the
slot is marked **erased**: a search steps over it, as if it were taken, and a new order may reuse it if its search
passes it.

The map keeps itself at most half full, because searches slow down sharply beyond half, and an erased slot counts as
used. A new order mostly lands on a slot never used before, so every order placed during the day uses up a slot,
whether or not it is later cancelled. Once about half the slots are used, about 134 million orders placed in one day
with 2^28 slots, the map copies its orders to a new table of the same size, which leaves the erased slots behind, and
holds both tables while it copies: a second 37.8 GB, asked for at a moment nobody chose.

A day of 50 million orders, the planned volume, never reaches that point, however many orders are cancelled. But the
copy exists, it is set off by volume alone, and it is the one thing that stops the map's memory being fixed from the
start of the day.

## 2. The method: backward-shift deletion

When an order is removed, its slot is emptied, and then the searches the empty slot would break are repaired by moving
orders back into it. This is the standard way to delete from a linear-probing table without marks: Knuth, *The Art of
Computer Programming*, volume 3, section 6.4, Algorithm R.

The rule that makes a linear-probing table work is this: **every order sits either in its home slot or further along,
with no empty slot between its home slot and where it sits.** A search relies on nothing else. Removing an order makes
an empty slot, a **gap**, which may break the rule for orders after it. The repair walks forward from the gap:

1. Look at the next slot. If it is empty, stop: no order beyond it can have a search that passes through the gap.
2. Otherwise it holds an order. Work out the order's home slot. If the gap lies between the home slot and the order's
   slot, counting forward and wrapping round, then the order's search passes through the gap, so move the order back
   into the gap; its old slot becomes the gap.
3. If the gap does not lie on its search path, its home slot is after the gap, and the order stays where it is.
4. Go to step 1, with the next slot along.

At the end, the gap is emptied. No slot is ever marked erased.

**A worked example,** with 8 slots. Four orders arrive: A's home is 2, so A goes in slot 2; B's home is also 2, slot 2
is taken, so B goes in slot 3; C's home is 3, slot 3 is taken, so C goes in slot 4; D's home is 5, so D goes in slot 5.

```
slot:   0   1   2   3   4   5   6   7
        .   .   A   B   C   D   .   .
```

A is cancelled, and slot 2 becomes the gap.

- Slot 3 holds B, whose home is 2. The gap, at 2, is on B's search path, from 2 to 3, so B moves back into slot 2.
  The gap is now at 3.
- Slot 4 holds C, whose home is 3. The gap, at 3, is on C's path, from 3 to 4, so C moves back into slot 3. The gap
  is now at 4.
- Slot 5 holds D, whose home is 5. The gap, at 4, is before D's home, so it is not on D's path, and D stays.
- Slot 6 is empty. The repair stops, and slot 4 is emptied.

```
        .   .   B   C   .   D   .   .
```

Every order is found from its home slot, B from 2, C from 3 and D from 5, and the table is exactly as if A had never
been placed.

**The test for "the gap lies on the order's path"**, with wrap-around, using the table's mask (its capacity less one,
the capacity being a power of two): the order at slot `j` with home `h` may move into the gap `g` when
`((j - h) & mask) >= ((j - g) & mask)`, that is, when the order is at least as far from its home as it is from the gap.

## 3. While the map is copying

`IncrementalRehashMap` still copies itself when its live orders fill half the table, to a table twice the size. That is
the growth a reserved map is meant never to need, but the code allows it, and a removal during a copy must still be
right. During a copy there are two tables: the one being copied **out of**, whose orders are moved a few at a time in
slot order by a cursor, and the one being copied **into**.

- **In the table being copied into,** backward shift is used, as in a table not being copied. Orders are placed there
  only by the copy and by new inserts, both of which keep the rule of section 2.
- **In the table being copied out of,** a removed order's slot is still marked erased, as today. Backward shift there
  could move an order from a slot the cursor has not reached into a slot it has already passed, and that order would
  never be copied and would be lost. The marks do no harm in that table: nothing is inserted into it, and it is
  released when the copy ends.

The copy itself already marks each slot it has moved an order out of as erased, for the same reason, and that is
unchanged.

## 4. What changes for a caller

Nothing in what the map holds, finds or reports. One thing changes in walking the map while removing from it:
`erase(iterator)` returns where the walk should go on from. Today that is the next slot. With backward shift, an order
from further along may have moved back into the slot just emptied, so the returned position is that same slot when it
is now taken, and the next slot only when it is empty. One case remains that a walk cannot avoid: when the repair wraps
from the last slot to the first, an order from the start of the table, which the walk has already passed, can move to
the end of the table and be visited a second time. No code in the venue walks the map while removing from it: the order
book collects the orders it is to cancel first and removes each by its identity, and so does the sequencer's
`MemberSessions`. The class's documentation will say this.

## 5. Threads

Backward shift moves orders between slots during a removal, so a reader on another thread that looked at the table
part-way through could miss an order or see one twice. No other thread looks at the map:

- `IncrementalRehashMap` is used by one thread only. In builds with thread checks it records the thread that first uses
  it and stops any other, and `hand_over_ownership()` allows the one hand-over the order book makes, from the thread
  that builds it at start-up to the engine's thread, while no other thread is using it.
- The figures published as metrics, the number of open orders, the slots and whether a copy is under way, are read on
  the engine's own thread by `MatchingEngineThread::publish_book_metrics()` and copied into Prometheus gauges; the
  scrape reads the gauges, never the map.
- The one figure the map's allocation code shares with other threads, the largest allocation it has made, is an atomic
  that only the map's thread writes.
- The sequencer's `MemberSessions`, the map's other user, is likewise used on the sequencer's thread alone.

So the change adds no race, and needs no lock. Should a reader on another thread ever be wanted, it would need a design
of its own, whatever the method of removal: today's erased marks do not make the map safe to read from another thread
either, because inserts and the copy move orders too.

## 6. What it costs

A removal now does more than mark a slot: it looks at each slot after the gap until it meets an empty one, works out
the home slot of each order it meets, which means hashing its identity again, and moves some of them, each move copying
one slot of about 140 bytes. The order's record in the open-order region does not move; only the map's slot that
names it does.

How far the repair walks depends on how full the table is. The map is never more than half full of live orders, and
with the map reserved for 100 million it is far emptier than that for most of the day. At half full, Knuth's analysis
gives about 1.5 slots looked at on average for a successful search, and the repair walks a run of similar length. The
worst case is a long run of orders with nearby home slots, which a good hash makes rare, and which would slow today's
searches as much.

What it saves: the copy that clears erased slots, a second table of 37.8 GB and the work of moving every order into it,
never happens.

These are estimates. Section 8 says what will be measured.

## 7. Tests

Each test is to be made to fail on purpose, with the change undone or broken, before it is trusted.

- **The worked example** of section 2, slot by slot.
- **Wrap-around:** a run that crosses from the last slot to the first, with a removal on each side of the end.
- **No marks:** in a table not being copied, after any number of inserts and removals, the count of slots used equals
  the count of live orders, so no copy to clear marks can ever start.
- **Against a reference:** millions of random inserts and removals, with keys chosen to collide often, compared after
  every step with `std::unordered_map`: every key present is found, every key removed is not, and the sizes agree.
- **During a copy:** removals from both tables while a copy is under way, every remaining key found afterwards, and
  none lost from the table being copied out of.
- **Walking while removing:** removing every order by walking the map visits each order at least once and leaves the
  map empty.
- **One thread:** the existing thread-confinement test still stops a second thread from using the map.

## 8. What will be measured

On the development machine, with the order book's own benchmark (`order_book_size_bench` and `order_book_bench` in
`applications/matching_engine/performance`):

- the time of a removal, before and after, at several fillings of the table, including a burst of cancels of orders
  placed close together;
- the time of a search and an insert, before and after, which should not change;
- the map's resident memory over a long run of orders and cancels with the map reserved, recorded with `vmstat` and
  the engine's own figures, which should be flat from the start.

## 9. Decisions

Each is a separate yes-or-no question.

1. **Is backward-shift deletion used in every table of `IncrementalRehashMap` except one being copied out of, where a
   removal still marks the slot erased?** Recommended: yes. Section 3 gives the reason.
2. **Does `erase(iterator)` return the emptied slot again when an order has moved back into it, with the class's
   documentation saying that a walk which removes as it goes may visit an order twice when the repair wraps round the
   end of the table?** Recommended: yes. No code walks the map while removing from it (section 4).
3. **Is the change built and measured first, as section 8 describes, and kept only if a removal stays within a few
   hundred nanoseconds and the search and insert times do not change?** Recommended: yes. The figure to keep it by is
   Andrew's to set; a few hundred nanoseconds is a proposal.
