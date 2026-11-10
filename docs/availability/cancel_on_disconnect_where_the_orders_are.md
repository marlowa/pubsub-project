# Cancel on disconnect, carried out where the orders are {#cancel_on_disconnect_where_the_orders_are}

**Status: agreed, not built.** This is the design for [BUG-0090](../bug_list.md#bug_0090) and
[BUG-0091](../bug_list.md#bug_0091). All three decisions in section 7 were agreed with Andrew on 9 November 2026.

## 1. The problem

A member can ask, at logon, for its resting orders to be cancelled if its connection ends, and how long the venue should
wait first (the grace period). Today the gateway carries this out from its own view of the session's open orders,
`FixSession::open_orders`, which it builds only from the execution reports that pass through it:

- **a restarted gateway** holds an empty view for every order placed before the restart, so when the member later
  disconnects it cancels nothing, and logs nothing ([BUG-0090](../bug_list.md#bug_0090));
- **a gateway that dies** carries out nothing at all for the sessions it held, which is R-0113's case;
- **a member that reconnects within its grace period** keeps its orders, but the gateway releases its record of them
  rather than attaching them to the new session, so when that session drops it cancels none of them;
- **the instructions themselves** are held only by the gateway that received them, so a restarted gateway applies the
  venue's default instead of what the member asked for ([BUG-0091](../bug_list.md#bug_0091), R-0103).

What the gateway does now, and which any design must keep: after an unexpected drop it waits the grace period, so that
a member that comes back in time keeps its orders; after a clean Logout, or with a grace period of zero, it cancels at
once; and it never cancels a GoodTillCancel or GoodTillDate order, which the member placed to outlive the session.

## 2. What is already decided

- **Where the instructions are kept** ([session_binding.md](session_binding.md)): they travel with the session's sequence
  numbers. `SessionBound` carries them to the sequencer, and `SessionBoundAck` returns them to a gateway taking the session
  on. "The member said nothing" and "the member said no" must stay different states in both messages.
- **Who carries out the cancel** (the BUG-0090 entry and `docs/book`): the component that owns the orders, the matching
  engine, which keys its book on the session, so what a session has resting is known there and survives a restart in its
  region. The gateway reports the disconnection and the terms; it does not walk a view of its own.

## 3. The design

### 3.1 The sequencer is told, or notices, that a session ended

- **A gateway that is running** sends the sequencer `SessionEnded`: the session, and whether the member logged out
  cleanly. It no longer cancels anything itself.
- **A gateway that dies** sends nothing, but the leading sequencer sees its connection lost, and treats every session bound
  through that gateway as ended without a clean Logout. That is R-0113's case, and it is the reason the sequencer, not the
  gateway, has to do this.

### 3.2 The sequencer waits the grace period, durably

The sequencer holds each session's instructions (3.4). When a session ends, it writes a record into the WAL saying so,
with the time and the grace period that applies: none after a clean Logout or with a grace period of zero, otherwise the
member's or the venue's. Written into the WAL, the pending cancel survives a change of leader: a sequencer that takes
the lead finds it in its own WAL and keeps waiting from the time recorded.

If the member binds again before the grace period runs out, through any gateway, the pending cancel is withdrawn, and
that is recorded too. Otherwise, when it runs out, the leader writes a command, `CancelSessionOrders`, naming the session.

### 3.3 The matching engine cancels the session's orders

`CancelSessionOrders` is sequenced like any other command. The engine applying it cancels every order the session has
resting except GoodTillCancel and GoodTillDate orders, and sends an execution report for each, which reaches the member
when it next connects, as reports do now. Because it is a command in the WAL, both engines apply it the same way, a
catch-up replays it, and nothing depends on a timer in the engine.

### 3.4 Where the instructions come from

As decided: the authentication service gives them to the gateway at logon, the gateway sends them on `SessionBound`, the
sequencer keeps them with the session's numbers, and `SessionBoundAck` returns them to a gateway taking the session on. The
sequencer keeps them in the WAL with the session's state, so a new leader has them; whether that needs its own record or
fits an existing one is to be settled when this is built.

### 3.5 What the gateway keeps

The gateway's view of open orders is no longer used for cancel on disconnect. Whether anything else uses it is to be
checked when this is built; if nothing does, it goes, with the per-order cancels the gateway sends today at disconnect.

## 4. What this costs

Two small records in the WAL for each session that ends with orders resting, and a third if the member comes back in
time; one command for each session cancelled, instead of one cancel for each order. A timer in the leading sequencer for
each pending cancel, checked on its housekeeping tick.

## 5. What it does not change

The grace period's meaning, the exclusion of GoodTillCancel and GoodTillDate orders, what a member is sent, and the
binary gateway's and the FIX gateway's behaviour from the member's side, apart from now being carried out after a
gateway restart or death.

## 6. Steps, and how each is known to be right

1. Scenarios first, each failing against the code as it is: a member with cancel on disconnect places orders, the
   gateway is restarted, the member disconnects, and its orders must be cancelled after the grace period (BUG-0090); and
   the same with the gateway killed instead (R-0113); and a member that asked to keep its orders, with the gateway
   restarted, must keep them (BUG-0091).
2. The instructions on `SessionBound` and `SessionBoundAck`, with "said nothing" and "said no" kept apart.
3. `SessionEnded`, the gateway-death case, the records in the WAL and the grace timer in the sequencer, with unit tests
   of the timing: cancelled at once after a clean Logout; after the grace period after a drop; not at all if the member
   binds again in time; and kept across a change of leader.
4. `CancelSessionOrders` in the engine, with unit tests that it leaves GoodTillCancel and GoodTillDate orders and cancels
   the rest, and the replay check.
5. The scenarios in step 1 pass, and the full high-availability suite.

## 7. Decisions

### Decision 1. Does the leading sequencer hold the grace period and write the cancel as a command? (Yes or no.)

Agreed: **yes**. Only the sequencer is running in every case that matters, it already learns of every session from
`SessionBound`, and writing both the pending cancel and the cancel itself into the WAL keeps them through a change of
leader and makes the engine's action deterministic. The alternative, a timer in the engine, would have to be reproduced on
replay, which the engine's design does not allow for timers of its own.

### Decision 2. Is a gateway's death treated as every session it held ending without a clean Logout? (Yes or no.)

Agreed: **yes**. It is what R-0113 asks for, and the grace period still protects a member whose gateway died: it
reconnects through the other gateway within the grace period and keeps its orders.

### Decision 3. One `CancelSessionOrders` command for each session, rather than one cancel for each order? (Yes or no.)

Agreed: **yes**. The engine knows the session's orders; the gateway, after a restart or death, does not. One command
also replaces the burst of per-order cancels a large session produces today.
