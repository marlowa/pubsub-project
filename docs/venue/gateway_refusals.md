# How a gateway answers a request to cancel that it refuses itself {#gateway_refusals}

**Status: built.** This is the design for [BUG-0072](../bug_list.md#bug_0072). The three decisions in section 8 were
agreed with Andrew on 10 November 2026.

## 1. Why a gateway keeps no list of a member's orders

A gateway could keep a list of every order its members have open, built from the matching engine's execution reports as
they pass back through it. The venue cancels a member's orders when its connection ends where the orders are, in the
matching engine, on the leading sequencer's instruction
([cancel_on_disconnect_where_the_orders_are.md](../availability/cancel_on_disconnect_where_the_orders_are.md)), so such
a list would serve two purposes only:

1. **Answering a refused request to cancel with an OrderCancelReject**, which FIX requires to carry the order's OrderID
   and status. The gateway could look the OrderID up in its list, but it would still not know the order's status: the
   list is not the book, and a status the gateway gave would be an assumption, wrong for an order that is unknown,
   filled or cancelled.
2. **Logging how many orders a session left** when it ends.

The list would cost memory in every gateway, held from start-up so that adding an entry never allocates on the thread
that handles orders, and work on the order path for every report that passes back. Sized so that a gateway is never the
first place the venue's capacity runs out, it would need the venue's own figure for open orders,
`order_book_region_capacity`: about 20 GB in each gateway in production. So the gateways keep no such list, and answer a
request they refuse as section 3 describes.

## 2. What other exchanges publish

The documents are kept in the project, in `external_documentation/`, whose README says where each came from.

**Who sends an OrderCancelReject, and what its status means.** In every exchange below, an OrderCancelReject comes from
the trading system, which holds the order:

- The London Stock Exchange's OrderCancelReject carries OrdStatus as the "current status of the order", which "will be
  Rejected (8) if the order is unknown or the request cannot be processed", and OrderID, which "will be 'NONE' if the
  order is unknown" (MIT202, section 6.4.6). It answers a cancel of a filled, cancelled or expired order with status
  Rejected (MIT202, section 2.10.3).
- Euronext's OrderCancelReject carries OrdStatus, mandatory, as "the current status of the order", and echoes back the
  order identifier the member sent (Optiq OEG client specifications, section 5.3.7).
- The FIX standard defines OrdStatus in an OrderCancelReject as the "OrdStatus value after this cancel reject is
  applied. If CxlRejReason = 'Unknown Order', specify Rejected" (FIX 5.0 SP2).

**How a message the gateway refuses is answered.** None of them answers with an OrderCancelReject:

| Exchange | A message refused before it reaches the trading system |
|---|---|
| Euronext | A session-level Reject (35=3). Its specification answers each kind of request in one of three ways: an acknowledgement, a "functional or technical rejection" (an OrderCancelReject, for a cancel), or a "session level rejection" (a Reject, for every kind of request). A message that is badly formed, missing a mandatory field, holds a value out of range, or is over the throttling limit, gets the Reject, its reason in SessionRejectReason, and no Order ID, because the matching engine never processed it (Optiq OEG client specifications, sections 4.8 and 4.10) |
| London Stock Exchange | A session-level Reject for a message missing a required tag (MIT202, section 2.9); a Business Message Reject (35=j) for a message over the user's rate limit, with the user disconnected if it exceeds the limit repeatedly (MIT201) |
| CME | A Business Message Reject (35=j) for messages over a session's messaging-control threshold |
| Deutsche Börse (T7 FIX LF) | A Business Message Reject (35=j); its message reference lists no OrderCancelReject |

## 3. The design

The venue answers as Euronext does.

**The gateways keep no list of open orders.** The pool, its configuration in every environment, the compiled-in
default, and the code that adds and removes entries as reports pass back, all go. BUG-0072 then has nothing left to
size.

**A request to cancel that a FIX gateway refuses itself is answered with a session-level Reject (35=3)**, never an
OrderCancelReject. It carries:

- RefSeqNum (45), the refused message's MsgSeqNum, and RefMsgType (372) = F, which identify the refused message;
- RefTagID (371), the tag at fault, where there is one, such as a missing field;
- SessionRejectReason (373), the kind of refusal;
- Text (58), the reason in words, naming the missing fields or the limit, as R-0143 and R-0150 require.

It says nothing about the order: no OrderID and no status, since the venue never processed the request.

| The gateway refuses a cancel because | SessionRejectReason |
|---|---|
| A required field is absent | 1, required tag missing, with the tag in RefTagID |
| `ClOrdID` or `OrigClOrdID` is longer than the venue accepts | 5, value is incorrect (out of range) for this tag, with the tag in RefTagID |
| A field holds a value the venue does not accept, such as an undefined Side | 5, with the tag in RefTagID |
| The session is over its limit on cancels | 100, over the throttle limit, naming the limit in Text |
| No sequencer is connected, the venue is not taking commands, or too many commands are waiting to be answered | 101, the venue cannot take the request now |
| The `ClOrdID` has been used before | 99, Other, with the text "ClOrdID already used today" |

A `ClOrdID` used before is found in one of two places: by the gateway, when a command with the same `ClOrdID` is still
waiting for its answer, and by the leading sequencer, which finds every earlier use in its WAL and sends the gateway a
CommandRefused message instead of writing the command. Either way the request has not reached the matching engine, and
the answer is the same Reject. A Reject must name the refused message by its MsgSeqNum, which the sequencer has no other
way of knowing, so the FIX gateway puts the MsgSeqNum of the member's message on the command's envelope
(`member_msg_seq_num`, optional, in `leader_follower.dsl`), and the sequencer returns it on CommandRefused (decision 3).

Values 1, 5 and 99 are the FIX standard's. It defines none for throttling or a busy venue, and its SessionRejectReason
accepts values from 100 agreed between the parties ("Reserved100Plus"), so the venue defines 100 and 101 and says so in
the FIX gateway's documentation. Euronext defines its own for the same purpose (25, "throttling queue full", 26,
"throttling rate exceeded", and 27, "system busy"); values from 100 cannot collide with a value the standard adds later.

The session stays open, as R-0150 requires, whatever the reason.

**The matching engine goes on answering a cancel it refuses with an OrderCancelReject**, through the gateway, with the
order's OrderID and status as the engine has them: OrderID "NONE" and status Rejected for an order it does not hold, as
FIX and the London Stock Exchange specify. With the gateway's own OrderCancelReject gone, every OrderCancelReject a
member receives carries a status the component holding the order has stated, and none carries the gateway's assumption.

**A new order the gateway refuses is unchanged**: an execution report that rejects it, with OrderID "NONE", which is
right, because such an order is not on the book.

**A session's end** is logged without a count of its orders. What the venue cancels is reported by the matching engine.

**The binary gateway answers the same way** (decision 2), with a new message in its own protocol, `RequestRefused`
(702, in `applications/binary_session.dsl`). It carries the refused request's ClOrdID, the message type of the refused
request, a reason code from the table above, the reason in text, and, where one field is at fault, that field's name.
`OrderCancelReject` (1003) is then sent only for the matching engine's refusals, as in FIX. Both gateways apply the same
limits to the same commands ([gateway_throttles.md](gateway_throttles.md)), and scenario 56 checks a burst of cancels
through each; this keeps their answers alike too. The binary protocol has no sequence numbers, so `RequestRefused` names
the request by its ClOrdID.

## 4. The requirement

R-0151, in the functional specification's applications chapter, states the rule: a gateway that refuses a request to
cancel before it reaches the matching engine answers with a reply that refuses the request and says nothing of the order,
in FIX a session-level Reject; the matching engine's refusal is an OrderCancelReject reporting the order's status. Its
purpose, that a member is never told its order is gone when the venue can still trade it, is what every exchange in
section 2 keeps: none answers a refused cancel with an execution report that rejects the order. The order's status is
said only by the component holding the order.

## 5. What a member sees

A FIX member whose cancel a gateway refuses receives a session-level Reject, which every FIX counterparty must handle,
for any message. A binary member receives `RequestRefused`. A cancel the matching engine refuses is answered with an
OrderCancelReject in both protocols, and a refused new order with an execution report that rejects it.

## 6. What it costs, and what it saves

Keeping no list saves memory in every gateway, about 20 GB each in production at the venue's figure, and the per-report
work of keeping the list on the order path. It removes a limit on the venue's capacity that would otherwise have to be
raised in step with the matching engine's and the sequencer's, and no reply carries a status the gateway assumed. It
costs a few bytes on each command record in the WAL, for the member's MsgSeqNum (decision 3).

## 7. How it is checked

- **Scenario 42**, with no matching engine running: a FIX cancel is answered with a Reject, SessionRejectReason 101,
  naming the cancel by its MsgSeqNum and carrying no OrderID or OrdStatus; a binary cancel with `RequestRefused`, reason
  101, and the same text. A cancel for an order the venue never held, once an engine runs again, is answered by the
  engine with an OrderCancelReject, CxlRejReason 1, OrdStatus Rejected and OrderID NONE.
- **Scenario 56**: a burst of FIX cancels beyond the session's limit is answered with Rejects, SessionRejectReason 100,
  naming the limit; a burst of binary cancels beyond the same limit with `RequestRefused`, reason 100, and the same text.
- **Scenario 57**: a binary cancel with an undefined Side is answered with `RequestRefused`, reason 5, naming the field.
- **Scenario 96**, on reused identifiers: a FIX cancel whose `ClOrdID` the sequencer finds in its WAL is answered with
  a Reject, SessionRejectReason 99, naming the cancel by the MsgSeqNum the sequencer returned; a binary one with
  `RequestRefused`, reason 99.
- **Scenarios 19 and 50**, on cancel on disconnect, count the orders the member was told are open, since the gateway
  keeps no count.
- Unit tests: `FixRejectionsTest`, `BinaryRejectionsTest` and `BinaryCommandChecksTest`.

## 8. Decisions

### Decision 1. Is R-0151 amended? (Yes or no.)

Agreed, **yes**, with Andrew on 10 November 2026. The amended text:

> Where a gateway refuses a member's request to cancel or amend an order, before it reaches the matching engine, it
> shall answer with a reply that refuses the request and says nothing of the order. In FIX, the reply shall be a
> session-level Reject naming the refused message. Where the matching engine refuses the request, the reply shall be an
> OrderCancelReject reporting the order's current status, which is Rejected for an order the venue does not hold.

### Decision 2. What does the binary gateway send when it refuses a request to cancel itself? (Choose one.)

- **A. A new message, `RequestRefused`**, the binary protocol's counterpart of the FIX Reject.
- **B. `OrderCancelReject` (1003), with no OrderID and no order status** when the gateway sends it.

Agreed, **A**, with Andrew on 10 November 2026: the two gateways must behave the same.

### Decision 3. How does the FIX gateway name a message the sequencer refused? (Choose one.)

When the leading sequencer refuses a cancel because its `ClOrdID` was used before, the gateway answers with a Reject,
which must carry the refused message's MsgSeqNum. Neither CommandRefused nor the gateway's copy of the command carried it,
and a gateway with high availability off keeps no copy.

- **A. The MsgSeqNum travels on the command's envelope**, as an optional field, and comes back on CommandRefused. It
  costs a few bytes on each command record in the WAL.
- **B. The gateway keeps the MsgSeqNum beside its copy of each unanswered command.** No change to the WAL, but it fails
  whenever the gateway keeps no copies.
- **C. This one case stays an OrderCancelReject**, which brings back a status the gateway does not know.

Agreed, **A**, with Andrew on 10 November 2026.
