# Recovering the open orders after a process restart {#open_order_checkpoint}

Design note, 2026-08-30.

**What is built:** the region itself, as `pubsub_itc_fw::MappedSlotStore`, and the
matching engine's book on top of it, as `matching_engine::OrderBook` — so an
open order is written to the region as it is accepted and taken out of it as it
is cancelled. **What is not:** reading the region back at startup, which is what
turns the record into a recovery, and the warming and halt behaviour below.

The matching engine's set of open orders exists only in the running process. When
the process dies they are gone: measured on 2026-08-29 with a thousand orders
open, the engine was killed and restarted and came back holding none of them,
while the gateway went on reporting them as open and the member went on believing
they were live. A later cancel for any of them is refused as an order the venue
does not recognise.

The requirement is R-0018 in the functional specification: an order open before
the engine restarted shall be open afterwards, on the same terms, and shall
remain cancellable by the member that placed it. R-0073 requires the same of a
failover, R-0102 of a checkpoint that cannot be trusted, and R-0108 that the work
a recovery must do is bounded. All four wait on this.

## Why the write-ahead log does not serve

It looks like the obvious source and it is not, for two independent reasons.

**It is truncated.** `Wal::take_snapshot()` writes a 48-byte header — magic,
version, last sequence number, record count, segment, offset, checksum — and
then unlinks every segment before the current one. The snapshot holds no state;
it is a truncation anchor. In a running system the retained log was two 4 MB
segments against a recorded count of about fifty million records.

**And replay is too slow even with full retention.** At forty million orders in a
trading day and one microsecond an order, a full replay is forty seconds. The
failure being recovered from is a process restart on a healthy machine, which the
supervisor completes in about two and a half seconds. A forty-second recovery
behind a two-second restart is not a recovery.

So the quantity that matters is **the number of orders currently open**, not the
number the day has seen, and the engine needs something of its own.

## Constraints

| | |
|---|---|
| (a) | The engine's thread must not block. No walk of the book, no wait on a disk, no lock, on the hot path |
| (b) | No heap allocation on the hot path |
| (c) | The pool allocator must not be changed for this. It chains slabs, hands out raw pointers, and is used by every component |
| (d) | Recovery must be proportional to the orders open, not to the day's volume |
| (e) | Process death is what must be survived. Machine death and power loss are what the second machine is for, and the specification says so |
| (f) | What is written must be usable by a successor that maps it at a different address |
| (g) | Custom allocators are preferred over ring buffers |

## What was ruled out

| Approach | Why not |
|---|---|
| Replay the sequencer's log | 40 s at realistic volume, and the log is truncated |
| Serialise the book periodically on the engine's thread | Walks the whole book; breaks (a) |
| Serialise it on another thread | Needs a lock or a full copy of a mutating structure; breaks (a) or (b) |
| Back the pool allocator with a mapped file | Breaks (c): chained slabs at arbitrary addresses, absolute pointers, and a persistence concern in the wrong class |
| Append-only record of book changes | Breaks (d): one record per accept and one per removal is proportional to the day |

## The design

**A memory-mapped region of fixed-size slots, one per open order**, separate from
the engine's own containers and from the pool allocator.

The region itself is `pubsub_itc_fw::MappedSlotStore`, which knows nothing about
orders: it holds equally sized records, hands out and takes back slots by index,
carries the published position, and refuses a file written with a different
record size or slot count. What a record contains is the matching engine's
business, described below.

### The slot

Holds the order's identity, the session that placed it, its terms, a live flag,
and **the sequence number of the order that put it there**. That last field is
what lets recovery tell a slot written before the published position from one
written after it; without it a slot written just before the process died would be
found by the scan *and* delivered again by the sequencer's tail, producing a
duplicate — which is worse than the loss it replaces, because it is silent.

### The form of the data

The region is a header followed by an array of fixed-size slots. Nothing else.

**The header**, at offset zero:

| Field | Purpose |
|---|---|
| magic, version | so a region from another build is refused rather than misread |
| slot size, slot count | so the successor can find slot *i* without agreeing anything else |
| published sequence number | 8 bytes, aligned. The only field written on the hot path |
| free-list head | the index of the first free slot, or a sentinel |

**A slot** holds one open order, and it is the same size whether it is live or
free:

| Field | Notes |
|---|---|
| state | free or live |
| sequence number | the order that put it here. What recovery compares against the published position |
| session | the protocol and comp id, as an inline fixed array of 64 characters plus a length |
| order identifier | the member's ClOrdID, inline, 64 characters plus a length |
| terms | side, order type, quantity, symbol, time in force, expiry |
| venue order number | the identity the venue gave it |
| next free index | when the slot is free, overlaying the payload |

**Every field is a value or an inline fixed-size array.** No pointer, no
`std::string`, no container, no reference to anything outside the slot. That is
what makes the region readable at whatever address it is mapped at, and it is why
the live structures cannot simply be persisted as they stand: the engine's book
is an `IncrementalRehashMap` whose entries reach each other by address.

Slot *i* is at `header_size + i * slot_size`. Index arithmetic and nothing else.

**Indicative size.** With today's limits — 64 characters each for comp id, order
identifier and symbol, 32 for quantity — a slot is roughly 270 bytes. A region
for a million simultaneously open orders is therefore around 320 MB mapped, which
is not resident unless touched. Those limits are generous rather than measured,
and the record can be tightened if the sizing turns out to matter.

### What is deliberately not in it

**The identifiers used earlier in the day.** The venue must refuse an order
identifier already used on that session that day, whatever became of the first
order, because the activity it publishes is identified downstream by that value
and two orders under one name cannot be read correctly by anyone. That set grows
with the day's volume rather than with what is open, so holding it here would
break the recovery bound this whole design exists for.

It belongs with the sequencer, which commits every order and every cancellation
and therefore has the facts already. The engine consults it only when refusing,
which is rare and not latency-critical.

### On the hot path

**accept**

1. write the slot's contents
2. set the live flag
3. emit the execution report
4. publish "this region is current to sequence N"

**remove**: clear the live flag, emit the report, publish.

Both are constant time: one copy of an order's terms and two stores. Nothing
walks, nothing waits, nothing locks.

**The publish comes last, and that ordering is the whole of the safety.** A death
anywhere before step 4 leaves the order above the published position, so recovery
discards the slot and the sequencer's tail re-runs the accept — rebuilding the
slot and emitting the report. Nothing is lost.

The price is that a death between 3 and 4 sends the report twice. That is the
right way round: a report a member never receives cannot be recovered by the
member, and one it receives twice can, provided it can tell. R-0122 requires the
repeat to be marked. Publishing before emitting would trade the duplicate for a
silent loss, where the venue holds an order the member was never told about.

### One copy of the order, not two

The engine's map from order identity to order becomes **a map to a slot index**,
and the region holds the only copy of the order itself.

The alternative — a region alongside the existing map of entries — writes every
order twice on the accept path and allows the two to disagree, which is a class
of defect with no upper bound on how confusing it gets.

The cost is an indirection on lookup: the map gives an index, and the order is at
`header_size + index * slot_size`, which on a region of a few hundred megabytes
is likely a cache miss. Both paths that pay it — the duplicate check on accept,
and finding an order to cancel — can afford it.

It does mean changing the engine's existing structures rather than adding a
region beside them. That is more work and it is the right kind.

The published position is a single aligned 64-bit value. A store to it does not
tear, so there is no partial publish to detect. **It is written on the engine's
thread and must stay there** — it is what defines the consistency point, and
moving it to another thread makes the position lag by an arbitrary amount, which
lengthens the tail and makes R-0108's bound harder to hold.

### The follower keeps one too

The passive instance maintains its replica of the book in a region of its own, on its own
file, exactly as the leader does. Its updates arrive as BookUpdate messages rather than as
orders, and it publishes each one as it applies it -- it sends nothing outward, so there is
no report for a publish to get ahead of.

This is decided rather than incidental. A follower without a region is a follower that has
to be handed the whole book again every time it restarts, which is the same recovery problem
one process along; with a region it comes back holding what it held. It costs one region per
instance and no work on any path that matters, because the follower's path is not the
latency-sensitive one.

The two regions are separate files and neither reads the other's. They are not a shared
store and must not become one: what makes the second machine the answer to a lost region is
that it is a different machine with a different copy.

### Addressing

Everything in the region is referred to by slot index, never by address —
including the free list, which lives in the region. Where the mapping lands on
restart therefore does not matter. This is the property that mapping the pool
could not provide.

### Recovery

1. Map the region and read the published position, N.
2. Scan for live slots, **ignoring any whose sequence number is above N**.
3. Rebuild the engine's ordinary in-memory structures from those slots, allocated
   as they are today. A slot stamped with sequence number zero is not live work:
   a new region publishes zero, so trusting such a slot would recover a record
   nothing had settled.
4. **Rebuild the free list from the complement of the live set.** The on-region
   free list is never trusted: it is mutated on every accept and removal, so a
   crash can leave it holding a dangling index or a cycle. The scan is already
   being paid for, so the complement is free.
5. Ask the sequencer for everything committed after N and apply it silently,
   which is the existing reconciliation path.

Step 2 is O(region size), and the region is sized to the peak open orders, which
is the bound (d) asks for.

**Touching the whole region first.** Mapping a file reserves addresses and reads
nothing, so the delay of fetching each page falls on whoever touches it first.
The engine touches every page before it reports itself ready, which is R-0121;
the region provides `warm()` for it. After a recovery the scan has already
touched everything, so the cost is paid either way and the only question is when.

### Crash consistency

There is no `fsync` and no `msync` anywhere on the hot path, and none is needed.
The region does not have to be consistent at every instant — only at the points
the engine published, and everything after the last of those is re-derived from
the sequencer. A torn slot is never live, because the live flag is set after the
contents; a slot marked live but not published is discarded by step 2; and a slot
freed but not published is re-freed by the tail.

**This survives the process dying and not the machine dying**, because dirty
pages sit in the page cache rather than on disk. That is the guarantee the
specification asks for and not a compromise: `docs/book` section 4.3.3 states
that lost or damaged durable state is where the disabled configuration is weak,
and that the enabled configuration's answer to it is the other machine.

## When the region cannot be used

Absent, damaged, or written by a different build: it is treated as no region at
all, under R-0102. A region whose header does not describe what the engine is
configured to read is refused outright rather than read as though it did, because
reading it the wrong way round produces orders that are wrong in ways nothing
downstream can detect. The engine then cannot name what it held, and cannot cancel
what it cannot name.

**The fallback is the sequencer's record.** It holds every order and every
cancellation, so what was open can be established by replaying it — the slow path
rejected above for ordinary recovery. Here it is worth the time: it happens once,
it is not the normal case, and what it buys is every member being told what
became of its orders. Cancel each, report each, halt. That is R-0123.

If that record cannot supply it either, the venue halts and says it cannot
account for what it was holding, which is the last resort and still better than
falling silent.

## When the region is full

**Refuse the order.** The alternatives are worse:

| Policy | Cost | What it does to a member |
|---|---|---|
| Refuse | Constant | A rejection it can act on and retry |
| Grow | Allocation, and possibly a page-fault burst on the hot path | Nothing, until the growth breaks (a) |
| Recycle the oldest slot | Constant | Silently loses an order the venue accepted |

The third is the one to be sure of rejecting. It trades a visible refusal for an
invisible loss, which is the exchange the whole availability chapter exists to
prevent.

**What the member is told.** An execution report with ExecType and OrdStatus of
Rejected, and OrdRejReason of Other, because none of the specific reasons fits:
nothing is wrong with the order, and nothing is wrong with the member that sent
it. The text says what is — that the venue is holding as many open orders as it
can — because a reason code of Other on its own leaves a member looking for a
fault of its own that it will not find.

## What this changes for the availability design

Section 11e of [design_notes.md](../availability/design_notes.md) records three
defects that had one shape between them: a rule that held while the epoch was
ephemeral, and stopped holding once it survived a restart. It says a fourth of the
same kind is likely. This is a second piece of state that now survives a restart,
so it is worth saying which rules it touches before one of them turns out to be
the fourth.

**A restarted engine no longer comes back empty.** Section 11 argues that a
restarted process comes back as a follower, and one of its two consequences is
that "resuming leadership must wait for reconciliation, not for the decision. A
restarted process has lost its state; that is why it restarted." With the region
that premise is no longer true: a restarted engine comes back holding the orders
it held. The conclusion still stands and for the better reason -- the region is
current only to its published position, so the tail after it still has to be
replayed before the engine can lead -- but the reason has changed, and the amount
of replaying is now bounded by the gap rather than by the whole day.

**Two instances now hold two regions and they may disagree.** That is correct
rather than a fault: each region records what that instance actually holds, which
is the only thing an instance can honestly write down. The follower's region is
what it replicated; the leader's is what it accepted. Promotion reads the promoted
instance's own region and then reconciles, exactly as before.

**Primary and secondary are permanent names; leader and follower are positions.**
The region is fixed to the instance, so it follows the permanent name: each
process has one file, named in its own configuration, and neither reads the
other's. A region is not a shared store and must not become one -- what makes the
second machine the answer to a lost region is that it is a different machine with
a different copy.

**The grace period gets longer.** Section 11 says the follower's wait before
promoting is measured rather than chosen, and equals what a supervised restart
actually takes plus a margin. Reading a region back adds to that; the figure is
below.

## What it costs

Measured 2026-08-30 by `applications/matching_engine/performance/order_book_bench`,
a million orders accepted and then cancelled, one record of 168 bytes each, so a
region of 160 MB. Figures in nanoseconds a call, and each carries about 35 ns of
clock overhead, which the harness reports so it can be read off. Three runs, and
they agreed to within a few per cent.

| Operation | The book as it was | The book as it is |
|---|---|---|
| accept, p50 | 74 | 53 |
| accept, p90 | 735 | 197 |
| accept, p99 | 1370 | 853 |
| accept, mean | 214 | 135 |
| cancel, p50 | 143 | 138 |
| cancel, p99 | 300 | 293 |

**Writing the record costs nothing; it pays for itself.** The accept path is
faster with the region than without it, which is not what one would guess from
"one more store per order". The reason is that the map no longer holds the orders:
it holds a four-byte index where it used to hold a 168-byte order, so its table is
a fortieth of the size, and the incremental migration that runs alongside every
insert moves a fortieth as much. That saving is larger than a 168-byte copy and an
aligned store, and the p90 is where it shows: 197 ns against 735.

The cancel path is unchanged within the noise, which is what it should be: it did
a lookup and a copy before, and it does a lookup and a copy now.

**Cold pages cost 8 microseconds each, and that is what the warming is for.**
Touching every page of a freshly created region takes about 0.5 ms, because the
file is all holes and there is nothing on the disk to read. A region left behind by
a process that died is another matter: written, pushed to the disk, and dropped
from the page cache, the same walk takes **338 ms, or 8.2 microseconds a page over
41,015 pages**. That is a disk read per page, and it is exactly the cost R-0121
exists to keep off the order path. Left unwarmed it would land on whichever orders
happened to touch each page first.

**Two consequences.**

- The 338 ms is part of what a restart takes, so it belongs in the grace period the
  follower waits before promoting -- which section 11 of
  [design_notes.md](../availability/design_notes.md) says is measured rather than
  chosen. It scales with the size of the region and not with the orders in it, so a
  region sized for a million orders costs that whether one order was open or a
  million.
- A recovery scan touches every page anyway, so after a recovery the walk is not an
  extra cost, only an earlier one.

## Still to be decided or measured

- **How the region is sized.** It must hold the peak simultaneously open orders,
  which nobody has measured. It is configured rather than derived, and the
  environment templates carry the figure; a million is what development uses. The
  measurement above gives the price of guessing high: 168 bytes and 8 microseconds
  of restart for each record never used.
- **Whether the kernel's background flushing** produces write bursts a
  latency-sensitive process can see. That is a measurement, and the trading-day run
  is where it would show.

## See also

- [wal.md](wal.md) — the log this deliberately does not rebuild from
- [Availability](../availability/README.md) — the failures this serves
- `docs/book` — R-0018, R-0073, R-0102 and R-0108, which wait on it

---

Back to [Durability](../durability/README.md).
