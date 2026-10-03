# Gateway High Availability {#gateway_ha}

How a member keeps trading when a gateway dies, and what it does and does not get back.

This document covers the order-entry gateways only. Sequencer and matching engine high
availability are in [WAL and High Availability](wal_and_ha.md).

The arrangement in one paragraph: each gateway protocol, FIX and binary, runs as two instances,
`a` and `b`. Each member is provisioned to a primary and a backup instance and may log on to
either, but no other. A member's session is identified by its comp id and protocol, not by the
connection it happens to be using, so a member that reconnects, to the same instance or to its
backup, finds its orders still its own, can cancel them, and receives their reports there. A FIX
member continues its message numbering across the reconnect and can ask for the execution reports
it missed. When a member's connection drops, its orders are held for a grace period before they
are cancelled, so that a reconnect cancels nothing.

---

## What a member experiences when things fail

*Written for a reader who does not know FIX. Everything in the tables is behaviour the venue has
been observed to produce, and the last column says which `ha_test.py` scenario checks it.*

### Two facts about FIX that govern everything else

**1. Every message to a member is numbered, and the two directions of error are not alike.**

A FIX session numbers each message it sends: 1, 2, 3, and so on. The member tracks what it
expects next. Two things can go wrong, and their consequences are very different:

| | What the member does |
|---|---|
| The venue's number is **higher** than expected | Assumes it missed messages. Asks for them with a **ResendRequest**. Recoverable. |
| The venue's number is **lower** than expected | Treats it as a broken session. **Disconnects.** Not recoverable. |

This is the most important thing to know here. When the venue is unsure where a session had
reached, it must guess **high**. Guessing high costs the member a round trip; guessing low ends
the session. Much of the design below is that rule, applied.

**2. FIX has no "disconnect" message.**

The session-layer messages are Logon, Logout, Heartbeat, TestRequest, ResendRequest, Reject and
SequenceReset, and that is all. A polite shutdown is a *Logout*; a process being killed is just a
TCP connection closing, carrying nothing.

So when a gateway is killed, no message is produced and nothing appears in a message blotter. That
is not a fault: there is nothing to show. It is also why nothing in the venue relies on a dying
process to tell anyone anything. A gateway reports the state the venue needs from it continuously,
not only when it shuts down.

### What each failure looks like from the member's seat

| What fails | What the member sees | What its orders do | Checked by |
|---|---|---|---|
| **Its own connection drops**, the gateway survives | Nothing until it reconnects | Held, not cancelled, for a grace period (30 seconds by default, set per member). If it reconnects inside the period, **nothing is cancelled** | Scenario 19 |
| **The gateway process is killed** | The connection closes, with no message (fact 2) | **Stay live on the book.** The process that would have cancelled them is the one that died | Scenarios 18 and 23 |
| It **reconnects to its backup instance** | A normal Logon, numbered where the session had reached, not restarted at 1 | Still live, and still its own | Scenario 23 |
| It **cancels an order it left resting**, from the new connection | A cancel report, on the new connection | Cancelled | Scenario 21, and `OrderKeyTest` for the key |
| It **asks for what it missed** | Its execution reports, sent again with `PossDupFlag=Y`, and a gap fill for the administrative messages | Unchanged | Scenario 22 |
| It logs on to an instance it is **not provisioned for** | Refused, with text naming the instance it should use | Untouched | Scenario 20 |
| **The matching engine** fails over | Cancel reports for its resting orders, on whichever connection it now has | Cancelled by the promoted engine, and the member is **told** | Scenarios 16 and 21 |

### Orders that were in flight when the gateway died

"In flight" covers five different situations, and they do not share a fate:

| Where the order was | What becomes of it |
|---|---|
| 1. Still in the member's socket, unread | **Gone.** It never reached the venue |
| 2. Read by the gateway, not yet forwarded | **Gone.** It died with the process |
| 3. Forwarded, on its way to the sequencers | Usually arrives and is sequenced |
| 4. Sequenced, no report produced yet | **A live order** |
| 5. Report produced, addressed to the dead gateway | **A live order**, and a report that could not be delivered |

Cases 4 and 5 are the ones that matter: the order is real, resting on the book, and the member has
not heard of it. Its reports are in the sequencer's log, tagged with the member's session. A
report the sequencer forwards while the member is connected somewhere is recoverable with a
ResendRequest, because it was given a number in the member's sequence. A report produced while
the member was connected nowhere is not given a number, so the member sees no gap and has nothing
to ask for, and nothing sends it the report when it returns. That is
[BUG-0088](../bug_list.md#bug_0088), and requirement R-0005 in the functional specification says
what must happen instead.

**Cases 1 and 2 are not recoverable, and cannot be made so.** Nothing distinguishes an order that
died in a socket from one that was never sent; that is what "in flight" means. The FIX answer is
for the member to ask the venue what it holds, with an Order Status Request or an Order Mass Status
Request, and this venue answers neither ([BUG-0089](../bug_list.md#bug_0089)). Until it does, a
member cannot fully reconcile its orders after a gateway death.

A change of *sequencer* leader is a different failure, with its own losses; see
[change_of_sequencer_leader.md](change_of_sequencer_leader.md).

### What this venue does not do

Each of these is a real limit, and each is recorded:

- **No order status enquiry.** A member cannot ask what the venue holds for it
  ([BUG-0089](../bug_list.md#bug_0089)).
- **A report produced while a member is connected nowhere is not delivered when it returns**
  ([BUG-0088](../bug_list.md#bug_0088)).
- **Only execution reports can be sent again.** They are the only messages to a member that are in
  the sequencer's log; administrative messages are gap-filled, which FIX permits. There is no
  separate store of what was sent to each member.
- **How far back a member can recover depends on the sequencer's log.** Nothing deletes the log
  today ([BUG-0048](../bug_list.md#bug_0048)), so the limit is disk space rather than a retention
  policy. A production venue would keep a store of each trading day's outbound messages.
- **The remembered sequence positions do not survive a restart of the sequencer pair.** They are
  held in the sequencers' memory.
- **The binary gateway cannot send reports again.** Its protocol has no session layer to build a
  resend on ([BUG-0046](../bug_list.md#bug_0046)).
- **A restarted gateway stops honouring cancel-on-disconnect** for the sessions it held before it
  died ([BUG-0090](../bug_list.md#bug_0090)), and more generally a member's standing instructions
  die with the gateway that received them ([BUG-0091](../bug_list.md#bug_0091)).
- **No defined way for a member to discover that its primary is down**
  ([BUG-0045](../bug_list.md#bug_0045)).
- **A comp id may hold a session only once venue-wide, and that is not enforced.** The sequencer
  logs when an identity binds while it is already bound elsewhere; refusing the second session is
  recorded in the [roadmap](../roadmap.md), under gateway availability, fairness and identity.
- **Disaster recovery is not modelled** ([BUG-0047](../bug_list.md#bug_0047)).

---

## The two decisions this rests on

**Sessions are pinned to a primary and a backup gateway instance.** A member's session is
provisioned against two named instances and may log on to either; it may not land on a third. This
follows how venues provision order entry: Eurex ETI partitions, CME iLink market-segment gateways
and LSE-lineage native connectivity all assign sessions to gateways when the member is provisioned,
rather than letting them float.

*Considered and rejected: any-of-N pooling,* in which a member may reconnect to any gateway. It
requires every gateway to be able to serve any session's recovery state, which is a distributed
state problem. A primary and a backup requires only that one nominated peer can, which is a
replication problem between two known endpoints. The second is tractable; the first is a different
project.

**Execution reports in flight survive the reconnect.** A member that reconnects, to its primary or
to its backup, is brought up to date: reports for orders it placed earlier reach it on the new
connection, and reports it missed can be asked for. This is the expensive decision, and most of
what follows is about paying for it.

---

## How it works

### Instance identity on every order

Every order envelope (`WalRecord`) carries two fields that together say which gateway it came
from:

- `origin_gateway_id` — which *protocol*: `fix_order_gateway = 1` or `binary_order_gateway = 2`
  (`GatewayIds.hpp`).
- `gateway_instance_id` — which *instance* of that protocol, numbered from 1.

Each gateway process carries its `gateway.instance_id` in its configuration and stamps both
fields on every order it sends. Both fields are optional in the message definition, because not
every `WalRecord` came from a gateway: replication records and some execution reports have no
gateway origin, and an absent field says so.

Two fields rather than one number, because protocol and instance are separate axes, and because a
record then describes itself: the sequencer can choose a report's encoding, and a person reading
the log can see which gateway an order came from, without consulting configuration.

*Considered and rejected: attributing an order from the connection it arrived on,* with an
announcement message sent when a gateway connects. The sequencer accepts every gateway on one port
and cannot otherwise tell instances apart. Since every order already carries its origin, the
announcement would only stop a gateway misreporting its own identity, a weak argument between the
venue's own components, at the cost of a protocol addition and state per connection. The one
benefit worth keeping, a point at which a misconfiguration is noticed, is kept without it: the
sequencer logs an error once for each `(protocol, instance)` pair it receives orders from and has
no configured endpoint for, naming the pair and the likely cause.

### The sequencer's gateway endpoints

Each sequencer's configuration has a `[[gateway]]` entry for every gateway instance, each with
`protocol`, `instance`, `host`, `port` and `enabled`. A duplicate pair is refused when the
configuration is loaded. The sequencer opens a connection to every enabled entry and sends each
execution report to the instance where the member's session is now bound.

The development environment runs all four: `fix_order_gateway_a` and `_b`, and
`binary_order_gateway_a` and `_b`. The other environment files enable only `fix_order_gateway_a`;
the other three are present with `enabled = false`, so adding an instance is a configuration change
rather than a template edit.

Instances are named `a` and `b`, not primary and secondary, because nothing elects a gateway: a
member chooses which instance to connect to. The suffix is on the component name and its
configuration file; there is one program and one `etc/` directory per protocol.

### Session provisioning

Each comp id in the database has a `primary_gateway_instance` and a `backup_gateway_instance`.
They reach the gateway by the same path as the member's credentials: database,
`db/export_credentials.py`, `credentials.toml`, the authentication service, and the
`AuthenticationResult` message at logon. The administration service edits them on the comp-id
form. Both gateways check them once, when authentication succeeds, and refuse a logon at an
instance the session is not provisioned for.

Four decisions, each with a plausible alternative:

- **They name an instance, not a protocol.** Instance 1 of the FIX gateway and instance 1 of the
  binary gateway hold the same position in their own protocols, so a member's pinning applies to
  whichever protocol it speaks. Pinning `(protocol, instance)` pairs would be truer to how a venue
  partitions, but it would put protocol knowledge into the authentication service, which must have
  none, and it buys a restriction nothing has asked for.
- **Not pinned means any instance, and is the default.** Both columns may be empty, and empty
  means the member expressed no preference. A venue that wants pinning to be mandatory provisions
  its members; an unset column does not mean "refused".
- **One backup, not a list.** This matches what venues publish, but the reason is structural: the
  point of pinning is that only one nominated peer must be able to serve a session's recovery
  state, and a third would make the session's sequence position and recovery consistent in three
  places instead of two. Disaster recovery is not that third backup; at a real venue it is a
  separate site with its own sequence regime.
- **The refusal is its own outcome.** The binary protocol answers `LogonOutcome::NotProvisionedForInstance`
  rather than `AuthenticationFailed`, and the FIX gateway's Logout says, for example,
  `Session not provisioned for gateway instance 1 -- use instance 2`. The credential was good, and
  saying otherwise would send the member off changing a password that was never the problem. The
  check runs after the authentication service has proved itself with its `ServerSignature`,
  because only then is the provisioning trustworthy.

A credential that names a backup and no primary makes the authentication service refuse to start,
naming the entry.

### Session identity, and where a report goes

**A session is identified by `(comp id, protocol)`.** The instance is not part of it, because a
failover moves a session between instances of one protocol. The protocol is, because a FIX and a
binary session under one comp id are two sessions and must not share a book or each other's
reports.

The sequencer keeps two maps rather than one:

- sequence number of an order → the session that placed it;
- session → where that session can be reached now (instance and connection).

The destination is looked up when a report is sent, not remembered when the order arrived, so a
member that has reconnected while a report was on its way still receives it.

The gateways tell the sequencer where each session is with `SessionBound`, sent when a session is
established, and `SessionUnbound`, sent when it goes away. The sequencer cannot work this out for
itself: a member that reconnects and sends no order would never announce itself. `SessionUnbound`
carries the connection id and is ignored if it does not name the current binding, so an unbind
that arrives after the reconnect it raced with cannot undo it.

Three consequences:

- **The matching engine's book is keyed on the session identity** and the `ClOrdID`
  (`OrderKey`), which is what lets a member cancel, from a new connection, an order it left resting.
- **`BookUpdate` replication carries the identity,** not a connection. On promotion, any connection
  in the replica would name the process whose death caused the promotion.
- **The matching engine stamps whose report it is, never where it goes.** The sequencer resolves
  the destination. The engine has no way to know where a member is, and on the cancel-on-failover
  path any address it remembered would be out of date by construction.

### Keeping a FIX member's message numbering across a reconnect

A reconnecting FIX member continues its numbering rather than starting again at 1. The sequencer
hands the session's outbound sequence number back to the gateway in `SessionBoundAck`. It cannot
count that number itself, because it covers every message sent to the member, including heartbeats
and rejects that never reach the sequencer, so the gateway reports it.

Because a gateway can die without warning, three things work together, each covering another's
blind spot:

- The gateway reports its outbound sequence number **periodically**, in `SessionSequenceUpdate`, as
  well as in `SessionUnbound`, so an abrupt death leaves a recent figure rather than nothing. The
  sequencer keeps the highest figure it has been sent and never lowers it.
- The sequencer **counts the execution reports it forwards** to each session after that figure. It
  resolves a destination for every report already, so the count is exact, and reports are most of
  what a member is sent.
- On an **unclean** rebind, recognised because the identity is still bound when the new
  `SessionBound` arrives, it resumes at the highest figure, plus the reports it has forwarded since,
  plus a small allowance for administrative messages it cannot see.

Every unknown is resolved **upward on purpose**: overstating leaves a gap the member closes with a
ResendRequest, which is answered with the real reports and a gap fill for the rest; understating
ends the session (fact 1).

A member that logs on with `ResetSeqNumFlag=Y` is asking to start again at 1, and by its own account
has nothing missing, so the venue does as it asks. Ignoring the flag would lock the two sides into a
resend loop neither could end.

### Sending a FIX member the reports it missed

When a member sends a ResendRequest, the gateway asks the sequencer for the session's execution
reports (`SessionReplayRequest`), sends them again with `PossDupFlag=Y` and `OrigSendingTime`, and
gap-fills only the administrative messages in the range, which is the split FIX prescribes.

- **The gateway asks for exactly as many reports as the gap the member described,** and the
  sequencer returns the session's most recent reports up to that number. What a member has missed
  is the end of its stream, not the beginning. A ResendRequest's `BeginSeqNo` is a number in the
  member's sequence and the log is numbered by the venue's own sequence, so there is no mapping
  between them; asking by width avoids needing one. It is exact when the gap is all execution
  reports, and when it is not, the remainder is gap-filled.
- **Only reports inside the gap are marked `PossDupFlag=Y`.** A resent report carries a lower
  sequence number than the member expects, and FIX requires a member to treat that as fatal unless
  the flag is set, so the flag is not decoration.
- **The range is answered in one pass,** and a second ResendRequest arriving while one is being
  answered is ignored rather than restarting it. Answering one message at a time, or restarting on
  each request, sets off a loop of requests that freezes the session.
- **The reports come from a scan of the sequencer's log.** Every report is already there, tagged
  with its session, so nothing is stored twice. The sequencer reads its log from the oldest
  retained segment and keeps the matching records. Measured: 18 ms to scan a 4 MB log and return
  3,223 records for one session. The log has no index, deliberately: an index would put work on the
  path every order takes so that a rare reconnect could be quicker.

### Cancel-on-disconnect, per comp id, with a grace period

When a member's connection drops, its gateway can cancel its open orders, so that nothing stays live
on the book behind a session nobody is managing. The grace period is what makes this compatible with
gateway failover: if cancellation waits long enough for a member to reach its backup, a gateway
failure becomes a reconnect and a book still standing, rather than every member on the failed
instance having its book flattened.

- **On by default, with a 30-second grace period,** set in each gateway's
  `[cancel_on_disconnect]` section (`enabled`, `grace_period`).
- **Settable per comp id.** `cancel_on_disconnect_enabled` and
  `cancel_on_disconnect_grace_period_seconds` travel the same path as the provisioning above and
  arrive with the session. An empty grace period means the member expressed no preference and the
  gateway's default applies; zero means cancel at once. The two are kept distinct at every step: an
  empty database column, an omitted TOML key, an optional message field, and `std::optional` in the
  session.
- **A dropped session's orders are held, not cancelled.** If the same comp id logs on again inside
  the grace period, nothing is cancelled at all.
- **GoodTillCancel and GoodTillDate orders are never cancelled on disconnect,** because they are
  meant to outlive the session. The matching engine echoes `TimeInForce` on every execution report so
  the gateway can tell them apart.
- **A clean FIX Logout cancels at once,** because the member has said what it wants. The binary
  protocol has no logout message, so every disconnect there takes the full grace period.
- **`enabled = false` leaves every order resting** and gives the member full responsibility for them.

What venues do, and what this follows: cancel-on-disconnect is configured per session or comp id
when the member is provisioned, on by default, with a grace period comfortably longer than a
reconnect, persistent order types excluded, and a clean logout treated differently from a dropped
connection.

---

## Tests

| Scenario | What it requires |
|---|---|
| 16, `primary_me_death` | After a matching engine failover, the gateway sends at least one report per order and one per cancel, so the cancel-on-failover reports reach the member |
| 18, `fix_gateway_a_death` | FIX instance `a` is killed and the matching engine then cancels the orders `a`'s members placed. Nothing is elected, instance `b` keeps running, and, with no member reconnected, the sequencer drops every cancel report. That is today's behaviour for a session bound nowhere, [BUG-0088](../bug_list.md#bug_0088), and the assertion must change when that is fixed |
| 19, `cancel_on_disconnect_grace` | With the gateway running, a dropped member's orders are held for the provisioned grace period, the number itself and not the gateway's default, and a reconnect inside it cancels nothing. It fails if the grace period is zero |
| 20, `session_provisioning` | The gateway admits a comp id provisioned for its instance, naming both numbers, and refuses it once it is provisioned elsewhere, through the database and a real credentials export |
| 21, `reconnect_inherits_reports` | 1,000 orders rest, the member reconnects on a new connection, the leading matching engine is killed, and all 1,000 cancel reports reach the new connection |
| 22, `resend_recovery` | With a client that does not reset its numbering, the numbering resumes, the member asks for what it missed, real reports come back, and they carry `PossDupFlag=Y` as received by the client |
| 23, `inflight_gateway_death` | A gateway killed with orders in flight; the member returns to its backup numbered where it had reached, with its orders still live |

The binary gateway's provisioning refusal and its cancel from a new connection, across instances,
have been checked by hand with `binary_client`; `ha_test.py` drives these scenarios through FIX.

---

## What this does not solve

**Access latency is not equalised.** Ordering is fair, because the sequencer's number, not the
gateway an order arrived at, decides the order in which orders are processed. But the time to
*reach* the sequencer is not equal across instances or network paths. That is answered with
symmetric paths, identical hardware per instance and enough capacity per gateway, which is how
venues meet the obligation. It is not a sequencing problem and the sequencer cannot fix it.

**A gateway instance is still a single point of failure for the sessions pinned to it,** until
those sessions reconnect to their backup. Pinning bounds how many members one failure affects; it
does not remove the interruption, which is a reconnect measured in seconds.

**Nothing here raises the limit on one session's throughput.** One reactor per gateway carries all
of its inbound TCP, decoding and dispatch. More instances raise the venue's total capacity, but a
session is still served by one instance.

---

## Open questions

- **How does a member discover that its primary is down?** [BUG-0045](../bug_list.md#bug_0045).
- **What does the binary gateway do instead of a resend?** Its protocol has no session layer, so
  surviving a reconnect needs its own mechanism. [BUG-0046](../bug_list.md#bug_0046).
- **Should the log scan become an indexed lookup?** A replay is linear in what the log holds:
  18 ms for 4 MB, which is comfortable for a reconnect and would not be for anything frequent. A
  cursor per session, or skipping segments, would trade work on the order path or memory for it,
  and neither is worth paying while a replay is rare.
- **Disaster recovery.** A second site is not a third backup. [BUG-0047](../bug_list.md#bug_0047).
