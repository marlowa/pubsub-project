# How a gateway answers a request to cancel that it refuses itself {#gateway_refusals}

**Status: agreed, not built.** This is the design for [BUG-0072](../bug_list.md#bug_0072). Both decisions in section 8
were agreed with Andrew on 10 November 2026.

## 1. The problem

Each gateway keeps a list of every order its members have open, built from the matching engine's execution reports as
they pass back through it: an entry is added when an order is accepted, and removed when a report ends it. The entries
come from a pool reserved at start-up, which grows by allocating more memory on the thread that handles orders when it
runs out. Its size is configured per environment, and the sizes were chosen by nobody: 4,096 in production, preprod and
test-1, which runs out at once, and from 65,536 to about 22 million in development ([BUG-0072](../bug_list.md#bug_0072)).

The list was kept so that the gateway could cancel a member's orders when its connection ended. Since
[BUG-0090](../bug_list.md#bug_0090) the venue does that, where the orders are
([cancel_on_disconnect_where_the_orders_are.md](../availability/cancel_on_disconnect_where_the_orders_are.md)). The list
is now used for two things only:

1. **When a gateway refuses a member's request to cancel itself**, before it reaches the venue, it answers with an
   OrderCancelReject, which FIX requires to carry the order's OrderID and its status. The gateway looks the OrderID up in
   its list, and reports the status as New (`39=0`) whatever the order's state (`FixRejections.cpp`): it has never known
   the order's status, and states an assumption, which is wrong for an order that is unknown, filled or cancelled.
2. **When a session ends**, the gateway logs how many open orders it left.

Keeping the list costs memory in every gateway, held from start-up because building the pool writes every entry, and
work on the order path for every report that passes back. Sized so that a gateway is never the first place the venue's
capacity runs out, it would need the venue's own figure for open orders, `order_book_region_capacity`: about 20 GB in
each gateway in production.

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
| A required field is absent | required tag missing, with the tag in RefTagID |
| `ClOrdID` or `OrigClOrdID` is longer than the venue accepts | value incorrect for the tag, with the tag in RefTagID |
| The session is over its limit on cancels | throttling, naming the limit in Text |
| No sequencer is connected, the venue is not taking commands, or too many commands are waiting to be answered | the venue is busy |
| The `ClOrdID` has been used before | other, "identifier already used" |

The values come from the FIX 5.0 SP2 code list where it has one; where it has none (throttling, the venue busy), the
venue defines one, as Euronext does (its values 25, "throttling queue full", 26, "throttling rate exceeded", and 27,
"system busy"), and says so in the FIX gateway's documentation.

The session stays open, as R-0150 requires, whatever the reason.

**The matching engine goes on answering a cancel it refuses with an OrderCancelReject**, through the gateway, with the
order's OrderID and status as the engine has them: OrderID "NONE" and status Rejected for an order it does not hold, as
FIX and the London Stock Exchange specify. With the gateway's own OrderCancelReject gone, every OrderCancelReject a
member receives carries a status the component holding the order has stated, and none carries the gateway's assumption.

**A new order the gateway refuses is unchanged**: an execution report that rejects it, with OrderID "NONE", which is
right, because such an order is not on the book.

**A session's end** is logged without a count of its orders. What the venue cancels is reported by the matching engine.

**The binary gateway answers the same way** (decision 2), with a new message in its own protocol, `RequestRefused`,
carrying the refused request's ClOrdID, a reason code from the same list and the reason in text.
`OrderCancelReject` (1003) is then sent only for the matching engine's refusals, as in FIX. Both gateways already apply
the same limits to the same commands ([gateway_throttles.md](gateway_throttles.md), scenario 56); this keeps their
answers alike too.

## 4. The requirement this changes

R-0151 read: "Where a gateway refuses a member's request to cancel or amend an order, it shall answer with a reply that
refuses the request, and shall not answer with a reply that rejects the order. The reply shall report the order's current
status. In FIX, the reply shall be an OrderCancelReject."

Its purpose, that a member is never told its order is gone when the venue can still trade it, is what every exchange in
section 2 keeps: none answers a refused cancel with an execution report that rejects the order. What changes is who may
speak about the order. A gateway that refuses a request before the venue sees it says only that the request was refused;
the order's status is said by the component holding the order. The amended text is decision 1.

## 5. What a member sees

A FIX member whose cancel a gateway refuses receives a session-level Reject instead of an OrderCancelReject. Every FIX
counterparty must handle a Reject, for any message, but it is a change, and a member that handles only an
OrderCancelReject in answer to a cancel would need to handle this too. A binary member receives `RequestRefused` instead
of `OrderCancelReject`. Nothing changes for a cancel the matching engine refuses, or for a refused new order.

## 6. What it costs, and what it saves

It saves the pool in every gateway, about 20 GB each in production at the venue's figure, and the per-report work of
keeping the list on the order path; it removes a limit on the venue's capacity, which would otherwise have to be raised
in step with the engine's and the sequencer's; and it removes the gateway's assumed status. It costs the change a member
sees (section 5), and the changes to the documents and scenarios in section 7.

## 7. Steps, and how each is known to be right

1. Scenarios first, each failing against the code as it is: through each gateway, a throttled cancel is answered with a
   Reject (FIX) or `RequestRefused` (binary) naming the limit, and a cancel with a required field absent with one naming
   the field; the session stays open in both.
2. The FIX gateway's refusals of a cancel become session-level Rejects; the binary gateway gains `RequestRefused`.
3. The list goes: the pool, its configuration, and the per-report bookkeeping, in both gateways.
4. R-0151 amended; `gateway_throttles.md`, `fix_order_gateway.md`, `binary_order_gateway.md` and the gateway HA document
   say what the gateways now send; scenarios 42 and 56, which expect an OrderCancelReject from the gateway, expect a
   Reject or `RequestRefused`.
5. The full high-availability suite, with the WALs cleared first.

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
