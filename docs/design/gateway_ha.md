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
`order_gateway = 1` and `binary_gateway = 2`. That value rides on the `WalRecord` envelope as
`origin_gateway_id` and is how the sequencer decides where to send an execution report.

Its own comment used to call these values a binding constraint — on-disk WAL format, never to be
reused. That was overstated and has been corrected: the project is pre-1.0 and makes no
compatibility promise across releases, so a WAL from an older build is discarded rather than
replayed. Reusing a value is worth avoiding, not forbidden.

**Both gateways stamp `origin_gateway_id` on every order.** The FIX gateway does it in
`forward_order_in_envelope`, a template in `OrderGatewayThread.hpp`; the binary gateway does it
at two sites in `BinaryGatewayThread.cpp`.

An earlier version of this document claimed the FIX gateway never constructs a `WalRecord` and
that `gateway_ids::default_when_absent` therefore covered a structural gap. **That was wrong** —
it came from grepping the `.cpp` and missing the template in the header. The default covers
records that have no gateway origin at all, which is a real category: `has_origin_gateway_id` is
set conditionally on there being a session connection, so execution reports and replayed records
can legitimately carry none.

That correction matters because it makes the fields **optional by design, not by legacy**. An
earlier draft proposed making both required; that would force a meaningless protocol and instance
onto every record that never came from a gateway.


**The sequencer dials a fixed pair of endpoints.** `SequencerConfiguration` holds scalars —
`gateway_host`/`gateway_port` for the FIX gateway, `binary_gateway_host`/`binary_gateway_port` and
a `binary_gateway_enabled` flag for the binary one. There is no collection, so there is nowhere to
express a second instance of either.

**An execution report for a disconnected gateway is dropped.**
`SequencerThread::send_er_to_origin_gateway` logs `gateway id {} not connected -- dropping ER
seq={}` and returns. Nothing retries and nothing queues.

**There is no outbound message store.** `FixSession` holds `outbound_seq_num` as a plain `int` and
no record of what was sent. `OrderGatewayThread::handle_resend_request` answers *every*
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
port. `binary_gateway_enabled` disappears: an absent entry is a gateway that is not deployed.

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

### Does cancel-on-disconnect stay?

**Open, and it needs deciding before implementation.** Cancel-on-disconnect is built and working
today. It is also the standard venue answer to the risk half of a gateway failure — a member that
cannot reach the venue cannot manage its exposure, so the venue flattens it.

It sits awkwardly with "in-flight reports survive": if every order is cancelled the moment the
connection drops, then what survives to be recovered is the *history* — fills that happened before
the cancel, and the cancel reports themselves — rather than working orders the member can continue
to manage. That is still worth having and members do expect it. But if the intent is that a member
reconnects to its backup and finds its book intact, cancel-on-disconnect has to become
configurable, and probably per comp id.

The two are not in conflict; they answer different questions. But the answer changes what the
recovery path is *for*, so it should be settled before the work starts.

---

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

   2a: the sequencer's scalar gateway host/port pairs and the `binary_gateway.enabled` flag
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

3. **Run two FIX gateway instances in dev.** The first point at which the SPOF is actually
   reduced, and the first honest test of steps 1 and 2. Expect this to surface things this document
   has not predicted.
4. **Session provisioning.** Primary and backup per comp id in the admin service and database;
   gateways reject a logon at an instance a session is not provisioned for.
5. **Re-key the routing entry** on session identity with the connection triple as destination.
6. **Session-slice replay on logon**, and retire the blanket gap-fill.

Steps 1-3 are the SPOF work and are worth landing on their own. Steps 4-6 are the in-flight-report
decision and are the larger half.

---

## Open questions

- Does cancel-on-disconnect stay, become configurable, or go? See above — it changes what recovery
  is for.
- What does a member nominate as its recovery position? FIX gives `BeginSeqNo` on the
  ResendRequest, which is a session-level sequence number and not the sequencer's. The mapping
  between the two has to live somewhere, and that somewhere is probably the session state that
  already has to survive the instance change.
- How does a member discover that its primary is down? Connection refusal is the simple answer and
  is what venues rely on, but it interacts with logon timeouts.
- Does the binary gateway get the same treatment, or does pinning apply only to FIX? It has no
  session-layer resend to build on, so "in-flight reports survive" means something different there
  and may need its own mechanism.
