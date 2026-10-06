# Inbound sequence checking {#fix_inbound_sequence_checking}

The venue checks the `MsgSeqNum` of every message a member sends, asks for what is missing, and
bounds how long it waits for an answer. This document describes the rules, why each is as it is,
and where the state lives. The protocol it implements is described in
[FIX sequence numbers, gaps and gap fill](sequence_numbers_and_gaps.md). It is the fix for
[BUG-0038](../bug_list.md#bug_0038). One piece of observability is still to do:
[BUG-0058](../bug_list.md#bug_0058), below.

## Why it matters

Without the check, this is what happens:

> A member sends an order. The connection drops before it arrives. The member reconnects and
> carries on numbering from the next value. The venue was expecting the missing number, receives
> the one after it, processes it, and carries on. **The member believes it has an order resting
> that the venue has never heard of, and neither side has any reason to think otherwise.**

Noticing exactly that is what the numbering is for.

## The rule

One gate, before the branch on message type, so no message can reach a handler without passing it.

**It sits on two paths, not one.** The parser has separate callbacks for a message that parses and
for one that is well framed but fails FIX validation, and the second sends a Reject (35=3). **A
rejected message has still consumed its sequence number** — that part is the specification — so if
that path were not checked, the next valid message would look like a gap and produce a
`ResendRequest` for a number the member had already sent, on every validation failure.

Sequence position is therefore decided first on both paths, because a message whose place in the
stream is unknown cannot be trusted whatever else is wrong with it. Three cases, and **only the
first is settled by the specification; the other two are choices** and can be revisited:

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
| **higher** | the member skipped numbers | send `ResendRequest(expected, 0)`; process it only if it is session-level |
| **lower**, `PossDupFlag=Y` | a retransmission of something already processed | discard silently |
| **lower**, no `PossDupFlag` | the far side has gone backwards | Logout and disconnect |

**Higher means lost, not late.** TCP already orders a single connection, so a gap on a live
connection means the member genuinely skipped those numbers — the messages are not in flight
behind. No reordering buffer is required, because there is nothing to reorder.

### A message that arrives while a gap is open is discarded

Not buffered. The `ResendRequest` names `EndSeqNo=0`, so the member resends everything from the
gap onward including the message just discarded, and it arrives in order with the rest.

The alternative — hold it and process it once the gap fills — is what QuickFIX does and is not
wrong, but it buys little here and costs a buffer whose behaviour when full would need designing.
Asking the member to send it again is cheaper than deciding what to do when the buffer is full.

### While a gap is open, that member's later application messages wait

**Session-level messages are still acted on** — Heartbeat, TestRequest, ResendRequest and
SequenceReset. None of them is an order or a cancel, so none carries the ordering risk the wait
exists to prevent, and holding them back breaks the very recovery the gap is waiting for. Logout is
deliberately not among them: it ends a session, so a badly numbered one should be questioned like
anything else.

Both narrower choices have been measured and fail:

- With **everything** blocked, a member recovering a gap sees silence, sends a `TestRequest`, gets
  no answer, and aborts the session. The venue has stopped answering the layer that keeps the
  connection alive.
- With only **Heartbeat and TestRequest** let through, the session deadlocks instead. A member's
  own `ResendRequest` is sent at *its* current number, which during a venue-side gap is by
  definition above what the venue expects — so it is discarded as part of the gap. The venue then
  waits for messages the member cannot send until the venue answers a request it has thrown away.

The second is the one worth remembering: the rule "nothing from that member may be processed"
sounds safe and is not, because the messages that end a gap arrive numbered inside it.

### Why the application messages wait

No application message from that member is processed while the gap is open, not merely the one that
revealed it.

The venue cannot process message 101 because it does not have 100, and the order matters: a member
sending `NewOrderSingle(100)` then `Cancel(101)`, with 100 lost, would have the cancel applied to an
order the venue never received. It would reject the cancel as unknown, then the resend would
deliver 100 and the order would rest — leaving the member holding an order it believes it
cancelled. Every conforming engine waits for the same reason.

The member stays connected throughout. Its resting orders are untouched, its execution reports
keep flowing outbound, and the wait is one round trip.

**The gap stays open until the counter passes the number that revealed it**
(`FixSession::inbound_gap_through`), not merely until the first in-sequence message. A member
answering a `ResendRequest` sends the missing messages in order, so the first of them is in
sequence; if that closed the gap, a further gap inside the same resend would provoke a second
request while the first was still being answered, and a member that receives one mid-resend
ignores it. So filling one number of a ninety-six-number gap leaves it open.

### Lower without `PossDupFlag` ends the session

FIXT.1.1 calls it a serious error, and the reasoning is that once the far side has gone backwards
its state cannot be trusted. The venue sends a Logout naming both numbers and disconnects. The
member reconnects and resynchronises, which is safer than continuing to accept orders through a
session whose numbering the venue does not believe.

## An unanswered ResendRequest: re-ask twice, then Logout {#fix_inbound_seq_unanswered}

A member whose engine is faulty, or which has lost its own store, may never answer the
`ResendRequest`, and its order flow then stops for as long as it stays connected. That is the
failure most likely to reach the venue as "you have stopped taking my orders", so the wait is
bounded.

**The `ResendRequest` is sent at most three times, five seconds apart, and if the gap is still open
five seconds after the third, the venue sends a Logout and disconnects.** So a member has about
fifteen seconds to answer. Measured against a running venue, with a member that opens a gap and
then says nothing:

```
ResendRequests at t+0.0s, t+5.0s, t+10.0s, all BeginSeqNo=3
Logout at t+15.0s -- "ResendRequest from 3 unanswered after 3 attempts"
connection closed by the venue
```

With a member that answers, the missing numbers are processed, no further request goes out, and
the session keeps trading.

Repeating costs nothing and covers the ordinary case: a request lost in flight, or a member that
was slow rather than broken. Only a member genuinely not answering reaches the Logout — and a
disconnect there is recoverable rather than destructive, because the venue still holds the
session's numbering, so the member reconnects and resynchronises from it.

Waiting indefinitely is rejected because it leaves a stopped session looking healthy to anyone not
reading the log. That is the shape of [BUG-0009](../bug_list.md#bug_0009), where the sequencer
knew for seven minutes that it had no matching engine, logged it a million times at Info, and told
the gateway nothing.

The interval and the count are constants in `FixOrderGatewayThread.cpp`
(`resend_request_retry_interval`, `max_resend_requests`), beside the logon and SCRAM timeouts,
rather than configuration: nothing suggests members need different values. If one ever does, the
gateway configuration is where it goes.

**Not built: the gap's age as a metric**, so that a member halted by a gap is visible without
reading a log ([BUG-0058](../bug_list.md#bug_0058)). Each repeat is logged at Warning, and the
Logout at Error, naming the member and the number awaited.

## Resuming after an unclean death: the direction is the opposite of the outbound one

**This is the part of the design most likely to be got wrong**, because the two counters sit beside
each other on the same three PDUs and the natural instinct is to treat them alike.

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

When `SessionBoundAck` returns the remembered number, the gateway takes the higher of it and its
own counter rather than assigning it. The member's Logon has already been seen by then — it is what
caused the bind — so the counter has already moved past it, while the sequencer's figure predates
it; assigning would wind the counter back over a message the venue has consumed.

**"A member can only have sent more" holds only because a reset is handled separately, and that
is the one thing this rests on.** A member may restart its numbering at any Logon with
`ResetSeqNumFlag=Y`, and clients make it easy — the venue's own Java test client offers it, and it
is the default in the stock fix8 configuration. On that path the member's next number is *lower*
than the venue remembers, by design. So the reset is carried on `SessionBound` (120) and the
sequencer discards everything it remembers for the session, which keeps the lower-bound argument
true for every other path. See [BUG-0055](../bug_list.md#bug_0055) for what happens otherwise: the
venue's memory sticks on a numbering the member has abandoned, and a returning member is judged to
have gone backwards on every reconnect.

**The price, stated plainly.** After an unclean death the venue may re-receive messages it already
processed, and the session layer cannot recognise them: they arrive with `PossDupFlag=Y` and a
number at or above what the venue now expects, which is indistinguishable from a legitimate
retransmission filling a real gap. The matching engine's duplicate-`ClOrdID` rejection is what
catches them. On the ordinary path the session layer discards a retransmission before it gets that
far; only after an unclean gateway death, where the ambiguity is genuine and the alternative is
disconnecting innocent members, does the application layer have to catch it.

## Where the state lives

`inbound_seq_num` sits beside `outbound_seq_num` on `SessionUnbound` (121), `SessionBoundAck` (122)
and `SessionSequenceUpdate` (126). In the gateway it is `FixSession::expected_inbound_seq_num`; in
the sequencer, `SequencerThread::SessionSequenceState::inbound_seq_num`. The sequencer hands it
back untouched, and the no-allowance rule is implemented at the same site as the outbound
allowance, with the reason beside it, so the two are read together.

Same mechanism as the outbound number, and for the same reason: the sequence series belongs to the
session and not to the connection, so it has to survive a member moving between gateway instances.
See [Session binding](../availability/session_binding.md). Scenario 23 shows it surviving the
death of the gateway that observed it:

```
resuming the venue's sequence state -- outbound=4140 inbound=1002
```

`outbound` biased high by its allowance, `inbound` exactly as reported.

## The Logon is a special case, and the ordering is awkward

A member's Logon carries a `MsgSeqNum` like any other message, and the specification is explicit
that a too-high one is **not** grounds for refusing the logon: complete it, then send the
`ResendRequest`. (`f8test` does the opposite by default, and configuring around it —
`ignore_logon_sequence_check` — is needed to test the outbound side at all.)

**But the venue does not know what to expect when the Logon arrives.** The expected-inbound number
comes back on `SessionBoundAck`, which is asynchronous and arrives after the Logon has been
received and the session bound.

A session is `awaiting_sequence_state` between binding and the acknowledgement, and nothing may be
sent to the member in that window because the Logon reply is itself a numbered message. So the
Logon's number is kept (`FixSession::logon_seq_num`, `logon_poss_dup`) and judged at the moment the
outbound number is restored (`judge_logon_sequence`, called from
`establish_session_after_logon_sequence`):

- **equal, or lower with `PossDupFlag`** — proceed normally.
- **higher** — complete the logon first, send the Logon reply, then send the `ResendRequest`.
- **lower without `PossDupFlag`** — Logout with the reason, and the session never opens.

`ResetSeqNumFlag=Y` resets both directions, so the expected inbound becomes 2 once the Logon
numbered 1 has been processed. The two must reset together or the session is half-reset.

## Where it is implemented

In `FixOrderGatewayThread.cpp`: `classify_inbound_sequence` decides a message's place with no side
effects, so both inbound paths ask the same question and then act on the answer differently;
`request_missing_messages` asks once per gap rather than once per message; `send_resend_request`
arms the five-second timer every time it asks, and `retry_or_abandon_resend_request` runs on it and
either asks again or ends the session; `end_session_on_sequence_error` sends the Logout and
disconnects. The timer is cancelled wherever the gap is declared closed, and on connection loss
beside the other per-session timers.

## Measured behaviour

Driven with `scripts/fix_raw_client.py` against a running venue:

| The member does | The venue does |
|---|---|
| order numbered 50, expecting 3 | `ResendRequest BeginSeqNo=3 EndSeqNo=0`; the order is **not** processed |
| another message while that gap is open | no second request — asked once |
| resends 3 with `PossDupFlag=Y` | **processed**: it fills a real gap, so it is new to the venue |
| order numbered 2, expecting 4, unmarked | `Logout` — *MsgSeqNum too low, expecting 4 but received 2* — and disconnect |
| order numbered 2, expecting 4, `PossDupFlag=Y` | discarded silently; the session stays open and usable |
| order with no `MsgSeqNum` | `Reject`, and the counter does **not** advance: the next in-sequence order still works |
| Logon numbered 25, expecting 5 | Logon **completed first**, then `ResendRequest BeginSeqNo=5` |
| Logon numbered 2, expecting 5, unmarked | `Logout` with the reason, and the session never opens |

## Testing {#fix_inbound_seq_testing}

**Two clients, for two jobs.**

`f8test` is a FIX engine and is trying to be correct: it always writes a valid `MsgSeqNum` and will
not send below its own expected without marking it. Its `-S` option sets its next **send** sequence
number, the mirror of the `-R` used to make the outbound gaps in `ha_test.py` scenarios 22 and 40,
so it can produce the conforming cases: a member that continues its numbering, restarts it, or
genuinely misses messages.

**`scripts/fix_raw_client.py` covers what a conforming engine will not do.** At least two of the
rules above can only be exercised by a client that misbehaves on purpose — a message with no
readable number, and a number below expected with no `PossDupFlag`. The raw client has no session
layer at all: it sends the bytes it is told to, computes the framing unless asked to get it wrong,
and never forms an opinion about what comes back. It needs no cryptography: the member's side of
authentication is a plaintext password on tag 554 of the Logon — an empty one is simply absent —
because the SCRAM exchange happens between the gateway and the authentication service, not between
the member and the gateway.

**`ha_test.py` scenario 41, `inbound_sequence_checking`**, drives the raw client and asserts each
of these on what the member is handed:

- a number above expected — asked about **from the right number**, and the message is not processed;
- a further message while the gap is open — no second request;
- the gap filled — the orders are processed and the session carries on;
- below expected with `PossDupFlag` — discarded, session kept usable;
- below expected unmarked — Logout naming both numbers, session ended;
- no `MsgSeqNum` — Reject, and **the counter does not move**, checked by sending an ordinary order
  afterwards and requiring no resend request.

With `classify_inbound_sequence` stubbed to return `InSequence`, the scenario fails on its first
assertion: *"an order numbered 43 arrived when the venue expected 3 and it asked for nothing"*.

**Not covered by any scenario's assertions:**

- **The inbound counter surviving a gateway failover.** Scenario 23 logs it, as above, but asserts
  only that the outbound number was resumed.
- **A member that continues its own send numbering across a reconnect, landing at or above where
  the venue expects.** `f8test -S` could supply it, but `send_burst` would need to set it per
  client rather than for the whole run.
- **A member that asks for a sequence reset while the provenance record holds ranges**, the shape
  of [BUG-0055](../bug_list.md#bug_0055).
- **A member that goes silent instead of answering a `ResendRequest`.** It has been measured by
  hand, as above, but no scenario asserts the Logout.

## What this does not solve

- **Duplicate suppression after an unclean death**, as above. The application layer remains the
  backstop, and doing better needs the venue to record the inbound number durably per order rather
  than per session — a field on the WAL envelope, which is a hot-path cost and has not been taken.
- **BUG-0006**, `ResendRequest` under load, which is about the outbound path and stays open.

## An adjacent behaviour this does not change

A member that sends anything before the venue's Logon reply — pipelining an order straight after
its own Logon — is disconnected by the `!session_established` branch in the inbound dispatch. That
window is where `SessionBoundAck` is awaited, so it widens slightly as a session's state grows, and
the Logon's number is kept across it.

**Left alone deliberately.** A member is entitled to expect a Logon response before sending, so
the behaviour is defensible, and changing it is a separate question from this one. Recorded here
because the design touches the window and the next person to work in it should know the branch is
there on purpose.

## See also

- [FIX sequence numbers, gaps and gap fill](sequence_numbers_and_gaps.md) — what the protocol requires, and the worked example
- [Session binding](../availability/session_binding.md) — how session state survives a gateway change, and the outbound counter's opposite bias
- [Resend provenance](../availability/resend_provenance.md) — the outbound half

---

Back to [FIX](../fix/README.md).
