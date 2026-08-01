# Gateway High Availability {#gateway_ha}

How a member keeps trading when a gateway dies.

> **Status: agreed direction, not yet built. Planned for 0.3.0.**
>
> Nothing in the Design section below exists in the code as of 0.2.0. This document records the
> direction settled on 2026-07-30 so that the decisions — and the reasoning behind them — are
> written down before implementation starts, rather than being reconstructed afterwards.
>
> The "What exists today" section *is* current and was verified against the code, not inferred
> from documentation. Read that section as fact and everything after it as intent.

This document covers the order-entry gateways only. Sequencer and matching engine HA are in
[WAL and High Availability](wal_and_ha.md); this one deliberately supersedes that document's
"Gateway Pool" section, for reasons set out below.

---

## Decisions

Both taken on 2026-07-30. Both change what was previously written down, and neither is yet
reflected in the code.

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
capacity you divide by hand, not a balanced pool. Session provisioning (step 4) is what will make
the division deliberate rather than incidental.

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

| What fails | What happens today |
|---|---|
| The client's connection to `a` drops, `a` survives | `a` cancels that session's entire book at once. The member reconnects — to `a` or to `b` — and finds itself flat. Its positions were closed by a network blip. |
| The `a` process dies | No cancels are sent, because the process that would send them is dead. The orders stay resting in the matching engine, but no gateway holds their session and every report for them is dropped by the sequencer. The member reconnects to `b` and cannot see or cancel orders that are still live in the book. |

The first is the cancel storm step 3b's grace period exists to prevent. The second is arguably
worse — orphaned live orders are a real risk position that the member cannot manage — and it is
what steps 4-6 (session provisioning, re-keyed routing, session-slice replay) exist to close.

**Neither case is a regression.** Both follow from running more than one instance at all, which is
exactly why the design sequences 3b immediately after step 3 rather than with the later work: step
3 makes the failure demonstrable, and 3b is what makes the demonstration worth having.

### The short version

Two instances today mean *a venue that keeps trading when one gateway dies*. They do not yet mean
*a session that survives its gateway dying*. Until steps 3b-6 land, treat instance `b` as a place
for new sessions to go, not as a place old ones can be recovered.

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
   sequencer pushing at a connection that may not exist.
2. **Key the routing entry on the session, not the connection.** The connection triple stays as the
   *current destination*. The *key* becomes the provisioned session identity, so that a logon can
   re-bind the destination to a new connection on a different instance. This is the direct answer
   to gap 6: the identity that must survive is the session's, and the connection is a mutable
   attribute of it.
3. **Replay on logon.** The gateway, having authenticated a session, asks the sequencer for that
   session's reports from the member's nominated position and encodes them to the wire in its own
   protocol, with FIX `PossDupFlag=Y` where the report was previously sent.
4. **Retire the blanket gap-fill.** `handle_resend_request` stops answering everything with
   `SequenceReset-GapFill`. The FIX convention is that administrative messages are gap-filled and
   application messages are resent; execution reports are application messages. The feedback-loop
   hazard the current comment describes is real and must be avoided by resending the range in one
   pass, not by reverting to one-at-a-time filling.

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

The last two are recommendations rather than settled decisions.


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

4. **Session provisioning.** Primary and backup per comp id in the admin service and database;
   gateways reject a logon at an instance a session is not provisioned for.
5. **Re-key the routing entry** on session identity with the connection triple as destination.
6. **Session-slice replay on logon**, and retire the blanket gap-fill.

Steps 1-3 are the SPOF work and are worth landing on their own. Steps 4-6 are the in-flight-report
decision and are the larger half.

---

## Open questions

- What does a member nominate as its recovery position? FIX gives `BeginSeqNo` on the
  ResendRequest, which is a session-level sequence number and not the sequencer's. The mapping
  between the two has to live somewhere, and that somewhere is probably the session state that
  already has to survive the instance change.
- How does a member discover that its primary is down? Connection refusal is the simple answer and
  is what venues rely on, but it interacts with logon timeouts.
- Does the binary order gateway get the same treatment, or does pinning apply only to FIX? It has no
  session-layer resend to build on, so "in-flight reports survive" means something different there
  and may need its own mechanism.
