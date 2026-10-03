# Refusing orders the venue cannot process {#ha_order_acceptance}

When no matching engine is reachable, the venue defers the orders it receives for a while, and then,
if the outage goes on longer than a failover plausibly takes, it refuses new orders and cancels with
a reply saying why. It starts accepting again on its own when a matching engine returns. `ha_test.py`
scenario 42 checks all of it.

## The condition

When the leading sequencer has no matching engine to send an order to, it writes the order to its
log and defers sending it. Whichever engine acts next reports the position it has reached and is
sent everything after it, so a deferred order is applied and reported to the member when an engine
returns ([BUG-0064](../bug_list.md#bug_0064), closed). That is the right policy for a brief
failover.

**It is cheap for the venue and expensive for the member.** A deferred order costs the venue
nothing beyond its record in the log. The member is told nothing at all: the execution report is the
matching engine's to send, and there is no matching engine, so an order placed into a deferral gets
no reply. The member cannot tell a deferred order from a slow one or a lost one, cannot cancel it,
because a cancel needs a matching engine too, and has to assume it may be live, because it may be.
Every second of deferral widens the gap between what the member believes and what is true.

So the limits below do not protect the venue's memory. **They bound how far a member's picture of
its own position may drift from the truth** before the venue starts saying no.

## When deferring becomes refusing

**Age first, with a count as a backstop.** The venue stops accepting orders when the current deferral
has lasted longer than `order_deferral_refusal_age` (45 seconds), or when more than
`order_deferral_refusal_count` (250,000) orders have been deferred, whichever comes first. Both are
constants in `SequencerThread.hpp`, with their reasoning beside them.

- **Age is the honest measure,** because the member's exposure is measured in time. The threshold
  must clear a normal change of matching engine leader, or the venue would refuse orders during
  routine recovery that members survive today, so it is set well above what one takes.
- **The count is a backstop for a burst.** At the peak rate measured on this venue, 45 seconds would
  let about 1.55 million orders be accepted and not processed. The count is deliberately reachable
  within a normal failover at peak rate, because a burst is exactly when the volume runs away.
- **The count counts orders, not members,** while the harm is to each member. A venue has a few
  hundred to a few thousand comp ids, and orders are many, so a venue-wide total stands in for how
  far any one member's position has drifted. *Considered and deferred: counting deferred orders per
  session,* which would model the harm directly and stop a quiet member being refused because a busy
  one filled the venue. It adds state per session to the sequencer for a threshold the age almost
  always reaches first. If the venue-wide count proves too blunt, this is the change to make.
- **Constants, not configuration,** because no operator has a reason to change them, and a setting
  nobody changes is only a value to keep in step; see
  [Inbound sequence checking](../fix/inbound_sequence_checking.md) for the same argument. If one ever
  needs changing, the sequencer's configuration is where it would go.

Acceptance is evaluated on every deferred order, not on a timer, so a threshold takes effect the
moment it is crossed. A venue with no traffic has nothing to refuse.

## What the venue does

### The sequencer reports the condition, not each order

A deferred order is not logged on its own. The sequencer counts deferred orders, records when the
deferral began (on a monotonic clock, so a change to the time of day cannot change how long it
believes it has been degraded), and logs a Warning every five seconds naming the count and the age.
When an engine is reachable again it logs one line saying how many orders were deferred and for how
long, at the moment the engine reconnects rather than when the next order arrives. A recovery while
nothing is trading is therefore still reported.

```
no matching engine reachable -- orders are being accepted and deferred, starting at seq=42009043
still no matching engine after  6s --  41 order(s) deferred so far
still no matching engine after 12s --  81 order(s) deferred so far
a matching engine is reachable again after 19s -- 120 order(s) were deferred ...
```

### The sequencer tells the gateways: `OrderAcceptance` (127)

The leading sequencer sends `OrderAcceptance` to every gateway it holds a connection to: when
acceptance changes in either direction, every five seconds while the venue is not accepting, and to
a gateway at the moment it connects, so that a gateway connecting during an outage learns the state
rather than assuming all is well. The message carries whether the venue is accepting, how many orders
are deferred, and for how long.

**Only the leader sends it.** A gateway holds a connection to both sequencers and cannot tell which
leads. A follower sends nothing to a matching engine, so it never defers, and if it answered it would
say "accepting" on a venue with no matching engine at all. Two things follow, both in
`SequencerThread::adopt_role`:

- **An instance that takes the lead sends `OrderAcceptance` at once,** because the gateways may be
  holding what the previous leader said before it died, and silence would leave a refusal in place
  that nothing would lift.
- **An instance that becomes a follower clears its deferral record,** so that if it leads again it
  does not come back already refusing, with an age measured from an outage that ended long ago.

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
- In the example, `awaiting=7` is the seven orders deferred before refusal began. They are in the
  log and pending, and they are answered when an engine returns. Deferred and refused are different
  states, and only the first is still to be resolved.

The timer ticks at a fifth of the interval and writes the line when five seconds have passed since
the last one, so a busy gateway is not reported twice and the interval is kept. The fields may be
added to at the end and never reordered, because `ha_test.py` and `perf_run.py` read the line.

## Resuming

**The venue starts accepting again on its own when a matching engine returns.** This is about the
venue's own capacity, which is not a matter of interpretation: there is no matching engine, and
accepting orders is the harm.

*Considered and rejected: requiring an operator to re-enable acceptance.* It would stop a flapping
matching engine reopening the venue repeatedly, but it makes recovery depend on someone watching,
and the failure this design answers is one in which nobody noticed for seven minutes.

Resuming without a person is right **while nothing has been lost.** Once orders have been lost, a
venue that reopens quietly hides the damage rather than recovering from it; that case belongs to a
declared halt, which does not lift by itself ([design notes, section 15](design_notes.md#ha_recovery_ends_at_loss),
[BUG-0065](../bug_list.md#bug_0065)).

## What this does not solve

- **A matching engine that is connected but not working.** Everything here depends on the
  connection. An engine that takes orders and does nothing with them looks healthy throughout
  ([BUG-0010](../bug_list.md#bug_0010)).
- **A matching engine that keeps failing and returning.** Each return ends the deferral and starts
  its clock again, so a venue whose engine flaps never reaches the age threshold and never stops
  accepting ([BUG-0066](../bug_list.md#bug_0066)).
- **Telling the member when acceptance resumes.** Nothing announces it; a member finds out by sending
  an order that is not refused.
- **Orders lost at a change of *sequencer* leader,** which is a different failure; see
  [change_of_sequencer_leader.md](change_of_sequencer_leader.md).

## Tests

`ha_test.py` scenario 42, `order_refusal`, kills the matching engine and does not restart it, and
asserts five things in order: the first order is deferred and not answered; orders are refused once
the outage has lasted longer than a failover plausibly takes; cancels are refused too; the health line
keeps reporting while nothing progresses; and acceptance resumes, with no operator action, when an
engine returns. The first assertion matters as much as the others: a venue that refused from the
first order would pass a test of refusal alone while refusing orders during every routine failover.

With the gateway's two refusal branches disabled, the scenario fails, reporting that orders were
taken with no matching engine in existence and the member told nothing about any of them.

## See also

- [BUG-0009](../bug_list.md#bug_0009) — the venue accepting orders indefinitely with no matching engine, closed by this design
- [BUG-0010](../bug_list.md#bug_0010) — failing over into a condition both instances share
- [WAL and High Availability](wal_and_ha.md) — why a deferred order is durable in the first place
- [Gateway throttles](../venue/gateway_throttles.md) — the other reason a gateway refuses a command

---

Back to [High availability](../availability/README.md).
