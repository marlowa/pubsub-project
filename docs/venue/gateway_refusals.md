# How a gateway answers a request to cancel that it refuses itself {#gateway_refusals}

**Status: designed, not built.** This is the design for [BUG-0072](../bug_list.md#bug_0072), written for review before
any code. Section 8 lists the decisions it needs.

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
   OrderCancelReject, which FIX requires to carry the order's OrderID, and which R-0151 requires to report the order's
   status. The gateway looks both up in its list.
2. **When a session ends**, the gateway logs how many open orders it left.

Keeping the list costs memory in every gateway, held from start-up because building the pool writes every entry, and
work on the order path for every report that passes back. Sized so that a gateway is never the first place the venue's
capacity runs out, it would need the venue's own figure for open orders, `order_book_region_capacity`: about 20 GB in
each gateway in production. Removing it means the gateway cannot fill in the OrderID or the order's status, and FIX
defines OrderID "NONE" only for an order the venue does not know, which is not the case here: the venue knows the order;
only the gateway does not.

## 2. What other exchanges publish

The documents are kept in the project, in `external_documentation/`, whose README says where each came from.

| Exchange | How it answers a message its gateway refuses | Who sends an OrderCancelReject |
|---|---|---|
| London Stock Exchange | A Business Message Reject (35=j): for a message over the user's rate limit (MIT201, which also disconnects a user that exceeds it repeatedly), and for an Order Cancel Request missing a required field (MIT202, "Trader Group not specified on message") | The trading system, which fills in the OrderID (MIT202) |
| Euronext | A session-level Reject (35=3) for a message over the throttling limit. A message rejected for technical reasons is not processed by the matching engine, so no Order ID is provided in the rejection (Optiq OEG client specifications) | The trading system |
| CME | A Business Message Reject (35=j) for messages over a session's messaging-control threshold | The trading system |
| Deutsche Börse (T7 FIX LF) | A Business Message Reject (35=j); its message reference lists no OrderCancelReject | None in that interface |

In every case, **an OrderCancelReject comes from the trading system, which holds the order and knows its OrderID and
status**, and **a refusal at the gateway is a different message, which names the refused message and carries no
OrderID**. The FIX standard defines OrderCancelReject as the answer to a request to cancel "which cannot be honored",
with OrderID required and "NONE" only for an unknown order (FIX 5.0 SP2).

## 3. The design

**The gateways keep no list of open orders.** The pool, its configuration in every environment, the compiled-in
default, and the code that adds and removes entries as reports pass back, all go. BUG-0072 then has nothing left to
size.

**A request to cancel that a FIX gateway refuses itself is answered with a Business Message Reject (35=j)**, never an
OrderCancelReject. The FIX gateway already sends one for other refusals (`FixOrderGatewayThread::send_business_reject`).
It carries:

- RefSeqNum (45), the refused message's MsgSeqNum, and RefMsgType (372) = F;
- BusinessRejectRefID (379), the refused request's ClOrdID, so that the member can match the answer to its request by
  the identifier it chose, as R-0143 requires;
- BusinessRejectReason (380), saying why: the value FIX defines for each case below, or 0 (Other) where none fits, to be
  checked against the FIX 5.0 SP2 code list when this is built;
- Text (58), the reason in words, naming the absent fields or the limit, as R-0143 and R-0150 require.

| The gateway refuses a cancel because | Reason |
|---|---|
| A required field is absent | the fields named in Text |
| `ClOrdID` or `OrigClOrdID` is longer than the venue accepts | the length named in Text |
| No sequencer is connected, or the venue is not taking commands | application not available |
| The session is over its limit on cancels | throttle limit exceeded, naming the limit |
| Too many commands are waiting to be answered | application not available |
| The `ClOrdID` has been used before | Other, "identifier already used" |

**The matching engine goes on answering a cancel it refuses with an OrderCancelReject**, carrying the order's OrderID
and status, or OrderID "NONE" and the reason "unknown order" for an order it does not hold, as FIX defines.

**A new order the gateway refuses is unchanged**: an execution report that rejects it, with OrderID "NONE", which is
right, because such an order is not on the book.

**A session's end** is logged without a count of its orders. What the venue cancels is reported by the matching engine.

**The binary gateway** speaks the venue's own protocol, in which `OrderCancelReject` (1003) is FIX's 35=9 with the same
fields. Decision 2 says what it sends instead.

## 4. The requirement this changes

R-0151 says, in full: "Where a gateway refuses a member's request to cancel or amend an order, it shall answer with a
reply that refuses the request, and shall not answer with a reply that rejects the order. The reply shall report the
order's current status. In FIX, the reply shall be an OrderCancelReject."

Its rationale is that a member sent a reply rejecting the order believes it has no order in the market while the venue
can still trade it. A Business Message Reject keeps to that: it refuses the request, says nothing of the order, and
leaves the order as the member last knew it. What the design cannot meet are the last two sentences: a gateway that
keeps no list does not know the order's status, and the reply is not an OrderCancelReject. Decision 1 is the amended
text.

## 5. What a member sees

A FIX member whose cancel a gateway refuses receives a Business Message Reject instead of an OrderCancelReject. Every
FIX counterparty must handle a Business Message Reject, for any message, so this is a message it already understands,
but it is a change, and a member that handles only an OrderCancelReject in answer to a cancel would need to handle this
too. Nothing changes for a cancel the matching engine refuses, or for a refused new order.

## 6. What it costs, and what it saves

It saves the pool in every gateway: about 20 GB each in production at the venue's figure, and the per-report work of
keeping the list on the order path. It removes a limit on the venue's capacity, which would otherwise have to be raised
in step with the engine's and the sequencer's. It costs the change a member sees (section 5), and the changes to the
documents and scenarios in section 7.

## 7. Steps, and how each is known to be right

1. Scenarios first, each failing against the code as it is: a cancel throttled at the FIX gateway is answered with a
   Business Message Reject naming the request's ClOrdID and the limit; a cancel with a required field absent is
   answered with one naming the field; and the same for the binary gateway, by decision 2.
2. The FIX gateway's refusals of a cancel become Business Message Rejects; the binary gateway's, by decision 2.
3. The list goes: the pool, its configuration, and the per-report bookkeeping, in both gateways.
4. R-0151 amended, by decision 1; `gateway_throttles.md`, `fix_order_gateway.md`, `binary_order_gateway.md` and the
   gateway HA document say what the gateways now send; scenarios 42 and 56, which expect an OrderCancelReject from the
   gateway, expect a Business Message Reject.
5. The full high-availability suite, with the WALs cleared first.

## 8. Decisions

### Decision 1. Is R-0151 amended as below? (Yes or no.)

> Where a gateway refuses a member's request to cancel or amend an order, before it reaches the matching engine, it
> shall answer with a reply that refuses the request and does not reject the order. In FIX, the reply shall be a
> Business Message Reject naming the request by its ClOrdID. Where the matching engine refuses the request, the reply
> shall be an OrderCancelReject reporting the order's current status.

Recommended: **yes**. It keeps the requirement's purpose, that a member is never told its order is gone when it is not,
and puts each answer where the exchanges in section 2 put it: the gateway refuses the message, and only the component
holding the order speaks about the order.

### Decision 2. What does the binary gateway send when it refuses a request to cancel itself? (Choose one.)

- **A. A new message, `RequestRefused`**, the binary protocol's counterpart of a Business Message Reject: the refused
  request's ClOrdID, a reason code and the reason in text. `OrderCancelReject` (1003) is then sent only by the matching
  engine, as in FIX.
- **B. `OrderCancelReject` (1003) as now, with no OrderID and no order status** when the gateway sends it. Both fields
  would become optional in the message, and a member would have to tell a gateway's refusal from the engine's by their
  absence.

Recommended: **A**. The two gateways then answer alike (R-0144's purpose for requests applies as much to their answers),
and a binary member is never sent a message that looks like a statement about its order but holds none.
