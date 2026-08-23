# FIX sequence numbers, gaps and gap fill

A primer, written for someone who knows the venue but not the FIX session layer, followed by
what this venue actually does and where it departs from the specification.

Everything below is FIXT.1.1, which is the session layer this venue uses. Check the version
before trusting anything else you read about FIX sequence handling.

---

## Which specification this is, and why so much material misleads

Up to FIX 4.4 there was one specification covering both layers: how a session is established
and kept in step, and what an order looks like. From FIX 5.0 the two were split. The session
layer became a separate specification called **FIXT**, versioned independently, so that the
business messages could evolve without disturbing the machinery underneath.

This venue uses **`BeginString=FIXT.1.1`** with **`DefaultApplVerID=FIX.5.0SP2`**, which is why
there are two data dictionaries in `libraries/fix_codec/data_dictionary/` rather than one:

- `FIXT11.xml` — the session layer. Logon, Logout, Heartbeat, TestRequest, **ResendRequest**,
  **SequenceReset**. Everything in this document.
- `FIX50SP2.xml` — the application layer. NewOrderSingle, ExecutionReport, OrderCancelRequest.

Most tutorials and blog posts describe FIX 4.2 or 4.4. The session mechanics are largely the
same but differ in detail, so material written for those versions can be wrong here without
looking wrong. FIXT.1.1 governs.

---

## The one idea underneath all of it

**Every message a side sends carries a sequence number, `MsgSeqNum` (tag 34), starting at 1 and
incrementing by exactly one, and the receiver checks it.**

Two counters per side, and they are independent. The venue numbers what it sends; the member
numbers what it sends. Neither renumbers on reconnect: the series belongs to the *session*, not
to the TCP connection, and can run across many connections over a day.

That is the whole mechanism. FIX is carried over TCP, which already guarantees that bytes
arrive in order and unduplicated *within one connection*. What TCP cannot tell you is whether
anything was lost when a connection died, or whether the process at the far end lost messages
it had accepted but not yet acted on. Sequence numbers survive the connection and answer both.

A receiver comparing a message's number against what it expected has exactly three cases:

| observed | means | response |
|---|---|---|
| equal to expected | normal | process it, increment expected |
| **higher** than expected | messages are missing | a **gap** — ask for them |
| **lower** than expected | already seen it | serious error unless flagged as a duplicate |

The third case means the far side has gone backwards, which should not happen. The
specification calls it a serious error and the usual response is to log out rather than guess.
The exception is a message marked as a possible duplicate, which is what a resend looks like;
see below.

---

## What a gap is

A gap is the second case: a message arrives with a higher number than expected.

Suppose the venue has sent messages 1 to 100 and the member has processed all of them, so it
expects 101 next. The connection drops. While it is down the venue sends 101 through 150 into a
socket that no longer works. The member reconnects, and the next thing it receives is numbered
151.

The member expected 101 and got 151. It does not know what was in 101 through 150 — it may not
even know that they exist — but it knows something is missing, because the numbering says so. It
must not simply carry on: 101 through 150 might be execution reports saying its orders filled.

Note what the member does **not** do: it does not discard 151. It holds it, asks for the gap to
be filled, and processes everything in order once it arrives.

---

## Asking for the missing messages: ResendRequest (35=2)

The member sends a `ResendRequest` with two fields:

- **`BeginSeqNo` (tag 7)** — the first message it wants.
- **`EndSeqNo` (tag 16)** — the last message it wants.

Three forms are permitted:

| what is wanted | BeginSeqNo | EndSeqNo |
|---|---|---|
| one message | n | n |
| a bounded range | first | last |
| **everything from here on** | first | **0** |

**`EndSeqNo=0` means infinity.** It is not a placeholder or a missing value; zero is the
protocol's way of writing "and everything after that". The specification recommends this form
for recovering from an out-of-sequence condition, because it is robust when both sides are
trying to recover at once — a bounded request can be overtaken by new messages and need
re-asking.

This is the detail most worth remembering, because a request for `100` to `0` looks malformed
if you do not know the convention, and a great deal of engine behaviour depends on it.

---

## Answering: resend, or fill the gap

The side that receives a `ResendRequest` has to account for **every** number in the range. It
cannot skip one silently, because the asker is counting. But it does not have to *resend* them
all. There are two ways to account for a number, and choosing correctly is the whole of the
protocol here.

### Resending a real message

Send the original message again, with:

- **`PossDupFlag` (tag 43) = `Y`** — "you may have seen this before". This is what makes a
  lower-than-expected number acceptable rather than a fatal error.
- **`OrigSendingTime` (tag 122)** — the time the message was *originally* sent. `SendingTime`
  (tag 52) is updated to now, so without tag 122 the receiver cannot tell when the event
  actually happened. It is required whenever `PossDupFlag=Y` and is the field implementations
  most often forget.
- The **same `MsgSeqNum`** it had the first time. A resent message keeps its original number.

### Filling the gap: SequenceReset-GapFill (35=4)

Some messages should not be resent. A Heartbeat from ten minutes ago is meaningless now, and
resending it tells the member nothing. The specification says administrative messages are
replaced rather than retransmitted.

But their numbers still have to be accounted for, or the member is left waiting. So the sender
emits a `SequenceReset` with:

- **`GapFillFlag` (tag 123) = `Y`**
- **`MsgSeqNum`** set to the first number being skipped
- **`NewSeqNo` (tag 36)** set to the next number the member should expect *after* the skip

Read it as: "numbers 105 to 109 were nothing you need; carry on from 110."

A reply to a resend request is therefore usually a mixture — real application messages resent
with `PossDupFlag=Y`, interleaved with gap-fills covering the administrative stretches between
them.

### The other SequenceReset: Reset mode

`SequenceReset` with `GapFillFlag=N` or absent does something different. It says "forget the
sequence, start counting from `NewSeqNo`", and the receiver accepts it regardless of what
number the reset itself carries.

It resolves any disagreement at once, by abandoning every message in between. Nothing records
what was in them. The specification restricts it to disaster recovery, when gap-fill cannot
work. **Using it to get past an awkward gap discards execution reports**, which for a venue
means a member never learning its order filled.

---

## A worked example

The venue has sent up to 104. The connection drops. During the outage the venue tries to send:

| number | message |
|---|---|
| 105 | Heartbeat |
| 106 | Heartbeat |
| 107 | ExecutionReport — order filled |
| 108 | Heartbeat |
| 109 | ExecutionReport — order cancelled |

The member reconnects and receives 110. It expected 105, so it sends
`ResendRequest BeginSeqNo=105 EndSeqNo=0`.

A correct answer is four messages:

| sent | what it is |
|---|---|
| `SequenceReset` MsgSeqNum=105, GapFillFlag=Y, NewSeqNo=107 | skips the two heartbeats |
| `ExecutionReport` MsgSeqNum=107, PossDupFlag=Y, OrigSendingTime=… | the fill, resent |
| `SequenceReset` MsgSeqNum=108, GapFillFlag=Y, NewSeqNo=109 | skips one heartbeat |
| `ExecutionReport` MsgSeqNum=109, PossDupFlag=Y, OrigSendingTime=… | the cancel, resent |

The member ends expecting 110, which it already holds, and both business events reached it.

Compare a single `SequenceReset MsgSeqNum=105 GapFillFlag=Y NewSeqNo=110` covering the whole
range. The session recovers, the numbering is consistent, and nothing appears wrong. The member
never learns its order filled. Nothing on the venue side reports this.

---

## What this venue does

`FixOrderGatewayThread::handle_resend_request`.

**The reports come from the sequencer's WAL, not from the gateway.** A gateway keeps no record
of what it has sent, and after a failover the reports in question may have been sent by a
different gateway instance entirely — so a gateway-local replay buffer would be empty in
exactly the case that matters. The WAL has every report stamped with the session it belongs to,
so the gateway asks the sequencer for that session's slice and replays it.

**Real reports are resent, not gap-filled.** This is the important half of the worked example
above, and it is done: application messages go back with `PossDupFlag=Y` and `OrigSendingTime`,
and only the administrative remainder is gap-filled when the replay finishes. An earlier version
answered every request with one blanket gap-fill; the comment in the code explains why that was
abandoned.

**`PossDupFlag` is scoped to the gap the member asked about.** A WAL slice can run past the
requested range, and reports beyond it were never sent — marking those as possible duplicates
would invite the member to discard news it is seeing for the first time. The code sets the flag
only below the resume point.

**A second ResendRequest during a replay is ignored** rather than restarting it, which would
re-issue numbers the first pass is still consuming.

---

## HA and the FIX session layer are separate mechanisms

Worth stating plainly, because the two touch the same session and are easy to conflate.

**FIX sequence numbers exist to recover from message loss on a session.** They are not a
failover mechanism and know nothing about one. A member counts what it receives and asks for
what it missed; that is all.

**HA exists to keep the venue serving when a process or machine dies.** It knows nothing about
FIX.

The rule that follows: **HA must be invisible to the FIX session layer.** A member reconnecting
after a failover must see an ordinary session that continued — the numbering carries on from
where it was, the gap is a normal gap, and a normal ResendRequest fills it. The member must not
have to know a failover happened, because nothing in FIX gives it a way to be told.

This constrains the HA design rather than the other way round:

- **Outbound numbering continues across a failover.** A promoted gateway resumes the session's
  numbering. If it restarted at 1 the member would see a large backwards jump, which is a
  session-fatal error.
- **The reports have to come from somewhere that survives.** A gateway holds no record of what
  it sent, and after a failover the reports may have been sent by a different instance. This is
  why the resend is served from the sequencer's WAL.
- **Two instances must never send to the same session at once.** Duplicate sequence numbers
  from separate senders is a state FIX has no way to describe, let alone recover from. This is
  what the arbiter and the epoch are for.

One asymmetry is worth knowing. FIX's duplicate handling covers *retransmission* — the same
message sent again with `PossDupFlag=Y`. It does not cover *reprocessing*: if HA were to replay
a report that had already been delivered, under a new sequence number, the member has no way to
recognise it as the same business event. Delivering each report once is the venue's
responsibility and not something the session layer can rescue.

---

## Where it departs from the specification

Three, in order of seriousness.

### 1. Inbound sequence numbers are not checked at all

`MsgSeqNum` on an inbound message is never compared against an expected value. There is no
expected-inbound counter, no gap detection on what the member sends, and the gateway never
sends a `ResendRequest` — the only `ResendRequest` code is the handler for receiving one.

The consequence is order loss that nobody observes. A member sends an order, the connection
drops before it arrives, the member reconnects and carries on numbering from the next value.
The venue was expecting the missing number and receives the one after it. Because nothing
checks, the order is processed as though nothing were missing. **The member believes it has an
order resting that the venue never received, and neither side has any reason to think
otherwise.** Detecting exactly this is what the numbering is for.

A message arriving with a *lower* number than expected is likewise accepted, where the
specification calls it a serious error.

### 2. `PossDupFlag` on inbound messages is ignored

`PossDupFlag` is only ever written, on the outbound resend path. It is never read.

A member recovering a gap of its own retransmits with `PossDupFlag=Y`, which means "you may
already have this". The gateway treats it as a new order and forwards it. What prevents a
duplicate order is the matching engine rejecting a repeated ClOrdID within a session — the
application layer catching a session-layer failure.

That backstop works, and it is the wrong answer twice over: the member receives a rejection for
an order that does in fact exist, and under load the rejections arrive in volume. A run has
been observed producing 132,000 duplicate-ClOrdID warnings from a client retransmitting orders
it had not been acknowledged.

### 3. `EndSeqNo` is not read on a ResendRequest

The handler parses `BeginSeqNo` and nothing else, so every request is treated as though
`EndSeqNo=0`.

For the case that actually happens this is correct, since a member recovering from a disconnect
sends 0 anyway. It stops being correct for a bounded request: a member asking for fifty messages
receives every report since, and because the slice is read from the sequencer's WAL, one small
request from one member becomes a large read on the path live traffic depends on.

See `docs/bug_list.md` for each of these.

---

## What is not tested

`ha_test.py` has one scenario, `resend_recovery`: the member reconnects, notices the gap, and
the venue answers with real execution reports rather than gap-filling them away. That covers the
single most important property and nothing else.

Untested, roughly in order of how much it would matter if it were wrong:

- **`PossDupFlag` correctness.** That it is `Y` inside the requested gap and absent beyond it.
  Wrong either way is silent: too few and the member rejects a resend as a sequence error, too
  many and it discards reports it has never seen.
- **`OrigSendingTime` present on every resent message.** A conformance failure that a tolerant
  engine will accept and a strict one will reject.
- **The terminating gap-fill.** That `MsgSeqNum` and `NewSeqNo` leave the member expecting
  exactly the number the venue will send next.
- **A bounded `EndSeqNo`**, which cannot pass today and should fail until the deviation above is
  settled one way or the other.
- **A duplicate ResendRequest mid-replay**, which the code handles deliberately and nothing
  checks.
- **`BeginSeqNo` beyond the current outbound number**, i.e. a member asking for messages that do
  not exist.
- **A resend spanning a gateway failover**, which is the case the WAL-backed design was chosen
  for and the one with no coverage at all.

The first three are checkable by capturing the wire bytes on the existing `resend_recovery`
scenario and asserting on tags, which needs no new venue behaviour — only assertions against
what is already produced.

---

## Sources

- [FIXT 1.1 Session Protocol](https://www.onixs.biz/fix-dictionary/fixt1.1/section_session_protocol.html)
- [ResendRequest (35=2)](https://www.onixs.biz/fix-dictionary/latest/msgType_2_2.html)
- [SequenceReset (35=4)](https://www.onixs.biz/fix-dictionary/latest/msgtype_4_4.html)
- [FIX Session Layer, FIX Trading Community](https://www.fixtrading.org/standards/fix-session-layer-online/)
