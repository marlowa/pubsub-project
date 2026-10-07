# Trading phases and the technical event {#trading_phases}

**Status: design. None of it is built.** The requirements it leads to are named, but not yet
written, in the high availability chapter of the functional specification (`docs/book`, the section
"The venue says what it is doing").

## What this is for

The venue has no way to say what it is doing. It can say one thing about itself — whether it is
accepting orders — and that is a single boolean broadcast to the gateways as `OrderAcceptance`
(127), alongside how many orders are waiting, how long the venue has been unable to pass them on,
and the epoch of the sequencer that sent it. Everything else is private to the process that knows it. The clearest case: a matching
engine that cannot account for what it was holding sets a local `halted_` flag and logs

```
MatchingEngineThread: trading is halted -- the venue is not accepting orders and a person must lift this (R-0023)
```

and **nothing in the sequencer or the gateways mentions a halt at all**. The gateway goes on
admitting members' orders and the sequencer goes on sequencing them into a venue whose engine has
stopped. The engine refuses each of those orders, with OrdRejReason Exchange closed and the text
"trading is halted", so a member that places an order learns of the halt from the reply. A member
that is connected and not trading, or that connects during the halt, is told nothing until it tries.
"Trading is halted" is one process's private opinion.

That is [BUG-0065](../bug_list.md), and it is what R-0126 waits on: the venue cannot announce that
it is unable to do a component's work, because it has no way to announce anything about itself.

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
afterwards. How the recorder's report combines with the phase's other work, such as receiving the
next day's instrument prices, is still to be designed. For the recorder's report to mean the day is
fully recorded, no order record may follow the `eod` event, so the open question below of whether a
member may cancel an order during `eod` must be settled with that in mind.

### The control-plane path already exists in the right shape

`OrderAcceptance` (127) carries `accepting`, `deferred_order_count` and `degraded_for_seconds` from
the sequencer to every gateway, and a gateway refuses on it with a member-visible rejection
carrying a reason string. What it cannot express is *why*: a bare boolean cannot distinguish

- degraded and deferring, from
- halted, which a person must lift, from
- closed, because it is after hours.

Those are the same refusal to a member and very different situations for an operator. So the
likeliest shape is that `OrderAcceptance` gains the phase and the halted flag rather than a second
mechanism competing with it — the gateway already knows how to refuse on a venue-wide signal and
already tells the member why.

---

## What a phase means, which is the part with the work in it

The enum is the cheap part. The substance is what each component may do in each phase, and that
table does not exist yet:

| | `sod` | `pre_open` | `open` | `closed` | `eod` |
|---|---|---|---|---|---|
| A member may log on | ? | ? | yes | ? | ? |
| A member may enter an order | no | yes | yes | no | no |
| A member may cancel an order | ? | ? | yes | ? | ? |
| The matching engine matches | no | no | yes | no | no |
| Instrument prices are accepted | ? | no | no | no | yes |

The question marks are real. Whether a member may cancel a resting order after the close is a
policy decision with consequences for every open order the venue holds overnight, and it is not
settled by naming the phases.

**Two further things this note deliberately does not settle:**

- **What a halt does to resting orders.** The engine's existing halt cancels everything on one path
  and deliberately refuses to on another, because cancelling what cannot be named would leave the
  rest unmentioned (R-0123). The phase says the venue is halted; what each component then does with
  what it holds is separate.
- **Whether a phase belongs to the venue or to an instrument.** Real venues halt one instrument far
  more often than the whole market. This venue is single-instrument in effect, so venue-wide is
  right and simple today — but the message shape decides whether per-instrument is an addition
  later or a change. Cheap to leave room for now, expensive to retrofit.

---

## Order of work

1. This note. Done.
2. Requirements in the specification's high availability chapter, including the refusal rule for a
   stale trading day and the announcement R-0126 needs. The section exists and names the
   requirements; they are not yet written.
3. The DSL message and the enum.
4. The per-phase table above, settled rather than guessed.
5. Implementation, and scenarios.

The reasoning and the requirements come before the code because writing the reasoning down is what
shows whether a rule is sufficient as well as necessary.

---

## See also

- [Sequencer Design](sequencer.md) — holds the venue's view of whether it is accepting orders, and
  broadcasts `OrderAcceptance`; see also [Order acceptance](../availability/order_acceptance.md)
- [Matching engine](matching_engine.md) — where `halted_` lives today, and the ways the engine comes to halt
- [Bug list](../bug_list.md) — BUG-0065, the venue having no way to declare a trading halt
- [Process death](../availability/process_death.md) — R-0126, which cannot be built until the venue
  can announce something about itself
