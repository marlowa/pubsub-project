# Trading phases and the technical event {#trading_phases}

**Status: the technical event, halting, every phase, the close, the gateways' refusals, telling
members, the topic for components that only need to know, and the schedule are built; the start-of-day and end-of-day procedures are design.** The `TechnicalEvent` command, the venue's halt and an operator's halt and lift are built
([voided_commands.md](../availability/voided_commands.md), section 6). So is every phase, each entered on
request or at the time in the day's schedule ([trading_day_plan.md](trading_day_plan.md), section 3), and the sequencer's refusal of members' commands outside pre-open and open
([sequencer.md](sequencer.md), "The trading day and its phases"), the close cancelling every order
still open ([matching_engine.md](matching_engine.md)), and both gateways refusing by phase and telling every
member what the venue is doing ([fix_order_gateway.md](fix_order_gateway.md),
[binary_order_gateway.md](binary_order_gateway.md)). The table of what each phase
allows is settled below. The requirements are written in the high availability chapter of the functional
specification (`docs/book`, the section "The venue says what it is doing", R-0164 to R-0175), and the
plan for building them is in [trading_day_plan.md](trading_day_plan.md).

## What this is for

The venue says little about what it is doing. It tells the gateways whether it is accepting orders,
and if not why, with `OrderAcceptance` (127), alongside how many orders are waiting, how long the
venue has been unable to pass them on, and the epoch of the sequencer that sent it.

Halting is a `TechnicalEvent` command in the WAL, which every matching engine applies at its position.
The leading sequencer writes one when a matching engine cannot account for what the venue holds, when
no engine could be trading within the longest recovery the venue does by itself, and when an operator
asks; and it writes one that lifts the halt when an operator asks and the venue is ready
([voided_commands.md](../availability/voided_commands.md), section 6). The sequencer keeps whether
trading is halted from those records, and while it is, it tells the gateways so with the reason
`Halted`: they refuse new orders, saying the venue is halted, with OrdRejReason Exchange closed, and
pass cancels on, which the engine carries out while halted.

Every logged-on member is told of a halt, and of its lift, by a trading session status (FIX 35=h, and its
binary counterpart), and a member that logs on during a halt is told at once ([fix_order_gateway.md](fix_order_gateway.md)).
What the venue does not do is halt one instrument: a halt is venue-wide. That is what remains of
[BUG-0065](../bug_list.md#bug_0065). R-0126, the venue saying it has no instance of a component, is still to
be built on these messages.

This note settles the shape of the mechanism.

---

## The phases

The venue is always in exactly one phase, and the phases form a cycle:

```
   SOD  ->  pre-open  ->  open  ->  closed  ->  EOD  ->  (next trading day) SOD
```

| Phase | What it is |
|---|---|
| `sod` | Start of day. The venue is preparing and is not trading. |
| `pre_open` | Members may connect and enter orders; nothing is matched yet. |
| `open` | Continuous trading. |
| `closed` | Trading has finished for the day. |
| `eod` | End of day. Work that belongs after trading, such as receiving instrument prices for the next day. |

**SOD and EOD are phases and not moments.** Work happens during them — end of day is when the
venue receives the instrument prices it will need tomorrow — and a period with duration and its own
permitted activity is a phase, whatever else it is. Modelling them as instants instead would force
an event for "we have left SOD and are now trading", which is a transition of a state machine
nobody has named.

### Halted is not a phase

A halt is a property the venue has *while in* a phase, not a phase of its own.

The alternative was considered and rejected. If `halted` were a sixth phase, then lifting a halt
has to answer *return to what?* — so the venue must remember the phase it was in, and a halt is a
poor moment to be relying on remembered state. As a flag alongside the phase, lifting is
unambiguous: the venue was open, it is open again.

It also composes. A halt during `pre_open` and a halt during `open` are different situations, and a
model that can express both is better than one that flattens them into a single stopped state.

---

## The technical event

One message announces every phase change. It carries:

| Field | Why |
|---|---|
| the **phase** the venue is now in | see *State, not only transition* below |
| whether trading is **halted** | the orthogonal flag above |
| the **trading day**, as `YYYYMMDD` | see *The trading day* below |

### State, not only transition

The message is *sent* when an event occurs; what it *says* is the resulting state.

The tempting alternative is to announce the event alone and let each subscriber record what state
it now believes the venue to be in. That works, and this venue's pub/sub is WAL-backed with
catch-up, so a late subscriber really could replay the day's events rather than being lost — which
makes the event-only form far more defensible here than it would be over plain multicast.

It is still the wrong choice, for one reason: **a subscriber that derives state from transitions
holds its own copy of the venue's state machine, and a subscriber that misses one transition is
then wrong in a way it cannot detect.** Its state machine is self-consistent; it simply is not the
venue's. Two gateways can disagree about whether the market is open, each behaving correctly by its
own lights, with nothing to reconcile them.

Carrying the resulting phase costs one field and removes the whole class. A component that receives
any technical event is correct regardless of what it missed, and no component needs a state machine
to be right.

This is not a new idea in this venue — it is what `OrderAcceptance` already does. The leading
sequencer sends it to each gateway when the gateway connects and whenever it changes, and repeats it
on an interval while the venue is not accepting orders, so that a gateway which missed the
transition, or connected after it, *"converges on the truth rather than sitting on a stale fine"*.
The technical event should be repeated on the same principle, and a component that has just started
must be able to ask rather than wait.

### The trading day

**Every technical event carries the trading day, not only SOD.**

SOD is where the day *changes*; every message *carries* it. If only SOD carried it, a component
would have to remember the day and assume later phases belonged to it — the derive-your-own-state
problem again — and a stale `eod` would arrive with nothing on it to refuse.

The rule that falls out is the one this venue applies everywhere else, with the trading day playing
the part the epoch plays in leadership:

> A component shall refuse a technical event whose trading day is older than the one it holds.

So a halt declared yesterday cannot stop trading today, which is the case where getting this wrong
would cost the most.

`YYYYMMDD` as an integer, rather than an opaque counter, for two reasons. It sorts correctly and
fits in 32 bits — `99991231` against a limit near 2.1 billion. And **a person reading a log can
check it**: `20260906` is self-evidently right or wrong in a way `epoch=574` is not.

**A component must never compute the trading day from its own clock.** It takes the value the venue
declared and holds it. The trading day is a business date and need not line up with a calendar day
anywhere — a session can open the evening before, and a venue can be mid-day while a machine in
another time zone has already turned over. Two components deriving it locally near a boundary
disagree, each correct by its own clock, and nothing detects it. That is the same failure as
deriving the phase locally, one level down.

---

## How it reaches components, and why by two paths

There are two audiences and they need different things.

**Components that must act on it** — a gateway refusing a member's order, the sequencer declining
to sequence one — need it on the control plane. A phase that some of them have and others do not is
worse than no phase at all: it produces gateways admitting orders into a halted venue and members
told their orders are live. The venue's standing rule that optional things stay out of the control
plane points the same way.

**Components that only need to know** — the publisher, the recorder, whatever ingests the
instrument prices that arrive during `eod` — are the pub/sub audience. That last one is the concrete
case: the phase gates work with nothing to do with order flow, and the component doing it has no
business on the order path.

So both paths earn their place for different components rather than as a hedge.

**Technical events are sequenced by the sequencer, in the same sequence as orders and execution
reports.** The pub/sub path must carry a technical event as a sequenced record, never sent to
subscribers directly by the publisher or any other component. The order activity recorder depends on
this twice. It finishes the trading day when every event up to the `eod` event is confirmed, which
means something only if the `eod` event has a definite place among the execution reports. And it
keeps one number as its position in everything it reads, which works only if everything it reads is
numbered from one sequence ([oar_external_stream.md](../pubsub/oar_external_stream.md) sections 12.1
and 13.6).

**End of day is not complete until the recorder reports that every event of the day is confirmed**
by the external messaging system. The start of `eod` is declared by the venue's schedule; the end of
it waits for the recorder, because trading that is not fully recorded cannot be reconstructed
afterwards. How the recorder's report reaches the end-of-day procedure is designed with the recorder.
For the recorder's report to mean the day is fully recorded, no order record may follow the `eod` event,
which is why the table below refuses cancels after the close.

### The control-plane path already exists in the right shape

`OrderAcceptance` (127) carries `accepting`, `held_order_count`, `no_engine_for_seconds` and a
reason from the sequencer to every gateway, and a gateway refuses on it with a member-visible
rejection carrying a reason string. The reason already distinguishes

- no matching engine leading, with orders held (`NoMatchingEngine`), from
- halted, which a person must lift (`Halted`, and `RecoveryTooLong` while no engine leads).

What it cannot yet express is closed, because it is after hours. Those are the same refusal to a
member and very different situations for an operator. So the likeliest shape is that
`OrderAcceptance` gains the phase rather than a second mechanism competing with it — the gateway
already knows how to refuse on a venue-wide signal and already tells the member why.

---

## What a phase means

What each component may do in each phase:

| | `sod` | `pre_open` | `open` | `closed` | `eod` |
|---|---|---|---|---|---|
| A member may log on | yes | yes | yes | yes | yes |
| A member may enter an order | no | yes | yes | no | no |
| A member may cancel an order | no | yes | yes | no | no |
| The matching engine acts on orders | no | yes, without matching | yes | no | no |
| What the venue itself does on entry | Fixes the trading date | Nothing | Nothing (where an opening auction would go) | Cancels every open order, telling each member | Nothing further is sequenced |

- **Members may log on in every phase**, so that a member is told what the venue is doing and why its
  orders would be refused.
- **Cancels are refused in `closed` and `eod`.** The `TechnicalEvent` that enters `closed` is itself the
  command that cancels every open order in each matching engine, with an execution report to each member,
  so there is nothing left to cancel; and the recorder needs no order record after `eod`.
- **The matching engine treats `pre_open` like `open`**, because it does no matching. The phases stay
  separate so that an opening auction can be added at the move to `open` without changing anything else.
- **Instrument prices and other reference data** are left until something in the venue uses them
  ([trading_day_and_reference_data.md](trading_day_and_reference_data.md)).

**What a halt does to the phase.** A halt keeps its meaning in every phase, and carries the phase the
venue is in. The phase moves on while trading is halted, except while it is halted because a matching
engine cannot account for the orders the venue holds (R-0123): the close cannot then cancel every open
order, because nobody knows which orders are open.

**What a halt does to resting orders** is separate from the phase. The engine's halt after a long absence
cancels everything first (R-0117), and its halt when it cannot account for what the venue holds
deliberately does not, because cancelling what can be named would leave the rest unmentioned (R-0123).

**The phase belongs to the venue.** A phase or a halt for one instrument is a different message, as FIX
separates `SecurityStatus` (35=f) from `TradingSessionStatus` (35=h), so a status for one instrument can be
added later without changing the `TechnicalEvent`.

---

## Order of work

1. This note. Done.
2. Requirements in the specification's high availability chapter, including the refusal rule for a
   stale trading day and the announcement R-0126 needs. Done: R-0164 to R-0175.
3. The DSL message and the enum. Built: `TechnicalEvent`, `TradingPhase` and `TechnicalEventReason` in
   `leader_follower.dsl`. The matching engine applies a technical event as a command in the WAL, takes
   from it whether trading is halted, and refuses one whose trading day is older than the trading date
   of the record carrying it ([deterministic_matching_engine.md](../availability/deterministic_matching_engine.md),
   section 4.1). The leading sequencer writes one to halt trading and one to lift the halt, each with
   the phase the venue is in (`HaltAndVoidRequests`, and
   [voided_commands.md](../availability/voided_commands.md), section 6), and one for each phase it enters on
   request, from the start of day to the end of day (`TradingPhaseRequests`). The gateways do not read the
   technical event itself: they act on the sequencer's `OrderAcceptance`, which says the trading day, the
   phase, and whether trading is halted.
4. The per-phase table above. Done.
5. Implementation, and scenarios, in the order [trading_day_plan.md](trading_day_plan.md) sets out.

The reasoning and the requirements come before the code because writing the reasoning down is what
shows whether a rule is sufficient as well as necessary.

---

## See also

- [Sequencer Design](sequencer.md) — holds the venue's view of whether it is accepting orders, and
  broadcasts `OrderAcceptance`; see also [Order acceptance](../availability/order_acceptance.md)
- [Matching engine](matching_engine.md) — whether trading is halted is kept in the book's file
  (`OrderBook`), so that it outlives the process, and the ways the engine comes to halt
- [Bug list](../bug_list.md#bug_0065) — BUG-0065, a trading halt that cannot be applied to one instrument
- [Process death](../availability/process_death.md) — R-0126, which cannot be built until the venue
  can announce something about itself
