# Orders that outlive the trading day {#long_orders}

**Status: deferred, and out of scope.** Principle V2 in
[`design_principles.md`](../design_principles.md) makes every order a day order
and leaves orders that outlive the trading day, called long orders here, deliberately unimplemented: they
are business functionality that would bring out no new requirement of the library. This note records how
they would be built if they were ever brought into scope, so that the start of the trading day
([`trading_day_and_reference_data.md`](trading_day_and_reference_data.md)) leaves room for them and
nothing built now has to be undone. Nothing here is built or agreed.

## What exists today

Every order is a day order. Anything still open at the end of the trading day is to be cancelled, and its
member told, before end of day: the close cancels every order still open ([BUG-0132](../bug_list.md#bug_0132)). Good Till Cancel (GTC) and
Good Till Date (GTD) orders are recognised in parts of the order handling code, and are exempt from some
cancellation rules such as cancel on disconnect, but nothing carries them from one trading day to the
next. The agreed behaviour of long orders on a halt is in the bug list's
[recurring shapes](../bug_list.md#bug_list_shapes).

## The shape: re-entered at start of day as sequenced commands

Each trading day starts with an empty WAL, and that WAL holds every command the venue accepted that day
(principle V12). Long orders keep that true by being carried across days as an explicit input to the start
of the day: the venue loads the previous day's surviving long orders and re-enters them as commands in the
new day's WAL. The matching engine's book can then always be rebuilt from the new day's WAL alone, and no
WAL ever spans two days.

A relational database is the store across days. Its role is limited to holding the set of long orders to
be re-entered at the next start of day. During the trading day the WAL is the authority for accepted
orders, fills, cancels and replacements; the database is not consulted on each order and is not a second
live order book. This follows the rule that no venue component holds a database connection in order to do
its work: what is needed is read at the start, and the store is brought up to date from the venue's own
record afterwards.

## Keeping the database up to date without slowing order entry

A process here called the long-order projector follows the committed WAL and applies long-order changes
to the database. It is never called while an order is being processed.

1. A member sends a GTC or GTD order through the normal gateway and sequencer path. The sequencer records
   it in the WAL under the same commit rules as any other order. Its acknowledgement does not wait for a
   database transaction.
2. The projector follows the committed WAL. For a new long order it records the order and its terms. For a
   fill it updates the remaining quantity or marks the order complete. For a cancel or a replacement it
   removes or updates the old order and records any replacement. Day orders are not copied to the
   database.
3. In the same database transaction as each batch of changes, the projector records the WAL sequence
   number it has applied through. If it stops, its replacement asks for records after that position. A
   record read twice, because an acknowledgement was lost, is not applied twice, because the position and
   the changes are committed together. Batching reduces the cost of transactions and lets the projection
   fall further behind the WAL, but adds no delay to order entry.
4. At end of day, the venue establishes the final WAL position holding everything that can affect the
   surviving long orders. It does not archive that WAL, or consider the day complete, until the projector
   has committed through that position. The rows at that position are the long orders to be re-entered the
   next day.
5. At start of day, the set is loaded and its trading date and identity checked. GTD orders whose expiry
   has passed are cancelled with sequenced cancel records and the member reports that go with them.
6. The remaining long orders are re-entered through the sequencer as commands in the new day's WAL. The
   venue enters pre-open only once the matching engine has applied them, as well as the day's reference
   data.

If the database or the projector is unavailable at end of day, the venue keeps the WAL and does not start
a new day from a set that is stale or incomplete. What happens when the projection has not caught up by
the time the next day is due to start (how operators are alerted, whether the venue stays closed, and how
the work resumes) belongs to the operations design.

The projector needs its own restart and failover design. If it runs as a primary and a secondary, only one
may write to the database at a time, and promotion must fence the old writer before the new one resumes
from the last recorded position. A single projector needs the same restart from its recorded position.
Its failure can delay the end of day, but cannot lose or reorder the committed WAL records from which the
projection can always be made again.

For each set of long orders, the database records the trading day it came from, the trading day it is for,
the final WAL position, the number of orders and a digest. The previous verified set is kept until a new
one is complete, so that an interrupted end of day can be diagnosed and retried, but the venue must never
use the previous day's set without saying so.

## Durability: the same as any other order

A long order has exactly the same durability and acknowledgement guarantee as any other order. It enters
the same WAL under the same commit rule, with no database write before it is acknowledged. When the
venue's high availability rules allow trading without a follower that has caught up, a long order can be
lost in exactly the circumstances any other accepted order can. The WAL is the authority for the day; the
database holds what survives across days once it has applied the day's records. If the projector is
behind, end of day waits; the order's acknowledgement is never made to wait for the database instead.

## Start of day, with long orders

With long orders in scope, the start of the day gains one step, between loading the reference data and
entering pre-open:

1. The sequencer writes the start-of-day `TechnicalEvent` for the trading day.
2. The day's reference data is loaded, checked and sequenced.
3. The set of long orders for the day is loaded. Expired GTD orders are cancelled, and every other order is
   checked against the day's instrument data and price limits.
4. The orders that pass are re-entered through the sequencer as commands in the new day's WAL.
5. The sequencer waits until the matching engine has applied everything up to the last of them.
6. The sequencer writes the `TechnicalEvent` that enters pre-open.

The process that loads long orders need not be the one that loads reference data; they are different data
with different owners.

## Decisions these orders would need

- **Expiry of a GTD order:** the time zone, and the exact point at which it expires, and whether it expires
  before the next day's book is made available.
- **Re-entry:** whether a re-entered order keeps its order identifier, what priority it has in the new
  book, and what report tells the member it was restored.
- **Checks against the new day:** whether price, quantity, instrument status, tick size and price limits are
  checked again against the new day's data, and what happens to an order that fails.
- **Re-entering an order twice:** how a retried start of day, or a change of leading sequencer during it,
  avoids entering the same order twice. Each order in the set needs a stable identity, and each command
  that re-enters one must be recognisable as doing so.
- **Replay:** the exact set of long orders a day started with must be archived with that day's WAL and
  reference data. The day's WAL alone does not reproduce the book it started with if that set is lost or
  later changed.
- **End of day:** [BUG-0132](../bug_list.md#bug_0132) would cancel day orders only, and keep long orders for
  the database. The existing handling of long orders on disconnect and on a halt is related, but does not
  settle the end of the day or the start of the next.

## Related design

- [`design_principles.md`](../design_principles.md) — principle V2, which leaves long orders
  out of scope, and V12, every trading day starts fresh.
- [`trading_day_and_reference_data.md`](trading_day_and_reference_data.md) — the start of the trading day,
  which leaves room for this step.
- [`trading_phases.md`](trading_phases.md) — the phases, and the open question of what members may do
  after the close.
