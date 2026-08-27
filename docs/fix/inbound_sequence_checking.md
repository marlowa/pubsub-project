# Inbound sequence checking {#fix_inbound_sequence_checking}

**Status: designed 2026-08-27, not built.** It addresses
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

One gate at the top of the inbound dispatch, before the branch on message type, so no message can
reach a handler without passing it.

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

### A gap halts that member, and that is a decision

While a gap is open, **nothing from that member may be processed**, not merely the message that
revealed it. Processing anything after the gap risks sequencing a cancel ahead of the order it
cancels, which is worse than a pause.

So a member with a gap is effectively halted until it answers the `ResendRequest`. That is a real
availability cost, borne by one member, and it is stated here so that it is a decision rather than
something discovered later in a log.

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

## What this does not solve

- **Duplicate suppression after an unclean death**, as above. The application layer remains the
  backstop, and doing better needs the venue to record the inbound number durably per order rather
  than per session — a field on the WAL envelope, which is a hot-path cost and was not taken.
- **BUG-0006**, `ResendRequest` under load, which is about the outbound path and stays open.

## Implementation order

Each step leaves the venue working.

1. **The field on the three PDUs**, populated and carried but not yet acted on. No behaviour
   change; the sequencer starts remembering a number nothing reads.
2. **The counter and the checks**, in the gateway, with the Logon path last because it is the one
   with the ordering subtlety.
3. **The scenarios**, written to fail first.

## See also

- [FIX sequence numbers, gaps and gap fill](sequence_numbers_and_gaps.md) — what the protocol requires, and the worked example
- [Session binding](../availability/session_binding.md) — how session state survives a gateway change, and the outbound counter's opposite bias
- [Resend provenance](../availability/resend_provenance.md) — the outbound half, built 2026-08-27

---

Back to [FIX](README.md).
