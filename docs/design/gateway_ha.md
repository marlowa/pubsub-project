# Gateway High Availability {#gateway_ha}

How a member keeps trading when a gateway dies.

> **Status: all six steps built. Targeted at 0.3.0.**
>
> The direction was settled on 2026-07-30 and written down before implementation started, so that
> the decisions — and the reasoning behind them — were recorded rather than reconstructed
> afterwards. Since then, instance identity, the sequencer endpoint collection, two instances of
> each protocol running, cancel-on-disconnect with a grace period, and session provisioning have
> all landed, and so have steps 5 and 6: routing is keyed on session identity, so a member
> that reconnects -- to its own instance or to its backup -- inherits reports for orders it
> placed earlier and can cancel what it left resting; and a member that asks for what it
> missed is sent the real execution reports rather than having them gap-filled away. Each
> step's entry under **Implementation order** says what was actually built and how it was
> proven.
>
> **What a member does not get** is set out at the end of step 6 and is worth reading before
> treating this as finished: there is no outbound message store, so only execution reports are
> replayable and only for as long as the WAL retains them; the remembered sequence numbers do
> not survive a venue restart; and the binary gateway has no resend at all.
>
> The "What exists today" section is the 2026-07-30 baseline this design was written against, kept
> because later sections argue against it. Where a step has superseded part of it, that is marked
> in place.

This document covers the order-entry gateways only. Sequencer and matching engine HA are in
[WAL and High Availability](wal_and_ha.md); this one deliberately supersedes that document's
"Gateway Pool" section, for reasons set out below.

---

## Decisions

Both taken on 2026-07-30. Both changed what was previously written down. Both are now in the
code: pinning landed as step 4, and the routing half of the second as step 5 -- with the
qualification recorded under that step, that a member inherits reports from the moment it returns
and not for the period it was away, which is step 6.

**Sessions are pinned to a primary and a backup gateway.** Not any-of-N pooling. A member's
session is provisioned against two named gateway instances and may log on to either; it may not
land on a third. This follows how venues actually provision order entry — Eurex ETI partitions,
CME iLink market-segment gateways and LSE-lineage native connectivity all assign sessions to
gateways at provisioning time rather than letting them float.

`wal_and_ha.md` currently claims "N-way pooled redundancy" with clients free to reconnect to any
member. **That claim is withdrawn.** It was never implemented, and it is not what the industry
does. The reason it matters is not fashion: any-of-N requires every gateway to be able to serve
any session's recovery state, which is a distributed-state problem. Primary/backup requires only
that *one nominated peer* can, which is a replication problem between two known endpoints. The
second is tractable; the first is a different project.

**In-flight execution reports survive the reconnect.** A member that reconnects — to its primary
or to its backup — is brought fully up to date: reports it missed while disconnected, and reports
that were sent to the old connection but may not have arrived. This is the expensive decision, and
the rest of this document is mostly about paying for it.

---

## What exists today

Verified in the code on 2026-07-30, not inferred from documentation.

**Gateway identity is per protocol, not per instance.** `GatewayIds.hpp` defines
`fix_order_gateway = 1` and `binary_order_gateway = 2`. That value rides on the `WalRecord` envelope as
`origin_gateway_id` and is how the sequencer decides where to send an execution report.

Its own comment used to call these values a binding constraint — on-disk WAL format, never to be
reused. That was overstated and has been corrected: the project is pre-1.0 and makes no
compatibility promise across releases, so a WAL from an older build is discarded rather than
replayed. Reusing a value is worth avoiding, not forbidden.

**Both gateways stamp `origin_gateway_id` on every order.** The FIX gateway does it in
`forward_order_in_envelope`, a template in `FixOrderGatewayThread.hpp`; the binary order gateway does it
at two sites in `BinaryOrderGatewayThread.cpp`.

An earlier version of this document claimed the FIX gateway never constructs a `WalRecord` and
that `gateway_ids::default_when_absent` therefore covered a structural gap. **That was wrong** —
it came from grepping the `.cpp` and missing the template in the header. The default covers
records that have no gateway origin at all, which is a real category: `has_origin_gateway_id` is
set conditionally on there being a session connection, so execution reports and replayed records
can legitimately carry none.

That correction matters because it makes the fields **optional by design, not by legacy**. An
earlier draft proposed making both required; that would force a meaningless protocol and instance
onto every record that never came from a gateway.


**The sequencer dialled a fixed pair of endpoints.** `SequencerConfiguration` held scalars —
`gateway_host`/`gateway_port` for the FIX gateway, `binary_gateway_host`/`binary_gateway_port` and
a `binary_gateway_enabled` flag for the binary one. There was no collection, so there was nowhere
to express a second instance of either. *Superseded by step 2a: it is now a `[[gateway]]`
collection keyed on `(protocol, instance)`, and dev configures four entries. Kept here because the
rest of this section is the 2026-07-30 baseline the design was written against.*

**An execution report for a disconnected gateway is dropped.**
`SequencerThread::send_er_to_origin_gateway` logs `gateway id {} not connected -- dropping ER
seq={}` and returns. Nothing retries and nothing queues.

**There is no outbound message store.** `FixSession` holds `outbound_seq_num` as a plain `int` and
no record of what was sent. `FixOrderGatewayThread::handle_resend_request` answers *every*
ResendRequest with a `SequenceReset-GapFill` spanning the whole gap — it does not resend, it
declares the missing range administrative and skips it. The comment explains why it was written
that way (one-at-a-time filling caused a feedback loop that froze the session), and as a way to
keep a session alive it works. As report delivery it means **in-flight reports do not survive a
reconnect today even to the same gateway.** This is a present single-gateway gap, not something
introduced by going multi-gateway.

**Cancel-on-disconnect is implemented.** `queue_session_for_cleanup` and
`drain_pending_cancels` send an `OrderCancelRequest` for every entry in the session's
`open_orders` when the connection drops, so nothing stays live on the book behind a dead session.

The last two facts together describe the current behaviour honestly: when a member's connection
drops, its orders are cancelled, and when it reconnects it is *not* told what happened — the
reports are gap-filled away.

---

## What running two instances gives you today — and what it does not

Steps 1-3 are done, so dev runs `fix_order_gateway_a`/`_b` and `binary_order_gateway_a`/`_b`. It
is easy to read that as "the gateway is now HA". It is not, and the difference is worth stating
plainly because the half that is missing is the half a member would notice.

### What works, and is proven

**Per-instance execution-report routing.** The sequencer holds every instance as a separate
endpoint keyed on `(protocol, instance)` — services `gateway_1_1`, `gateway_1_2`, `gateway_2_1`,
`gateway_2_2` — and sends each report back to the instance its order arrived on. Verified
empirically on 2026-08-01, in both directions for both protocols: driving instance `b` produced
`GW-PROGRESS` lines on `b` only, with `a` running and idle, and vice versa. This is the "complex
routing" the multi-instance work existed to build, and it is correct.

**Reduced single point of failure, for new sessions.** If instance `a` is down, a member
configured for instance `b` logs on and trades normally. Nothing about `a`'s absence stops `b`.

### What does not work

**There is no load sharing.** Nothing distributes sessions between instances. There is no shared
session registry, no least-loaded selection, no proxy in front. A member connects to the endpoint
it was configured with, and that is the instance it uses. Two instances are redundancy plus
capacity you divide by hand, not a balanced pool. Session provisioning (step 4) has since made
that division deliberate rather than incidental — a member now belongs to named instances and is
refused elsewhere — but it is still a division an operator makes, not one the venue balances.

**Instance `b` does not inherit anything from instance `a`.** There is no handover of any kind:

- On losing a gateway connection, `SequencerThread::on_connection_lost` simply erases the entry
  from `gateway_conn_ids_`. It does not reroute to another instance of the same protocol.
- Every subsequent report for that instance hits the `connection == nullptr` branch of
  `send_er_to_origin_gateway` and is logged and dropped: `gateway protocol=1 instance=1 not
  connected -- dropping ER seq=N`. There is no outbound message store, so it is gone, not queued.
- Instance `b` has no knowledge of `a`'s sessions, their open orders, or their sequence numbers.

**And cancel-on-disconnect makes the two failure modes worse in opposite directions.**
`queue_session_for_cleanup` fires the moment a client session drops and `drain_pending_cancels`
sends an `OrderCancelRequest` for every resting order, immediately, unconditionally — no grace
period, no per-comp-id switch. So:

| What fails | What happened before steps 3b-5 |
|---|---|
| The client's connection to `a` drops, `a` survives | `a` cancelled that session's entire book at once. The member reconnected — to `a` or to `b` — and found itself flat. Its positions were closed by a network blip. *Closed by 3b: the orders are held for a grace period and a reconnect inside it cancels nothing.* |
| The `a` process dies | No cancels are sent, because the process that would send them is dead. The orders stay resting in the matching engine — but the member reconnecting to `b` could neither see nor cancel them, because both the book key and the report address named the connection that placed them. *Closed by step 5: the book is keyed on the session's identity and reports are addressed to wherever it is now, so the member cancels them from `b` and is told what happened.* |

The first is the cancel storm step 3b's grace period exists to prevent. The second is arguably
worse — orphaned live orders are a real risk position that the member cannot manage — and it is
what steps 4-6 (session provisioning, re-keyed routing, session-slice replay) exist to close.

**Neither case is a regression.** Both follow from running more than one instance at all, which is
exactly why the design sequences 3b immediately after step 3 rather than with the later work: step
3 makes the failure demonstrable, and 3b is what makes the demonstration worth having.

### Two defects found while testing this — both now fixed

`ha_test.py` scenario 18 (`fix_gateway_a_death`) was written to pin the no-handover behaviour
above. Writing it turned up two separate defects, neither caused by the multi-instance work.

**Cancel-on-failover execution reports were never delivered when the sequencer had a follower.**
The promoted matching engine sends each cancel ER with `seq_no = 0`, because the cancel is
generated on promotion rather than driven by a sequenced order. But `SequencerThread::on_pdu`
forwarded an ER only once `wal_acked_seq_nos_` contained its seq_no, and no WalAck for seq_no 0
can ever arrive. So under `needs_wal_ack()` — `ha_enabled` with a follower connected, which is
the normal configuration — every cancel ER was parked in `pending_er_` forever: not delivered,
not dropped, traced only by a `Debug` line that `applog_level = "info"` suppresses. And because
`pending_er_` is keyed on the gate sequence, all of them collided on key 0 and only the first was
even retained. **The whole book was cancelled and no client was ever told.**

Fixed by gating an ER that has no originating order sequence on **its own** WAL record instead.
The follower acks every `WalRecord` it receives, so `er_wal_seq` is acked exactly as an order's
seq_no is, and it is unique per ER so the keys no longer collide. That is strictly the stronger
guarantee — the client learns of the cancel only once the backup holds the cancel record — and it
is the "full two-tier commit of ERs" the code already noted as a follow-up, applied to the one
case that had no working gate at all.

**The matching engine did not store the gateway instance.** `OrderEntry` carried
`gateway_session_conn_id` and `origin_gateway_id` but no instance. Its own comment said the
connection id "alone is only unique within one gateway, so both are needed" — true before
instances existed, one axis short afterwards. A cancel-on-failover ER for an order placed through
instance `b` would have been addressed to instance 1, which is `a`, and delivered to whatever
session happened to hold that connection id there: one client's report handed to another. Latent
only because the first defect stopped these reports reaching the routing decision, so fixing that
alone would have turned silence into misdelivery.

Fixed by carrying the instance alongside the gateway id everywhere the id already travelled:
`OrderEntry`, the `BookUpdate` replication message (a trailing optional field, as
`origin_gateway_id` is), and the envelope `send_er_to_sequencer` stamps.

Scenario 16 caught neither, and read as though it did: it counted the *gateway's*
`has no gateway_session_conn_id -- dropping` lines and required 0, which passes vacuously when
the reports never reach the gateway at all. It now asserts delivery positively — the gateway must
have sent at least one report per order **plus** one per cancel — and would have failed before the
fix.

**A third defect in the same family, also now fixed: `OrderKey` was one axis short.** It keyed the
book on `(session_id, gateway_id, cl_ord_id)`, and its own documentation made exactly the argument
that extends to instances: a connection id "is only unique within one gateway, because each
gateway numbers its own client connections from its own counter". With two instances of one
protocol, instance `a`'s connection 5 and instance `b`'s connection 5 were unrelated sessions
sharing a book key — so a second order with the same ClOrdID from the other instance was rejected
as a duplicate, or a cancel from one session retired the other's order. Unlike the two above this
one is about order *identity* rather than report *delivery*, and it became reachable the moment a
second instance of any protocol started taking orders.

`OrderKey` now carries `gateway_instance`, in the key, the equality and the hash.
`OrderKey::make` takes it as a required parameter rather than a defaulted one: this axis was
forgotten once already, and a default would let the next call site forget it silently instead of
failing to compile. Four new tests cover it, mirroring the cross-gateway ones — two instances
coexisting and cancelling independently, protocol and instance not being interchangeable, and
absent meaning instance 1.

### The short version

*Written when steps 1-3 were all that existed, and kept because the distinction it draws is the
right one.* Two instances then meant *a venue that keeps trading when one gateway dies*, but not
*a session that survives its gateway dying*: instance `b` was a place for new sessions to go, not
a place old ones could be recovered.

Steps 3b, 4 and 5 have since closed that. A member whose gateway dies reconnects to its backup,
finds its orders still resting, can cancel them, and receives reports for them there. What is
still missing is the period in between: reports generated while it was disconnected are not
replayed to it, which is step 6.

---

## Gaps to close

| # | Gap | Consequence today |
|---|-----|-------------------|
| 1 | `origin_gateway_id` names a protocol, not an instance | Two FIX gateways would both stamp `1`; the sequencer could not tell them apart |
| 2 | Sequencer gateway endpoints are scalars | Nowhere to configure a second instance |
| 3 | Reports are dropped when the target gateway is down | A member loses reports for the entire outage, permanently |
| 4 | No outbound message store; ResendRequest is gap-filled | A member cannot recover reports even on the same gateway |
| 5 | No session→gateway provisioning | Nothing says which two gateways a session may use |
| 6 | Routing entry is keyed on a gateway-local connection id | A reconnect cannot inherit the previous connection's reports |

Gaps 1, 2 and 5 are the SPOF work. Gaps 3, 4 and 6 are the in-flight-report decision. They are
separable, and the SPOF half is worth having on its own.

**Gaps 1, 2, 5 and 6 are closed** (steps 1, 2, 4 and 5). Gap 3 is narrowed: a report for a
session that has reconnected follows it, and only a session connected nowhere at all has its
reports dropped -- and those are recoverable afterwards, which is what made the drop tolerable.
**Gap 4 is answered rather than closed**: there is still no outbound message store, but the
WAL serves as one for execution reports, which are the messages a member actually needs back.
Its limits -- retention, and administrative messages -- are stated under step 6.

---

## Design

### Instance identity

Add a **`gateway_instance_id`** to the envelope alongside `origin_gateway_id`, rather than
repurposing the existing field.

**The reason is not backwards compatibility.** An earlier draft argued the existing values could
never be reassigned because they are on disk in every WAL record. That does not hold: the project
is pre-1.0, so an old WAL is discarded rather than replayed. Nothing forces two fields on those
grounds.

The reasons that do hold are quieter but real. Protocol and instance are orthogonal, so separate
fields model them honestly rather than encoding two things in one number. And a record stays
self-describing: the sequencer can pick the execution report's wire encoding, and a person reading
a WAL can see which gateway an order came from, without consulting configuration. The cost is one
`i16`.

- `origin_gateway_id` keeps its present meaning — which *protocol* the order arrived on.
- `gateway_instance_id` says *which instance of that protocol*, numbered from 1.

The session is then identified venue-wide by the triple
`(origin_gateway_id, gateway_instance_id, gateway_session_conn_id)`. That preserves the ClOrdID
collision fix that motivated connection-id routing in the first place — a connection identifies
exactly one session and cannot collide — while making the identity unambiguous across instances.

Both fields stay **optional**, because not every `WalRecord` has a gateway origin. What is
guaranteed is that a gateway always sets both: each gateway process carries its own
`gateway.instance_id` in configuration and stamps it beside the protocol on every order envelope.

Attribution from the arrival connection was considered and rejected as unnecessary. It would have
needed an announce PDU on connect, because the sequencer listens on one port and accepts, so it
cannot otherwise tell instances apart. Since both gateways already stamp per message, the
announce would buy only that a gateway could not misreport its own identity — a weak argument
between venue components — at the cost of a protocol addition and per-connection state.

What that would also have bought, a validation point, is kept without it: the sequencer warns
once per unrecognised `(protocol, instance)` pair when it has orders to answer but no configured
endpoint, naming the pair and the likely cause. Once per pair rather than once per report, so a
busy gateway cannot flood the log with the same misconfiguration.


### Sequencer endpoint collection

Replace the scalar endpoint fields with a list, each entry carrying protocol, instance id, host and
port. `binary_order_gateway_enabled` disappears: an absent entry is a gateway that is not deployed.

The sequencer dials every configured entry and keeps the connections open, exactly as it does for
the two today. `send_er_to_origin_gateway` becomes a lookup on the `(protocol, instance)` pair.

This is a configuration schema change across the environment TOMLs, so it needs the
`${admin_service_...}`-style flattened placeholder convention already used elsewhere.

### Session provisioning

A session's primary and backup are **configuration, not discovery**. The member is told both
endpoints out of band, as venues do. The venue side needs the same knowledge so it can reject a
logon that arrives at the wrong instance — otherwise "pinned" is a convention rather than a rule,
and the recovery guarantees below do not hold.

The natural home is the admin service, which already owns comp-id provisioning and already writes
the database that is the source of truth for credentials. A comp id gains a primary and a backup
gateway instance, and the gateways learn their own assignments the same way they learn credentials.

This interacts with the already-decided **one comp id may hold a session only once venue-wide**
(recorded in `pubsub_itc_fw_summary.md`). Pinning narrows that problem usefully: with only two
instances able to host a given session, the duplicate check has two places to look rather than N.
It still needs the sequencer as the shared authority, and it is still a cross-component protocol
change; it is not solved by pinning, only made smaller.

**Implemented 2026-08-05, in both gateways.** `pubsub_comp_id` gained
`primary_gateway_instance` and `backup_gateway_instance`, and the values travel the path 3b
already built: database → `export_credentials.py` → `credentials.toml` → authentication service →
`AuthenticationResult` → gateway. They arrive *with* the session, on a path where the gateway has
no database access, and the admin service edits them on the comp-id form.

Four decisions are worth recording, because each had a plausible alternative:

**They name an instance, not a protocol.** Instance 1 of the FIX gateway and instance 1 of the
binary gateway are separate processes holding the same position in their own protocol, so a
member's pinning applies to whichever order-entry protocol it speaks. The alternative — pinning
`(protocol, instance)` pairs, so a comp id could be provisioned for FIX and not binary — is truer
to how a venue partitions, but it would put protocol knowledge into the authentication service,
which its own DSL header says must have none, and it would need the gateway to declare its
protocol in `AuthenticationRequest`. It buys a restriction nothing has asked for yet.

**Not pinned means any instance, and is the default.** Both columns are nullable and null means
this member expressed no preference. That is the same "silence is not a value" rule the v2 grace
period follows, and it is what stops the change locking out every comp id provisioned before it
existed. The design says the venue must "reject a logon that arrives at the wrong instance" — it
does, for every session that *has* a wrong instance. A venue that wants pinning to be mandatory
provisions its members; it does not get there by having an unset column mean "denied".

**One backup, not a list.** This matches what venues publish — Eurex T7, CME iLink and
LSEG-lineage native all hand a session a primary and a backup — but the reason to keep it is
structural rather than imitative: the argument for pinning at all is that only *one nominated
peer* must be able to serve a session's recovery state. A third live peer reopens the
distributed-state problem the pinning exists to avoid, and makes the outbound sequence number and
replay cursor of steps 5 and 6 consistent in three places instead of two. Generalising later means
a child table keyed on `(comp_id, rank)`, which is a contained change if it is ever wanted.
Disaster recovery is *not* that third backup: at a real venue it is a separate site with its own
sequence regime, and it is not modelled here at all.

**The refusal is its own outcome.** The binary protocol gained
`LogonOutcome::NotProvisionedForInstance` rather than reusing `AuthenticationFailed`, and the FIX
gateway's Logout carries `Session not provisioned for gateway instance 1 -- use instance 2`. The
credential was good; telling the member otherwise would send it off rotating a password that was
never the problem. Naming its own provisioning gives nothing away — it is authenticated by the
time the check runs, which is also why the check runs *after* the ServerSignature is verified
rather than before: the provisioning is only trustworthy once the service has proved itself.

The check itself is one decision taken once, at the moment authentication succeeds, in both
gateways. Nothing is stored on the session: re-deciding it later would need state that can drift
from the thing it was derived from.

**A defect found and fixed while building this.** `AuthenticationThread::persist_credentials`
rewrites `credentials.toml` in full from its in-memory SCRAM map every time an admin sets, removes
or restores a credential — so it had been silently stripping every member's cancel-on-disconnect
provisioning since 3b, leaving them on gateway defaults with nothing in any log to say so. The
same rewriter would have eaten the pinning. It now writes the session policy back out beside the
credential it belongs to. This is the third instance of the same shape: a component that owns one
part of a record regenerating the whole record and discarding the rest.

### Recovering in-flight reports

This is gaps 3, 4 and 6, and it is the substantial part.

The requirement, stated precisely: on logon, a session must be able to receive every execution
report generated for it since a sequence position it nominates, whether those reports were
generated while it was connected, while it was disconnected, or in the moment its connection died.

**The reports already exist and are already durable.** The sequencer's WAL holds every one, with a
sequence number, tagged with the session that originated the order. Nothing needs to be invented to
store them; what is missing is the ability to *replay a single session's slice* of that stream. The
project already has the primitive for this — the topic pub/sub layer from slice 10 provides replay
from a cursor, and the ER topic already streams execution reports.

So the shape is:

1. **Stop dropping.** When the target gateway is not connected, the report is not discarded; it
   stays in the WAL, which it is in anyway. The drop path becomes a no-op rather than a loss,
   because delivery is driven by the reconnecting session asking for its slice, not by the
   sequencer pushing at a connection that may not exist. *Built differently, and the
   difference is worth being clear about. A report for a session bound nowhere is still
   dropped at the moment it is generated -- but it is in the WAL, and the reconnecting session
   asks for its slice, which is what this point was really describing. The drop is no longer a
   loss.*
2. **Key the routing entry on the session, not the connection.** The connection triple stays as the
   *current destination*. The *key* becomes the provisioned session identity, so that a logon can
   re-bind the destination to a new connection on a different instance. This is the direct answer
   to gap 6: the identity that must survive is the session's, and the connection is a mutable
   attribute of it. **Done, as step 5.** The identity turned out to be `(comp id, protocol)`
   rather than the provisioned pinning itself, which names instances and so cannot survive an
   instance change.
3. **Replay on logon.** The gateway, having authenticated a session, asks the sequencer for that
   session's reports from the member's nominated position and encodes them to the wire in its own
   protocol, with FIX `PossDupFlag=Y` where the report was previously sent. **Done, as step 6** --
   though driven by the member's ResendRequest rather than by the logon itself, which is what FIX
   prescribes: the venue restores the session's numbering, and the member decides whether it is
   missing anything.
4. **Retire the blanket gap-fill.** `handle_resend_request` stops answering everything with
   `SequenceReset-GapFill`. The FIX convention is that administrative messages are gap-filled and
   application messages are resent; execution reports are application messages. The feedback-loop
   hazard the current comment describes is real and must be avoided by resending the range in one
   pass, not by reverting to one-at-a-time filling. **Done, as step 6.** The range is answered in
   one pass, and a second ResendRequest arriving while one is running is ignored rather than
   restarting it -- which is the same feedback loop reached from the other direction.

The FIX outbound sequence number needs care. It is per session, not per connection, and it must
survive the instance change — otherwise the backup starts at 1 and the member sees a sequence
break it cannot reconcile. It belongs with the session's provisioned state, alongside the primary
and backup assignment.

### Cancel-on-disconnect: configurable per comp id, with a grace period

**Decided 2026-07-31.** It stays, becomes configurable per comp id, and gains a configurable
grace period.

Today it is unconditional and immediate. `queue_session_for_cleanup` cancels every open order the
moment a connection drops; the 1ms drain timer paces the emission at 500 per batch and is not a
delay before cancelling.

**The grace period is not a refinement, it is what makes gateway failover coherent.** As things
stand, when a gateway process dies every session on it drops and every member's book is flattened
immediately. Run two instances, kill one, and the high-availability mechanism produces exactly the
outcome high availability exists to prevent. The reconnect window and the cancel delay are the
same number: if cancellation waits long enough for a member to reach its backup, a gateway failure
becomes a reconnect, some replayed reports, and a book still standing.

This is therefore not a separate question from steps 4-6. It is the same question.

What venues do, and what this follows:

- **Configurable per session or comp id**, applied at provisioning rather than toggled by the
  client. Here that means the admin service and the database, which already own comp-id
  provisioning -- so a Liquibase changeset, a DAO field and an admin UI control.
- **Default on.** An unmanaged book behind a dead session is the worse failure.
- **A grace period before cancelling**, defaulting to comfortably longer than a FIX reconnect.
- **Persistent order types excluded.** GTC and GTD are by definition meant to outlive the session;
  killing them because a socket dropped defeats what the member asked for. TimeInForce is already
  carried in the data dictionary, so the information is to hand.
- **A clean Logout treated differently from an unexpected drop.** A member that logs out has said
  what it wants; a socket that vanished has not.

The last two were recommendations when this was written; both were confirmed on 2026-08-01
and are implemented.

**Implemented 2026-08-01, in both gateways.** `[cancel_on_disconnect] enabled` and
`grace_period` in each gateway's configuration, defaulting to on and 30 seconds:

- A dropped session's orders are parked, not cancelled. If the same comp id logs on again
  inside the window they are released untouched and **nothing is cancelled at all**.
- GoodTillCancel and GoodTillDate are never cancelled on disconnect. The matching engine
  now echoes `TimeInForce` on every execution report so the gateway can tell them apart --
  it previously did not, which is why this could not have been built without that change.
- A clean FIX Logout cancels immediately, bypassing the window. The binary protocol has no
  logout message, so every disconnect there takes the full grace period.
- `enabled = false` leaves every order resting and hands the member full responsibility.

`ha_test.py` scenario 19 covers it: drop the client with the gateway still running, prove
the gateway holds rather than cancels, prove nothing is cancelled while the window is open,
then reconnect the same comp id and prove no cancel is ever sent. It fails if `grace_period`
is set to zero, so it discriminates rather than merely passing.

**What this deliberately did not do, when it was built: the reconnecting session did not
adopt the held orders.** They stayed resting on the book, but the new connection could not
cancel them, because the matching engine keyed an order by the connection id it arrived on.
Adopting the entries in the gateway would have made it claim a control it did not have, so
the fix belonged one layer down. *Step 5 did that: the book is keyed on the session's identity,
so a reconnected member cancels what it left resting without anything being adopted or
transferred. "Your book survives" and "you can manage it from the new session" are now both
true.*

**Per comp id as well as venue-wide, as of 2026-08-01.** `pubsub_comp_id` gained
`cancel_on_disconnect_enabled` and `cancel_on_disconnect_grace_period_seconds`; the values
travel database -> `export_credentials.py` -> `credentials.toml` -> authentication service ->
`AuthenticationResult` -> gateway, so they arrive *with* the session rather than needing a
second lookup on a path where the gateway has no database access. The admin service edits
them on the comp-id form.

The grace period column is deliberately **nullable, and null is not zero**: null means the
member expressed no preference and the gateway's configured default applies, whereas zero
means cancel immediately. Keeping those distinguishable is what lets an operator raise the
venue-wide window without revisiting every member, and the distinction is preserved at every
hop -- a nullable column, an omitted TOML key, an optional DSL field, and `std::optional` in
the session.

Scenario 19 asserts the *number* the gateway holds for, not merely that it held, because
every hop that drops the value leaves the gateway silently on its default. That assertion
immediately earned its keep: the test harness's own credential rewriter was discarding the
provisioning while refreshing SCRAM material, which looked exactly like the gateway ignoring
the setting.


## What this does not solve

**Access latency is not equalised.** Pinning fixes ordering fairness — the sequencer stamps the
monotonic number and that, not the arrival gateway, dictates processing order. It does not make the
time to *reach* the sequencer equal across instances. Two members on different gateway instances,
or different network paths, can still see different latencies to the sequencing point. That has to
be argued on symmetric paths, identical hardware per instance and per-gateway capacity, which is
how venues actually discharge the duty. It is not a sequencing problem and the sequencer cannot fix
it.

**A gateway instance is still a single point of failure for the sessions pinned to it**, until
those sessions fail over to their backup. Pinning bounds the blast radius rather than removing it;
the member-visible interruption remains a reconnect, measured in seconds.

**Nothing here addresses the inbound ceiling.** One reactor per gateway still caps inbound TCP,
frame decode and dispatch. More instances raise the aggregate ceiling, which is a real benefit, but
each session's own throughput is still bounded by the one instance serving it.

---

## Implementation order

Each step leaves the system working.

1. **Instance identity on the envelope.** `gateway_instance_id` plus the encode and decode paths
   and their round-trip tests. No behaviour change. **Done** — committed as an *optional* field
   with absent-means-1, which step 2 replaces with a required field once the sequencer can supply
   it on every path. The optional form is a staging post, not the intended end state.
2. **Sequencer endpoint collection, and gateways stamping their instance.** **Done.**

   2a: the sequencer's scalar gateway host/port pairs and the `binary_order_gateway.enabled` flag
   become a `[[gateway]]` array of tables, each entry carrying protocol, instance, host, port and
   its own `enabled` flag. `gateway_conn_ids_` is rekeyed on `(protocol, instance)` and
   `send_er_to_origin_gateway` takes both axes. Duplicate pairs are rejected at load.

   2b: each gateway process carries `gateway.instance_id` in its configuration and stamps it onto
   every order envelope beside the protocol id.

   An earlier draft of this step proposed attributing origin from the arrival connection instead,
   via an announce PDU sent on connect. That was dropped: it rested on the mistaken belief that
   the FIX gateway sends bare orders and could not stamp an origin. Both gateways already stamp
   per message, so the announce would have bought only that a gateway cannot misreport its own
   identity — weak between venue components — at the cost of a protocol addition and
   per-connection state.

   Its one genuine benefit, a validation point, is kept without it: the sequencer logs an error
   once per unrecognised `(protocol, instance)` pair for which it has reports but no configured
   endpoint, naming the pair and the likely cause. Once per pair rather than once per report.

   Still missing: the loader has no unit test, there being no sequencer configuration loader test
   to extend. Worth adding before step 3 runs two instances for real.

3. **Run two gateway instances of each protocol in dev.** **Done.** The first point at which the
   single point of failure is actually reduced rather than described, and the first honest test of
   steps 1 and 2. Needed `gateway.instance_id` in the gateway app TOMLs, a second component per
   protocol in the environment with its own ports, a second `[[gateway]]` entry per protocol in the
   sequencer configuration, and devenv launching them.

   FIX went first and was proven both ways: 1,000 orders driven at instance `a` and then at
   instance `b`, with both processes running in both cases, each instance receiving only its own
   orders and their execution reports. The binary gateway then took the same split, so dev now runs
   four gateway processes and the sequencer carries four `[[gateway]]` entries.

   Instances are named `_a`/`_b`, not `_primary`/`_secondary`: nothing elects a gateway, a member
   chooses which to connect to, so this is caller-selected redundancy and follows the
   authentication service's precedent. The suffix goes on the component name and its config file;
   the binary and the working directory stay unsuffixed, because there is still one program and one
   `etc/` directory per protocol.

   Only the `_a` instance of each protocol is deployed outside dev. The `_b` entries exist in
   preprod, prod and test-1 with `enabled = false`, so a second instance is a configuration change
   rather than a template edit.

3b. **Cancel-on-disconnect made configurable, with a grace period.** Decided 2026-07-31; see the
   section above. Sequenced here rather than with steps 4-6 because without the grace period, a
   gateway failover flattens every book on the failed instance -- so step 3 demonstrates the
   failure mode, and this is what makes the demonstration worth having. Spans a Liquibase
   changeset, a comp-id DAO field, an admin UI control, gateway configuration and the cancel path
   itself.

4. **Session provisioning.** **Done 2026-08-05.** Primary and backup per comp id in the admin
   service and database; both gateways refuse a logon at an instance a session is not provisioned
   for, and admit one that is not pinned at all. See the section above for the four decisions and
   for the `persist_credentials` defect this turned up.

   `ha_test.py` scenario 20 covers the FIX gateway: it pins the test comp id to the instance the
   harness runs with another as its backup, requires the gateway to name *both numbers* on
   admission, then re-provisions the comp id onto an instance the harness does not run — through
   the database and a real credentials export — and requires the next logon to be refused with no
   session established. Both halves were verified to fail when broken: pointing the second half at
   the instance the gateway already is makes it fail to refuse, and dropping the values in
   `export_credentials.py` makes the first half fail to see them. The scenario restores the comp
   id to unpinned in teardown, because a comp id left pinned to an instance the harness does not
   run refuses the baseline logon of every scenario after it.

   The binary gateway was proven live against the dev sandbox rather than in `ha_test.py`, which
   drives FIX only: with the comp id pinned to instance 2, `binary_client` was refused at instance
   1 with `NotProvisionedForInstance` and the text naming instance 2, accepted and trading at
   instance 2, and accepted at instance 1 again once unpinned.

   Half-dropped provisioning is a startup failure, not a silent default: a credential carrying a
   backup with no primary makes the authentication service refuse to start, naming the entry. That
   was observed rather than designed — it fell out of the loader validation while the negative
   controls above were being run — and it is the right behaviour, so it stays.
5. **Re-key the routing entry** on session identity with the connection triple as destination.
   **Done 2026-08-06.**

   The sequencer's routing entry was `seq_no -> (connection, protocol, instance)`: an order
   was filed under the *address* it arrived at. That address is a socket on a process, so it
   died with the connection, was renumbered on reconnect, and did not exist at all at the
   member's backup gateway. It is now split in two — `seq_no -> session identity`, and
   `session identity -> current destination` — and the destination is resolved at the moment
   a report is sent rather than remembered when the order was placed.

   **The identity is `(comp id, protocol)`.** Not the comp id alone: an instance failover
   moves a session between instances of one protocol, so the instance must not be part of
   it, but a FIX and a binary session sharing one comp id are genuinely two sessions and
   must not share a book or each other's reports. The venue rule that one comp id holds a
   session only once still applies within a protocol, and is still unenforced; the sequencer
   now logs when it sees a comp id bind while already bound, which is the first half of it.

   **The bindings come from the gateways**, as `SessionBound` and `SessionUnbound` PDUs sent
   when a session is established and when it goes away. The sequencer cannot infer them: it
   listens on one port and accepts, so it cannot tell instances apart from a connection, and
   a member that reconnects and sends no order would never announce itself. `SessionUnbound`
   carries the connection id and is ignored when it does not name the current binding, so a
   reconnect that overtakes the old connection's unbind — two gateways racing, which is what
   a failover produces — cannot unbind the session it just bound.

   Three consequences worth stating plainly:

   - **The matching engine's book is keyed on the identity too** (`OrderKey`), which is what
     lets a reconnected member cancel an order it left resting. Until now it could see the
     order but not touch it, because the key held the connection that placed it. That was
     called out as the gap at the end of the cancel-on-disconnect section, and it is closed.
   - **`BookUpdate` replication carries the identity**, not the connection id. Replicating an
     address was wrong in the one case the message exists for: on promotion, the addresses
     in the replica named the process whose death caused the promotion.
   - **The matching engine no longer stamps a destination on any report.** It stamps whose
     report it is; the sequencer resolves where that member currently is. The ME has no way
     to know, and on the cancel-on-failover path any address it remembered would be stale by
     construction.

   Proven live, in the way that matters most — from a client rather than from a log:
   `binary_client` placed an order on instance 1, disconnected, and a *new* connection
   cancelled it; then the same across instances, placed on instance 1 and cancelled from
   instance 2, with the report coming back on instance 2. The sequencer's log shows the
   session re-binding from instance 1 to instance 2 between the two.

   `ha_test.py` scenario 21 covers the FIX path: 1,000 orders rest on the book, the client
   drops and returns on a new connection, then the matching-engine primary is killed and the
   promoted secondary cancels the whole book. All 1,000 cancel reports must reach the new
   connection — reports for orders whose originating connection no longer exists.

   **The first negative control for that scenario passed, which meant it proved nothing.**
   Suppressing re-binds alone was not enough, because the client's disconnect unbinds the
   session first, so a reconnect faces no existing binding to refuse. Freezing the
   destination properly — suppressing the unbind as well — reproduces the pre-step-5
   behaviour, and the scenario then fails with 1,000 reports instead of 2,000. Worth
   recording because the weak control looked exactly like a passing test.

   One harness change fell out of the re-keying. `perf_run.py --clients N` ran N FIX clients
   under a single comp id, which only worked because the book key included the connection
   id; f8test numbers its ClOrdIDs from one in every process, so under an identity-keyed
   book all but the first client's orders would be duplicates. Each client now gets its own
   comp id, its own credential and its own generated session config, exactly as the binary
   load client already did.
6. **Session-slice replay on logon**, and retire the blanket gap-fill. **Done 2026-08-06**,
   to a deliberately bounded scope: the replay path in full, and enough FIX conformance to be
   honest about what the member is handed. What was *not* built is listed at the end.

   `handle_resend_request` no longer answers everything with a `SequenceReset-GapFill`. It
   asks the sequencer for the session's execution reports, resends them with `PossDupFlag=Y`
   and `OrigSendingTime`, and gap-fills only the administrative remainder -- which is the
   split FIX actually prescribes.

   **Sequence continuity comes first, because without it the member never asks.** A session's
   outbound number is reported to the sequencer at `SessionUnbound` and handed back at
   `SessionBoundAck`, so a reconnect continues the member's numbering instead of restarting
   at 1. The sequencer cannot count that number itself -- the FIX outbound number covers every
   message sent to the member, including the heartbeats and rejects that never reach the
   sequencer -- so it is reported rather than derived. That makes it only as current as the
   last clean unbind: a killed gateway sends none, and the returning member finds the venue
   behind it, which its own ResendRequest then resolves.

   **`ResetSeqNumFlag=Y` is honoured, and had to be.** A member that asks to start again at 1
   is declining continuity, and by its own account has nothing missing. Ignoring that would
   deadlock the two sides into a resend loop neither could end.

   **The replay is a filtered WAL scan.** Nothing is stored twice: every report is already in
   the WAL with the session that originated it on its envelope, as of step 5. The sequencer
   walks its WAL, keeps the records matching the identity, and streams them back. Measured
   rather than assumed: **18 ms to scan a 4 MB retained WAL and return 3,223 records** for one
   session. The cost is a scan from the oldest retained segment, because the WAL is an
   append-only log with no index -- deliberately, since indexing it would put work on the
   write path that every order pays for so that a rare reconnect can be quicker.

   Two mistakes made while building it, both worth recording because both looked like
   working code:

   - **The replay first streamed matches as it found them, oldest first.** What a member has
     missed is the *tail* of its stream, so filling the gap from the beginning hands it
     ancient history and never reaches what it actually missed. It also made the answer
     unbounded: a member asking for a thousand messages was sent the session's whole retained
     history. The sequencer now collects the most recent `max_records` matches in a window and
     sends those, and the gateway asks for exactly the gap width the member described.
   - **`PossDupFlag` was applied to every replayed report**, including those past the
     requested range -- reports the venue had never delivered, marked as possible duplicates.
     It now marks only what falls inside the gap the member asked about.

   `ha_test.py` scenario 22 covers it, driven by a client configured *not* to reset its
   sequence numbers. It asserts the numbering resumed, that the member asked, that real
   reports came back -- and then counts `PossDupFlag=Y` in the **client's own received
   messages**, because whether a resent report is marked is a fact about what the member was
   handed, and the gateway's record of it sits below the deployed log level.

   The negative control for that last assertion is the most instructive result of the whole
   step. Removing `PossDupFlag` does not merely mislabel the messages: the member **closed the
   connection**. A resent report carries a sequence number lower than expected, and FIX
   requires a member to treat that without `PossDupFlag` as fatal. The flag is not decoration.

   **Deliberately not built**, and the honest limits of what a member gets back:

   - **No outbound message store.** Only execution reports are replayable, because only they
     are in the WAL. Administrative messages are gap-filled, as FIX permits.
   - **Replay depth is bounded by WAL retention.** Snapshots truncate the WAL, and anything
     older than the retained segments cannot be replayed. A real venue would keep a separate
     outbound store for the trading day.
   - **No durability across a venue restart.** The remembered sequence numbers live in the
     sequencer's memory; restarting the pair loses them.
   - **The binary gateway is unchanged.** It has no session layer to hang a resend on, so
     "in-flight reports survive" means something different there -- see the open questions.

Steps 1-3 are the SPOF work and are worth landing on their own. Steps 4-6 are the in-flight-report
decision and are the larger half.

---

## Open questions

- ~~What does a member nominate as its recovery position?~~ **Answered by step 6, and the
  answer avoided the mapping rather than building it.** `BeginSeqNo` is a session-level number
  and the WAL is numbered by the venue's own sequence, so no mapping between them exists. What
  the gateway sends instead is the *width* of the gap the member described, and the sequencer
  returns that many of the session's most recent reports. The member's numbering is then
  applied on the way out. That is exact when the gap is all execution reports, and when it is
  not, the administrative remainder is gap-filled -- which is the case the FIX split already
  covers. A true mapping would need the outbound store this deliberately does not have.
- How does a member discover that its primary is down? Connection refusal is the simple answer and
  is what venues rely on, but it interacts with logon timeouts.
- Does the binary order gateway get the same treatment, or does pinning apply only to FIX? It has no
  session-layer resend to build on, so "in-flight reports survive" means something different there
  and may need its own mechanism. *Settled for step 4: both gateways enforce pinning, and both
  refuse the same way. The question remains open for steps 5 and 6, which are the resend half.*
- **Should the WAL scan become an indexed lookup?** A replay is a scan from the oldest
  retained segment: 18 ms for 4 MB today, and linear in what the WAL holds. That is
  comfortable for a reconnect and would not be for anything frequent. The alternatives -- a
  per-session cursor, or segment skipping -- trade write-path cost or memory for it, and none
  is worth paying until a replay stops being rare. The user has separately raised making
  replay a first-class framework capability, which is where this belongs.
- **Disaster recovery is not modelled at all.** Venues publish a third address at a second site,
  under a different regime from the primary/backup pair: typically a start-of-day sequence reset
  and no in-flight state continuity, so it is not a third backup and must not be built as one. Not
  urgent, and deliberately out of scope for 0.3.0, but it will need its own design rather than an
  extra column.
