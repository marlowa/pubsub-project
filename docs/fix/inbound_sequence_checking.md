# Inbound sequence checking {#fix_inbound_sequence_checking}

**Status: step 1 of 4 built, 2026-08-27. Nothing in the design is open.** See
[Implementation order](#implementation-order) for what is done and what is not. It addresses
[BUG-0038](../bug_list.md#bug_0038), and items 1 and 2 of the departures in
[FIX sequence numbers, gaps and gap fill](sequence_numbers_and_gaps.md), which are one piece of
work and cannot be split.

## The problem

`MsgSeqNum` on an inbound message is never compared against anything. The gateway keeps no
expected-inbound counter, never detects a gap in what a member sends, and never sends a
`ResendRequest` — the only `ResendRequest` code in the gateway is the handler for receiving one.

What that costs, in the case it is for:

> A member sends an order. The connection drops before it arrives. The member reconnects and
> carries on numbering from the next value. The venue was expecting the missing number, receives
> the one after it, processes it, and carries on. **The member believes it has an order resting
> that the venue has never heard of, and neither side has any reason to think otherwise.**

Noticing exactly that is what the numbering is for. It is the only open defect where the venue can
lose a member's order without either side noticing.

## The rule

One gate, before the branch on message type, so no message can reach a handler without passing it.

**It has to sit on two paths, not one.** The parser has separate callbacks for a message that
parses and one that is well framed but fails FIX validation, and the second currently sends a
Reject (35=3) without the counter ever seeing the message. **A rejected message has still consumed
its sequence number** — that part is the specification — so leaving that path alone would make the
next valid message look like a gap and produce a `ResendRequest` for a number the member had
already sent, on every validation failure.

Sequence position is therefore decided first on both paths, because a message whose place in the
stream is unknown cannot be trusted whatever else is wrong with it. Three cases, and **only the
first is settled by the specification; the other two were chosen** and can be revisited:

| The message | What happens | Why |
|---|---|---|
| number readable, **in sequence** | send the Reject, advance | the specification: it consumed its number |
| number readable, **out of sequence** | apply the gap rules; **do not** send the Reject | *chosen.* It arrives again in the resend and is rejected then, at the point the venue can place it. A Reject now would name a message the venue is simultaneously asking to be sent again |
| number **not readable** | send the Reject, **do not** advance | *chosen.* It consumed some number, but not knowably the expected one, and assuming is how a counter drifts. The next message reveals a gap and the resend repairs it |

The second and third have coherent alternatives — reject immediately as well as handling the gap;
advance optimistically, or treat a message with no sequence number as grounds for disconnection.
They are recorded as decisions rather than as consequences so that a reader who disagrees knows
there is something to disagree with.

`expected_inbound_seq_num` is **the next number the venue expects from this member**. Getting that
definition wrong by one is the whole game, so it is stated rather than implied.

| Received | What it means | What the venue does |
|---|---|---|
| **equal** | in sequence | process it, then increment |
| **higher** | the member skipped numbers | do not process; send `ResendRequest(expected, 0)` |
| **lower**, `PossDupFlag=Y` | a retransmission of something already processed | discard silently |
| **lower**, no `PossDupFlag` | the far side has gone backwards | Logout and disconnect |

**Higher means lost, not late.** TCP already orders a single connection, so a gap on a live
connection means the member genuinely skipped those numbers — the messages are not in flight
behind. That settles a question that would otherwise need answering: no reordering buffer is
required, because there is nothing to reorder.

### A message that arrives while a gap is open is discarded

Not buffered. The `ResendRequest` names `EndSeqNo=0`, so the member resends everything from the
gap onward including the message just discarded, and it arrives in order with the rest.

The alternative — hold it and process it once the gap fills — is what QuickFIX does and is not
wrong, but it buys little here and costs a buffer whose exhaustion behaviour would need designing.
Asking the member to send it again is cheaper than deciding what to do when the buffer is full.

### While a gap is open, that member's later messages wait

**Nothing from that member may be processed while the gap is open**, not merely the message that
revealed it.

This is not the venue imposing anything. It cannot process message 101 because it does not have
100, and the ordering matters: a member sending `NewOrderSingle(100)` then `Cancel(101)`, with 100
lost, would have the cancel applied to an order the venue never received. It rejects the cancel as
unknown, then the resend delivers 100 and the order rests — leaving the member holding an order it
believes it cancelled. Every conforming engine waits for the same reason.

The member stays connected throughout. Its resting orders are untouched, its execution reports
keep flowing outbound, and the wait is one round trip.

**The decision is what to do when that round trip does not come back.** A member whose engine is
faulty, or which has lost its own store, may never answer the `ResendRequest` — and then its order
flow really does stop, with the venue sitting silent and nothing saying why. The venue re-asks twice
and then disconnects — see [An unanswered ResendRequest](#an-unanswered-resendrequest-re-ask-twice-then-logout).

### Lower without `PossDupFlag` ends the session

FIXT.1.1 calls it a serious error, and the reasoning is that once the far side has gone backwards
its state cannot be trusted. The venue sends a Logout and disconnects. The member reconnects and
resynchronises, which is safer than continuing to accept orders through a session whose numbering
the venue does not believe.

## Resuming after an unclean death: the direction is the opposite of the outbound one

**This is the part of the design most likely to be got wrong**, because the two counters will sit
beside each other on the same three PDUs and the natural instinct is to treat them alike.

The sequencer resumes the *outbound* number deliberately **high**
([Session binding](../availability/session_binding.md)), because the two errors are not
symmetrical: too high leaves a gap the member closes with a `ResendRequest`, and too low sends the
member a number below what it expects, which FIX requires it to treat as fatal.

For the inbound number every term in that sentence reverses:

| | Too high | Too low |
|---|---|---|
| **Outbound** (venue → member) | member sees a gap, asks, recovers | member sees a fatal low number, drops the session |
| **Inbound** (member → venue) | **venue** treats an innocent member as committing a serious error, and disconnects it | venue sees a gap, asks, recovers |

So the inbound number resumes at **whatever the sequencer last heard, with no allowance added**.
That figure is already a lower bound — a member can only have sent *more* since it was reported —
which is exactly the safe side. Adding an allowance, by symmetry with the outbound field, would
disconnect members who had done nothing wrong.

**The price, stated plainly.** After an unclean death the venue may re-receive messages it already
processed, and the session layer cannot recognise them: they arrive with `PossDupFlag=Y` and a
number at or above what the venue now expects, which is indistinguishable from a legitimate
retransmission filling a real gap. The matching engine's duplicate-`ClOrdID` rejection is what
catches them.

That backstop exists today and BUG-0038 rightly complains about it — a run produced 132,000
duplicate-`ClOrdID` warnings. **This design does not remove the backstop; it moves when it fires.**
Today it fires on the ordinary path, because nothing checks anything. After this it fires only
after an unclean gateway death, where the ambiguity is genuine and the alternative is disconnecting
innocent members.

## Where the state lives

Beside `outbound_seq_num`, on `SessionUnbound` (121), `SessionBoundAck` (122) and
`SessionSequenceUpdate` (126). The DSL comment at that field anticipated it:

> Only the outbound number is carried. The gateway does not track what the member sends it --
> there is no inbound gap detection to hold a number for -- so a field for it would be one
> nothing populates. When that is built, it belongs here beside this one.

Same mechanism, unchanged, and the reason is the same: the sequence series belongs to the session
and not to the connection, so it has to survive a member moving between gateway instances. See
[Session binding](../availability/session_binding.md).

## The Logon is a special case, and the ordering is awkward

A member's Logon carries a `MsgSeqNum` like any other message, and the specification is explicit
that a too-high one is **not** grounds for refusing the logon: complete it, then send the
`ResendRequest`. (The opposite behaviour is what `f8test` does by default, and configuring around
it — `ignore_logon_sequence_check` — was needed to test the outbound side at all.)

**But the venue does not yet know what to expect when the Logon arrives.** The expected-inbound
number comes back on `SessionBoundAck`, which is asynchronous and arrives after the Logon has been
received and the session bound.

The existing structure already answers this. A session is `awaiting_sequence_state` between
binding and the ack, and nothing may be sent to the member in that window because the Logon reply
is itself a numbered message. So the Logon's number is stashed and checked at the same moment the
outbound number is restored:

- **equal or lower with `PossDupFlag`** — proceed normally.
- **higher** — complete the logon first, send the Logon reply, then send the `ResendRequest`.
- **lower without `PossDupFlag`** — Logout and disconnect, as for any other message.

`ResetSeqNumFlag=Y` resets both directions, so the expected inbound becomes 2 once the Logon
numbered 1 has been processed. The venue already honours the flag for the outbound side; the
inbound side follows it for the same reason, and the two must reset together or the session is
half-reset.

## Testing

`f8test -S` sets the client's next **send** sequence number, the exact mirror of the `-R` used to
manufacture the outbound gaps in `ha_test.py` scenarios 22 and 40. An inbound gap is therefore as
manufacturable as an outbound one, in a harness that now knows how to drive it.

Scenarios worth having, and each should be seen to fail before the code exists:

- **A gap is detected and closed.** Client starts with `-S` ahead of where the venue expects. The
  venue must send a `ResendRequest` naming the right number, the member resends, and the orders in
  the gap must reach the matching engine — assert on `ME-ORD` counts, not merely on the request
  going out.
- **Nothing after the gap is processed until it is filled.** The order that revealed the gap must
  not reach the matching engine before the orders that preceded it.
- **A retransmission is not a new order.** A message with `PossDupFlag=Y` below the expected number
  produces no order and no duplicate-`ClOrdID` rejection.
- **Too low without `PossDupFlag` ends the session**, with a Logout the member can see.
- **The counter survives a gateway failover**, in the shape of scenario 23: the member's inbound
  numbering must continue across the instance change, and must not be resumed high.

## An unanswered ResendRequest: re-ask twice, then Logout

A member that never answers has its flow stopped for as long as it stays connected, which is the
failure mode most likely to reach the venue as "you have stopped taking my orders". So the wait is
bounded and the venue acts at the end of it.

**On a timer, the `ResendRequest` is repeated, up to twice. If the gap is still open after that,
the venue sends a Logout and disconnects.**

Repeating costs nothing and covers the ordinary case: a request lost in flight, or a member that
was slow rather than broken. Only a member genuinely not answering reaches the Logout — and a
disconnect there is recoverable rather than destructive, because the venue still holds the
session's numbering, so the member reconnects and resynchronises from it. That is the same
property the outbound side relies on.

Waiting indefinitely was rejected for a reason with a precedent in this venue: it leaves a stopped
session looking healthy to anyone not reading the log. That is the shape of
[BUG-0009](../bug_list.md#bug_0009), where the sequencer knew for seven minutes that it had no
matching engine, logged it a million times at INFO, and told the gateway nothing.

**Five seconds between attempts, two attempts, then the Logout** — so a member has about fifteen
seconds to answer before it loses the session. A named constant beside the existing logon and
SCRAM timeouts, not configuration: nothing yet suggests members need different values, and a
figure in a TOML that no operator has a reason to change is a field to keep in step for nothing.
If one ever does, the gateway configuration is where it goes, and the per-comp-id route that
cancel-on-disconnect takes is the step beyond that.

Three things this needs, and none is new machinery: a per-session timer of the kind already armed
for logon and for the SCRAM exchange; a WARNING when the retries are exhausted that names the
member and the gap; and the gap's age exposed as a metric, so a member sitting in this state is
visible without reading logs.

## What this does not solve

- **Duplicate suppression after an unclean death**, as above. The application layer remains the
  backstop, and doing better needs the venue to record the inbound number durably per order rather
  than per session — a field on the WAL envelope, which is a hot-path cost and was not taken.
- **BUG-0006**, `ResendRequest` under load, which is about the outbound path and stays open.

## Implementation order

Each step leaves the venue working.

### Step 1 — the field, carried but not acted on. **Done 2026-08-27.**

`inbound_seq_num` is on `SessionUnbound` (121), `SessionBoundAck` (122) and
`SessionSequenceUpdate` (126); `FixSession::expected_inbound_seq_num` holds it in the gateway and
`SequencerThread::SessionSequenceState::inbound_seq_num` in the sequencer.
`FixOrderGatewayThread::note_inbound_seq_num` observes it from **both** inbound callbacks, and the
sequencer hands it back untouched — the no-allowance rule is implemented at the same site as the
outbound allowance, with the reason beside it, because that is where the two will be read
together.

**No behaviour changed**, which was the point: nothing compares an arriving number against it, so
a member that skips numbers is still processed as though nothing were missing. What the venue has
gained is that it now *knows* where a member's numbering stands and keeps that across a gateway
change.

Verified end to end by `ha_test.py` scenario 23, which kills the gateway holding a session and
brings the member back on the surviving instance:

```
resuming the venue's sequence state -- outbound=4140 inbound=1002
```

The member's inbound position survived the death of the gateway that observed it, and the
asymmetry is legible in that one line: `outbound` biased high by its allowance, `inbound` exactly
as reported.

### Step 2 — the counter and the checks. **Not started.**

In the gateway, on both the parse and the reject paths, per [The rule](#the-rule). The Logon path
last, because it is the one with the ordering subtlety — the expected number arrives
asynchronously on `SessionBoundAck`, so the Logon's own number is checked retrospectively in the
existing `awaiting_sequence_state` window.

### Step 3 — the retry timer and the Logout. **Not started.**

Five seconds, two retries, then Logout; a named constant beside the logon and SCRAM timeouts.
Needs step 2 above it to have anything to time. Also the WARNING and the gap-age metric.

### Step 4 — the scenarios, written to fail first. **Not started.**

Driven by `f8test -S`, per [Testing](#testing). Including the member that goes silent instead of
answering, which is the one whose absence would not otherwise be noticed.

## An adjacent behaviour this does not change

A member that sends anything before the venue's Logon reply — pipelining an order straight after
its own Logon — is disconnected today by the `!session_established` branch in the inbound dispatch.
That window is where `SessionBoundAck` is awaited, so it widens slightly as a session's state grows,
and step 2 stashes the Logon's number across it.

**Left alone deliberately.** A member is entitled to expect a Logon response before sending, so
the behaviour is defensible, and changing it is a separate question from this one. Recorded here
because the design touches the window and the next person to work in it should know the branch is
there on purpose.

## See also

- [FIX sequence numbers, gaps and gap fill](sequence_numbers_and_gaps.md) — what the protocol requires, and the worked example
- [Session binding](../availability/session_binding.md) — how session state survives a gateway change, and the outbound counter's opposite bias
- [Resend provenance](../availability/resend_provenance.md) — the outbound half, built 2026-08-27

---

Back to [FIX](README.md).
