# Recovering the open orders after a process restart {#open_order_checkpoint}

Design note, 2026-08-30.

**All of this is built.** The region itself, as `pubsub_itc_fw::MappedSlotStore`;
the matching engine's book on top of it, as `matching_engine::OrderBook`, so an
open order is written to the region as it is accepted and taken out of it as it is
cancelled; reading the region back at startup; the warming; the absence rule; and
what happens when the region cannot be used at all.

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

**Size, as built.** A record is **232 bytes** and a slot is 248 with the store's
own header, so a region for a million simultaneously open orders is **248 MB**
mapped, which is not resident unless touched. Most of it is the two identifiers:
64 characters for the comp id and 64 for the order identifier, both generous
rather than measured, and both able to be tightened if the sizing turns out to
matter.

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

## When the venue was unable to match for too long

An order is not dangerous because it is old. An order that rested all morning in a
working market is fine: its owner could have cancelled it at any moment and chose
not to. What makes an order dangerous is that its owner was **locked out of it** —
the venue could not match, so the member could not cancel, and the market moved
while it could do nothing. The order is now priced for a market that has gone, and
its owner was denied every opportunity to act.

So the period that matters is the length of the outage, not the age of the order,
and not whether the order was recoverable: an order correctly recovered from the
region, whose member was locked out of it for ten minutes, is exactly as dangerous
as one that could not be recovered at all.

**How the engine knows.** The region carries a wall-clock stamp saying when its
owner was last able to match, written by a timer once a second. A successor
subtracts it from the current time. That is the whole absence — noticing the
death, restarting the process and rebuilding the book are all terms in it — and
the engine works it out alone, because nothing in the availability design may
depend on a supervisor being present.

**The stamp is written by a timer and never by the order path.** A book that is
idle because nothing is being traded is not a book nobody is tending. Stamping it
on orders would read a ten-minute lull followed by a two-second restart as a
twelve-minute absence, and cancel every member's orders for having been quiet.
That is R-0118, and there is a test that a record write does not stamp it.

**What happens.** Longer than the stated period, with orders open: cancel each,
tell the member that placed it, halt. Longer than the stated period with nothing
open: resume without halting, because nothing went stale, there is nobody to tell,
and halting would buy an outage. That is R-0117.

The period is `order_book.absence_limit_seconds`, five minutes in the environment
templates. It is a trading decision rather than a measurement: it says how long a
member's resting order stays meaningful in this market.

**A halted venue refuses orders and says why**, in the reply to the order the
member sent — `ExchangeClosed`, with text saying trading is halted. That is the
answer to the question the member actually asked, and it reaches the member who is
trading, which is the member who needs to know. A broadcast should exist as well
and does not replace it; that part is not built. Lifting a halt needs a person,
under R-0023.

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

**But that record is truncated as it is consumed**, so a replay from the
beginning starts wherever truncation left off rather than at the start of the day.
The two cases have to be told apart, and `MePositionAck` carries the earliest
record the sequencer still holds so that they can be:

| What the sequencer still holds | What the engine does |
|---|---|
| The first record the venue took | Cancel each order, report each cancellation, halt |
| Only a later part of the day | Halt without cancelling, saying it cannot account for what it was holding |

The second is the last resort and is still better than falling silent. **It does
not cancel**, and that is deliberate: cancelling only the orders it can name would
leave every earlier one unmentioned, which is the outcome R-0020 exists to
prevent, arrived at while appearing to have done something about it.

**The region itself is kept.** It is moved aside rather than overwritten, so that
whatever made it unreadable can still be looked at.

## Changing how many orders it holds

**Clear the region when you change `order_book_region_capacity`, or the venue will halt
rather than resize.** The record count is in the region's header, and a header that does not
describe what the engine is configured to read is refused --- which is right, and is what
stops a region being reinterpreted at the wrong stride. But a deliberate resize looks
identical to a damaged region from inside `open()`, so the engine moves the old one aside,
finds it cannot say what it held, and halts under R-0123.

That is the correct answer to the question it is actually being asked. It is not the answer
anybody wants from a resize, so the resize is: stop the engine, delete the region, change the
figure, start it. Any orders open at the time are gone, which is why this is a change to make
when nothing is resting rather than during a trading day.

**How big it needs to be** depends on what removes orders from the book, which is worth
saying plainly because a load profile can make it look far worse than it is. This venue's
matching engine does no matching, so nothing removes a resting order except a cancel. Under
`profiles/trading_day.toml`, whose cancel ratio is deliberately a memory dial rather than a
realistic figure, ninety per cent of every order sent rests for ever: a region sized for a
million filled after eleven minutes on 2026-08-31 and the venue refused 8.8 million orders
after that. Correctly --- but it says nothing about a venue that fills orders.

So size it to the peak orders open at once, and be clear about which of the two situations
you are sizing for.

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

**Cancelling everything on a promotion has stopped.** It was the chosen baseline
in [wal_and_ha.md](../availability/wal_and_ha.md): a promoted engine issued a
cancel for every order on the reconciled book, and members resubmitted. That was
right while the book could not survive the process holding it. The region is what
changed it. The correctness rule in that document already said what follows --
"any order still on the reconciled book is genuinely outstanding" -- and the only
reason to cancel one anyway was that the engine could not vouch for the book it
had reconstructed. It can now: its own region says what it held and to what
position, and the sequencer's tail supplies the rest.

So the ordinary path keeps the book, and cancelling everything becomes the answer
where the region has failed. That is the same answer as before, moved to the case
that still needs it.


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
a million orders accepted and then cancelled, one record of 232 bytes each, so a
region of 221 MB. Figures in nanoseconds a call, and each carries about 10 ns of
clock overhead, which the harness reports so it can be read off.

| Operation | The book as it was | The book as it is |
|---|---|---|
| accept, p50 | 76 | 57 |
| accept, p90 | 772 | 209 |
| accept, p99 | 1487 | 892 |
| accept, mean | 243 | 140 |
| cancel, p50 | 147 | 139 |
| cancel, p99 | 304 | 295 |

**Writing the record costs nothing; it pays for itself.** The accept path is
faster with the region than without it, which is not what one would guess from
"one more store per order". The reason is that the map no longer holds the orders:
it holds a four-byte index where it used to hold a 232-byte order, so its table is
a sixtieth of the size, and the incremental migration that runs alongside every
insert moves a sixtieth as much. That saving is larger than a 232-byte copy and an
aligned store, and the p90 is where it shows: 209 ns against 772.

The cancel path is unchanged within the noise, which is what it should be: it did
a lookup and a copy before, and it does a lookup and a copy now.

**A restart on a healthy machine costs about 56 milliseconds.** Measured in
`scripts/ha_test.py` scenario 50: a thousand open orders recovered from a 248 MB
region in 56 ms. The pages the dead process wrote are still in the page cache,
which is the case a process restart on a live machine actually presents.

**When the cache has given them up it costs 457 milliseconds.** Written, pushed
to the disk, and dropped from the page cache, the same walk takes 457 ms, or
**8.1 microseconds a page over 56,640 pages** — a disk read per page. That is what
R-0121's warming exists to keep off the order path. Touching every page of a
region that was freshly created costs 0.7 ms instead, because the file is all
holes and there is nothing to read.

**Two consequences.**

- The restart cost belongs in the grace period the follower waits before
  promoting, which section 11 of
  [design_notes.md](../availability/design_notes.md) says is measured rather than
  chosen. 56 ms is the ordinary case and 457 ms the worst, and both scale with the
  size of the region rather than with the orders in it: a region sized for a
  million orders costs that whether one order was open or a million.
- A recovery scan touches every page anyway, so after a recovery the warming is
  not an extra cost, only an earlier one. The engine warms only where there was
  nothing to recover.

## Still to be decided or measured

- **How the region is sized.** It must hold the peak simultaneously open orders,
  which nobody has measured. It is configured rather than derived, and the
  environment templates carry the figure; a million is what development uses. The
  measurement above gives the price of guessing high: 232 bytes and, when the page
  cache has gone cold, about 8 microseconds of restart for every 17 records never
  used.
- **Whether the kernel's background flushing** produces write bursts a
  latency-sensitive process can see. That is a measurement, and the trading-day run
  is where it would show.

## See also

- [wal.md](wal.md) — the log this deliberately does not rebuild from
- [Availability](../availability/README.md) — the failures this serves
- `docs/book` — R-0018, R-0073, R-0102 and R-0108, which wait on it

---

Back to [Durability](../durability/README.md).
