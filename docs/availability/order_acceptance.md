# Refusing orders the venue cannot process {#ha_order_acceptance}

While no matching engine leads, the venue holds the orders it receives for as long as a normal change of
engine leader takes, and then, if none leads yet, it refuses new orders and cancels with a reply saying why.
It starts accepting again on its own when a matching engine leads, unless trading is halted, which only a person
lifts. `ha_test.py` scenarios 42 and 92 check it.

## The condition

When the leading sequencer has no matching engine that leads to send an order to, it writes the order to its
WAL and holds it. It sends no engine the order, not even one that follows the WAL, until an engine leads; that
engine is caught up to the position released before the first held order, and is then sent the held orders
live, in order, so that it answers each with a first report
([voided_commands.md](voided_commands.md), section 5.1). That is the right policy for a change of engine
leader, which happens on every failover and whose length is known.

**It is cheap for the venue and expensive for the member.** A held order costs the venue nothing beyond its
record in the WAL. The member is told nothing at all: the execution report is the matching engine's to send,
and no engine leads, so an order placed while orders are held gets no reply. The member cannot tell a held
order from a slow one or a lost one, cannot cancel it, because a cancel needs a matching engine too, and has
to assume it may be live, because it may be. Every second of holding widens the gap between what the member
believes and what is true.

So the limit below does not protect the venue's memory. **It bounds how far a member's picture of its own
position may drift from the truth** before the venue starts saying no.

## When holding becomes refusing

The venue stops accepting new orders once no matching engine has led for `[recovery]
orders_held_at_most_seconds`, 5 seconds in every environment. Both sequencers of the pair must have the same
figure, so it is declared once, in the environment's `[shared]` section.

- **It is the longest normal change of engine leader, with a margin.** When the leading engine dies, the
  other can take the lead once the dead engine's lease has run out. The dead engine may have renewed its
  lease just before it died, so the gap is at most one renewal interval, plus the lease period, plus the
  drift allowance: 4.25 seconds with the lease timings every environment uses (1,000, 3,000 and 250
  milliseconds), plus the new leader's catch-up, which is a few records. Holding orders across that gap is
  right, because it happens on every failover, its length is known, and it can be stated to members.
- **Time, not a count of orders,** because the member's exposure is measured in time. At the highest rate
  measured on this venue, about 34,500 orders a second, five seconds of orders is about 172,500, which the
  venue holds in its WAL without difficulty, so a count would never be the first limit reached.
- **The time is counted from when no engine took orders, not from the first order held.** So an order that
  arrives after five seconds without an engine is refused at once by the gateway, however few orders came
  before it.

The sequencer evaluates the limit when an order arrives and on its engine absence timer, about once a
second, so the gateways are told within a second of the limit passing whether or not any order arrives.

**Refusing is not halting.** It is automatic, and it ends by itself when an engine leads. The orders already
held stay held, and are released to the engine that leads, which applies them. Halting trading is a separate
rule, the limit on automatic recovery ([far_back_catch_up.md](far_back_catch_up.md), section 5). If the venue
halts while orders are held, the sequencer voids them, and the engine that next leads rejects each, saying
trading halted while it was waiting ([voided_commands.md](voided_commands.md), section 5.3).

## What the venue does

### The sequencer reports the condition, not each order

A held order is not logged on its own. The sequencer notes when no engine took orders (on a monotonic clock,
so a change to the time of day cannot change how long it believes it has been without one), counts the
orders and cancels that arrive meanwhile, and logs a Warning every five seconds naming the count and the
time. When an engine takes orders again it logs one line saying how long none did and how many orders were
held, at the moment the engine is caught up or announces that it leads, rather than when the next order
arrives. A recovery while nothing is trading is therefore still reported.

```
no matching engine takes orders -- every order and cancel is held, in the WAL, until one leads, and is then released to it. After 5s without one, the gateways are told to refuse new orders
no longer accepting orders -- no matching engine has led for 5s, the longest orders are held (41 order(s) and cancel(s) held) ...
still no matching engine takes orders after 10s -- 41 order(s) and cancel(s) held so far
a matching engine takes orders again after 12650 ms without one -- the 41 order(s) and cancel(s) held meanwhile are released to it
```

When a test started the sequencer, the last of these is also sent as the test event `EngineTakesOrdersAgain`,
which says whether an engine had led before, or the venue was starting. `ha_test.py` logs each at the end of
every scenario, so that the limit can be compared with the changes of engine leader the scenarios make.

### The sequencer tells the gateways: `OrderAcceptance` (127)

The leading sequencer sends `OrderAcceptance` to every gateway it holds a connection to: when
acceptance changes in either direction, every five seconds while the venue is not accepting, and to
a gateway at the moment it connects, so that a gateway connecting during an outage learns the state
rather than assuming all is well. The message carries whether the venue is accepting, how many orders
are held and for how long no engine has led, and the epoch of the leadership that sent it. A gateway that
sees an epoch higher than any before knows a new instance leads, and sends it again every command still
unanswered ([Commands during a change of leader](commands_during_a_change_of_leader.md)).

**Only the leader sends it.** A gateway holds a connection to both sequencers and cannot tell which
leads. A follower sends nothing to a matching engine, so it holds nothing for one, and if it answered it
would say "accepting" on a venue with no matching engine at all. Two things follow, both in
`SequencerThread::on_role_changed`:

- **An instance that takes the lead sends `OrderAcceptance` at once,** because the gateways may be
  holding what the previous leader said before it died, and silence would leave a refusal in place
  that nothing would lift.
- **An instance that becomes a follower clears its record of the time without an engine,** so that if it
  leads again it does not come back already refusing, with a time measured from an outage that ended long
  ago.

### The gateways refuse, and say why

While the venue is not accepting:

- **A new order** is answered with an `ExecutionReport` with `ExecType` and `OrdStatus` Rejected
  (8), `OrdRejReason` 99 and the text *"Venue is not accepting orders: no matching engine
  available"*. That is an ordinary outcome for an order, which a member already handles, and its
  risk systems see the order as dead rather than pending.
- **A cancel** is answered with an `OrderCancelReject` saying the order is still open, with the text
  *"Venue cannot process cancels: no matching engine available. The order is unchanged"*, as every
  cancel refusal is (R-0151). Cancels must be refused too: a cancel needs a matching engine exactly
  as an order does, and a member believing it had cancelled would be more dangerously wrong than one
  believing it had traded.

While trading is halted the reason differs, and so does what is refused. With `RecoveryTooLong`, halted because no
engine could be trading within the longest recovery the venue does by itself and none trades yet, new orders and
cancels are refused, saying the venue is halted while it recovers. With `Halted`, halted while an engine leads, only
new orders are refused, with the text *"Venue is halted: new orders are refused until the halt is lifted; cancels are
carried out"*, and cancels are passed on, because the engine carries them out while trading is halted. A new order
refused for either is given `OrdRejReason` 2, exchange closed, as the matching engine gives one it refuses because
trading is halted. [Sequencer](../venue/sequencer.md) has the table of which reason is sent when.

Both gateways refuse the same way, with the same texts. *Considered and rejected: a
`BusinessMessageReject`,* which many members would read as a fault in their message rather than an
outcome for the order.

**Each refusal is logged at Debug.** The change of state is logged once at Warning, and the running
counts are on the gateway's health line every five seconds. A line per refused order would flood the
log with exactly what the health line already says.

### The gateway's health line keeps reporting when nothing moves

The FIX gateway's `GW-PROGRESS` line is written every 1,000 orders accounted for and also on a timer, every five
seconds, so it keeps reporting when nothing is progressing. A line written only as orders are
accounted for cannot report that they have stopped being accounted for.

```
GW-PROGRESS accounted=8 sent=2 dropped=0 nos_received=15 awaiting=7 refused=6 refused_cancels=2
```

- **`awaiting`** is the number of orders taken from members for which no execution report has been
  accounted: orders accepted and going nowhere.
- **`refused`** orders count as accounted, because the member has been told they are dead.
  **`refused_cancels`** do not, because a cancel was never counted in `nos_received`, and including
  it would drive `awaiting` below zero.
- In the example, `awaiting=7` is the seven orders held before refusal began. They are in the
  WAL and pending, and they are answered when an engine leads. Held and refused are different
  states, and only the first is still to be resolved.

The timer ticks at a fifth of the interval and writes the line when five seconds have passed since
the last one, so a busy gateway is not reported twice and the interval is kept. The fields may be
added to at the end and never reordered, because `ha_test.py` and `perf_run.py` read the line.

## Resuming

**The venue starts accepting again on its own when a matching engine leads.** This is about the
venue's own capacity, which is not a matter of interpretation: there is no matching engine, and
accepting orders is the harm.

*Considered and rejected: requiring an operator to re-enable acceptance.* It would stop a flapping
matching engine reopening the venue repeatedly, but it makes recovery depend on someone watching,
and the failure this design answers is one in which nobody noticed for seven minutes.

Resuming without a person is right **while nothing has been lost.** Once orders have been lost, a
venue that reopens quietly hides the damage rather than recovering from it; that case belongs to a
declared halt, which does not lift by itself ([design notes, section 15](design_notes.md#ha_recovery_ends_at_loss)).
A halt, whether for a recovery that took too long, for an engine that cannot account for the orders the venue holds,
or declared by an operator, stays until an operator lifts it with `scripts/operator_halt.py`, and the leading
sequencer lifts it only once an engine leads and every order and cancel written before the halt has been answered
([voided_commands.md](voided_commands.md), section 6.2).

## What this does not solve

- **A matching engine that is connected but not working.** Everything here depends on the
  connection. An engine that takes orders and does nothing with them looks healthy throughout
  ([BUG-0194](../bug_list.md#bug_0194)).
- **A matching engine that keeps failing and returning.** Each time an engine leads, the time without
  one ends, and the next failure starts it again, so a venue whose engine flaps may never reach the limit
  and never stop accepting ([BUG-0066](../bug_list.md#bug_0066)).
- **Telling the member when acceptance resumes.** Nothing announces it; a member finds out by sending
  an order that is not refused.
- **Orders lost at a change of *sequencer* leader,** which is a different failure; see
  [change_of_sequencer_leader.md](change_of_sequencer_leader.md).

## Tests

`ha_test.py` scenario 42, `order_refusal`, kills the matching engine and does not restart it, and
asserts five things in order: the first order is held and not answered; orders are refused once
no engine has led for longer than a change of leader takes; cancels are refused too; the health line
keeps reporting while nothing progresses; and acceptance resumes, with no operator action, when an
engine leads. The first assertion matters as much as the others: a venue that refused from the
first order would pass a test of refusal alone while refusing orders during every routine failover.

Scenario 92, `orders_held_then_refused_without_an_engine`, kills both engines of the pair, places
orders, and goes on placing them until the gateway refuses one. It requires every order placed before the
refusal to be answered by the engine that then leads, each with a first report, not marked PossResend, and
the refused one not to be answered again. Scenario 53 does the same with one order and no refusal.

With the gateway's two refusal branches disabled, the scenario fails, reporting that orders were
taken with no matching engine in existence and the member told nothing about any of them.

## See also

- [BUG-0009](../bug_list.md#bug_0009) — the venue accepting orders indefinitely with no matching engine, closed by this design
- [BUG-0028](../bug_list.md#bug_0028) — the order book growing until the machine runs out of memory, a condition both engines share
- [WAL and High Availability](wal_and_ha.md) — why a held order is durable in the first place
- [Voided commands, and halting when no matching engine leads](voided_commands.md) — sections 5.1 and 5.2, which this page describes as built
- [Gateway throttles](../venue/gateway_throttles.md) — the other reason a gateway refuses a command

---

Back to [High availability](../availability/README.md).
